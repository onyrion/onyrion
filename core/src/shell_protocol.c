#include "action.h"
#include "fallback.h"
#include "shell_protocol.h"

#include "group.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "window.h"
#include "workspace.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <wayland-server-core.h>

#include "onyrion-shell-unstable-v1-server-protocol.h"

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

typedef struct onyrion_shell_client {
    OnyrionServer *server;
    struct wl_resource *resource;
    struct wl_list link;
    bool controller;
} OnyrionShellClient;

typedef struct onyrion_shell_pending_invoke {
    uint32_t serial;
    struct wl_resource *controller;
    struct wl_list link;
} OnyrionShellPendingInvoke;

static bool format_id(
        uint64_t value,
        char id[static 32]) {
    const int written =
        snprintf(
            id,
            32,
            "%" PRIu64,
            value
        );

    return written >= 0 &&
        written < 32;
}

static OnyrionWindow *find_window_by_toplevel(
        OnyrionServer *server,
        struct wlr_xdg_toplevel *toplevel) {
    if (!server || !toplevel) {
        return NULL;
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &server->windows,
            link) {
        if (window->toplevel == toplevel) {
            return window;
        }
    }

    return NULL;
}

static bool parse_id(
        const char *text,
        uint64_t *value) {
    if (!text ||
            !*text ||
            !value) {
        return false;
    }

    errno = 0;

    char *end = NULL;

    const uintmax_t parsed =
        strtoumax(
            text,
            &end,
            10
        );

    if (errno != 0 ||
            !end ||
            *end != '\0' ||
            parsed == 0 ||
            parsed > UINT64_MAX) {
        return false;
    }

    *value = (uint64_t)parsed;

    return true;
}

static uint32_t wire_count(size_t count) {
    if (count > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)count;
}

static size_t count_workspaces(
        OnyrionServer *server) {
    size_t count = 0;
    OnyrionWorkspace *workspace;

    wl_list_for_each(
            workspace,
            &server->workspaces,
            link) {
        count++;
    }

    return count;
}

static size_t count_workspace_tiles(
        OnyrionWorkspace *workspace) {
    size_t count = 0;
    OnyrionTile *tile;

    wl_list_for_each(
            tile,
            &workspace->tiles,
            link) {
        count++;
    }

    return count;
}

static void get_workspace_counts(
        OnyrionServer *server,
        OnyrionWorkspace *workspace,
        size_t *group_count,
        size_t *window_count) {
    *group_count = 0;
    *window_count = 0;

    OnyrionGroup *group;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (group->workspace == workspace) {
            (*group_count)++;
        }
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &server->windows,
            link) {
        if (window->workspace == workspace) {
            (*window_count)++;
        }
    }
}

static void send_snapshot(
        OnyrionShellClient *client) {
    OnyrionServer *server =
        client->server;

    const uint32_t generation =
        server->shell_generation;

    onyrion_shell_unstable_v1_send_state_begin(
        client->resource,
        generation,
        wire_count(
            count_workspaces(server)
        )
    );

    OnyrionWorkspace *workspace;

    wl_list_for_each(
            workspace,
            &server->workspaces,
            link) {
        size_t group_count = 0;
        size_t window_count = 0;

        get_workspace_counts(
            server,
            workspace,
            &group_count,
            &window_count
        );

        char id[32];

        if (!format_id(
                workspace->id,
                id)) {
            wlr_log(
                WLR_ERROR,
                "Failed to serialize workspace id"
            );
            continue;
        }

        onyrion_shell_unstable_v1_send_workspace(
            client->resource,
            id,
            workspace ==
                onyrion_workspace_focused(
                    server
                ),
            wire_count(group_count),
            wire_count(
                count_workspace_tiles(
                    workspace
                )
            ),
            wire_count(window_count)
        );
    }

    if (wl_resource_get_version(
            client->resource) >= 6) {
        OnyrionOutput *output;

        wl_list_for_each(
                output,
                &server->outputs,
                link) {
            char visible_id[32] = "";

            if (output->visible_workspace &&
                    !format_id(
                        output->visible_workspace->id,
                        visible_id)) {
                wlr_log(
                    WLR_ERROR,
                    "Failed to serialize visible workspace id"
                );
                continue;
            }

            size_t assigned_count = 0;
            OnyrionWorkspace *assigned;

            wl_list_for_each(
                    assigned,
                    &output->assigned_workspaces,
                    output_link) {
                assigned_count++;
            }

            onyrion_shell_unstable_v1_send_output(
                client->resource,
                output->wlr_output &&
                        output->wlr_output->name
                    ? output->wlr_output->name
                    : "",
                output == server->focused_output,
                visible_id,
                wire_count(assigned_count)
            );
        }

        OnyrionWorkspace *relation;

        wl_list_for_each(
                relation,
                &server->workspaces,
                link) {
            char workspace_id[32];

            if (!format_id(
                    relation->id,
                    workspace_id)) {
                continue;
            }

            const char *output_name = "";

            if (relation->output &&
                    relation->output->wlr_output &&
                    relation->output->wlr_output->name) {
                output_name =
                    relation->output->wlr_output->name;
            }

            onyrion_shell_unstable_v1_send_workspace_output(
                client->resource,
                workspace_id,
                output_name,
                onyrion_workspace_is_visible(
                    relation
                )
            );
        }
    }

    if (wl_resource_get_version(
            client->resource) >= 7) {
        OnyrionWorkspace *metadata;

        wl_list_for_each(
                metadata,
                &server->workspaces,
                link) {
            char workspace_id[32];

            if (!format_id(
                    metadata->id,
                    workspace_id)) {
                continue;
            }

            onyrion_shell_unstable_v1_send_workspace_metadata(
                client->resource,
                workspace_id,
                metadata->name,
                metadata->icon,
                metadata->persistent,
                metadata->startup,
                metadata->output_affinity
            );
        }
    }

    if (wl_resource_get_version(
            client->resource) >= 2) {
        OnyrionGroup *group;

        wl_list_for_each(
                group,
                &server->groups,
                link) {
            char id[32];
            char workspace_id[32];

            if (!format_id(group->id, id) ||
                    !group->workspace ||
                    !format_id(
                        group->workspace->id,
                        workspace_id)) {
                wlr_log(
                    WLR_ERROR,
                    "Failed to serialize group relation"
                );
                continue;
            }

            onyrion_shell_unstable_v1_send_group(
                client->resource,
                id,
                workspace_id,
                group->workspace->active_group ==
                    group,
                wire_count(group->window_count)
            );
        }

        OnyrionWindow *window;

        wl_list_for_each(
                window,
                &server->windows,
                link) {
            char id[32];
            char group_id[32] = "";

            if (!format_id(window->id, id)) {
                wlr_log(
                    WLR_ERROR,
                    "Failed to serialize window id"
                );
                continue;
            }

            if (window->group &&
                    !format_id(
                        window->group->id,
                        group_id)) {
                wlr_log(
                    WLR_ERROR,
                    "Failed to serialize window group relation"
                );
                continue;
            }

            onyrion_shell_unstable_v1_send_window(
                client->resource,
                id,
                group_id,
                window->group &&
                    window->group->active == window,
                window->fullscreen,
                window->toplevel->app_id
                    ? window->toplevel->app_id
                    : "",
                window->toplevel->title
                    ? window->toplevel->title
                    : ""
            );

            if (wl_resource_get_version(
                    client->resource) >= 9) {
                char workspace_id[32] = "";
                char parent_id[32] = "";

                if (window->workspace &&
                        !format_id(
                            window->workspace->id,
                            workspace_id)) {
                    wlr_log(
                        WLR_ERROR,
                        "Failed to serialize window workspace relation"
                    );
                    continue;
                }

                OnyrionWindow *parent =
                    window->toplevel->parent
                        ? find_window_by_toplevel(
                            server,
                            window->toplevel->parent
                        )
                        : NULL;

                if (parent &&
                        !format_id(
                            parent->id,
                            parent_id)) {
                    continue;
                }

                onyrion_shell_unstable_v1_send_window_placement(
                    client->resource,
                    id,
                    workspace_id,
                    window->placement,
                    parent_id
                );
            }
        }
    }

    onyrion_shell_unstable_v1_send_state_end(
        client->resource,
        generation
    );
}

static OnyrionShellClient *shell_client_from_resource(
        struct wl_resource *resource) {
    return wl_resource_get_user_data(
        resource
    );
}

static OnyrionShellClient *find_controller(
        OnyrionServer *server) {
    OnyrionShellClient *client;

    wl_list_for_each(
            client,
            &server->shell_clients,
            link) {
        if (client->controller) {
            return client;
        }
    }

    return NULL;
}

static OnyrionShellPendingInvoke *find_pending_invoke(
        OnyrionServer *server,
        uint32_t serial) {
    OnyrionShellPendingInvoke *pending;

    wl_list_for_each(
            pending,
            &server->shell_pending_invokes,
            link) {
        if (pending->serial == serial) {
            return pending;
        }
    }

    return NULL;
}

static void clear_controller_pending(
        OnyrionServer *server,
        struct wl_resource *resource,
        const char *reason) {
    OnyrionShellPendingInvoke *pending;
    OnyrionShellPendingInvoke *tmp;

    wl_list_for_each_safe(
            pending,
            tmp,
            &server->shell_pending_invokes,
            link) {
        if (pending->controller != resource) {
            continue;
        }

        wlr_log(
            WLR_INFO,
            "Shell invoke result: serial=%u success=0 reason=%s",
            pending->serial,
            reason
        );

        wl_list_remove(
            &pending->link
        );

        free(pending);
    }
}

static void handle_shell_destroy(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    wl_resource_destroy(resource);
}

static void handle_shell_get_state(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    send_snapshot(client);
}

static void send_action_result(
        struct wl_resource *resource,
        uint32_t action,
        bool success) {
    onyrion_shell_unstable_v1_send_action_result(
        resource,
        action,
        success
    );
}

static void handle_workspace_next(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_WORKSPACE_NEXT,
    };

    const bool success =
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_NEXT,
        success
    );
}


static void handle_workspace_previous(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_WORKSPACE_PREVIOUS,
    };

    const bool success =
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_PREVIOUS,
        success
    );
}


static bool decode_direction(
        uint32_t wire,
        OnyrionDirection *direction) {
    if (!direction) {
        return false;
    }

    switch (wire) {
    case ONYRION_SHELL_UNSTABLE_V1_DIRECTION_LEFT:
        *direction = ONYRION_DIRECTION_LEFT;
        return true;

    case ONYRION_SHELL_UNSTABLE_V1_DIRECTION_RIGHT:
        *direction = ONYRION_DIRECTION_RIGHT;
        return true;

    case ONYRION_SHELL_UNSTABLE_V1_DIRECTION_UP:
        *direction = ONYRION_DIRECTION_UP;
        return true;

    case ONYRION_SHELL_UNSTABLE_V1_DIRECTION_DOWN:
        *direction = ONYRION_DIRECTION_DOWN;
        return true;
    }

    return false;
}


static bool decode_split_orientation(
        uint32_t wire,
        OnyrionActionSplitOrientation *orientation) {
    if (!orientation) {
        return false;
    }

    switch (wire) {
    case ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_HORIZONTAL:
        *orientation =
            ONYRION_ACTION_SPLIT_HORIZONTAL;
        return true;

    case ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_VERTICAL:
        *orientation =
            ONYRION_ACTION_SPLIT_VERTICAL;
        return true;
    }

    return false;
}


static void handle_split_active(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        uint32_t wire_orientation) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionSplitOrientation orientation =
        ONYRION_ACTION_SPLIT_ORIENTATION_COUNT;

    const bool decoded =
        decode_split_orientation(
            wire_orientation,
            &orientation
        );

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_SPLIT_ACTIVE,
        .split_orientation = orientation,
    };

    const bool success =
        decoded &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_SPLIT_ACTIVE,
        success
    );
}


static void handle_focus_direction(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        uint32_t wire_direction) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionDirection direction =
        ONYRION_DIRECTION_COUNT;

    const bool decoded =
        decode_direction(
            wire_direction,
            &direction
        );

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_FOCUS_DIRECTION,
        .direction = direction,
    };

    const bool success =
        decoded &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_FOCUS_DIRECTION,
        success
    );
}


static void handle_move_direction(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        uint32_t wire_direction) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionDirection direction =
        ONYRION_DIRECTION_COUNT;

    const bool decoded =
        decode_direction(
            wire_direction,
            &direction
        );

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_MOVE_DIRECTION,
        .direction = direction,
    };

    const bool success =
        decoded &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_MOVE_DIRECTION,
        success
    );
}


static void handle_resize_direction(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        uint32_t wire_direction) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionDirection direction =
        ONYRION_DIRECTION_COUNT;

    const bool decoded =
        decode_direction(
            wire_direction,
            &direction
        );

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_RESIZE_DIRECTION,
        .direction = direction,
    };

    const bool success =
        decoded &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_RESIZE_DIRECTION,
        success
    );
}


static void handle_flip_active_split(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const OnyrionActionRequest request = {
        .kind =
            ONYRION_ACTION_FLIP_ACTIVE_SPLIT,
    };

    const bool success =
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_FLIP_ACTIVE_SPLIT,
        success
    );
}


static void handle_toggle_fullscreen_active(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const OnyrionActionRequest request = {
        .kind =
            ONYRION_ACTION_TOGGLE_FULLSCREEN_ACTIVE,
    };

    const bool success =
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_TOGGLE_FULLSCREEN_ACTIVE,
        success
    );
}


static void handle_close_active(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_CLOSE_ACTIVE,
    };

    const bool success =
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_CLOSE_ACTIVE,
        success
    );
}


static void execute_id_action(
        struct wl_resource *resource,
        OnyrionActionKind kind,
        uint32_t wire_action,
        const char *id) {
    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind = kind,
    };

    const bool success =
        parse_id(
            id,
            &request.object_id
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        wire_action,
        success
    );
}

static void handle_activate_workspace(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *id) {
    (void)wl_client;

    execute_id_action(
        resource,
        ONYRION_ACTION_WORKSPACE_ACTIVATE,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_ACTIVATE,
        id
    );
}

static void handle_focus_group(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *id) {
    (void)wl_client;

    execute_id_action(
        resource,
        ONYRION_ACTION_GROUP_FOCUS,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_FOCUS,
        id
    );
}

static void handle_focus_window(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *id) {
    (void)wl_client;

    execute_id_action(
        resource,
        ONYRION_ACTION_WINDOW_FOCUS,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_FOCUS,
        id
    );
}

static void handle_window_next(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const bool success =
        onyrion_action_execute(
            client->server,
            (OnyrionActionRequest){
                .kind = ONYRION_ACTION_WINDOW_NEXT,
            }
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_NEXT,
        success
    );
}

static void handle_window_previous(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const bool success =
        onyrion_action_execute(
            client->server,
            (OnyrionActionRequest){
                .kind =
                    ONYRION_ACTION_WINDOW_PREVIOUS,
            }
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_PREVIOUS,
        success
    );
}

static bool parse_pair_ids(
        const char *source_id,
        const char *target_id,
        OnyrionActionObjectPair *pair) {
    return pair &&
        parse_id(
            source_id,
            &pair->source_id
        ) &&
        parse_id(
            target_id,
            &pair->target_id
        );
}

static void execute_pair_action(
        struct wl_resource *resource,
        OnyrionActionKind kind,
        uint32_t wire_action,
        const char *source_id,
        const char *target_id) {
    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind = kind,
    };

    const bool success =
        parse_pair_ids(
            source_id,
            target_id,
            &request.object_pair
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        wire_action,
        success
    );
}

static void handle_merge_group(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *source_group_id,
        const char *target_group_id) {
    (void)wl_client;

    execute_pair_action(
        resource,
        ONYRION_ACTION_GROUP_MERGE,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_MERGE,
        source_group_id,
        target_group_id
    );
}

static void handle_move_window_to_group(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *window_id,
        const char *target_group_id) {
    (void)wl_client;

    execute_pair_action(
        resource,
        ONYRION_ACTION_WINDOW_MOVE_TO_GROUP,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_MOVE_TO_GROUP,
        window_id,
        target_group_id
    );
}

static void handle_split_window(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *window_id,
        uint32_t wire_orientation) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_WINDOW_SPLIT,
    };

    const bool success =
        parse_id(
            window_id,
            &request.object_split.object_id
        ) &&
        decode_split_orientation(
            wire_orientation,
            &request.object_split.
                split_orientation
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_SPLIT,
        success
    );
}

static void handle_float_window(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *window_id) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_WINDOW_FLOAT,
    };

    const bool success =
        parse_id(
            window_id,
            &request.object_id
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_FLOAT,
        success
    );
}

static void handle_tile_window(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *window_id) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_WINDOW_TILE,
    };

    const bool success =
        parse_id(
            window_id,
            &request.object_id
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_TILE,
        success
    );
}

static void handle_split_group_at(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *window_id,
        uint32_t wire_orientation) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind = ONYRION_ACTION_GROUP_SPLIT_AT,
    };

    const bool success =
        parse_id(
            window_id,
            &request.object_split.object_id
        ) &&
        decode_split_orientation(
            wire_orientation,
            &request.object_split.
                split_orientation
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_SPLIT_AT,
        success
    );
}

static void handle_move_window_range(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *first_window_id,
        const char *last_window_id,
        const char *target_group_id) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionActionRequest request = {
        .kind =
            ONYRION_ACTION_WINDOW_RANGE_MOVE,
    };

    const bool success =
        parse_id(
            first_window_id,
            &request.object_range.first_id
        ) &&
        parse_id(
            last_window_id,
            &request.object_range.last_id
        ) &&
        parse_id(
            target_group_id,
            &request.object_range.target_id
        ) &&
        onyrion_action_execute(
            client->server,
            request
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_RANGE_MOVE,
        success
    );
}

static void handle_move_group_to_workspace(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *group_id,
        const char *workspace_id) {
    (void)wl_client;

    execute_pair_action(
        resource,
        ONYRION_ACTION_GROUP_MOVE_TO_WORKSPACE,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_MOVE_TO_WORKSPACE,
        group_id,
        workspace_id
    );
}

static void handle_move_window_to_workspace(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        const char *window_id,
        const char *workspace_id) {
    (void)wl_client;

    execute_pair_action(
        resource,
        ONYRION_ACTION_WINDOW_MOVE_TO_WORKSPACE,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_MOVE_TO_WORKSPACE,
        window_id,
        workspace_id
    );
}

static void handle_session_exit(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    const bool success =
        onyrion_action_execute(
            client->server,
            (OnyrionActionRequest){
                .kind = ONYRION_ACTION_SESSION_EXIT,
            }
        );

    send_action_result(
        resource,
        ONYRION_SHELL_UNSTABLE_V1_ACTION_SESSION_EXIT,
        success
    );
}

static void handle_claim_controller(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    OnyrionShellClient *controller =
        find_controller(
            client->server
        );

    const bool success =
        !controller ||
        controller == client;

    if (success) {
        client->controller = true;
        onyrion_fallback_set_active(
            client->server,
            false
        );

        wlr_log(
            WLR_INFO,
            "Shell controller claimed:"
            " fallback_active=0"
        );
    } else {
        wlr_log(
            WLR_INFO,
            "Shell controller claim rejected"
        );
    }

    onyrion_shell_unstable_v1_send_controller_claim_result(
        resource,
        success
    );
}

static void handle_release_controller(
        struct wl_client *wl_client,
        struct wl_resource *resource) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client ||
            !client->controller) {
        return;
    }

    clear_controller_pending(
        client->server,
        resource,
        "controller-release"
    );

    client->controller = false;
    onyrion_fallback_set_active(
        client->server,
        true
    );

    wlr_log(
        WLR_INFO,
        "Shell controller released:"
        " fallback_active=1"
    );
}

static void handle_controller_result(
        struct wl_client *wl_client,
        struct wl_resource *resource,
        uint32_t serial,
        uint32_t success) {
    (void)wl_client;

    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client ||
            !client->controller) {
        wlr_log(
            WLR_INFO,
            "Shell controller result ignored: non-controller serial=%u",
            serial
        );
        return;
    }

    OnyrionShellPendingInvoke *pending =
        find_pending_invoke(
            client->server,
            serial
        );

    if (!pending ||
            pending->controller != resource) {
        wlr_log(
            WLR_INFO,
            "Shell controller result ignored: unknown serial=%u",
            serial
        );
        return;
    }

    wlr_log(
        WLR_INFO,
        "Shell invoke result: serial=%u success=%u",
        serial,
        (unsigned)(success != 0)
    );

    wl_list_remove(
        &pending->link
    );

    free(pending);
}

bool onyrion_shell_protocol_invoke(
        struct onyrion_server *server,
        const char *capability,
        const char *action) {
    if (!server ||
            !capability ||
            !*capability ||
            !action ||
            !*action) {
        return false;
    }

    OnyrionShellClient *controller =
        find_controller(server);

    if (!controller) {
        wlr_log(
            WLR_INFO,
            "Shell invoke failed: no controller capability=%s action=%s",
            capability,
            action
        );
        return false;
    }

    const uint32_t serial =
        server->shell_next_invoke_serial;

    server->shell_next_invoke_serial++;

    if (server->shell_next_invoke_serial == 0) {
        server->shell_next_invoke_serial = 1;
    }

    if (serial == 0 ||
            find_pending_invoke(
                server,
                serial)) {
        wlr_log(
            WLR_ERROR,
            "Shell invoke failed: serial collision serial=%u",
            serial
        );
        return false;
    }

    OnyrionShellPendingInvoke *pending =
        calloc(1, sizeof(*pending));

    if (!pending) {
        wlr_log(
            WLR_ERROR,
            "Shell invoke failed: out of memory"
        );
        return false;
    }

    pending->serial = serial;
    pending->controller =
        controller->resource;

    wl_list_insert(
        server->shell_pending_invokes.prev,
        &pending->link
    );

    onyrion_shell_unstable_v1_send_controller_invoke(
        controller->resource,
        serial,
        capability,
        action
    );

    wlr_log(
        WLR_INFO,
        "Shell invoke sent: serial=%u capability=%s action=%s",
        serial,
        capability,
        action
    );

    return true;
}

static const struct onyrion_shell_unstable_v1_interface
shell_implementation = {
    .destroy = handle_shell_destroy,
    .get_state = handle_shell_get_state,
    .workspace_next = handle_workspace_next,
    .workspace_previous =
        handle_workspace_previous,
    .split_active = handle_split_active,
    .focus_direction = handle_focus_direction,
    .move_direction = handle_move_direction,
    .resize_direction = handle_resize_direction,
    .flip_active_split = handle_flip_active_split,
    .toggle_fullscreen_active =
        handle_toggle_fullscreen_active,
    .close_active = handle_close_active,
    .activate_workspace =
        handle_activate_workspace,
    .focus_group = handle_focus_group,
    .focus_window = handle_focus_window,
    .window_next = handle_window_next,
    .window_previous =
        handle_window_previous,
    .claim_controller =
        handle_claim_controller,
    .release_controller =
        handle_release_controller,
    .controller_result =
        handle_controller_result,
    .session_exit = handle_session_exit,
    .merge_group = handle_merge_group,
    .move_window_to_group =
        handle_move_window_to_group,
    .split_window = handle_split_window,
    .move_window_range =
        handle_move_window_range,
    .move_group_to_workspace =
        handle_move_group_to_workspace,
    .move_window_to_workspace =
        handle_move_window_to_workspace,
    .split_group_at =
        handle_split_group_at,
    .float_window =
        handle_float_window,
    .tile_window =
        handle_tile_window,
};

static void handle_shell_resource_destroy(
        struct wl_resource *resource) {
    OnyrionShellClient *client =
        shell_client_from_resource(
            resource
        );

    if (!client) {
        return;
    }

    if (client->controller) {
        clear_controller_pending(
            client->server,
            resource,
            "controller-disconnect"
        );

        client->controller = false;
        onyrion_fallback_set_active(
            client->server,
            true
        );

        wlr_log(
            WLR_INFO,
            "Shell controller disconnected:"
            " fallback_active=1"
        );
    }

    wl_list_remove(
        &client->link
    );

    free(client);
}

static void bind_shell(
        struct wl_client *wl_client,
        void *data,
        uint32_t version,
        uint32_t id) {
    OnyrionServer *server = data;

    struct wl_resource *resource =
        wl_resource_create(
            wl_client,
            &onyrion_shell_unstable_v1_interface,
            version,
            id
        );

    if (!resource) {
        wl_client_post_no_memory(
            wl_client
        );
        return;
    }

    OnyrionShellClient *client =
        calloc(1, sizeof(*client));

    if (!client) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(
            wl_client
        );
        return;
    }

    client->server = server;
    client->resource = resource;

    wl_list_insert(
        server->shell_clients.prev,
        &client->link
    );

    wl_resource_set_implementation(
        resource,
        &shell_implementation,
        client,
        handle_shell_resource_destroy
    );

    wlr_log(
        WLR_INFO,
        "Shell protocol client connected"
    );
}

bool onyrion_shell_protocol_init(
        struct onyrion_server *server) {
    wl_list_init(
        &server->shell_clients
    );

    wl_list_init(
        &server->shell_pending_invokes
    );

    server->shell_generation = 1;
    server->shell_next_invoke_serial = 1;

    server->shell_global =
        wl_global_create(
            server->display,
            &onyrion_shell_unstable_v1_interface,
            11,
            server,
            bind_shell
        );

    if (!server->shell_global) {
        wlr_log(
            WLR_ERROR,
            "Failed to create onyrion_shell_unstable_v1 global"
        );
        return false;
    }

    wlr_log(
        WLR_INFO,
        "Shell protocol ready: onyrion_shell_unstable_v1 version=11"
    );

    return true;
}

void onyrion_shell_protocol_mark_changed(
        struct onyrion_server *server) {
    if (!server->shell_global) {
        return;
    }

    server->shell_generation++;

    if (server->shell_generation == 0) {
        server->shell_generation = 1;
    }

    OnyrionShellClient *client;

    wl_list_for_each(
            client,
            &server->shell_clients,
            link) {
        onyrion_shell_unstable_v1_send_changed(
            client->resource,
            server->shell_generation
        );
    }
}

void onyrion_shell_protocol_finish(
        struct onyrion_server *server) {
    if (!server->shell_global) {
        return;
    }

    OnyrionShellClient *client;
    OnyrionShellClient *tmp;

    wl_list_for_each_safe(
            client,
            tmp,
            &server->shell_clients,
            link) {
        wl_resource_destroy(
            client->resource
        );
    }

    wl_global_destroy(
        server->shell_global
    );

    server->shell_global = NULL;
    server->shell_generation = 0;
    server->shell_next_invoke_serial = 0;
}
