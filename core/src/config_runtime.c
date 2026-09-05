#include "config_runtime.h"

#include "config.h"
#include "input.h"
#include "layout.h"
#include "workspace.h"
#include "server.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

typedef struct onyrion_config_runtime {
    OnyrionServer *server;
    OnyrionCoreConfig *config;

    char *path;
    char *directory;
    char *basename;

    int inotify_fd;
    int watch_descriptor;

    struct wl_event_source *fd_source;
    struct wl_event_source *reload_timer;
} OnyrionConfigRuntime;

enum {
    RELOAD_DEBOUNCE_MS = 75,
};

static char *duplicate_range(
        const char *value,
        size_t length) {
    char *copy =
        malloc(length + 1);

    if (!copy) {
        return NULL;
    }

    memcpy(copy, value, length);
    copy[length] = '\0';

    return copy;
}

static bool split_path(
        const char *path,
        char **directory,
        char **basename) {
    *directory = NULL;
    *basename = NULL;

    const char *slash =
        strrchr(path, '/');

    if (!slash) {
        *directory = strdup(".");
        *basename = strdup(path);
    } else if (slash == path) {
        *directory = strdup("/");
        *basename = strdup(slash + 1);
    } else {
        *directory =
            duplicate_range(
                path,
                (size_t)(slash - path)
            );

        *basename = strdup(slash + 1);
    }

    if (!*directory ||
            !*basename ||
            (*basename)[0] == '\0') {
        free(*directory);
        free(*basename);

        *directory = NULL;
        *basename = NULL;

        return false;
    }

    return true;
}

static bool ensure_directory_tree(
        const char *directory) {
    if (!directory ||
            directory[0] == '\0') {
        return false;
    }

    char *path = strdup(directory);

    if (!path) {
        return false;
    }

    char *cursor =
        path[0] == '/'
            ? path + 1
            : path;

    for (;; cursor++) {
        if (*cursor != '/' &&
                *cursor != '\0') {
            continue;
        }

        const char saved = *cursor;
        *cursor = '\0';

        if (path[0] != '\0' &&
                mkdir(path, 0700) != 0 &&
                errno != EEXIST) {
            const int error = errno;

            free(path);
            errno = error;

            return false;
        }

        *cursor = saved;

        if (saved == '\0') {
            break;
        }
    }

    free(path);
    return true;
}

static int handle_reload_timer(void *data) {
    OnyrionConfigRuntime *runtime =
        data;

    char error[256] = {0};

    OnyrionCoreConfig candidate = {0};

    if (!onyrion_core_config_load(
            &candidate,
            runtime->path,
            false,
            error,
            sizeof(error))) {
        wlr_log(
            WLR_ERROR,
            "Config reload rejected;"
            " keeping last-known-good compositor policy:"
            " %s",
            error
        );

        return 0;
    }

    candidate.explicit_config =
        runtime->config->explicit_config;

    candidate.source =
        runtime->config->explicit_config
            ? ONYRION_CONFIG_SOURCE_EXPLICIT
            : runtime->config->source ==
                ONYRION_CONFIG_SOURCE_LEGACY
                ? ONYRION_CONFIG_SOURCE_LEGACY
                : ONYRION_CONFIG_SOURCE_CURRENT;

    if (!onyrion_workspace_policy_validate_runtime(
            runtime->server,
            &candidate.policy.workspaces,
            error,
            sizeof(error))) {
        wlr_log(
            WLR_ERROR,
            "Config reload rejected;"
            " workspace policy validation failed;"
            " keeping last-known-good compositor policy:"
            " %s",
            error
        );

        onyrion_core_config_finish(
            &candidate
        );

        return 0;
    }

    if (!onyrion_input_apply_keyboard_policy(
            runtime->server,
            &candidate.policy.input.keyboard)) {
        wlr_log(
            WLR_ERROR,
            "Config reload rejected;"
            " keyboard policy apply failed;"
            " keeping last-known-good compositor policy"
        );

        onyrion_core_config_finish(
            &candidate
        );

        return 0;
    }

    if (!onyrion_input_apply_pointer_policy(
            runtime->server,
            &candidate.policy.input)) {
        wlr_log(
            WLR_ERROR,
            "Config reload rejected;"
            " pointer policy apply failed;"
            " restoring last-known-good keyboard policy"
        );

        (void)onyrion_input_apply_keyboard_policy(
            runtime->server,
            &runtime->config->
                policy.input.keyboard
        );

        onyrion_core_config_finish(
            &candidate
        );

        return 0;
    }

    onyrion_workspace_policy_apply_runtime(
        runtime->server,
        &candidate.policy.workspaces
    );

    OnyrionCoreConfig previous =
        *runtime->config;

    *runtime->config =
        candidate;

    candidate =
        previous;

    runtime->server->policy =
        &runtime->config->policy;

    onyrion_layout_reflow(
        runtime->server
    );

    char persist_error[256] = {0};

    if (!onyrion_core_config_persist_lkg(
            runtime->config,
            persist_error,
            sizeof(persist_error))) {
        wlr_log(
            WLR_ERROR,
            "Config policy applied but persistent LKG update failed: %s",
            persist_error
        );
    }

    wlr_log(
        WLR_INFO,
        "Config compositor policy reloaded:"
        " bindings=%zu repeat_rate=%d"
        " repeat_delay=%d layout=%s options=%s"
        " split_ratio=%.6f resize_step=%.6f"
        " resize_min=%.6f resize_max=%.6f"
        " outer_gap=%d inner_gap=%d"
        " workspaces=%zu window_rules=%zu"
        " source=%s",
        runtime->config->
            policy.input.binding_count,
        runtime->config->
            policy.input.keyboard.repeat_rate,
        runtime->config->
            policy.input.keyboard.repeat_delay,
        runtime->config->
            policy.input.keyboard.layout[0]
                ? runtime->config->
                    policy.input.keyboard.layout
                : "<default>",
        runtime->config->
            policy.input.keyboard.options[0]
                ? runtime->config->
                    policy.input.keyboard.options
                : "<default>",
        runtime->config->
            policy.layout.initial_split_ratio,
        runtime->config->
            policy.layout.resize_step,
        runtime->config->
            policy.layout.resize_min,
        runtime->config->
            policy.layout.resize_max,
        runtime->config->
            policy.layout.outer_gap,
        runtime->config->
            policy.layout.inner_gap,
        runtime->config->
            policy.workspaces.count,
        runtime->config->
            policy.window_rules.count,
        onyrion_core_config_source_name(
            runtime->config->source)
    );

    onyrion_core_config_finish(
        &candidate
    );

    return 0;
}

static void schedule_reload(
        OnyrionConfigRuntime *runtime) {
    if (!runtime->reload_timer) {
        return;
    }

    if (wl_event_source_timer_update(
            runtime->reload_timer,
            RELOAD_DEBOUNCE_MS) != 0) {
        wlr_log(
            WLR_ERROR,
            "Config reload debounce timer update failed"
        );
    }
}

static int handle_inotify(
        int fd,
        uint32_t mask,
        void *data) {
    OnyrionConfigRuntime *runtime =
        data;

    if (mask &
            (WL_EVENT_HANGUP |
             WL_EVENT_ERROR)) {
        wlr_log(
            WLR_ERROR,
            "Config inotify event source failed"
        );

        return 0;
    }

    union {
        struct inotify_event alignment;
        char bytes[8192];
    } buffer;

    for (;;) {
        const ssize_t count =
            read(
                fd,
                buffer.bytes,
                sizeof(buffer.bytes)
            );

        if (count < 0) {
            if (errno == EAGAIN ||
                    errno == EWOULDBLOCK) {
                break;
            }

            wlr_log(
                WLR_ERROR,
                "Config inotify read failed: %s",
                strerror(errno)
            );

            break;
        }

        if (count == 0) {
            break;
        }

        size_t offset = 0;

        while (offset <
                (size_t)count) {
            const struct inotify_event *event =
                (const struct inotify_event *)
                    (buffer.bytes + offset);

            if (event->mask &
                    IN_Q_OVERFLOW) {
                schedule_reload(runtime);
            } else if (event->len > 0 &&
                    strcmp(
                        event->name,
                        runtime->basename) == 0 &&
                    (event->mask &
                        (IN_CLOSE_WRITE |
                         IN_MOVED_TO |
                         IN_CREATE |
                         IN_DELETE |
                         IN_ATTRIB))) {
                schedule_reload(runtime);
            }

            offset +=
                sizeof(*event) +
                event->len;
        }
    }

    return 0;
}

static void runtime_destroy(
        OnyrionConfigRuntime *runtime) {
    if (!runtime) {
        return;
    }

    if (runtime->reload_timer) {
        wl_event_source_remove(
            runtime->reload_timer
        );
    }

    if (runtime->fd_source) {
        wl_event_source_remove(
            runtime->fd_source
        );
    }

    if (runtime->watch_descriptor >= 0 &&
            runtime->inotify_fd >= 0) {
        inotify_rm_watch(
            runtime->inotify_fd,
            runtime->watch_descriptor
        );
    }

    if (runtime->inotify_fd >= 0) {
        close(runtime->inotify_fd);
    }

    free(runtime->path);
    free(runtime->directory);
    free(runtime->basename);
    free(runtime);
}

bool onyrion_config_runtime_init(
        struct onyrion_server *server,
        struct onyrion_core_config *config,
        const char *path) {
    if (!server ||
            !config ||
            !path ||
            path[0] == '\0') {
        return false;
    }

    OnyrionConfigRuntime *runtime =
        calloc(
            1,
            sizeof(*runtime)
        );

    if (!runtime) {
        return false;
    }

    runtime->server = server;
    runtime->config = config;
    runtime->inotify_fd = -1;
    runtime->watch_descriptor = -1;
    runtime->path = strdup(path);

    if (!runtime->path ||
            !split_path(
                path,
                &runtime->directory,
                &runtime->basename)) {
        runtime_destroy(runtime);
        return false;
    }

    if (!ensure_directory_tree(
            runtime->directory)) {
        wlr_log(
            WLR_ERROR,
            "Config directory creation failed for '%s': %s",
            runtime->directory,
            strerror(errno)
        );

        runtime_destroy(runtime);
        return false;
    }

    runtime->inotify_fd =
        inotify_init1(
            IN_NONBLOCK |
            IN_CLOEXEC
        );

    if (runtime->inotify_fd < 0) {
        wlr_log(
            WLR_ERROR,
            "Config inotify init failed: %s",
            strerror(errno)
        );

        runtime_destroy(runtime);
        return false;
    }

    runtime->watch_descriptor =
        inotify_add_watch(
            runtime->inotify_fd,
            runtime->directory,
            IN_CLOSE_WRITE |
            IN_MOVED_TO |
            IN_CREATE |
            IN_DELETE |
            IN_ATTRIB
        );

    if (runtime->watch_descriptor < 0) {
        wlr_log(
            WLR_ERROR,
            "Config watch failed for '%s': %s",
            runtime->directory,
            strerror(errno)
        );

        runtime_destroy(runtime);
        return false;
    }

    struct wl_event_loop *event_loop =
        wl_display_get_event_loop(
            server->display
        );

    if (!event_loop) {
        runtime_destroy(runtime);
        return false;
    }

    runtime->fd_source =
        wl_event_loop_add_fd(
            event_loop,
            runtime->inotify_fd,
            WL_EVENT_READABLE,
            handle_inotify,
            runtime
        );

    if (!runtime->fd_source) {
        runtime_destroy(runtime);
        return false;
    }

    runtime->reload_timer =
        wl_event_loop_add_timer(
            event_loop,
            handle_reload_timer,
            runtime
        );

    if (!runtime->reload_timer) {
        runtime_destroy(runtime);
        return false;
    }

    server->config_runtime =
        runtime;

    wlr_log(
        WLR_INFO,
        "Config compositor watcher ready:"
        " path=%s debounce_ms=%d",
        runtime->path,
        RELOAD_DEBOUNCE_MS
    );

    return true;
}

void onyrion_config_runtime_finish(
        struct onyrion_server *server) {
    if (!server ||
            !server->config_runtime) {
        return;
    }

    OnyrionConfigRuntime *runtime =
        server->config_runtime;

    server->config_runtime = NULL;

    runtime_destroy(runtime);
}
