#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ONYRION_INSTALLED_CONFIG_PATH
#define ONYRION_INSTALLED_CONFIG_PATH \
    "/usr/share/onyrion/config/compositor.kdl"
#endif

static bool set_error(
        char *error,
        size_t error_size,
        const char *format,
        ...) {
    if (error &&
            error_size > 0) {
        va_list args;
        va_start(args, format);

        vsnprintf(
            error,
            error_size,
            format,
            args
        );

        va_end(args);
    }

    return false;
}

static char *xdg_path(
        const char *xdg_variable,
        const char *home_suffix,
        const char *suffix,
        char *error,
        size_t error_size) {
    const char *base =
        getenv(xdg_variable);

    char *owned_base = NULL;

    if (!base || !*base) {
        const char *home =
            getenv("HOME");

        if (!home || !*home) {
            set_error(
                error,
                error_size,
                "HOME is unset and %s is unavailable",
                xdg_variable
            );

            return NULL;
        }

        const size_t home_length =
            strlen(home);
        const size_t home_suffix_length =
            strlen(home_suffix);

        if (home_length >
                SIZE_MAX -
                    home_suffix_length - 1) {
            set_error(
                error,
                error_size,
                "XDG base path is too long"
            );

            return NULL;
        }

        owned_base =
            malloc(
                home_length +
                home_suffix_length +
                1
            );

        if (!owned_base) {
            set_error(
                error,
                error_size,
                "out of memory"
            );

            return NULL;
        }

        memcpy(
            owned_base,
            home,
            home_length
        );

        memcpy(
            owned_base + home_length,
            home_suffix,
            home_suffix_length + 1
        );

        base = owned_base;
    }

    const size_t base_length =
        strlen(base);
    const size_t suffix_length =
        strlen(suffix);

    if (base_length >
            SIZE_MAX -
                suffix_length - 1) {
        free(owned_base);

        set_error(
            error,
            error_size,
            "XDG path is too long"
        );

        return NULL;
    }

    char *path =
        malloc(
            base_length +
            suffix_length +
            1
        );

    if (!path) {
        free(owned_base);

        set_error(
            error,
            error_size,
            "out of memory"
        );

        return NULL;
    }

    memcpy(
        path,
        base,
        base_length
    );

    memcpy(
        path + base_length,
        suffix,
        suffix_length + 1
    );

    free(owned_base);

    return path;
}

char *onyrion_core_config_legacy_path(
        char *error,
        size_t error_size) {
    return xdg_path(
        "XDG_CONFIG_HOME",
        "/.config",
        "/onyrion/core.kdl",
        error,
        error_size
    );
}

char *onyrion_core_config_state_dir(
        char *error,
        size_t error_size) {
    return xdg_path(
        "XDG_STATE_HOME",
        "/.local/state",
        "/onyrion",
        error,
        error_size
    );
}

const char *onyrion_core_config_source_name(
        OnyrionConfigSource source) {
    switch (source) {
    case ONYRION_CONFIG_SOURCE_DEFAULTS:
        return "defaults";
    case ONYRION_CONFIG_SOURCE_CURRENT:
        return "current";
    case ONYRION_CONFIG_SOURCE_LEGACY:
        return "legacy";
    case ONYRION_CONFIG_SOURCE_LKG:
        return "lkg";
    case ONYRION_CONFIG_SOURCE_INSTALLED:
        return "installed";
    case ONYRION_CONFIG_SOURCE_EXPLICIT:
        return "explicit";
    case ONYRION_CONFIG_SOURCE_COUNT:
        return "invalid";
    }

    return "invalid";
}

static bool ensure_directory_tree(
        const char *directory,
        char *error,
        size_t error_size) {
    if (!directory ||
            directory[0] == '\0') {
        return set_error(
            error,
            error_size,
            "invalid directory path"
        );
    }

    char *path =
        strdup(directory);

    if (!path) {
        return set_error(
            error,
            error_size,
            "out of memory"
        );
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
            const int saved_errno =
                errno;

            free(path);

            return set_error(
                error,
                error_size,
                "cannot create directory '%s': %s",
                directory,
                strerror(saved_errno)
            );
        }

        *cursor = saved;

        if (saved == '\0') {
            break;
        }
    }

    free(path);

    return true;
}

static bool parent_directory(
        const char *path,
        char **directory) {
    *directory = NULL;

    const char *slash =
        strrchr(path, '/');

    if (!slash) {
        *directory = strdup(".");
        return *directory != NULL;
    }

    if (slash == path) {
        *directory = strdup("/");
        return *directory != NULL;
    }

    const size_t length =
        (size_t)(slash - path);

    *directory =
        malloc(length + 1);

    if (!*directory) {
        return false;
    }

    memcpy(
        *directory,
        path,
        length
    );

    (*directory)[length] = '\0';

    return true;
}

static bool write_all(
        int fd,
        const char *contents,
        size_t length) {
    size_t offset = 0;

    while (offset < length) {
        const ssize_t written =
            write(
                fd,
                contents + offset,
                length - offset
            );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (written == 0) {
            errno = EIO;
            return false;
        }

        offset +=
            (size_t)written;
    }

    return true;
}

static bool atomic_write(
        const char *path,
        const char *contents,
        size_t length,
        char *error,
        size_t error_size) {
    char *directory = NULL;

    if (!parent_directory(
            path,
            &directory)) {
        return set_error(
            error,
            error_size,
            "out of memory"
        );
    }

    if (!ensure_directory_tree(
            directory,
            error,
            error_size)) {
        free(directory);
        return false;
    }

    const char template_suffix[] =
        ".tmp.XXXXXX";
    const size_t path_length =
        strlen(path);

    if (path_length >
            SIZE_MAX -
                sizeof(template_suffix)) {
        free(directory);

        return set_error(
            error,
            error_size,
            "temporary config path is too long"
        );
    }

    char *temporary =
        malloc(
            path_length +
            sizeof(template_suffix)
        );

    if (!temporary) {
        free(directory);

        return set_error(
            error,
            error_size,
            "out of memory"
        );
    }

    memcpy(
        temporary,
        path,
        path_length
    );

    memcpy(
        temporary + path_length,
        template_suffix,
        sizeof(template_suffix)
    );

    const int fd =
        mkstemp(temporary);

    if (fd < 0) {
        const int saved_errno =
            errno;

        set_error(
            error,
            error_size,
            "cannot create temporary file for '%s': %s",
            path,
            strerror(saved_errno)
        );

        free(temporary);
        free(directory);

        return false;
    }

    bool success =
        fchmod(fd, 0600) == 0 &&
        write_all(
            fd,
            contents,
            length
        );

    if (success &&
            fsync(fd) != 0) {
        success = false;
    }

    const int saved_errno =
        errno;

    if (close(fd) != 0 &&
            success) {
        success = false;
    }

    if (!success) {
        unlink(temporary);

        set_error(
            error,
            error_size,
            "cannot write '%s': %s",
            path,
            strerror(
                saved_errno
                    ? saved_errno
                    : errno)
        );

        free(temporary);
        free(directory);

        return false;
    }

    if (rename(
            temporary,
            path) != 0) {
        const int rename_errno =
            errno;

        unlink(temporary);

        set_error(
            error,
            error_size,
            "cannot replace '%s': %s",
            path,
            strerror(rename_errno)
        );

        free(temporary);
        free(directory);

        return false;
    }

    free(temporary);
    free(directory);

    return true;
}

static char *slot_path(
        const char *state_dir,
        unsigned slot,
        char *error,
        size_t error_size) {
    const int required =
        snprintf(
            NULL,
            0,
            "%s/compositor.lkg.%u.kdl",
            state_dir,
            slot
        );

    if (required < 0) {
        set_error(
            error,
            error_size,
            "cannot format LKG path"
        );

        return NULL;
    }

    char *path =
        malloc(
            (size_t)required + 1
        );

    if (!path) {
        set_error(
            error,
            error_size,
            "out of memory"
        );

        return NULL;
    }

    snprintf(
        path,
        (size_t)required + 1,
        "%s/compositor.lkg.%u.kdl",
        state_dir,
        slot
    );

    return path;
}

static int compare_mtime(
        const struct stat *left,
        const struct stat *right) {
    if (left->st_mtim.tv_sec <
            right->st_mtim.tv_sec) {
        return -1;
    }

    if (left->st_mtim.tv_sec >
            right->st_mtim.tv_sec) {
        return 1;
    }

    if (left->st_mtim.tv_nsec <
            right->st_mtim.tv_nsec) {
        return -1;
    }

    if (left->st_mtim.tv_nsec >
            right->st_mtim.tv_nsec) {
        return 1;
    }

    return 0;
}

static bool load_lkg(
        OnyrionCoreConfig *config) {
    char error[256] = {0};

    char *state_dir =
        onyrion_core_config_state_dir(
            error,
            sizeof(error)
        );

    if (!state_dir) {
        return false;
    }

    char *slots[2] = {
        slot_path(
            state_dir,
            0,
            error,
            sizeof(error)
        ),
        slot_path(
            state_dir,
            1,
            error,
            sizeof(error)
        ),
    };

    free(state_dir);

    if (!slots[0] ||
            !slots[1]) {
        free(slots[0]);
        free(slots[1]);
        return false;
    }

    struct stat stats[2];
    bool exists[2] = {
        stat(
            slots[0],
            &stats[0]) == 0,
        stat(
            slots[1],
            &stats[1]) == 0,
    };

    unsigned order[2] = {0, 1};

    if ((!exists[0] &&
            exists[1]) ||
            (exists[0] &&
             exists[1] &&
             compare_mtime(
                &stats[0],
                &stats[1]) < 0)) {
        order[0] = 1;
        order[1] = 0;
    }

    bool loaded = false;

    for (size_t index = 0;
            index < 2;
            index++) {
        const unsigned slot =
            order[index];

        if (!exists[slot]) {
            continue;
        }

        char load_error[256] = {0};

        if (!onyrion_core_config_load(
                config,
                slots[slot],
                false,
                load_error,
                sizeof(load_error))) {
            continue;
        }

        config->source =
            ONYRION_CONFIG_SOURCE_LKG;
        loaded = true;
        break;
    }

    free(slots[0]);
    free(slots[1]);

    return loaded;
}

bool onyrion_core_config_persist_lkg(
        const OnyrionCoreConfig *config,
        char *error,
        size_t error_size) {
    if (!config) {
        return set_error(
            error,
            error_size,
            "invalid LKG config"
        );
    }

    if (config->explicit_config ||
            (config->source !=
                ONYRION_CONFIG_SOURCE_CURRENT &&
             config->source !=
                ONYRION_CONFIG_SOURCE_LEGACY) ||
            !config->source_text) {
        return true;
    }

    char *state_dir =
        onyrion_core_config_state_dir(
            error,
            error_size
        );

    if (!state_dir) {
        return false;
    }

    if (!ensure_directory_tree(
            state_dir,
            error,
            error_size)) {
        free(state_dir);
        return false;
    }

    char *slots[2] = {
        slot_path(
            state_dir,
            0,
            error,
            error_size
        ),
        slot_path(
            state_dir,
            1,
            error,
            error_size
        ),
    };

    if (!slots[0] ||
            !slots[1]) {
        free(slots[0]);
        free(slots[1]);
        free(state_dir);

        return false;
    }

    struct stat stats[2];
    const bool exists[2] = {
        stat(
            slots[0],
            &stats[0]) == 0,
        stat(
            slots[1],
            &stats[1]) == 0,
    };

    unsigned target = 0;

    if (!exists[0]) {
        target = 0;
    } else if (!exists[1]) {
        target = 1;
    } else {
        target =
            compare_mtime(
                &stats[0],
                &stats[1]) <= 0
                ? 0
                : 1;
    }

    const bool success =
        atomic_write(
            slots[target],
            config->source_text,
            config->source_length,
            error,
            error_size
        );

    free(slots[0]);
    free(slots[1]);
    free(state_dir);

    return success;
}

static bool load_installed(
        OnyrionCoreConfig *config) {
    char error[256] = {0};

    if (!onyrion_core_config_load(
            config,
            ONYRION_INSTALLED_CONFIG_PATH,
            false,
            error,
            sizeof(error))) {
        return false;
    }

    config->source =
        ONYRION_CONFIG_SOURCE_INSTALLED;

    return true;
}

bool onyrion_core_config_load_startup(
        OnyrionCoreConfig *config,
        const char *explicit_path,
        bool explicit_config,
        bool migrate_legacy,
        char **watch_path,
        char *error,
        size_t error_size) {
    if (!config ||
            !watch_path ||
            (explicit_config &&
                (!explicit_path ||
                 explicit_path[0] == '\0'))) {
        return set_error(
            error,
            error_size,
            "invalid startup config arguments"
        );
    }

    *watch_path = NULL;

    if (explicit_config) {
        if (!onyrion_core_config_load(
                config,
                explicit_path,
                false,
                error,
                error_size)) {
            return false;
        }

        config->explicit_config = true;
        config->source =
            ONYRION_CONFIG_SOURCE_EXPLICIT;

        *watch_path =
            strdup(explicit_path);

        if (!*watch_path) {
            onyrion_core_config_finish(
                config
            );

            return set_error(
                error,
                error_size,
                "out of memory"
            );
        }

        return true;
    }

    char *canonical =
        onyrion_core_config_default_path(
            error,
            error_size
        );

    if (!canonical) {
        return false;
    }

    if (access(
            canonical,
            F_OK) == 0) {
        char load_error[256] = {0};

        if (onyrion_core_config_load(
                config,
                canonical,
                false,
                load_error,
                sizeof(load_error))) {
            config->source =
                ONYRION_CONFIG_SOURCE_CURRENT;

            *watch_path = canonical;
            return true;
        }
    } else if (errno != ENOENT) {
        const int saved_errno =
            errno;

        free(canonical);

        return set_error(
            error,
            error_size,
            "cannot inspect current config: %s",
            strerror(saved_errno)
        );
    } else {
        char *legacy =
            onyrion_core_config_legacy_path(
                error,
                error_size
            );

        if (!legacy) {
            free(canonical);
            return false;
        }

        if (access(
                legacy,
                F_OK) == 0) {
            char load_error[256] = {0};

            if (onyrion_core_config_load(
                    config,
                    legacy,
                    false,
                    load_error,
                    sizeof(load_error))) {
                config->source =
                    ONYRION_CONFIG_SOURCE_LEGACY;

                if (migrate_legacy &&
                        atomic_write(
                            canonical,
                            config->source_text,
                            config->source_length,
                            load_error,
                            sizeof(load_error))) {
                    config->source =
                        ONYRION_CONFIG_SOURCE_CURRENT;

                    free(config->source_path);

                    config->source_path =
                        strdup(canonical);

                    if (!config->source_path) {
                        onyrion_core_config_finish(
                            config
                        );

                        free(legacy);
                        free(canonical);

                        return set_error(
                            error,
                            error_size,
                            "out of memory"
                        );
                    }

                    *watch_path = canonical;
                    free(legacy);
                    return true;
                }

                *watch_path = legacy;
                free(canonical);
                return true;
            }
        }

        free(legacy);
    }

    if (load_lkg(config)) {
        *watch_path = canonical;
        return true;
    }

    if (load_installed(config)) {
        *watch_path = canonical;
        return true;
    }

    if (!onyrion_core_config_load_defaults(
            config,
            error,
            error_size)) {
        free(canonical);
        return false;
    }

    config->source =
        ONYRION_CONFIG_SOURCE_DEFAULTS;

    *watch_path = canonical;

    return true;
}
