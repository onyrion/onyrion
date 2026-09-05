#include "provider_runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct provider_runtime {
    ProviderManager *manager;
    const ProviderConfig *config;

    GPid pid;
    guint child_watch_source;
    guint restart_source;

    guint health_source;
    GPid health_pid;
    guint health_child_watch_source;
    guint health_timeout_source;
    bool health_timed_out;
    gint64 health_started_us;

    ProviderRuntimeState state;
    unsigned restart_attempt;
};

static const char *runtime_state_name(
        ProviderRuntimeState state) {
    switch (state) {
    case PROVIDER_RUNTIME_STOPPED:
        return "stopped";
    case PROVIDER_RUNTIME_STARTING:
        return "starting";
    case PROVIDER_RUNTIME_READY:
        return "ready";
    case PROVIDER_RUNTIME_BACKOFF:
        return "backoff";
    case PROVIDER_RUNTIME_UNAVAILABLE:
        return "unavailable";
    }

    return "unknown";
}

static void log_resolution_for_capability_action(
        const ProviderManager *manager,
        const char *capability,
        const char *action) {
    const ProviderConfig *provider =
        provider_manager_resolve_capability_action(
            manager,
            capability,
            action
        );

    if (provider) {
        printf(
            "PROVIDER RESOLVE capability=%s action=%s provider=%s priority=%d\n",
            capability,
            action,
            provider->id,
            provider->priority
        );
    } else {
        printf(
            "PROVIDER RESOLVE capability=%s action=%s provider=-\n",
            capability,
            action
        );
    }

    fflush(stdout);
}

static void log_runtime_state(
        ProviderRuntime *runtime) {
    printf(
        "PROVIDER STATE id=%s state=%s pid=%ld restart_attempt=%u\n",
        runtime->config->id,
        runtime_state_name(runtime->state),
        (long)runtime->pid,
        runtime->restart_attempt
    );

    for (size_t i = 0; i < runtime->config->capability_count; i++) {
        const ProviderCapabilityConfig *capability =
            &runtime->config->capabilities[i];

        for (size_t j = 0; j < capability->action_count; j++) {
            log_resolution_for_capability_action(
                runtime->manager,
                capability->id,
                capability->actions[j].name
            );
        }
    }
}

static unsigned restart_delay_ms(
        const ProviderRuntime *runtime) {
    uint64_t delay =
        runtime->config->restart_delay_ms;

    for (unsigned i = 1; i < runtime->restart_attempt; i++) {
        delay *= 2;

        if (delay >= runtime->config->restart_max_delay_ms) {
            return runtime->config->restart_max_delay_ms;
        }
    }

    if (delay > runtime->config->restart_max_delay_ms) {
        delay = runtime->config->restart_max_delay_ms;
    }

    return (unsigned)delay;
}

static bool should_restart(
        const ProviderRuntime *runtime,
        bool success) {
    switch (runtime->config->restart_policy) {
    case PROVIDER_RESTART_NEVER:
        return false;
    case PROVIDER_RESTART_ON_FAILURE:
        return !success;
    case PROVIDER_RESTART_ALWAYS:
        return true;
    }

    return false;
}

static bool spawn_provider(
    ProviderRuntime *runtime
);

static gboolean start_health_probe(
    gpointer data
);

static void cancel_health_probe(
        ProviderRuntime *runtime) {
    if (runtime->health_source != 0) {
        g_source_remove(runtime->health_source);
        runtime->health_source = 0;
    }

    if (runtime->health_timeout_source != 0) {
        g_source_remove(runtime->health_timeout_source);
        runtime->health_timeout_source = 0;
    }

    if (runtime->health_child_watch_source != 0) {
        g_source_remove(runtime->health_child_watch_source);
        runtime->health_child_watch_source = 0;
    }

    if (runtime->health_pid > 0) {
        const GPid pid = runtime->health_pid;
        runtime->health_pid = 0;

        (void)kill(pid, SIGKILL);

        while (waitpid(
                pid,
                NULL,
                0) < 0 &&
                errno == EINTR) {
        }

        g_spawn_close_pid(pid);
    }

    runtime->health_timed_out = false;
}

static void schedule_health_probe(
        ProviderRuntime *runtime,
        unsigned delay_ms) {
    if (runtime->manager->stopping ||
            runtime->pid <= 0 ||
            runtime->health_source != 0 ||
            runtime->health_pid > 0) {
        return;
    }

    runtime->health_source =
        g_timeout_add(
            delay_ms > 0 ? delay_ms : 1,
            start_health_probe,
            runtime
        );
}

static bool health_startup_expired(
        const ProviderRuntime *runtime) {
    if (runtime->health_started_us <= 0) {
        return false;
    }

    const gint64 elapsed_us =
        g_get_monotonic_time() -
        runtime->health_started_us;

    return elapsed_us >=
        (gint64)runtime->config->health_startup_timeout_ms * 1000;
}

static void handle_health_result(
        ProviderRuntime *runtime,
        bool success,
        const char *reason) {
    printf(
        "PROVIDER HEALTH id=%s result=%s reason=%s\n",
        runtime->config->id,
        success ? "pass" : "fail",
        reason
    );
    fflush(stdout);

    if (runtime->manager->stopping ||
            runtime->pid <= 0) {
        return;
    }

    if (success) {
        if (runtime->state != PROVIDER_RUNTIME_READY) {
            runtime->state = PROVIDER_RUNTIME_READY;
            log_runtime_state(runtime);
        }

        schedule_health_probe(
            runtime,
            runtime->config->health_interval_ms
        );
        return;
    }

    if (runtime->state == PROVIDER_RUNTIME_STARTING &&
            health_startup_expired(runtime)) {
        runtime->state = PROVIDER_RUNTIME_UNAVAILABLE;

        printf(
            "PROVIDER HEALTH STARTUP TIMEOUT id=%s timeout_ms=%u\n",
            runtime->config->id,
            runtime->config->health_startup_timeout_ms
        );

        log_runtime_state(runtime);

        if (kill(runtime->pid, SIGTERM) < 0 &&
                errno != ESRCH) {
            fprintf(
                stderr,
                "PROVIDER WARN id=%s health-terminate=%s\n",
                runtime->config->id,
                strerror(errno)
            );
        }

        return;
    }

    if (runtime->state == PROVIDER_RUNTIME_READY) {
        runtime->state = PROVIDER_RUNTIME_UNAVAILABLE;
        log_runtime_state(runtime);
    }

    schedule_health_probe(
        runtime,
        runtime->config->health_interval_ms
    );
}

static gboolean health_probe_timeout(
        gpointer data) {
    ProviderRuntime *runtime = data;

    runtime->health_timeout_source = 0;

    if (runtime->health_pid > 0) {
        runtime->health_timed_out = true;
        (void)kill(runtime->health_pid, SIGKILL);
    }

    return G_SOURCE_REMOVE;
}

static void health_probe_exited(
        GPid pid,
        gint wait_status,
        gpointer data) {
    ProviderRuntime *runtime = data;

    runtime->health_child_watch_source = 0;
    runtime->health_pid = 0;

    if (runtime->health_timeout_source != 0) {
        g_source_remove(runtime->health_timeout_source);
        runtime->health_timeout_source = 0;
    }

    const bool timed_out =
        runtime->health_timed_out;

    runtime->health_timed_out = false;

    bool success =
        !timed_out &&
        WIFEXITED(wait_status) &&
        WEXITSTATUS(wait_status) == 0;

    const bool startup_expired =
        runtime->state == PROVIDER_RUNTIME_STARTING &&
        health_startup_expired(runtime);

    if (startup_expired) {
        success = false;
    }

    g_spawn_close_pid(pid);

    handle_health_result(
        runtime,
        success,
        startup_expired
            ? "startup-timeout"
            : timed_out
                ? "timeout"
                : success
                    ? "exit-0"
                    : "nonzero"
    );
}

static gboolean start_health_probe(
        gpointer data) {
    ProviderRuntime *runtime = data;

    runtime->health_source = 0;

    if (runtime->manager->stopping ||
            runtime->pid <= 0 ||
            runtime->config->health_mode != PROVIDER_HEALTH_COMMAND) {
        return G_SOURCE_REMOVE;
    }

    if (runtime->state == PROVIDER_RUNTIME_STARTING &&
            health_startup_expired(runtime)) {
        handle_health_result(
            runtime,
            false,
            "startup-timeout"
        );
        return G_SOURCE_REMOVE;
    }

    GError *error = NULL;
    GPid pid = 0;

    const gboolean spawned =
        g_spawn_async(
            NULL,
            runtime->config->health_argv,
            NULL,
            G_SPAWN_SEARCH_PATH |
                G_SPAWN_DO_NOT_REAP_CHILD |
                G_SPAWN_STDOUT_TO_DEV_NULL |
                G_SPAWN_STDERR_TO_DEV_NULL,
            NULL,
            NULL,
            &pid,
            &error
        );

    if (!spawned) {
        fprintf(
            stderr,
            "PROVIDER HEALTH FAIL id=%s reason=spawn error=%s\n",
            runtime->config->id,
            error
                ? error->message
                : "unknown"
        );

        g_clear_error(&error);

        handle_health_result(
            runtime,
            false,
            "spawn"
        );

        return G_SOURCE_REMOVE;
    }

    runtime->health_pid = pid;
    runtime->health_timed_out = false;

    runtime->health_child_watch_source =
        g_child_watch_add(
            pid,
            health_probe_exited,
            runtime
        );

    if (runtime->health_child_watch_source == 0) {
        (void)kill(pid, SIGKILL);

        while (waitpid(
                pid,
                NULL,
                0) < 0 &&
                errno == EINTR) {
        }

        g_spawn_close_pid(pid);
        runtime->health_pid = 0;

        handle_health_result(
            runtime,
            false,
            "child-watch"
        );

        return G_SOURCE_REMOVE;
    }

    runtime->health_timeout_source =
        g_timeout_add(
            runtime->config->health_timeout_ms,
            health_probe_timeout,
            runtime
        );

    if (runtime->health_timeout_source == 0) {
        runtime->health_timed_out = true;
        (void)kill(runtime->health_pid, SIGKILL);
    }

    return G_SOURCE_REMOVE;
}

static gboolean restart_provider(
        gpointer data) {
    ProviderRuntime *runtime = data;

    runtime->restart_source = 0;

    if (runtime->manager->stopping) {
        return G_SOURCE_REMOVE;
    }

    (void)spawn_provider(runtime);

    return G_SOURCE_REMOVE;
}

static void schedule_restart(
        ProviderRuntime *runtime) {
    runtime->restart_attempt++;

    const unsigned delay =
        restart_delay_ms(runtime);

    runtime->state = PROVIDER_RUNTIME_BACKOFF;

    printf(
        "PROVIDER RESTART id=%s delay_ms=%u attempt=%u\n",
        runtime->config->id,
        delay,
        runtime->restart_attempt
    );

    log_runtime_state(runtime);

    runtime->restart_source =
        g_timeout_add(
            delay,
            restart_provider,
            runtime
        );

    if (runtime->restart_source == 0) {
        runtime->state = PROVIDER_RUNTIME_UNAVAILABLE;
        log_runtime_state(runtime);
    }
}

static void provider_child_exited(
        GPid pid,
        gint wait_status,
        gpointer data) {
    ProviderRuntime *runtime = data;

    runtime->child_watch_source = 0;
    runtime->pid = 0;
    cancel_health_probe(runtime);

    const bool success =
        WIFEXITED(wait_status) &&
        WEXITSTATUS(wait_status) == 0;

    if (WIFEXITED(wait_status)) {
        printf(
            "PROVIDER EXIT id=%s exit=%d\n",
            runtime->config->id,
            WEXITSTATUS(wait_status)
        );
    } else if (WIFSIGNALED(wait_status)) {
        printf(
            "PROVIDER EXIT id=%s signal=%d\n",
            runtime->config->id,
            WTERMSIG(wait_status)
        );
    } else {
        printf(
            "PROVIDER EXIT id=%s status=%d\n",
            runtime->config->id,
            wait_status
        );
    }

    g_spawn_close_pid(pid);

    if (runtime->manager->stopping) {
        runtime->state = PROVIDER_RUNTIME_STOPPED;
        log_runtime_state(runtime);
        return;
    }

    if (should_restart(runtime, success)) {
        schedule_restart(runtime);
    } else {
        runtime->state = PROVIDER_RUNTIME_UNAVAILABLE;
        log_runtime_state(runtime);
    }
}

static bool spawn_provider(
        ProviderRuntime *runtime) {
    if (!runtime->config->exec_argv ||
            runtime->config->exec_argc == 0) {
        fprintf(
            stderr,
            "PROVIDER FAIL id=%s reason=no-exec\n",
            runtime->config->id
        );

        return false;
    }

    GError *error = NULL;
    GPid pid = 0;

    const gboolean spawned =
        g_spawn_async(
            NULL,
            runtime->config->exec_argv,
            NULL,
            G_SPAWN_SEARCH_PATH |
                G_SPAWN_DO_NOT_REAP_CHILD,
            NULL,
            NULL,
            &pid,
            &error
        );

    if (!spawned) {
        fprintf(
            stderr,
            "PROVIDER FAIL id=%s reason=spawn error=%s\n",
            runtime->config->id,
            error
                ? error->message
                : "unknown"
        );

        g_clear_error(&error);

        if (!runtime->manager->stopping &&
                runtime->config->restart_policy != PROVIDER_RESTART_NEVER) {
            schedule_restart(runtime);
        } else {
            runtime->state = PROVIDER_RUNTIME_UNAVAILABLE;
            log_runtime_state(runtime);
        }

        return false;
    }

    runtime->pid = pid;

    runtime->child_watch_source =
        g_child_watch_add(
            pid,
            provider_child_exited,
            runtime
        );

    if (runtime->child_watch_source == 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        g_spawn_close_pid(pid);
        runtime->pid = 0;

        fprintf(
            stderr,
            "PROVIDER FAIL id=%s reason=child-watch\n",
            runtime->config->id
        );

        if (!runtime->manager->stopping &&
                runtime->config->restart_policy != PROVIDER_RESTART_NEVER) {
            schedule_restart(runtime);
        } else {
            runtime->state = PROVIDER_RUNTIME_UNAVAILABLE;
            log_runtime_state(runtime);
        }

        return false;
    }

    printf(
        "PROVIDER START id=%s pid=%ld health=%s\n",
        runtime->config->id,
        (long)runtime->pid,
        provider_health_mode_name(
            runtime->config->health_mode
        )
    );

    if (runtime->config->health_mode == PROVIDER_HEALTH_COMMAND) {
        runtime->state = PROVIDER_RUNTIME_STARTING;
        runtime->health_started_us = g_get_monotonic_time();
        schedule_health_probe(runtime, 1);
    } else {
        runtime->state = PROVIDER_RUNTIME_READY;
        runtime->health_started_us = 0;
    }

    log_runtime_state(runtime);

    return true;
}

void provider_manager_init(
        ProviderManager *manager,
        const ShellConfig *config) {
    *manager = (ProviderManager){
        .config = config,
        .provider_count = config->provider_count,
    };

    if (manager->provider_count == 0) {
        return;
    }

    manager->providers =
        g_new0(
            ProviderRuntime,
            manager->provider_count
        );

    if (!manager->providers) {
        manager->provider_count = 0;
        return;
    }

    for (size_t i = 0; i < manager->provider_count; i++) {
        manager->providers[i] = (ProviderRuntime){
            .manager = manager,
            .config = &config->providers[i],
            .state = PROVIDER_RUNTIME_STOPPED,
        };
    }
}

void provider_manager_finish(
        ProviderManager *manager) {
    provider_manager_stop(manager);
    g_free(manager->providers);
    *manager = (ProviderManager){0};
}

bool provider_manager_start(
        ProviderManager *manager) {
    if (manager->provider_count > 0 &&
            !manager->providers) {
        return false;
    }

    bool all_required_started = true;

    for (size_t i = 0; i < manager->provider_count; i++) {
        ProviderRuntime *runtime =
            &manager->providers[i];

        if (!runtime->config->autostart) {
            continue;
        }

        const bool started =
            spawn_provider(runtime);

        if (!started && runtime->config->required) {
            all_required_started = false;
        }
    }

    return all_required_started;
}

static void stop_provider(
        ProviderRuntime *runtime) {
    cancel_health_probe(runtime);

    if (runtime->restart_source != 0) {
        g_source_remove(runtime->restart_source);
        runtime->restart_source = 0;
    }

    if (runtime->child_watch_source != 0) {
        g_source_remove(runtime->child_watch_source);
        runtime->child_watch_source = 0;
    }

    if (runtime->pid <= 0) {
        runtime->state = PROVIDER_RUNTIME_STOPPED;
        return;
    }

    const GPid pid = runtime->pid;
    runtime->pid = 0;

    if (kill(pid, SIGTERM) < 0 &&
            errno != ESRCH) {
        fprintf(
            stderr,
            "PROVIDER WARN id=%s terminate=%s\n",
            runtime->config->id,
            strerror(errno)
        );
    }

    int status = 0;
    bool exited = false;

    for (unsigned i = 0; i < 20; i++) {
        const pid_t result =
            waitpid(
                pid,
                &status,
                WNOHANG
            );

        if (result == pid ||
                (result < 0 && errno == ECHILD)) {
            exited = true;
            break;
        }

        if (result < 0 && errno != EINTR) {
            break;
        }

        g_usleep(25 * 1000);
    }

    if (!exited) {
        kill(pid, SIGKILL);

        while (waitpid(
                pid,
                &status,
                0) < 0 &&
                errno == EINTR) {
        }
    }

    g_spawn_close_pid(pid);
    runtime->state = PROVIDER_RUNTIME_STOPPED;
    log_runtime_state(runtime);
}

void provider_manager_stop(
        ProviderManager *manager) {
    if (!manager || manager->stopping) {
        return;
    }

    manager->stopping = true;

    for (size_t i = 0; i < manager->provider_count; i++) {
        stop_provider(
            &manager->providers[i]
        );
    }
}

const ProviderConfig *provider_manager_resolve_capability_action(
        const ProviderManager *manager,
        const char *capability,
        const char *action) {
    const ProviderConfig *best = NULL;

    if (!manager || !capability || !action) {
        return NULL;
    }

    for (size_t i = 0; i < manager->provider_count; i++) {
        const ProviderRuntime *runtime = &manager->providers[i];

        if (runtime->state != PROVIDER_RUNTIME_READY ||
                !provider_config_find_action(
                    runtime->config,
                    capability,
                    action)) {
            continue;
        }

        if (!best || runtime->config->priority > best->priority) {
            best = runtime->config;
        }
    }

    return best;
}

static const ProviderRuntime *resolve_action_runtime(
        const ProviderManager *manager,
        const char *capability,
        const char *action,
        const bool *excluded) {
    const ProviderRuntime *best = NULL;

    for (size_t i = 0; i < manager->provider_count; i++) {
        const ProviderRuntime *runtime = &manager->providers[i];

        if (excluded && excluded[i]) {
            continue;
        }

        if (runtime->state != PROVIDER_RUNTIME_READY ||
                !provider_config_find_action(
                    runtime->config,
                    capability,
                    action)) {
            continue;
        }

        if (!best || runtime->config->priority > best->config->priority) {
            best = runtime;
        }
    }

    return best;
}

bool provider_manager_invoke(
        ProviderManager *manager,
        const char *capability,
        const char *action,
        const ProviderConfig **used_provider,
        GError **error) {
    if (used_provider) {
        *used_provider = NULL;
    }

    if (!manager || !capability || !action) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "invalid provider invocation request"
        );
        return false;
    }

    bool *excluded = g_new0(bool, manager->provider_count);
    if (manager->provider_count > 0 && !excluded) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOMEM,
            "out of memory"
        );
        return false;
    }

    for (size_t attempt = 0; attempt < manager->provider_count; attempt++) {
        const ProviderRuntime *runtime = resolve_action_runtime(
            manager,
            capability,
            action,
            excluded
        );
        if (!runtime) {
            break;
        }

        const size_t index = (size_t)(runtime - manager->providers);
        excluded[index] = true;

        const ProviderActionConfig *provider_action = provider_config_find_action(
            runtime->config,
            capability,
            action
        );

        GError *spawn_error = NULL;
        const gboolean spawned = g_spawn_async(
            NULL,
            provider_action->argv,
            NULL,
            G_SPAWN_SEARCH_PATH,
            NULL,
            NULL,
            NULL,
            &spawn_error
        );

        if (spawned) {
            printf(
                "PROVIDER INVOKE capability=%s action=%s provider=%s\n",
                capability,
                action,
                runtime->config->id
            );
            fflush(stdout);

            if (used_provider) {
                *used_provider = runtime->config;
            }

            g_free(excluded);
            return true;
        }

        fprintf(
            stderr,
            "PROVIDER ACTION FAIL capability=%s action=%s provider=%s error=%s\n",
            capability,
            action,
            runtime->config->id,
            spawn_error ? spawn_error->message : "unknown"
        );
        g_clear_error(&spawn_error);
    }

    g_free(excluded);

    g_set_error(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_NOENT,
        "no ready provider can handle capability '%s' action '%s'",
        capability,
        action
    );
    return false;
}
