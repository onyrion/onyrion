#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "onyrion-shell-unstable-v1-client-protocol.h"

typedef enum test_shell_mode {
    MODE_WATCH,
    MODE_SNAPSHOT,
    MODE_WORKSPACE_NEXT,
    MODE_WORKSPACE_PREVIOUS,
    MODE_SPLIT_ACTIVE,
    MODE_FOCUS_DIRECTION,
    MODE_MOVE_DIRECTION,
    MODE_RESIZE_DIRECTION,
    MODE_FLIP_ACTIVE_SPLIT,
    MODE_TOGGLE_FULLSCREEN_ACTIVE,
    MODE_CLOSE_ACTIVE,
    MODE_WORKSPACE_ACTIVATE,
    MODE_GROUP_FOCUS,
    MODE_WINDOW_FOCUS,
    MODE_WINDOW_NEXT,
    MODE_WINDOW_PREVIOUS,
    MODE_SESSION_EXIT,
} TestShellMode;

typedef struct test_shell {
    struct wl_display *display;
    struct wl_registry *registry;
    struct onyrion_shell_unstable_v1 *shell;

    TestShellMode mode;

    uint32_t direction;
    uint32_t split_orientation;
    const char *object_id;

    bool initial_snapshot_complete;
    bool action_sent;
    bool action_result_received;
    bool action_succeeded;
    bool action_change_seen;

    bool snapshot_in_progress;
    bool refresh_pending;

    uint32_t snapshot_generation;
    uint32_t latest_generation;
} TestShell;

static const char *mode_name(TestShellMode mode) {
    switch (mode) {
    case MODE_WATCH:
        return "watch";

    case MODE_SNAPSHOT:
        return "snapshot";

    case MODE_WORKSPACE_NEXT:
        return "workspace-next";

    case MODE_WORKSPACE_PREVIOUS:
        return "workspace-previous";

    case MODE_SPLIT_ACTIVE:
        return "split-active";

    case MODE_FOCUS_DIRECTION:
        return "focus-direction";

    case MODE_MOVE_DIRECTION:
        return "move-direction";

    case MODE_RESIZE_DIRECTION:
        return "resize-direction";

    case MODE_FLIP_ACTIVE_SPLIT:
        return "flip-active-split";

    case MODE_TOGGLE_FULLSCREEN_ACTIVE:
        return "toggle-fullscreen-active";

    case MODE_CLOSE_ACTIVE:
        return "close-active";

    case MODE_WORKSPACE_ACTIVATE:
        return "workspace-activate";

    case MODE_GROUP_FOCUS:
        return "group-focus";

    case MODE_WINDOW_FOCUS:
        return "window-focus";

    case MODE_WINDOW_NEXT:
        return "window-next";

    case MODE_WINDOW_PREVIOUS:
        return "window-previous";

    case MODE_SESSION_EXIT:
        return "session-exit";
    }

    return "<invalid>";
}

static const char *action_name(uint32_t action) {
    switch (action) {
    case ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_NEXT:
        return "workspace-next";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_PREVIOUS:
        return "workspace-previous";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_SPLIT_ACTIVE:
        return "split-active";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_FOCUS_DIRECTION:
        return "focus-direction";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_MOVE_DIRECTION:
        return "move-direction";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_RESIZE_DIRECTION:
        return "resize-direction";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_FLIP_ACTIVE_SPLIT:
        return "flip-active-split";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_TOGGLE_FULLSCREEN_ACTIVE:
        return "toggle-fullscreen-active";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_CLOSE_ACTIVE:
        return "close-active";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_ACTIVATE:
        return "workspace-activate";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_FOCUS:
        return "group-focus";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_FOCUS:
        return "window-focus";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_NEXT:
        return "window-next";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_PREVIOUS:
        return "window-previous";

    case ONYRION_SHELL_UNSTABLE_V1_ACTION_SESSION_EXIT:
        return "session-exit";

    default:
        return "<unknown>";
    }
}

static bool mode_is_action(TestShellMode mode) {
    switch (mode) {
    case MODE_WORKSPACE_NEXT:
    case MODE_WORKSPACE_PREVIOUS:
    case MODE_SPLIT_ACTIVE:
    case MODE_FOCUS_DIRECTION:
    case MODE_MOVE_DIRECTION:
    case MODE_RESIZE_DIRECTION:
    case MODE_FLIP_ACTIVE_SPLIT:
    case MODE_TOGGLE_FULLSCREEN_ACTIVE:
    case MODE_CLOSE_ACTIVE:
    case MODE_WORKSPACE_ACTIVATE:
    case MODE_GROUP_FOCUS:
    case MODE_WINDOW_FOCUS:
    case MODE_WINDOW_NEXT:
    case MODE_WINDOW_PREVIOUS:
    case MODE_SESSION_EXIT:
        return true;

    case MODE_WATCH:
    case MODE_SNAPSHOT:
        return false;
    }

    return false;
}

static void request_snapshot(TestShell *state) {
    if (state->snapshot_in_progress) {
        state->refresh_pending = true;
        return;
    }

    state->snapshot_in_progress = true;

    onyrion_shell_unstable_v1_get_state(
        state->shell
    );
}

static void send_action(TestShell *state) {
    if (state->action_sent) {
        return;
    }

    state->action_sent = true;

    printf(
        "ACTION request=%s\n",
        mode_name(state->mode)
    );

    switch (state->mode) {
    case MODE_WORKSPACE_NEXT:
        onyrion_shell_unstable_v1_workspace_next(
            state->shell
        );
        break;

    case MODE_WORKSPACE_PREVIOUS:
        onyrion_shell_unstable_v1_workspace_previous(
            state->shell
        );
        break;

    case MODE_SPLIT_ACTIVE:
        onyrion_shell_unstable_v1_split_active(
            state->shell,
            state->split_orientation
        );
        break;

    case MODE_FOCUS_DIRECTION:
        onyrion_shell_unstable_v1_focus_direction(
            state->shell,
            state->direction
        );
        break;

    case MODE_MOVE_DIRECTION:
        onyrion_shell_unstable_v1_move_direction(
            state->shell,
            state->direction
        );
        break;

    case MODE_RESIZE_DIRECTION:
        onyrion_shell_unstable_v1_resize_direction(
            state->shell,
            state->direction
        );
        break;

    case MODE_FLIP_ACTIVE_SPLIT:
        onyrion_shell_unstable_v1_flip_active_split(
            state->shell
        );
        break;

    case MODE_TOGGLE_FULLSCREEN_ACTIVE:
        onyrion_shell_unstable_v1_toggle_fullscreen_active(
            state->shell
        );
        break;

    case MODE_CLOSE_ACTIVE:
        onyrion_shell_unstable_v1_close_active(
            state->shell
        );
        break;

    case MODE_WORKSPACE_ACTIVATE:
        onyrion_shell_unstable_v1_activate_workspace(
            state->shell,
            state->object_id
        );
        break;

    case MODE_GROUP_FOCUS:
        onyrion_shell_unstable_v1_focus_group(
            state->shell,
            state->object_id
        );
        break;

    case MODE_WINDOW_FOCUS:
        onyrion_shell_unstable_v1_focus_window(
            state->shell,
            state->object_id
        );
        break;

    case MODE_WINDOW_NEXT:
        onyrion_shell_unstable_v1_window_next(
            state->shell
        );
        break;

    case MODE_WINDOW_PREVIOUS:
        onyrion_shell_unstable_v1_window_previous(
            state->shell
        );
        break;

    case MODE_SESSION_EXIT:
        onyrion_shell_unstable_v1_session_exit(
            state->shell
        );
        break;

    case MODE_WATCH:
    case MODE_SNAPSHOT:
        break;
    }
}

static void handle_state_begin(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation,
        uint32_t workspace_count) {
    (void)shell;

    TestShell *state = data;

    state->snapshot_generation =
        generation;

    if (generation > state->latest_generation) {
        state->latest_generation =
            generation;
    }

    printf(
        "STATE BEGIN generation=%u workspaces=%u\n",
        generation,
        workspace_count
    );
}

static void handle_workspace(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        uint32_t active,
        uint32_t group_count,
        uint32_t tile_count,
        uint32_t window_count) {
    (void)data;
    (void)shell;

    printf(
        "WORKSPACE id=%s active=%u groups=%u tiles=%u windows=%u\n",
        id,
        active,
        group_count,
        tile_count,
        window_count
    );
}

static void handle_group(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        const char *workspace_id,
        uint32_t active,
        uint32_t window_count) {
    (void)data;
    (void)shell;

    printf(
        "GROUP id=%s workspace_id=%s active=%u windows=%u\n",
        id,
        workspace_id,
        active,
        window_count
    );
}

static void handle_window(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        const char *group_id,
        uint32_t active,
        uint32_t fullscreen,
        const char *app_id,
        const char *title) {
    (void)data;
    (void)shell;

    printf(
        "WINDOW id=%s group_id=%s active=%u fullscreen=%u app_id=%s title=%s\n",
        id,
        group_id,
        active,
        fullscreen,
        app_id,
        title
    );
}

static void handle_state_end(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation) {
    (void)shell;

    TestShell *state = data;

    printf(
        "STATE END generation=%u\n",
        generation
    );

    if (generation !=
            state->snapshot_generation) {
        fprintf(
            stderr,
            "FAIL: snapshot generation mismatch "
            "begin=%u end=%u\n",
            state->snapshot_generation,
            generation
        );

        wl_display_disconnect(
            state->display
        );

        state->display = NULL;
        return;
    }

    state->snapshot_in_progress = false;

    const bool first_snapshot =
        !state->initial_snapshot_complete;

    state->initial_snapshot_complete =
        true;

    if (state->refresh_pending) {
        state->refresh_pending = false;

        request_snapshot(state);
        return;
    }

    if (first_snapshot &&
            mode_is_action(state->mode)) {
        send_action(state);
    }
}

static void handle_changed(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation) {
    (void)shell;

    TestShell *state = data;

    printf(
        "EVENT changed generation=%u\n",
        generation
    );

    if (generation > state->latest_generation) {
        state->latest_generation =
            generation;
    }

    if (state->action_sent) {
        state->action_change_seen =
            true;
    }

    request_snapshot(state);
}

static void handle_action_result(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t action,
        uint32_t success) {
    (void)shell;

    TestShell *state = data;

    state->action_result_received =
        true;

    state->action_succeeded =
        success != 0;

    printf(
        "ACTION RESULT action=%s success=%u\n",
        action_name(action),
        success
    );
}

static const struct onyrion_shell_unstable_v1_listener shell_listener = {
    .state_begin = handle_state_begin,
    .workspace = handle_workspace,
    .group = handle_group,
    .window = handle_window,
    .state_end = handle_state_end,
    .changed = handle_changed,
    .action_result = handle_action_result,
};

static void handle_registry_global(
        void *data,
        struct wl_registry *registry,
        uint32_t name,
        const char *interface,
        uint32_t version) {
    TestShell *state = data;

    if (strcmp(
            interface,
            onyrion_shell_unstable_v1_interface.name) != 0) {
        return;
    }

    const uint32_t bind_version =
        version < 5
            ? version
            : 5;

    state->shell =
        wl_registry_bind(
            registry,
            name,
            &onyrion_shell_unstable_v1_interface,
            bind_version
        );

    if (!state->shell) {
        return;
    }

    onyrion_shell_unstable_v1_add_listener(
        state->shell,
        &shell_listener,
        state
    );

    printf(
        "TEST-SHELL BOUND interface=%s version=%u\n",
        interface,
        bind_version
    );
}

static void handle_registry_global_remove(
        void *data,
        struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_registry_global,
    .global_remove = handle_registry_global_remove,
};

static bool parse_direction(
        const char *value,
        uint32_t *direction) {
    if (strcmp(value, "left") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_LEFT;
    } else if (strcmp(value, "right") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_RIGHT;
    } else if (strcmp(value, "up") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_UP;
    } else if (strcmp(value, "down") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_DOWN;
    } else {
        return false;
    }

    return true;
}

static bool parse_split_orientation(
        const char *value,
        uint32_t *orientation) {
    if (strcmp(value, "horizontal") == 0) {
        *orientation =
            ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_HORIZONTAL;
    } else if (strcmp(value, "vertical") == 0) {
        *orientation =
            ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_VERTICAL;
    } else {
        return false;
    }

    return true;
}

static bool parse_mode(
        int argc,
        char **argv,
        TestShell *state) {
    if (argc == 1) {
        state->mode = MODE_WATCH;
        return true;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "watch") == 0) {
            state->mode = MODE_WATCH;
        } else if (strcmp(argv[1], "snapshot") == 0) {
            state->mode = MODE_SNAPSHOT;
        } else if (strcmp(argv[1], "next") == 0) {
            state->mode = MODE_WORKSPACE_NEXT;
        } else if (strcmp(argv[1], "previous") == 0) {
            state->mode = MODE_WORKSPACE_PREVIOUS;
        } else if (strcmp(argv[1], "split") == 0) {
            state->mode = MODE_SPLIT_ACTIVE;
            state->split_orientation =
                ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_HORIZONTAL;
        } else if (strcmp(argv[1], "flip") == 0) {
            state->mode = MODE_FLIP_ACTIVE_SPLIT;
        } else if (strcmp(argv[1], "fullscreen") == 0) {
            state->mode =
                MODE_TOGGLE_FULLSCREEN_ACTIVE;
        } else if (strcmp(argv[1], "close") == 0) {
            state->mode = MODE_CLOSE_ACTIVE;
        } else if (strcmp(argv[1], "window-next") == 0) {
            state->mode = MODE_WINDOW_NEXT;
        } else if (strcmp(argv[1], "window-previous") == 0) {
            state->mode = MODE_WINDOW_PREVIOUS;
        } else if (strcmp(argv[1], "exit") == 0) {
            state->mode = MODE_SESSION_EXIT;
        } else {
            return false;
        }

        return true;
    }

    if (argc != 3) {
        return false;
    }

    if (strcmp(argv[1], "focus") == 0) {
        state->mode = MODE_FOCUS_DIRECTION;

        return parse_direction(
            argv[2],
            &state->direction
        );
    }

    if (strcmp(argv[1], "move") == 0) {
        state->mode = MODE_MOVE_DIRECTION;

        return parse_direction(
            argv[2],
            &state->direction
        );
    }

    if (strcmp(argv[1], "resize") == 0) {
        state->mode = MODE_RESIZE_DIRECTION;

        return parse_direction(
            argv[2],
            &state->direction
        );
    }

    if (strcmp(argv[1], "split") == 0) {
        state->mode = MODE_SPLIT_ACTIVE;

        return parse_split_orientation(
            argv[2],
            &state->split_orientation
        );
    }

    if (strcmp(
            argv[1],
            "activate-workspace") == 0) {
        state->mode =
            MODE_WORKSPACE_ACTIVATE;
        state->object_id = argv[2];
        return true;
    }

    if (strcmp(
            argv[1],
            "focus-group") == 0) {
        state->mode =
            MODE_GROUP_FOCUS;
        state->object_id = argv[2];
        return true;
    }

    if (strcmp(
            argv[1],
            "focus-window") == 0) {
        state->mode =
            MODE_WINDOW_FOCUS;
        state->object_id = argv[2];
        return true;
    }

    return false;
}

static void print_usage(const char *argv0) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s [watch|snapshot|next|previous]\n"
        "  %s focus left|right|up|down\n"
        "  %s move left|right|up|down\n"
        "  %s resize left|right|up|down\n"
        "  %s split [horizontal|vertical]\n"
        "  %s flip\n"
        "  %s fullscreen\n"
        "  %s close\n"
        "  %s activate-workspace ID\n"
        "  %s focus-group ID\n"
        "  %s focus-window ID\n"
        "  %s window-next\n"
        "  %s window-previous\n"
        "  %s exit\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0
    );
}

int main(int argc, char **argv) {
    TestShell state = {0};

    if (!parse_mode(
            argc,
            argv,
            &state)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    state.display =
        wl_display_connect(NULL);

    if (!state.display) {
        fprintf(
            stderr,
            "FAIL: cannot connect to WAYLAND_DISPLAY\n"
        );
        return EXIT_FAILURE;
    }

    state.registry =
        wl_display_get_registry(
            state.display
        );

    if (!state.registry) {
        fprintf(
            stderr,
            "FAIL: wl_display_get_registry\n"
        );

        wl_display_disconnect(
            state.display
        );

        return EXIT_FAILURE;
    }

    wl_registry_add_listener(
        state.registry,
        &registry_listener,
        &state
    );

    if (wl_display_roundtrip(
            state.display) < 0) {
        fprintf(
            stderr,
            "FAIL: initial registry roundtrip\n"
        );

        wl_registry_destroy(
            state.registry
        );

        wl_display_disconnect(
            state.display
        );

        return EXIT_FAILURE;
    }

    if (!state.shell) {
        fprintf(
            stderr,
            "FAIL: compositor does not advertise "
            "onyrion_shell_unstable_v1\n"
        );

        wl_registry_destroy(
            state.registry
        );

        wl_display_disconnect(
            state.display
        );

        return EXIT_FAILURE;
    }

    printf(
        "TEST-SHELL CONNECTED mode=%s\n",
        mode_name(state.mode)
    );

    request_snapshot(&state);

    int result = EXIT_SUCCESS;

    while (state.display) {
        if (wl_display_dispatch(
                state.display) < 0) {
            if (state.display) {
                fprintf(
                    stderr,
                    "FAIL: Wayland dispatch failed\n"
                );

                result = EXIT_FAILURE;
            }

            break;
        }

        if (state.mode == MODE_SNAPSHOT &&
                state.initial_snapshot_complete) {
            break;
        }

        if (mode_is_action(state.mode) &&
                state.action_result_received) {
            if (!state.action_succeeded) {
                result = EXIT_FAILURE;
                break;
            }

            if (!state.action_change_seen) {
                break;
            }

            if (state.initial_snapshot_complete &&
                    !state.snapshot_in_progress &&
                    state.snapshot_generation ==
                        state.latest_generation) {
                break;
            }
        }
    }

    if (state.shell) {
        onyrion_shell_unstable_v1_destroy(
            state.shell
        );
    }

    if (state.registry) {
        wl_registry_destroy(
            state.registry
        );
    }

    if (state.display) {
        wl_display_disconnect(
            state.display
        );
    }

    return result;
}
