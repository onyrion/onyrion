#include "recovery.h"

#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

extern char **environ;

static bool terminal_name_valid(
        const char *value) {
    if (!value || value[0] == '\0') {
        return false;
    }

    for (const char *cursor = value;
            *cursor != '\0';
            cursor++) {
        if (*cursor == ' ' ||
                *cursor == '\t' ||
                *cursor == '\n' ||
                *cursor == '\r') {
            return false;
        }
    }

    return true;
}

static char **recovery_spawn_environment(
        const char *wayland_display,
        char **owned_display_entry) {
    static const char prefix[] =
        "WAYLAND_DISPLAY=";

    if (!wayland_display ||
            wayland_display[0] == '\0' ||
            !owned_display_entry) {
        return NULL;
    }

    const size_t prefix_length =
        sizeof(prefix) - 1;

    size_t inherited_count = 0;

    for (char **entry = environ;
            *entry;
            entry++) {
        if (strncmp(
                *entry,
                prefix,
                prefix_length) != 0) {
            inherited_count++;
        }
    }

    char **spawn_environment =
        calloc(
            inherited_count + 2,
            sizeof(*spawn_environment)
        );

    if (!spawn_environment) {
        return NULL;
    }

    const size_t display_length =
        strlen(wayland_display);

    char *display_entry =
        malloc(
            prefix_length +
            display_length +
            1
        );

    if (!display_entry) {
        free(spawn_environment);
        return NULL;
    }

    memcpy(
        display_entry,
        prefix,
        prefix_length
    );

    memcpy(
        display_entry + prefix_length,
        wayland_display,
        display_length + 1
    );

    size_t output = 0;

    for (char **entry = environ;
            *entry;
            entry++) {
        if (strncmp(
                *entry,
                prefix,
                prefix_length) == 0) {
            continue;
        }

        spawn_environment[output++] =
            *entry;
    }

    spawn_environment[output++] =
        display_entry;

    spawn_environment[output] = NULL;
    *owned_display_entry = display_entry;

    return spawn_environment;
}

bool onyrion_recovery_spawn_terminal(
        const char *wayland_display) {
    const char *const candidates[] = {
        getenv("TERMINAL"),
        "foot",
        "kitty",
        "alacritty",
        "wezterm",
        "kgx",
        "gnome-terminal",
        "konsole",
        "xterm",
    };

    char *owned_display_entry = NULL;

    char **spawn_environment =
        recovery_spawn_environment(
            wayland_display,
            &owned_display_entry
        );

    if (!spawn_environment) {
        wlr_log(
            WLR_ERROR,
            "Fallback terminal environment unavailable"
        );

        return false;
    }

    int last_error = ENOENT;

    for (size_t i = 0;
            i < sizeof(candidates) /
                sizeof(candidates[0]);
            i++) {
        const char *candidate =
            candidates[i];

        if (!terminal_name_valid(candidate)) {
            continue;
        }

        pid_t pid = 0;

        char *const argv[] = {
            (char *)candidate,
            NULL,
        };

        const int rc =
            posix_spawnp(
                &pid,
                candidate,
                NULL,
                NULL,
                argv,
                spawn_environment
            );

        if (rc != 0) {
            last_error = rc;
            continue;
        }

        wlr_log(
            WLR_INFO,
            "Fallback terminal spawned:"
            " pid=%ld command=%s display=%s",
            (long)pid,
            candidate,
            wayland_display
        );

        free(owned_display_entry);
        free(spawn_environment);

        return true;
    }

    free(owned_display_entry);
    free(spawn_environment);

    wlr_log(
        WLR_ERROR,
        "Fallback terminal unavailable: %s",
        strerror(last_error)
    );

    return false;
}
