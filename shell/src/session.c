#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client-core.h>
#include <xcb/xcb.h>

#define READY_TIMEOUT_MS_DEFAULT 10000U
#define XWAYLAND_READY_TIMEOUT_MS_DEFAULT 10000U
#define XWAYLAND_CORE_EXIT_GRACE_MS 500U
#define CHILD_TERM_GRACE_MS 2000U
#define GROUP_TERM_GRACE_MS 1000U
#define POLL_INTERVAL_MS 50U
#define X_DISPLAY_MIN 0
#define X_DISPLAY_MAX 63
#define X_DISPLAY_NAME_CAPACITY 32
#define SHELL_BACKOFF_INITIAL_MS 250U
#define SHELL_BACKOFF_MAX_MS 4000U
#define SHELL_STABLE_MS 10000U
#define POLKIT_BACKOFF_INITIAL_MS 250U
#define POLKIT_BACKOFF_MAX_MS 4000U
#define POLKIT_STABLE_MS 10000U
#define POLKIT_RESTART_MAX 5U

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_signal(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

static uint64_t monotonic_ms(void) {
    struct timespec now = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0;
    }

    return
        (uint64_t)now.tv_sec * 1000U +
        (uint64_t)now.tv_nsec / 1000000U;
}

static void sleep_ms(unsigned milliseconds) {
    struct timespec request = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec =
            (long)(milliseconds % 1000U) *
            1000000L,
    };

    while (nanosleep(&request, &request) < 0 &&
            errno == EINTR &&
            !g_stop_requested) {
    }
}

static const char *env_or(
        const char *name,
        const char *fallback) {
    const char *value = getenv(name);

    return value && value[0] != '\0'
        ? value
        : fallback;
}


static bool ensure_config_directory(
        const char *path) {
    char buffer[PATH_MAX] = {0};

    const int length = snprintf(
        buffer,
        sizeof(buffer),
        "%s",
        path
    );

    if (length <= 0 ||
            (size_t)length >= sizeof(buffer) ||
            buffer[0] != '/') {
        errno = EINVAL;
        return false;
    }

    for (char *cursor = buffer + 1;
            *cursor;
            cursor++) {
        if (*cursor != '/') {
            continue;
        }

        *cursor = '\0';

        if (mkdir(buffer, 0700) < 0 &&
                errno != EEXIST) {
            return false;
        }

        *cursor = '/';
    }

    if (mkdir(buffer, 0700) < 0 &&
            errno != EEXIST) {
        return false;
    }

    struct stat st = {0};

    if (stat(buffer, &st) < 0 ||
            !S_ISDIR(st.st_mode)) {
        if (errno == 0) {
            errno = ENOTDIR;
        }
        return false;
    }

    return true;
}

static bool seed_shell_config(
        const char *template_path,
        const char *config_path,
        const char *config_directory) {
    int source = -1;
    int target = -1;
    bool ok = false;
    int saved_errno = 0;
    char temporary[PATH_MAX] = {0};

    source = open(
        template_path,
        O_RDONLY | O_CLOEXEC
    );

    if (source < 0) {
        goto out;
    }

    const int length = snprintf(
        temporary,
        sizeof(temporary),
        "%s/.shell.kdl.seed.XXXXXX",
        config_directory
    );

    if (length <= 0 ||
            (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        goto out;
    }

    target = mkstemp(temporary);

    if (target < 0 ||
            fchmod(target, 0600) < 0) {
        goto out;
    }

    char buffer[16384];

    for (;;) {
        const ssize_t count = read(
            source,
            buffer,
            sizeof(buffer)
        );

        if (count == 0) {
            break;
        }

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            goto out;
        }

        size_t offset = 0;

        while (offset < (size_t)count) {
            const ssize_t written = write(
                target,
                buffer + offset,
                (size_t)count - offset
            );

            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                goto out;
            }

            offset += (size_t)written;
        }
    }

    if (fsync(target) < 0 ||
            close(target) < 0) {
        target = -1;
        goto out;
    }
    target = -1;

    if (link(
            temporary,
            config_path) < 0) {
        goto out;
    }

    if (unlink(temporary) < 0) {
        const int unlink_errno = errno;
        (void)unlink(config_path);
        errno = unlink_errno;
        goto out;
    }

    temporary[0] = '\0';

    const int directory = open(
        config_directory,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );

    if (directory >= 0) {
        (void)fsync(directory);
        close(directory);
    }

    ok = true;

out:
    if (!ok) {
        saved_errno = errno;
    }

    if (target >= 0) {
        close(target);
    }
    if (source >= 0) {
        close(source);
    }
    if (temporary[0] != '\0') {
        (void)unlink(temporary);
    }

    if (!ok) {
        errno = saved_errno;
    }

    return ok;
}

static bool resolve_shell_config(
        char *path,
        size_t capacity,
        bool *seeded) {
    const char *explicit_path = getenv(
        "ONYRION_SHELL_CONFIG"
    );

    *seeded = false;

    if (explicit_path &&
            explicit_path[0] != '\0') {
        const int written = snprintf(
            path,
            capacity,
            "%s",
            explicit_path
        );

        return
            written > 0 &&
            (size_t)written < capacity;
    }

    char config_home[PATH_MAX] = {0};
    const char *xdg_config_home = getenv(
        "XDG_CONFIG_HOME"
    );

    if (xdg_config_home &&
            xdg_config_home[0] != '\0') {
        if (xdg_config_home[0] != '/') {
            errno = EINVAL;
            return false;
        }

        const int written = snprintf(
            config_home,
            sizeof(config_home),
            "%s",
            xdg_config_home
        );

        if (written <= 0 ||
                (size_t)written >= sizeof(config_home)) {
            errno = ENAMETOOLONG;
            return false;
        }
    } else {
        const char *home = getenv("HOME");

        if (!home || home[0] != '/') {
            errno = EINVAL;
            return false;
        }

        const int written = snprintf(
            config_home,
            sizeof(config_home),
            "%s/.config",
            home
        );

        if (written <= 0 ||
                (size_t)written >= sizeof(config_home)) {
            errno = ENAMETOOLONG;
            return false;
        }
    }

    if (!ensure_config_directory(
            config_home)) {
        return false;
    }

    char config_directory[PATH_MAX] = {0};

    const int directory_length = snprintf(
        config_directory,
        sizeof(config_directory),
        "%s/onyrion",
        config_home
    );

    if (directory_length <= 0 ||
            (size_t)directory_length >= sizeof(config_directory)) {
        errno = ENAMETOOLONG;
        return false;
    }

    if (!ensure_config_directory(
            config_directory)) {
        return false;
    }

    const int path_length = snprintf(
        path,
        capacity,
        "%s/shell.kdl",
        config_directory
    );

    if (path_length <= 0 ||
            (size_t)path_length >= capacity) {
        errno = ENAMETOOLONG;
        return false;
    }

    struct stat st = {0};

    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            return false;
        }

        return true;
    }

    if (errno != ENOENT) {
        return false;
    }

    const char *template_path = env_or(
        "ONYRION_SHELL_CONFIG_TEMPLATE",
        "/usr/share/onyrion/config/shell.kdl"
    );

    if (!seed_shell_config(
            template_path,
            path,
            config_directory)) {
        return false;
    }

    *seeded = true;
    return true;
}

static bool parse_unsigned_env(
        const char *name,
        unsigned fallback,
        unsigned *value) {
    const char *raw = getenv(name);

    if (!raw || raw[0] == '\0') {
        *value = fallback;
        return true;
    }

    errno = 0;
    char *end = NULL;
    const unsigned long parsed =
        strtoul(raw, &end, 10);

    if (errno != 0 ||
            end == raw ||
            !end ||
            *end != '\0' ||
            parsed == 0 ||
            parsed > UINT_MAX) {
        fprintf(
            stderr,
            "SESSION FAIL invalid %s=%s\n",
            name,
            raw
        );

        return false;
    }

    *value = (unsigned)parsed;
    return true;
}

static int status_exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return EXIT_FAILURE;
}

static void log_status(
        const char *label,
        int status) {
    if (WIFEXITED(status)) {
        printf(
            "SESSION %s exit=%d\n",
            label,
            WEXITSTATUS(status)
        );
    } else if (WIFSIGNALED(status)) {
        printf(
            "SESSION %s signal=%d\n",
            label,
            WTERMSIG(status)
        );
    } else {
        printf(
            "SESSION %s status=%d\n",
            label,
            status
        );
    }

    fflush(stdout);
}

static bool process_group_exists(pid_t group) {
    if (group <= 0) {
        return false;
    }

    if (kill(-group, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}

static void terminate_group(
        pid_t group,
        unsigned grace_ms) {
    if (group <= 0) {
        return;
    }

    if (kill(-group, SIGTERM) < 0 &&
            errno != ESRCH) {
        fprintf(
            stderr,
            "SESSION WARN group TERM pgid=%ld: %s\n",
            (long)group,
            strerror(errno)
        );
    }

    const uint64_t deadline =
        monotonic_ms() + grace_ms;

    while (process_group_exists(group) &&
            monotonic_ms() < deadline) {
        sleep_ms(POLL_INTERVAL_MS);
    }

    if (!process_group_exists(group)) {
        return;
    }

    if (kill(-group, SIGKILL) < 0 &&
            errno != ESRCH) {
        fprintf(
            stderr,
            "SESSION WARN group KILL pgid=%ld: %s\n",
            (long)group,
            strerror(errno)
        );
    }

    for (unsigned i = 0; i < 20U; i++) {
        if (!process_group_exists(group)) {
            return;
        }

        sleep_ms(POLL_INTERVAL_MS);
    }
}

static void reap_if_child(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    int status = 0;

    while (waitpid(pid, &status, 0) < 0 &&
            errno == EINTR) {
    }
}

static pid_t spawn_exec(
        const char *path,
        char *const argv[],
        bool process_group,
        bool core_environment) {
    const pid_t pid = fork();

    if (pid < 0) {
        fprintf(
            stderr,
            "SESSION FAIL fork %s: %s\n",
            path,
            strerror(errno)
        );

        return -1;
    }

    if (pid == 0) {
        if (process_group &&
                setpgid(0, 0) < 0) {
            fprintf(
                stderr,
                "SESSION CHILD FAIL setpgid %s: %s\n",
                path,
                strerror(errno)
            );

            _exit(125);
        }

        if (core_environment) {
            (void)unsetenv("WAYLAND_DISPLAY");
            (void)unsetenv("DISPLAY");
            (void)unsetenv("WLR_BACKENDS");
            (void)unsetenv("WLR_WL_OUTPUTS");
            (void)unsetenv("WLR_HEADLESS_OUTPUTS");
        }

        execvp(path, argv);

        fprintf(
            stderr,
            "SESSION CHILD FAIL exec %s: %s\n",
            path,
            strerror(errno)
        );

        _exit(127);
    }

    if (process_group) {
        if (setpgid(pid, pid) < 0 &&
                errno != EACCES &&
                errno != ESRCH) {
            fprintf(
                stderr,
                "SESSION WARN parent setpgid pid=%ld: %s\n",
                (long)pid,
                strerror(errno)
            );
        }
    }

    return pid;
}

static int run_command_timeout(
        const char *path,
        char *const argv[],
        unsigned timeout_ms) {
    const pid_t pid =
        spawn_exec(
            path,
            argv,
            false,
            false
        );

    if (pid < 0) {
        return -1;
    }

    const uint64_t deadline =
        monotonic_ms() + timeout_ms;

    for (;;) {
        int status = 0;
        const pid_t result =
            waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            return status_exit_code(status);
        }

        if (result < 0 && errno != EINTR) {
            return -1;
        }

        if (g_stop_requested ||
                monotonic_ms() >= deadline) {
            (void)kill(pid, SIGTERM);

            const uint64_t term_deadline =
                monotonic_ms() + 500U;

            while (monotonic_ms() <
                    term_deadline) {
                const pid_t wait_result =
                    waitpid(
                        pid,
                        &status,
                        WNOHANG
                    );

                if (wait_result == pid) {
                    return status_exit_code(
                        status
                    );
                }

                sleep_ms(POLL_INTERVAL_MS);
            }

            (void)kill(pid, SIGKILL);
            reap_if_child(pid);
            return 124;
        }

        sleep_ms(POLL_INTERVAL_MS);
    }
}

static bool core_socket_exists(
        const char *runtime_dir,
        const char *socket_name) {
    char path[PATH_MAX];

    const int written =
        snprintf(
            path,
            sizeof(path),
            "%s/%s",
            runtime_dir,
            socket_name
        );

    if (written < 0 ||
            (size_t)written >= sizeof(path)) {
        return false;
    }

    struct stat status = {0};

    if (stat(path, &status) < 0) {
        return false;
    }

    return S_ISSOCK(status.st_mode);
}

struct x11_listener {
    int filesystem_fd;
    int abstract_fd;
    bool path_owned;
    dev_t path_device;
    ino_t path_inode;
    char path[PATH_MAX];
};

static void x11_listener_reset(
        struct x11_listener *listener) {
    listener->filesystem_fd = -1;
    listener->abstract_fd = -1;
    listener->path_owned = false;
    listener->path_device = 0;
    listener->path_inode = 0;
    listener->path[0] = '\0';
}

static void x11_listener_close(
        struct x11_listener *listener) {
    if (!listener) {
        return;
    }

    if (listener->filesystem_fd >= 0) {
        close(listener->filesystem_fd);
        listener->filesystem_fd = -1;
    }

    if (listener->abstract_fd >= 0) {
        close(listener->abstract_fd);
        listener->abstract_fd = -1;
    }

    if (listener->path_owned &&
            listener->path[0] != '\0') {
        struct stat status = {0};

        if (lstat(
                listener->path,
                &status) == 0) {
            if (status.st_dev ==
                        listener->path_device &&
                    status.st_ino ==
                        listener->path_inode) {
                if (unlink(
                        listener->path) < 0 &&
                        errno != ENOENT) {
                    fprintf(
                        stderr,
                        "SESSION WARN cannot remove owned X11 socket %s: %s\n",
                        listener->path,
                        strerror(errno)
                    );
                }
            } else {
                fprintf(
                    stderr,
                    "SESSION WARN X11 socket ownership changed; preserving %s\n",
                    listener->path
                );
            }
        } else if (errno != ENOENT) {
            fprintf(
                stderr,
                "SESSION WARN cannot inspect owned X11 socket %s: %s\n",
                listener->path,
                strerror(errno)
            );
        }
    }

    listener->path_owned = false;
    listener->path_device = 0;
    listener->path_inode = 0;
    listener->path[0] = '\0';
}

static int x11_bind_listener(
        int fd,
        const struct sockaddr_un *address,
        socklen_t address_size) {
    if (bind(
            fd,
            (const struct sockaddr *)address,
            address_size) < 0) {
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        return -1;
    }

    return 0;
}

static int x11_listener_open(
        int display_number,
        struct x11_listener *listener) {
    x11_listener_reset(listener);

    char lock_path[PATH_MAX];

    const int socket_written =
        snprintf(
            listener->path,
            sizeof(listener->path),
            "/tmp/.X11-unix/X%d",
            display_number
        );

    const int lock_written =
        snprintf(
            lock_path,
            sizeof(lock_path),
            "/tmp/.X%d-lock",
            display_number
        );

    if (socket_written <= 0 ||
            (size_t)socket_written >=
                sizeof(listener->path) ||
            lock_written <= 0 ||
            (size_t)lock_written >=
                sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct stat status = {0};

    if (lstat(lock_path, &status) == 0) {
        return 0;
    }

    if (errno != ENOENT) {
        return -1;
    }

    if (lstat(listener->path, &status) == 0) {
        return 0;
    }

    if (errno != ENOENT) {
        return -1;
    }

    listener->filesystem_fd =
        socket(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC,
            0
        );

    if (listener->filesystem_fd < 0) {
        return -1;
    }

    struct sockaddr_un filesystem_address = {
        .sun_family = AF_UNIX,
    };

    memcpy(
        filesystem_address.sun_path,
        listener->path,
        (size_t)socket_written + 1
    );

    const socklen_t filesystem_size =
        (socklen_t)(
            offsetof(
                struct sockaddr_un,
                sun_path
            ) +
            (size_t)socket_written +
            1
        );

    if (x11_bind_listener(
            listener->filesystem_fd,
            &filesystem_address,
            filesystem_size) < 0) {
        const int saved_errno = errno;
        x11_listener_close(listener);

        if (saved_errno == EADDRINUSE ||
                saved_errno == EEXIST) {
            return 0;
        }

        errno = saved_errno;
        return -1;
    }

    if (chmod(
            listener->path,
            0777) < 0) {
        const int saved_errno = errno;
        (void)unlink(listener->path);
        x11_listener_close(listener);
        errno = saved_errno;
        return -1;
    }

    if (lstat(
            listener->path,
            &status) < 0) {
        const int saved_errno = errno;
        (void)unlink(listener->path);
        x11_listener_close(listener);
        errno = saved_errno;
        return -1;
    }

    listener->path_owned = true;
    listener->path_device = status.st_dev;
    listener->path_inode = status.st_ino;

    listener->abstract_fd =
        socket(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC,
            0
        );

    if (listener->abstract_fd < 0) {
        const int saved_errno = errno;
        x11_listener_close(listener);
        errno = saved_errno;
        return -1;
    }

    struct sockaddr_un abstract_address = {
        .sun_family = AF_UNIX,
    };

    const size_t abstract_length =
        (size_t)socket_written;

    if (abstract_length + 1 >
            sizeof(abstract_address.sun_path)) {
        x11_listener_close(listener);
        errno = ENAMETOOLONG;
        return -1;
    }

    abstract_address.sun_path[0] = '\0';

    memcpy(
        abstract_address.sun_path + 1,
        listener->path,
        abstract_length
    );

    const socklen_t abstract_size =
        (socklen_t)(
            offsetof(
                struct sockaddr_un,
                sun_path
            ) +
            1 +
            abstract_length
        );

    if (x11_bind_listener(
            listener->abstract_fd,
            &abstract_address,
            abstract_size) < 0) {
        const int saved_errno = errno;
        x11_listener_close(listener);

        if (saved_errno == EADDRINUSE ||
                saved_errno == EEXIST) {
            return 0;
        }

        errno = saved_errno;
        return -1;
    }

    return 1;
}

static bool x11_roundtrip_ready(
        const char *display_name) {
    int preferred_screen = 0;
    xcb_connection_t *connection =
        xcb_connect(
            display_name,
            &preferred_screen
        );

    (void)preferred_screen;

    if (!connection) {
        return false;
    }

    bool ready = false;

    if (xcb_connection_has_error(connection) == 0) {
        const xcb_get_input_focus_cookie_t cookie =
            xcb_get_input_focus(connection);

        xcb_generic_error_t *error = NULL;
        xcb_get_input_focus_reply_t *reply =
            xcb_get_input_focus_reply(
                connection,
                cookie,
                &error
            );

        ready =
            reply != NULL &&
            error == NULL &&
            xcb_connection_has_error(connection) == 0;

        free(error);
        free(reply);
    }

    xcb_disconnect(connection);
    return ready;
}

static bool clear_close_on_exec(
        int fd) {
    const int flags =
        fcntl(fd, F_GETFD);

    if (flags < 0) {
        return false;
    }

    return fcntl(
        fd,
        F_SETFD,
        flags & ~FD_CLOEXEC
    ) == 0;
}

static bool wayland_roundtrip_ready(void) {
    struct wl_display *display =
        wl_display_connect(NULL);

    if (!display) {
        return false;
    }

    const int result =
        wl_display_roundtrip(display);

    wl_display_disconnect(display);

    return result >= 0;
}

static bool wait_core_ready(
        pid_t core_pid,
        const char *runtime_dir,
        const char *socket_name,
        unsigned timeout_ms,
        int *early_status,
        bool *core_reaped) {
    const uint64_t deadline =
        monotonic_ms() + timeout_ms;

    while (!g_stop_requested &&
            monotonic_ms() < deadline) {
        int status = 0;

        const pid_t result =
            waitpid(
                core_pid,
                &status,
                WNOHANG
            );

        if (result == core_pid) {
            *early_status = status;
            *core_reaped = true;
            return false;
        }

        if (result < 0 &&
                errno != EINTR) {
            return false;
        }

        if (core_socket_exists(
                runtime_dir,
                socket_name) &&
                wayland_roundtrip_ready()) {
            /*
             * Check once more after the functional probe so a
             * foreign/stale socket cannot hide an already-dead
             * Core process.
             */
            const pid_t after =
                waitpid(
                    core_pid,
                    &status,
                    WNOHANG
                );

            if (after == 0) {
                return true;
            }

            if (after == core_pid) {
                *early_status = status;
                *core_reaped = true;
            }

            return false;
        }

        sleep_ms(POLL_INTERVAL_MS);
    }

    return false;
}


static int xwayland_notify_socket(
        char *notify_path,
        size_t notify_path_size) {
    const char *runtime_dir =
        getenv("XDG_RUNTIME_DIR");

    if (!runtime_dir ||
            runtime_dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    const int written =
        snprintf(
            notify_path,
            notify_path_size,
            "%s/onyrion-xwayland-notify-%ld-%llu.sock",
            runtime_dir,
            (long)getpid(),
            (unsigned long long)monotonic_ms()
        );

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };

    if (written <= 0 ||
            (size_t)written >= notify_path_size ||
            (size_t)written >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct stat status = {0};

    if (lstat(
            notify_path,
            &status) == 0) {
        errno = EEXIST;
        return -1;
    }

    if (errno != ENOENT) {
        return -1;
    }

    const int fd =
        socket(
            AF_UNIX,
            SOCK_DGRAM |
                SOCK_CLOEXEC |
                SOCK_NONBLOCK,
            0
        );

    if (fd < 0) {
        fprintf(
            stderr,
            "SESSION FAIL Xwayland notify socket: %s\n",
            strerror(errno)
        );
        return -1;
    }

    memcpy(
        address.sun_path,
        notify_path,
        (size_t)written + 1
    );

    const socklen_t address_size =
        (socklen_t)(
            offsetof(
                struct sockaddr_un,
                sun_path
            ) +
            (size_t)written +
            1
        );

    if (bind(
            fd,
            (const struct sockaddr *)&address,
            address_size) < 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

static void xwayland_notify_finish(
        int notify_fd,
        const char *notify_path) {
    if (notify_fd >= 0) {
        close(notify_fd);
    }

    if (notify_path &&
            notify_path[0] != '\0') {
        if (unlink(notify_path) < 0 &&
                errno != ENOENT) {
            fprintf(
                stderr,
                "SESSION WARN cannot remove Xwayland notify socket %s: %s\n",
                notify_path,
                strerror(errno)
            );
        }
    }
}

static bool xwayland_notify_ready(
        int notify_fd) {
    char message[512];

    for (;;) {
        const ssize_t length =
            recv(
                notify_fd,
                message,
                sizeof(message) - 1,
                MSG_DONTWAIT
            );

        if (length < 0) {
            return errno == EAGAIN ||
                errno == EWOULDBLOCK
                ? false
                : false;
        }

        if (length == 0) {
            return false;
        }

        message[length] = '\0';

        char *cursor = message;

        while (*cursor != '\0') {
            char *end =
                strchr(cursor, '\n');

            if (end) {
                *end = '\0';
            }

            if (strcmp(
                    cursor,
                    "READY=1") == 0) {
                return true;
            }

            if (!end) {
                break;
            }

            cursor = end + 1;
        }
    }
}

static pid_t spawn_xwayland_satellite(
        const char *path,
        char *const argv[],
        const char *notify_path,
        const struct x11_listener *listener) {
    const pid_t pid = fork();

    if (pid < 0) {
        fprintf(
            stderr,
            "SESSION FAIL fork %s: %s\n",
            path,
            strerror(errno)
        );
        return -1;
    }

    if (pid == 0) {
        if (setpgid(0, 0) < 0) {
            fprintf(
                stderr,
                "SESSION CHILD FAIL setpgid %s: %s\n",
                path,
                strerror(errno)
            );
            _exit(125);
        }

        if (!clear_close_on_exec(
                    listener->filesystem_fd) ||
                !clear_close_on_exec(
                    listener->abstract_fd)) {
            fprintf(
                stderr,
                "SESSION CHILD FAIL inherit X11 listen fd %s: %s\n",
                path,
                strerror(errno)
            );
            _exit(125);
        }

        if (setenv(
                "NOTIFY_SOCKET",
                notify_path,
                1) < 0) {
            fprintf(
                stderr,
                "SESSION CHILD FAIL NOTIFY_SOCKET %s: %s\n",
                path,
                strerror(errno)
            );
            _exit(125);
        }

        execvp(path, argv);

        fprintf(
            stderr,
            "SESSION CHILD FAIL exec %s: %s\n",
            path,
            strerror(errno)
        );
        _exit(127);
    }

    if (setpgid(pid, pid) < 0 &&
            errno != EACCES &&
            errno != ESRCH) {
        fprintf(
            stderr,
            "SESSION WARN parent setpgid pid=%ld: %s\n",
            (long)pid,
            strerror(errno)
        );
    }

    return pid;
}

static bool wait_xwayland_ready(
        pid_t satellite_pid,
        const char *display_name,
        int notify_fd,
        unsigned timeout_ms,
        int *early_status,
        bool *satellite_reaped) {
    const uint64_t deadline =
        monotonic_ms() + timeout_ms;

    bool notified_ready = false;

    while (!g_stop_requested &&
            monotonic_ms() < deadline) {
        int status = 0;

        const pid_t result =
            waitpid(
                satellite_pid,
                &status,
                WNOHANG
            );

        if (result == satellite_pid) {
            *early_status = status;
            *satellite_reaped = true;
            return false;
        }

        if (result < 0 &&
                errno != EINTR) {
            return false;
        }

        if (!notified_ready &&
                xwayland_notify_ready(
                    notify_fd)) {
            notified_ready = true;

            printf(
                "SESSION XWAYLAND_NOTIFY_READY display=%s\n",
                display_name
            );
            fflush(stdout);
        }

        if (notified_ready &&
                x11_roundtrip_ready(
                    display_name)) {
            const pid_t after =
                waitpid(
                    satellite_pid,
                    &status,
                    WNOHANG
                );

            if (after == 0) {
                return true;
            }

            if (after == satellite_pid) {
                *early_status = status;
                *satellite_reaped = true;
            }

            return false;
        }

        sleep_ms(POLL_INTERVAL_MS);
    }

    return false;
}

static bool xwayland_listenfd_supported(
        const char *satellite_bin) {
    char *const argv[] = {
        (char *)satellite_bin,
        "--test-listenfd-support",
        NULL,
    };

    return run_command_timeout(
        satellite_bin,
        argv,
        3000U
    ) == 0;
}

static pid_t start_xwayland_satellite(
        const char *satellite_bin,
        unsigned timeout_ms,
        char selected_display[
            static X_DISPLAY_NAME_CAPACITY
        ],
        struct x11_listener *selected_listener) {
    if (!xwayland_listenfd_supported(
            satellite_bin)) {
        fprintf(
            stderr,
            "SESSION FAIL xwayland-satellite lacks -listenfd support: %s\n",
            satellite_bin
        );
        return -1;
    }

    for (int display_number = X_DISPLAY_MIN;
            display_number <= X_DISPLAY_MAX;
            display_number++) {
        if (g_stop_requested) {
            break;
        }

        char display_name[
            X_DISPLAY_NAME_CAPACITY
        ];

        const int written =
            snprintf(
                display_name,
                sizeof(display_name),
                ":%d",
                display_number
            );

        if (written < 0 ||
                (size_t)written >=
                    sizeof(display_name)) {
            continue;
        }

        struct x11_listener listener;
        const int listener_result =
            x11_listener_open(
                display_number,
                &listener
            );

        if (listener_result == 0) {
            printf(
                "SESSION XWAYLAND_SKIP display=%s reason=occupied\n",
                display_name
            );
            fflush(stdout);
            continue;
        }

        if (listener_result < 0) {
            fprintf(
                stderr,
                "SESSION FAIL cannot reserve X11 display=%s: %s\n",
                display_name,
                strerror(errno)
            );
            return -1;
        }

        char filesystem_fd_text[32];
        char abstract_fd_text[32];

        const int filesystem_fd_written =
            snprintf(
                filesystem_fd_text,
                sizeof(filesystem_fd_text),
                "%d",
                listener.filesystem_fd
            );

        const int abstract_fd_written =
            snprintf(
                abstract_fd_text,
                sizeof(abstract_fd_text),
                "%d",
                listener.abstract_fd
            );

        if (filesystem_fd_written <= 0 ||
                (size_t)filesystem_fd_written >=
                    sizeof(filesystem_fd_text) ||
                abstract_fd_written <= 0 ||
                (size_t)abstract_fd_written >=
                    sizeof(abstract_fd_text)) {
            x11_listener_close(&listener);
            continue;
        }

        char *const argv[] = {
            (char *)satellite_bin,
            display_name,
            "-listenfd",
            filesystem_fd_text,
            "-listenfd",
            abstract_fd_text,
            NULL,
        };

        char notify_path[
            sizeof(((struct sockaddr_un *)0)->sun_path)
        ] = {0};

        const int notify_fd =
            xwayland_notify_socket(
                notify_path,
                sizeof(notify_path)
            );

        if (notify_fd < 0) {
            const int saved_errno = errno;
            x11_listener_close(&listener);
            errno = saved_errno;

            fprintf(
                stderr,
                "SESSION FAIL cannot create Xwayland readiness channel: %s\n",
                strerror(errno)
            );
            return -1;
        }

        const pid_t satellite_pid =
            spawn_xwayland_satellite(
                satellite_bin,
                argv,
                notify_path,
                &listener
            );

        if (satellite_pid < 0) {
            xwayland_notify_finish(
                notify_fd,
                notify_path
            );
            x11_listener_close(&listener);
            return -1;
        }

        printf(
            "SESSION XWAYLAND_START display=%s pid=%ld pgid=%ld listenfd=%d,%d\n",
            display_name,
            (long)satellite_pid,
            (long)satellite_pid,
            listener.filesystem_fd,
            listener.abstract_fd
        );
        fflush(stdout);

        int early_status = 0;
        bool satellite_reaped = false;

        if (wait_xwayland_ready(
                satellite_pid,
                display_name,
                notify_fd,
                timeout_ms,
                &early_status,
                &satellite_reaped)) {
            xwayland_notify_finish(
                notify_fd,
                notify_path
            );

            memcpy(
                selected_display,
                display_name,
                (size_t)written + 1
            );

            *selected_listener =
                listener;

            printf(
                "SESSION XWAYLAND_READY display=%s probe=notify+xcb listenfd=owned\n",
                selected_display
            );
            fflush(stdout);

            return satellite_pid;
        }

        xwayland_notify_finish(
            notify_fd,
            notify_path
        );

        if (satellite_reaped) {
            log_status(
                "XWAYLAND_EARLY_EXIT",
                early_status
            );
        } else if (!g_stop_requested) {
            fprintf(
                stderr,
                "SESSION WARN Xwayland readiness timeout display=%s timeout_ms=%u\n",
                display_name,
                timeout_ms
            );
        }

        terminate_group(
            satellite_pid,
            CHILD_TERM_GRACE_MS
        );

        if (!satellite_reaped) {
            reap_if_child(
                satellite_pid
            );
        }

        x11_listener_close(&listener);
    }

    return -1;
}

static bool import_activation_environment(
        const char *tool) {
    char *const argv[] = {
        (char *)tool,
        "--systemd",
        "XDG_SESSION_TYPE",
        "XDG_CURRENT_DESKTOP",
        "XDG_SESSION_DESKTOP",
        "WAYLAND_DISPLAY",
        "DISPLAY",
        NULL,
    };

    const int result =
        run_command_timeout(
            tool,
            argv,
            5000U
        );

    if (result != 0) {
        fprintf(
            stderr,
            "SESSION FAIL activation environment import rc=%d\n",
            result
        );

        return false;
    }

    printf(
        "SESSION ENV_IMPORT tool=%s result=PASS\n",
        tool
    );
    fflush(stdout);

    return true;
}

static pid_t start_shell(
        const char *shell_bin,
        const char *shell_config) {
    char *const argv[] = {
        (char *)shell_bin,
        "daemon",
        "--config",
        (char *)shell_config,
        NULL,
    };

    const pid_t pid =
        spawn_exec(
            shell_bin,
            argv,
            true,
            false
        );

    if (pid > 0) {
        printf(
            "SESSION SHELL_START pid=%ld pgid=%ld\n",
            (long)pid,
            (long)pid
        );
        fflush(stdout);
    }

    return pid;
}

static pid_t start_polkit_agent(
        const char *agent_bin) {
    char *const argv[] = {
        (char *)agent_bin,
        NULL,
    };

    const pid_t pid =
        spawn_exec(
            agent_bin,
            argv,
            true,
            false
        );

    if (pid > 0) {
        printf(
            "SESSION POLKIT_START pid=%ld pgid=%ld\n",
            (long)pid,
            (long)pid
        );
        fflush(stdout);
    }

    return pid;
}

static void stop_polkit_group(
        pid_t polkit_group,
        pid_t polkit_pid) {
    if (polkit_group <= 0) {
        return;
    }

    terminate_group(
        polkit_group,
        GROUP_TERM_GRACE_MS
    );

    if (polkit_pid > 0) {
        reap_if_child(polkit_pid);
    }
}

static void stop_core_group(
        pid_t core_group,
        pid_t core_pid,
        bool core_reaped) {
    if (core_group <= 0) {
        return;
    }

    terminate_group(
        core_group,
        CHILD_TERM_GRACE_MS
    );

    if (!core_reaped) {
        reap_if_child(core_pid);
    }
}

static void stop_xwayland_group(
        pid_t satellite_group,
        pid_t satellite_pid,
        bool satellite_reaped,
        struct x11_listener *listener) {
    if (satellite_group > 0) {
        terminate_group(
            satellite_group,
            CHILD_TERM_GRACE_MS
        );

        if (!satellite_reaped) {
            reap_if_child(
                satellite_pid
            );
        }
    }

    x11_listener_close(listener);
}

static int supervise(
        pid_t core_pid,
        pid_t core_group,
        pid_t satellite_pid,
        pid_t satellite_group,
        struct x11_listener *listener,
        const char *shell_bin,
        const char *shell_config,
        const char *polkit_agent_bin) {
    pid_t shell_pid =
        start_shell(
            shell_bin,
            shell_config
        );

    pid_t shell_group =
        shell_pid;

    uint64_t shell_started =
        monotonic_ms();

    uint64_t next_shell_start = 0;
    unsigned backoff =
        SHELL_BACKOFF_INITIAL_MS;

    pid_t polkit_pid =
        start_polkit_agent(
            polkit_agent_bin
        );

    pid_t polkit_group =
        polkit_pid;

    uint64_t polkit_started =
        monotonic_ms();

    uint64_t next_polkit_start = 0;
    unsigned polkit_backoff =
        POLKIT_BACKOFF_INITIAL_MS;
    unsigned polkit_failures = 0;
    bool polkit_degraded = false;

    if (polkit_pid < 0) {
        polkit_failures = 1;
        next_polkit_start =
            monotonic_ms() +
            polkit_backoff;
    }

    for (;;) {
        if (g_stop_requested) {
            printf(
                "SESSION STOP reason=signal\n"
            );
            fflush(stdout);

            stop_polkit_group(
                polkit_group,
                polkit_pid
            );

            terminate_group(
                shell_group,
                GROUP_TERM_GRACE_MS
            );

            if (shell_pid > 0) {
                reap_if_child(shell_pid);
            }

            stop_xwayland_group(
                satellite_group,
                satellite_pid,
                false,
                listener
            );

            stop_core_group(
                core_group,
                core_pid,
                false
            );

            return EXIT_SUCCESS;
        }

        int core_status = 0;

        const pid_t core_result =
            waitpid(
                core_pid,
                &core_status,
                WNOHANG
            );

        if (core_result == core_pid) {
            log_status(
                "CORE_EXIT",
                core_status
            );

            stop_polkit_group(
                polkit_group,
                polkit_pid
            );

            terminate_group(
                shell_group,
                GROUP_TERM_GRACE_MS
            );

            if (shell_pid > 0) {
                reap_if_child(shell_pid);
            }

            stop_xwayland_group(
                satellite_group,
                satellite_pid,
                false,
                listener
            );

            terminate_group(
                core_group,
                250U
            );

            return status_exit_code(
                core_status
            );
        }

        if (core_result < 0 &&
                errno != EINTR) {
            fprintf(
                stderr,
                "SESSION FAIL wait Core: %s\n",
                strerror(errno)
            );

            stop_polkit_group(
                polkit_group,
                polkit_pid
            );

            terminate_group(
                shell_group,
                GROUP_TERM_GRACE_MS
            );

            if (shell_pid > 0) {
                reap_if_child(shell_pid);
            }

            stop_xwayland_group(
                satellite_group,
                satellite_pid,
                false,
                listener
            );

            stop_core_group(
                core_group,
                core_pid,
                false
            );

            return EXIT_FAILURE;
        }

        int satellite_status = 0;

        const pid_t satellite_result =
            waitpid(
                satellite_pid,
                &satellite_status,
                WNOHANG
            );

        if (satellite_result ==
                satellite_pid) {
            log_status(
                "XWAYLAND_EXIT",
                satellite_status
            );

            /*
             * Stop accepting X clients immediately. The listener
             * path is session-owned, so this cleanup does not touch
             * a foreign display.
             */
            x11_listener_close(listener);

            /*
             * Core teardown can close satellite's Wayland
             * connection before waitpid(core_pid) becomes
             * observable. Give only Core a short causal window.
             */
            int causal_core_status = 0;
            bool causal_core_reaped = false;
            const uint64_t causal_deadline =
                monotonic_ms() +
                XWAYLAND_CORE_EXIT_GRACE_MS;

            while (!g_stop_requested &&
                    monotonic_ms() <
                        causal_deadline) {
                const pid_t causal_result =
                    waitpid(
                        core_pid,
                        &causal_core_status,
                        WNOHANG
                    );

                if (causal_result ==
                        core_pid) {
                    causal_core_reaped = true;
                    break;
                }

                if (causal_result < 0 &&
                        errno != EINTR) {
                    break;
                }

                sleep_ms(
                    POLL_INTERVAL_MS
                );
            }

            stop_polkit_group(
                polkit_group,
                polkit_pid
            );

            terminate_group(
                shell_group,
                GROUP_TERM_GRACE_MS
            );

            if (shell_pid > 0) {
                reap_if_child(shell_pid);
            }

            stop_xwayland_group(
                satellite_group,
                satellite_pid,
                true,
                listener
            );

            if (causal_core_reaped) {
                printf(
                    "SESSION XWAYLAND_EXIT_CAUSE=core\n"
                );
                fflush(stdout);

                log_status(
                    "CORE_EXIT",
                    causal_core_status
                );

                terminate_group(
                    core_group,
                    250U
                );

                return status_exit_code(
                    causal_core_status
                );
            }

            if (g_stop_requested) {
                stop_core_group(
                    core_group,
                    core_pid,
                    false
                );
                return EXIT_SUCCESS;
            }

            stop_core_group(
                core_group,
                core_pid,
                false
            );

            const int code =
                status_exit_code(
                    satellite_status
                );

            return code == 0
                ? EXIT_FAILURE
                : code;
        }

        if (satellite_result < 0 &&
                errno != EINTR) {
            fprintf(
                stderr,
                "SESSION FAIL wait Xwayland satellite: %s\n",
                strerror(errno)
            );

            stop_polkit_group(
                polkit_group,
                polkit_pid
            );

            terminate_group(
                shell_group,
                GROUP_TERM_GRACE_MS
            );

            if (shell_pid > 0) {
                reap_if_child(shell_pid);
            }

            stop_xwayland_group(
                satellite_group,
                satellite_pid,
                false,
                listener
            );

            stop_core_group(
                core_group,
                core_pid,
                false
            );

            return EXIT_FAILURE;
        }

        const uint64_t now =
            monotonic_ms();

        if (polkit_pid > 0) {
            int polkit_status = 0;

            const pid_t polkit_result =
                waitpid(
                    polkit_pid,
                    &polkit_status,
                    WNOHANG
                );

            if (polkit_result ==
                    polkit_pid) {
                const uint64_t runtime =
                    now - polkit_started;

                log_status(
                    "POLKIT_EXIT",
                    polkit_status
                );

                terminate_group(
                    polkit_group,
                    GROUP_TERM_GRACE_MS
                );

                polkit_pid = -1;
                polkit_group = -1;

                if (runtime >=
                        POLKIT_STABLE_MS) {
                    polkit_failures = 0;
                    polkit_backoff =
                        POLKIT_BACKOFF_INITIAL_MS;
                }

                polkit_failures++;

                if (polkit_failures >=
                        POLKIT_RESTART_MAX) {
                    polkit_degraded = true;

                    printf(
                        "SESSION POLKIT_DEGRADED reason=restart-limit failures=%u\n",
                        polkit_failures
                    );
                    fflush(stdout);
                } else {
                    next_polkit_start =
                        now + polkit_backoff;

                    printf(
                        "SESSION POLKIT_RESTART in_ms=%u failure=%u/%u\n",
                        polkit_backoff,
                        polkit_failures,
                        POLKIT_RESTART_MAX
                    );
                    fflush(stdout);

                    if (polkit_backoff <
                            POLKIT_BACKOFF_MAX_MS) {
                        const unsigned doubled =
                            polkit_backoff * 2U;

                        polkit_backoff =
                            doubled >
                                POLKIT_BACKOFF_MAX_MS
                            ? POLKIT_BACKOFF_MAX_MS
                            : doubled;
                    }
                }
            } else if (polkit_result < 0 &&
                    errno != EINTR) {
                fprintf(
                    stderr,
                    "SESSION WARN wait Polkit agent: %s\n",
                    strerror(errno)
                );

                terminate_group(
                    polkit_group,
                    GROUP_TERM_GRACE_MS
                );

                polkit_pid = -1;
                polkit_group = -1;
                polkit_failures++;

                if (polkit_failures >=
                        POLKIT_RESTART_MAX) {
                    polkit_degraded = true;

                    printf(
                        "SESSION POLKIT_DEGRADED reason=wait-failure failures=%u\n",
                        polkit_failures
                    );
                    fflush(stdout);
                } else {
                    next_polkit_start =
                        now + polkit_backoff;
                }
            }
        } else if (!polkit_degraded &&
                now >= next_polkit_start) {
            polkit_pid =
                start_polkit_agent(
                    polkit_agent_bin
                );

            if (polkit_pid > 0) {
                polkit_group =
                    polkit_pid;
                polkit_started =
                    monotonic_ms();
            } else {
                polkit_failures++;

                if (polkit_failures >=
                        POLKIT_RESTART_MAX) {
                    polkit_degraded = true;

                    printf(
                        "SESSION POLKIT_DEGRADED reason=spawn-failure failures=%u\n",
                        polkit_failures
                    );
                    fflush(stdout);
                } else {
                    next_polkit_start =
                        now + polkit_backoff;

                    if (polkit_backoff <
                            POLKIT_BACKOFF_MAX_MS) {
                        const unsigned doubled =
                            polkit_backoff * 2U;

                        polkit_backoff =
                            doubled >
                                POLKIT_BACKOFF_MAX_MS
                            ? POLKIT_BACKOFF_MAX_MS
                            : doubled;
                    }
                }
            }
        }

        if (shell_pid > 0) {
            int shell_status = 0;

            const pid_t shell_result =
                waitpid(
                    shell_pid,
                    &shell_status,
                    WNOHANG
                );

            if (shell_result == shell_pid) {
                const uint64_t runtime =
                    now - shell_started;

                log_status(
                    "SHELL_EXIT",
                    shell_status
                );

                /*
                 * The Shell is a process-group leader. Provider
                 * descendants inherit this group. Clean the whole
                 * group before any restart so a crashed Shell cannot
                 * leave Ewwii/launcher children behind.
                 *
                 * Xwayland is session-owned and intentionally remains
                 * alive across Shell restarts, preserving DISPLAY.
                 */
                terminate_group(
                    shell_group,
                    GROUP_TERM_GRACE_MS
                );

                shell_pid = -1;
                shell_group = -1;

                if (runtime >=
                        SHELL_STABLE_MS) {
                    backoff =
                        SHELL_BACKOFF_INITIAL_MS;
                }

                next_shell_start =
                    now + backoff;

                printf(
                    "SESSION SHELL_RESTART in_ms=%u\n",
                    backoff
                );
                fflush(stdout);

                if (backoff <
                        SHELL_BACKOFF_MAX_MS) {
                    const unsigned doubled =
                        backoff * 2U;

                    backoff =
                        doubled >
                            SHELL_BACKOFF_MAX_MS
                        ? SHELL_BACKOFF_MAX_MS
                        : doubled;
                }
            } else if (shell_result < 0 &&
                    errno != EINTR) {
                fprintf(
                    stderr,
                    "SESSION WARN wait Shell: %s\n",
                    strerror(errno)
                );

                terminate_group(
                    shell_group,
                    GROUP_TERM_GRACE_MS
                );

                shell_pid = -1;
                shell_group = -1;
                next_shell_start =
                    now + backoff;
            }
        } else if (now >=
                next_shell_start) {
            shell_pid =
                start_shell(
                    shell_bin,
                    shell_config
                );

            if (shell_pid > 0) {
                shell_group =
                    shell_pid;
                shell_started =
                    monotonic_ms();
            } else {
                next_shell_start =
                    now + backoff;

                if (backoff <
                        SHELL_BACKOFF_MAX_MS) {
                    const unsigned doubled =
                        backoff * 2U;

                    backoff =
                        doubled >
                            SHELL_BACKOFF_MAX_MS
                        ? SHELL_BACKOFF_MAX_MS
                        : doubled;
                }
            }
        }

        sleep_ms(POLL_INTERVAL_MS);
    }
}

int main(
        int argc,
        char **argv) {
    if (argc != 1) {
        fprintf(
            stderr,
            "usage: %s\n",
            argv[0]
        );

        return 64;
    }

    const char *runtime_dir =
        getenv("XDG_RUNTIME_DIR");

    if (!runtime_dir ||
            runtime_dir[0] != '/') {
        fprintf(
            stderr,
            "SESSION FAIL XDG_RUNTIME_DIR is missing or not absolute\n"
        );

        return EXIT_FAILURE;
    }

    const char *core_bin =
        env_or(
            "ONYRION_CORE_BIN",
            "/usr/bin/onyrion"
        );

    const char *shell_bin =
        env_or(
            "ONYRION_SHELL_BIN",
            "/usr/bin/onyrion-shell"
        );

    const char *core_config =
        getenv("ONYRION_CORE_CONFIG");

    const bool core_config_explicit =
        core_config &&
        core_config[0] != '\0';

    char shell_config_path[PATH_MAX] = {0};
    bool shell_config_seeded = false;

    if (!resolve_shell_config(
            shell_config_path,
            sizeof(shell_config_path),
            &shell_config_seeded)) {
        fprintf(
            stderr,
            "SESSION FAIL resolve shell config: %s\n",
            strerror(errno)
        );
        return EXIT_FAILURE;
    }

    const char *shell_config =
        shell_config_path;

    const char *socket_name =
        env_or(
            "ONYRION_SOCKET",
            "onyrion-0"
        );

    const char *dbus_update =
        env_or(
            "ONYRION_DBUS_UPDATE_BIN",
            "dbus-update-activation-environment"
        );

    const char *satellite_bin =
        env_or(
            "ONYRION_XWAYLAND_SATELLITE_BIN",
            "/usr/bin/xwayland-satellite"
        );

    const char *polkit_agent_bin =
        env_or(
            "ONYRION_POLKIT_AGENT_BIN",
            "/usr/lib/polkit-kde-authentication-agent-1"
        );

    unsigned ready_timeout = 0;
    unsigned xwayland_ready_timeout = 0;

    if (!parse_unsigned_env(
            "ONYRION_READY_TIMEOUT_MS",
            READY_TIMEOUT_MS_DEFAULT,
            &ready_timeout) ||
            !parse_unsigned_env(
                "ONYRION_XWAYLAND_READY_TIMEOUT_MS",
                XWAYLAND_READY_TIMEOUT_MS_DEFAULT,
                &xwayland_ready_timeout)) {
        return EXIT_FAILURE;
    }

    struct sigaction action = {
        .sa_handler = handle_signal,
    };

    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) < 0 ||
            sigaction(SIGTERM, &action, NULL) < 0 ||
            sigaction(SIGHUP, &action, NULL) < 0) {
        fprintf(
            stderr,
            "SESSION FAIL cannot install signal handlers: %s\n",
            strerror(errno)
        );

        return EXIT_FAILURE;
    }

    if (setenv(
            "XDG_SESSION_TYPE",
            "wayland",
            1) < 0 ||
            setenv(
                "XDG_CURRENT_DESKTOP",
                "Onyrion",
                1) < 0 ||
            setenv(
                "XDG_SESSION_DESKTOP",
                "onyrion",
                1) < 0 ||
            setenv(
                "WAYLAND_DISPLAY",
                socket_name,
                1) < 0) {
        fprintf(
            stderr,
            "SESSION FAIL cannot establish session environment: %s\n",
            strerror(errno)
        );

        return EXIT_FAILURE;
    }

    (void)unsetenv("DISPLAY");

    printf(
        "SESSION START socket=%s core=%s shell=%s polkit=%s shell_config=%s seeded=%s\n",
        socket_name,
        core_bin,
        shell_bin,
        polkit_agent_bin,
        shell_config,
        shell_config_seeded
            ? "yes"
            : "no"
    );
    fflush(stdout);

    char *const core_argv_explicit[] = {
        (char *)core_bin,
        "--config",
        (char *)core_config,
        "--socket",
        (char *)socket_name,
        NULL,
    };

    char *const core_argv_default[] = {
        (char *)core_bin,
        "--socket",
        (char *)socket_name,
        NULL,
    };

    char *const *core_argv =
        core_config_explicit
            ? core_argv_explicit
            : core_argv_default;

    const pid_t core_pid =
        spawn_exec(
            core_bin,
            core_argv,
            true,
            true
        );

    if (core_pid < 0) {
        return EXIT_FAILURE;
    }

    const pid_t core_group =
        core_pid;

    printf(
        "SESSION CORE_START pid=%ld pgid=%ld\n",
        (long)core_pid,
        (long)core_group
    );
    fflush(stdout);

    int early_core_status = 0;
    bool core_reaped = false;

    if (!wait_core_ready(
            core_pid,
            runtime_dir,
            socket_name,
            ready_timeout,
            &early_core_status,
            &core_reaped)) {
        if (core_reaped) {
            log_status(
                "CORE_EARLY_EXIT",
                early_core_status
            );
        } else if (g_stop_requested) {
            printf(
                "SESSION STOP reason=signal-before-ready\n"
            );
            fflush(stdout);
        } else {
            fprintf(
                stderr,
                "SESSION FAIL Core readiness timeout socket=%s timeout_ms=%u\n",
                socket_name,
                ready_timeout
            );
        }

        stop_core_group(
            core_group,
            core_pid,
            core_reaped
        );

        return g_stop_requested
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    printf(
        "SESSION CORE_READY socket=%s probe=wayland-roundtrip\n",
        socket_name
    );
    fflush(stdout);

    char selected_display[
        X_DISPLAY_NAME_CAPACITY
    ] = {0};

    struct x11_listener x11_listener;
    x11_listener_reset(
        &x11_listener
    );

    const pid_t satellite_pid =
        start_xwayland_satellite(
            satellite_bin,
            xwayland_ready_timeout,
            selected_display,
            &x11_listener
        );

    if (satellite_pid < 0) {
        if (g_stop_requested) {
            printf(
                "SESSION STOP reason=signal-before-xwayland-ready\n"
            );
            fflush(stdout);
        } else {
            fprintf(
                stderr,
                "SESSION FAIL no usable X display in range=:%d..:%d\n",
                X_DISPLAY_MIN,
                X_DISPLAY_MAX
            );
        }

        stop_core_group(
            core_group,
            core_pid,
            false
        );

        return g_stop_requested
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    const pid_t satellite_group =
        satellite_pid;

    if (setenv(
            "DISPLAY",
            selected_display,
            1) < 0) {
        fprintf(
            stderr,
            "SESSION FAIL cannot set DISPLAY=%s: %s\n",
            selected_display,
            strerror(errno)
        );

        stop_xwayland_group(
            satellite_group,
            satellite_pid,
            false,
            &x11_listener
        );

        stop_core_group(
            core_group,
            core_pid,
            false
        );

        return EXIT_FAILURE;
    }

    printf(
        "SESSION DISPLAY_READY display=%s\n",
        selected_display
    );
    fflush(stdout);

    if (!import_activation_environment(
            dbus_update)) {
        stop_xwayland_group(
            satellite_group,
            satellite_pid,
            false,
            &x11_listener
        );

        stop_core_group(
            core_group,
            core_pid,
            false
        );

        return EXIT_FAILURE;
    }

    return supervise(
        core_pid,
        core_group,
        satellite_pid,
        satellite_group,
        &x11_listener,
        shell_bin,
        shell_config,
        polkit_agent_bin
    );
}
