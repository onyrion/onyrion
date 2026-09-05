#include "config_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

enum {
    CONFIG_RELOAD_DEBOUNCE_MS = 75,
};

struct shell_config_runtime {
    char *path;
    char *directory;
    char *basename;
    char *lkg_path;

    ShellConfig *config;
    ProviderManager *providers;

    int inotify_fd;
    int watch_descriptor;
    guint fd_source;
    guint reload_source;
    uint64_t generation;
};


static const char *const DEFAULT_WALLPAPER =
    "/usr/share/onyrion/ui/ewwii/default-wallpaper.svg";

static bool wallpaper_extension_supported(
        const char *name) {
    const char *dot =
        strrchr(
            name,
            '.'
        );

    if (!dot || dot[1] == '\0') {
        return false;
    }

    return
        g_ascii_strcasecmp(dot, ".png") == 0 ||
        g_ascii_strcasecmp(dot, ".jpg") == 0 ||
        g_ascii_strcasecmp(dot, ".jpeg") == 0 ||
        g_ascii_strcasecmp(dot, ".webp") == 0 ||
        g_ascii_strcasecmp(dot, ".svg") == 0;
}

static gint wallpaper_name_compare(
        gconstpointer left,
        gconstpointer right) {
    return g_strcmp0(
        left,
        right
    );
}

static char *resolve_auto_wallpaper(void) {
    const char *home =
        g_get_home_dir();

    if (!home ||
            home[0] == '\0') {
        return NULL;
    }

    g_autofree char *directory =
        g_build_filename(
            home,
            "Pictures",
            "Wallpapers",
            NULL
        );

    if (!directory) {
        return NULL;
    }

    GError *error = NULL;
    GDir *dir =
        g_dir_open(
            directory,
            0,
            &error
        );

    if (!dir) {
        g_clear_error(&error);
        return NULL;
    }

    GList *names = NULL;
    const char *name = NULL;

    while ((name = g_dir_read_name(dir))) {
        if (!wallpaper_extension_supported(
                name)) {
            continue;
        }

        names =
            g_list_prepend(
                names,
                g_strdup(name)
            );
    }

    g_dir_close(dir);

    names =
        g_list_sort(
            names,
            wallpaper_name_compare
        );

    char *selected = NULL;

    for (GList *item = names;
            item;
            item = item->next) {
        const char *candidate_name =
            item->data;

        g_autofree char *candidate =
            g_build_filename(
                directory,
                candidate_name,
                NULL
            );

        if (candidate &&
                g_file_test(
                    candidate,
                    G_FILE_TEST_IS_REGULAR)) {
            selected =
                g_strdup(
                    candidate
                );
            break;
        }
    }

    g_list_free_full(
        names,
        g_free
    );

    return selected;
}

static char *resolve_wallpaper(
        const ShellConfig *config,
        const char **source_out) {
    const char *requested =
        config &&
        config->appearance.wallpaper
            ? config->appearance.wallpaper
            : "auto";

    if (strcmp(
            requested,
            "auto") == 0) {
        char *automatic =
            resolve_auto_wallpaper();

        if (automatic) {
            *source_out = "auto";
            return automatic;
        }

        *source_out = "bundled";
        return g_strdup(
            DEFAULT_WALLPAPER
        );
    }

    g_autofree char *expanded = NULL;

    if (g_str_has_prefix(
            requested,
            "~/")) {
        const char *home =
            g_get_home_dir();

        if (home &&
                home[0] != '\0') {
            expanded =
                g_build_filename(
                    home,
                    requested + 2,
                    NULL
                );
        }
    } else {
        expanded =
            g_strdup(
                requested
            );
    }

    if (expanded &&
            g_file_test(
                expanded,
                G_FILE_TEST_IS_REGULAR)) {
        *source_out = "config";
        return g_steal_pointer(
            &expanded
        );
    }

    *source_out = "bundled-missing";
    return g_strdup(
        DEFAULT_WALLPAPER
    );
}

bool shell_config_runtime_publish_environment(
        const ShellConfig *config) {
    const char *source = NULL;

    g_autofree char *effective =
        resolve_wallpaper(
            config,
            &source
        );

    if (!effective) {
        fprintf(
            stderr,
            "FAIL: cannot resolve effective wallpaper\n"
        );
        return false;
    }

    if (!g_setenv(
            "ONYRION_WALLPAPER",
            effective,
            true)) {
        fprintf(
            stderr,
            "FAIL: cannot publish ONYRION_WALLPAPER\n"
        );
        return false;
    }

    printf(
        "WALLPAPER requested=%s effective=%s source=%s\n",
        config &&
        config->appearance.wallpaper
            ? config->appearance.wallpaper
            : "auto",
        effective,
        source
            ? source
            : "unknown"
    );
    fflush(stdout);

    return true;
}
static char *default_lkg_path(void) {
    const char *state_dir =
        g_get_user_state_dir();

    if (!state_dir ||
            state_dir[0] == '\0') {
        return NULL;
    }

    return g_build_filename(
        state_dir,
        "onyrion",
        "shell.kdl.lkg",
        NULL
    );
}

static bool write_all_fd(
        int fd,
        const char *data,
        size_t length) {
    size_t offset = 0;

    while (offset < length) {
        const ssize_t written =
            write(
                fd,
                data + offset,
                length - offset
            );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static bool persist_lkg(
        const char *source_path,
        const char *lkg_path,
        GError **error) {
    g_autofree char *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents(
            source_path,
            &contents,
            &length,
            error)) {
        return false;
    }

    g_autofree char *directory =
        g_path_get_dirname(
            lkg_path
        );

    if (g_mkdir_with_parents(
            directory,
            0700) < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot create LKG directory '%s': %s",
            directory,
            strerror(errno)
        );
        return false;
    }

    if (chmod(directory, 0700) < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot secure LKG directory '%s': %s",
            directory,
            strerror(errno)
        );
        return false;
    }

    g_autofree char *template =
        g_build_filename(
            directory,
            ".shell.kdl.lkg.tmp.XXXXXX",
            NULL
        );

    if (!template) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_NOMEM,
            "cannot allocate LKG temp path"
        );
        return false;
    }

    const int fd =
        g_mkstemp_full(
            template,
            O_RDWR | O_CLOEXEC,
            0600
        );

    if (fd < 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(errno),
            "cannot create LKG temp file: %s",
            strerror(errno)
        );
        return false;
    }

    bool ok =
        write_all_fd(
            fd,
            contents,
            length
        );

    int saved_errno = errno;

    if (ok && fsync(fd) < 0) {
        ok = false;
        saved_errno = errno;
    }

    if (close(fd) < 0 && ok) {
        ok = false;
        saved_errno = errno;
    }

    if (ok && rename(
            template,
            lkg_path) < 0) {
        ok = false;
        saved_errno = errno;
    }

    if (!ok) {
        (void)unlink(template);

        g_set_error(
            error,
            G_FILE_ERROR,
            g_file_error_from_errno(saved_errno),
            "cannot persist LKG '%s': %s",
            lkg_path,
            strerror(saved_errno)
        );
        return false;
    }

    const int directory_fd =
        open(
            directory,
            O_RDONLY |
                O_DIRECTORY |
                O_CLOEXEC
        );

    if (directory_fd >= 0) {
        (void)fsync(directory_fd);
        close(directory_fd);
    }

    return true;
}

static bool move_candidate(
        ShellConfig *target,
        ShellConfig *candidate) {
    if (!target ||
            !candidate) {
        return false;
    }

    shell_config_finish(target);
    *target = *candidate;
    *candidate = (ShellConfig){0};
    return true;
}

bool shell_config_runtime_bootstrap(
        ShellConfig *config,
        const char *path,
        char **lkg_path_out) {
    if (!config ||
            !path ||
            path[0] == '\0' ||
            !lkg_path_out) {
        return false;
    }

    *lkg_path_out = NULL;

    g_autofree char *lkg_path =
        default_lkg_path();

    if (!lkg_path) {
        fprintf(
            stderr,
            "FAIL: cannot construct Shell LKG path\n"
        );
        return false;
    }

    ShellConfig candidate;
    shell_config_init(&candidate);

    GError *current_error = NULL;

    if (shell_config_load(
            &candidate,
            path,
            false,
            &current_error)) {
        GError *persist_error = NULL;

        if (!persist_lkg(
                path,
                lkg_path,
                &persist_error)) {
            fprintf(
                stderr,
                "FAIL: valid current Shell config cannot persist LKG: %s\n",
                persist_error
                    ? persist_error->message
                    : "unknown error"
            );

            g_clear_error(&persist_error);
            shell_config_finish(&candidate);
            return false;
        }

        move_candidate(
            config,
            &candidate
        );

        printf(
            "CONFIG STARTUP source=current path=%s lkg=%s providers=%zu\n",
            path,
            lkg_path,
            config->provider_count
        );
        fflush(stdout);

        *lkg_path_out =
            g_steal_pointer(
                &lkg_path
            );
        return true;
    }

    fprintf(
        stderr,
        "CONFIG STARTUP WARN current rejected path=%s error=%s\n",
        path,
        current_error
            ? current_error->message
            : "unknown error"
    );
    g_clear_error(&current_error);
    shell_config_finish(&candidate);
    shell_config_init(&candidate);

    GError *lkg_error = NULL;

    if (shell_config_load(
            &candidate,
            lkg_path,
            false,
            &lkg_error)) {
        move_candidate(
            config,
            &candidate
        );

        printf(
            "CONFIG STARTUP source=lkg path=%s lkg=%s providers=%zu\n",
            path,
            lkg_path,
            config->provider_count
        );
        fflush(stdout);

        *lkg_path_out =
            g_steal_pointer(
                &lkg_path
            );
        return true;
    }

    fprintf(
        stderr,
        "CONFIG STARTUP WARN lkg rejected path=%s error=%s\n",
        lkg_path,
        lkg_error
            ? lkg_error->message
            : "unknown error"
    );
    g_clear_error(&lkg_error);
    shell_config_finish(&candidate);

    printf(
        "CONFIG STARTUP source=safe-default path=%s lkg=%s providers=%zu fallback=%u\n",
        path,
        lkg_path,
        config->provider_count,
        config->fallback.enabled ? 1U : 0U
    );
    fflush(stdout);

    *lkg_path_out =
        g_steal_pointer(
            &lkg_path
        );
    return true;
}

static bool restore_previous_runtime(
        ShellConfigRuntime *runtime,
        ShellConfig *previous) {
    ShellConfig rejected =
        *runtime->config;

    *runtime->config =
        *previous;
    *previous = (ShellConfig){0};

    const bool appearance_ok =
        shell_config_runtime_publish_environment(
            runtime->config
        );

    provider_manager_init(
        runtime->providers,
        runtime->config
    );

    const bool allocation_ok =
        runtime->providers->provider_count ==
            runtime->config->provider_count;

    const bool required_ok =
        appearance_ok &&
        allocation_ok &&
        provider_manager_start(
            runtime->providers
        );

    shell_config_finish(
        &rejected
    );

    return allocation_ok && required_ok;
}

static bool apply_candidate(
        ShellConfigRuntime *runtime,
        ShellConfig *candidate) {
    ShellConfig previous =
        *runtime->config;

    provider_manager_finish(
        runtime->providers
    );

    *runtime->config =
        *candidate;
    *candidate = (ShellConfig){0};

    const bool appearance_ok =
        shell_config_runtime_publish_environment(
            runtime->config
        );

    provider_manager_init(
        runtime->providers,
        runtime->config
    );

    if (!appearance_ok ||
            runtime->providers->provider_count !=
                runtime->config->provider_count) {
        fprintf(
            stderr,
            "CONFIG RELOAD FAIL appearance/provider runtime prepare; restoring previous config\n"
        );

        provider_manager_finish(
            runtime->providers
        );

        const bool restored =
            restore_previous_runtime(
                runtime,
                &previous
            );

        if (!restored) {
            fprintf(
                stderr,
                "CONFIG RELOAD FAIL previous provider runtime could not be fully restored\n"
            );
        }

        return false;
    }

    const bool required_ok =
        provider_manager_start(
            runtime->providers
        );

    shell_config_finish(
        &previous
    );

    if (!required_ok) {
        fprintf(
            stderr,
            "CONFIG RELOAD WARN one or more required providers failed to start\n"
        );
    }

    return true;
}

static gboolean handle_reload(
        gpointer data) {
    ShellConfigRuntime *runtime =
        data;

    runtime->reload_source = 0;

    ShellConfig candidate;
    shell_config_init(&candidate);

    GError *error = NULL;

    if (!shell_config_load(
            &candidate,
            runtime->path,
            false,
            &error)) {
        fprintf(
            stderr,
            "CONFIG RELOAD REJECT path=%s generation=%llu keep=lkg error=%s\n",
            runtime->path,
            (unsigned long long)runtime->generation,
            error
                ? error->message
                : "unknown error"
        );
        fflush(stderr);

        g_clear_error(&error);
        shell_config_finish(&candidate);
        return G_SOURCE_REMOVE;
    }

    GError *persist_error = NULL;

    if (!persist_lkg(
            runtime->path,
            runtime->lkg_path,
            &persist_error)) {
        fprintf(
            stderr,
            "CONFIG RELOAD REJECT path=%s generation=%llu keep=lkg error=%s\n",
            runtime->path,
            (unsigned long long)runtime->generation,
            persist_error
                ? persist_error->message
                : "cannot persist LKG"
        );
        fflush(stderr);

        g_clear_error(&persist_error);
        shell_config_finish(&candidate);
        return G_SOURCE_REMOVE;
    }

    if (!apply_candidate(
            runtime,
            &candidate)) {
        shell_config_finish(
            &candidate
        );
        return G_SOURCE_REMOVE;
    }

    runtime->generation++;

    printf(
        "CONFIG RELOAD APPLY generation=%llu providers=%zu source=current lkg=%s\n",
        (unsigned long long)runtime->generation,
        runtime->config->provider_count,
        runtime->lkg_path
    );
    fflush(stdout);

    return G_SOURCE_REMOVE;
}

static void schedule_reload(
        ShellConfigRuntime *runtime) {
    if (runtime->reload_source != 0) {
        g_source_remove(
            runtime->reload_source
        );
        runtime->reload_source = 0;
    }

    runtime->reload_source =
        g_timeout_add(
            CONFIG_RELOAD_DEBOUNCE_MS,
            handle_reload,
            runtime
        );

    if (runtime->reload_source == 0) {
        fprintf(
            stderr,
            "CONFIG RELOAD FAIL cannot arm debounce timer\n"
        );
    }
}

static gboolean handle_inotify(
        gint fd,
        GIOCondition condition,
        gpointer data) {
    ShellConfigRuntime *runtime =
        data;

    if (condition &
            (G_IO_ERR |
             G_IO_HUP |
             G_IO_NVAL)) {
        fprintf(
            stderr,
            "CONFIG WATCH FAIL condition=0x%x\n",
            (unsigned)condition
        );
        return G_SOURCE_CONTINUE;
    }

    if (!(condition & G_IO_IN)) {
        return G_SOURCE_CONTINUE;
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

            fprintf(
                stderr,
                "CONFIG WATCH FAIL read=%s\n",
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
                (const struct inotify_event *)(
                    buffer.bytes + offset
                );

            if (event->mask &
                    IN_Q_OVERFLOW) {
                schedule_reload(
                    runtime
                );
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
                schedule_reload(
                    runtime
                );
            }

            offset +=
                sizeof(*event) +
                event->len;
        }
    }

    return G_SOURCE_CONTINUE;
}

ShellConfigRuntime *shell_config_runtime_start(
        const char *path,
        const char *lkg_path,
        ShellConfig *config,
        ProviderManager *providers) {
    if (!path ||
            path[0] == '\0' ||
            !lkg_path ||
            lkg_path[0] == '\0' ||
            !config ||
            !providers) {
        return NULL;
    }

    ShellConfigRuntime *runtime =
        g_new0(
            ShellConfigRuntime,
            1
        );

    if (!runtime) {
        return NULL;
    }

    runtime->inotify_fd = -1;
    runtime->watch_descriptor = -1;
    runtime->generation = 1;
    runtime->path = g_strdup(path);
    runtime->directory =
        g_path_get_dirname(path);
    runtime->basename =
        g_path_get_basename(path);
    runtime->lkg_path =
        g_strdup(lkg_path);
    runtime->config = config;
    runtime->providers = providers;

    if (!runtime->path ||
            !runtime->directory ||
            !runtime->basename ||
            !runtime->lkg_path) {
        shell_config_runtime_finish(
            runtime
        );
        return NULL;
    }

    if (g_mkdir_with_parents(
            runtime->directory,
            0700) < 0 &&
            errno != EEXIST) {
        fprintf(
            stderr,
            "CONFIG WATCH FAIL cannot create directory '%s': %s\n",
            runtime->directory,
            strerror(errno)
        );

        shell_config_runtime_finish(
            runtime
        );
        return NULL;
    }

    runtime->inotify_fd =
        inotify_init1(
            IN_NONBLOCK |
            IN_CLOEXEC
        );

    if (runtime->inotify_fd < 0) {
        fprintf(
            stderr,
            "CONFIG WATCH FAIL inotify_init=%s\n",
            strerror(errno)
        );

        shell_config_runtime_finish(
            runtime
        );
        return NULL;
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
        fprintf(
            stderr,
            "CONFIG WATCH FAIL path=%s error=%s\n",
            runtime->directory,
            strerror(errno)
        );

        shell_config_runtime_finish(
            runtime
        );
        return NULL;
    }

    runtime->fd_source =
        g_unix_fd_add(
            runtime->inotify_fd,
            G_IO_IN |
                G_IO_ERR |
                G_IO_HUP |
                G_IO_NVAL,
            handle_inotify,
            runtime
        );

    if (runtime->fd_source == 0) {
        fprintf(
            stderr,
            "CONFIG WATCH FAIL cannot register inotify source\n"
        );

        shell_config_runtime_finish(
            runtime
        );
        return NULL;
    }

    printf(
        "CONFIG WATCH READY path=%s lkg=%s debounce_ms=%u generation=%llu\n",
        runtime->path,
        runtime->lkg_path,
        CONFIG_RELOAD_DEBOUNCE_MS,
        (unsigned long long)runtime->generation
    );
    fflush(stdout);

    return runtime;
}

void shell_config_runtime_finish(
        ShellConfigRuntime *runtime) {
    if (!runtime) {
        return;
    }

    if (runtime->reload_source != 0) {
        g_source_remove(
            runtime->reload_source
        );
    }

    if (runtime->fd_source != 0) {
        g_source_remove(
            runtime->fd_source
        );
    }

    if (runtime->watch_descriptor >= 0 &&
            runtime->inotify_fd >= 0) {
        (void)inotify_rm_watch(
            runtime->inotify_fd,
            runtime->watch_descriptor
        );
    }

    if (runtime->inotify_fd >= 0) {
        close(runtime->inotify_fd);
    }

    g_free(runtime->path);
    g_free(runtime->directory);
    g_free(runtime->basename);
    g_free(runtime->lkg_path);
    g_free(runtime);
}
