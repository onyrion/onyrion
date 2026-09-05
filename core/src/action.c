#include "action.h"

#include "group.h"
#include "layout.h"
#include "server.h"
#include "shell_protocol.h"
#include "window.h"
#include "workspace.h"

static bool action_direction_to_layout(
        OnyrionDirection direction,
        OnyrionFocusDirection *layout_direction) {
    if (!layout_direction) {
        return false;
    }

    switch (direction) {
    case ONYRION_DIRECTION_LEFT:
        *layout_direction = ONYRION_FOCUS_LEFT;
        return true;

    case ONYRION_DIRECTION_RIGHT:
        *layout_direction = ONYRION_FOCUS_RIGHT;
        return true;

    case ONYRION_DIRECTION_UP:
        *layout_direction = ONYRION_FOCUS_UP;
        return true;

    case ONYRION_DIRECTION_DOWN:
        *layout_direction = ONYRION_FOCUS_DOWN;
        return true;

    case ONYRION_DIRECTION_COUNT:
        return false;
    }

    return false;
}

static bool action_split_to_layout(
        OnyrionActionSplitOrientation orientation,
        OnyrionSplitOrientation *layout_orientation) {
    if (!layout_orientation) {
        return false;
    }

    switch (orientation) {
    case ONYRION_ACTION_SPLIT_HORIZONTAL:
        *layout_orientation =
            ONYRION_SPLIT_HORIZONTAL;
        return true;

    case ONYRION_ACTION_SPLIT_VERTICAL:
        *layout_orientation =
            ONYRION_SPLIT_VERTICAL;
        return true;

    case ONYRION_ACTION_SPLIT_ORIENTATION_COUNT:
        return false;
    }

    return false;
}

bool onyrion_action_execute(
        struct onyrion_server *server,
        OnyrionActionRequest request) {
    if (!server) {
        return false;
    }

    switch (request.kind) {
    case ONYRION_ACTION_WORKSPACE_NEXT:
        return onyrion_workspace_next(server);

    case ONYRION_ACTION_WORKSPACE_PREVIOUS:
        return onyrion_workspace_previous(server);

    case ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE:
    case ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE:
        /* Pointer-context actions are started by input.c. */
        return false;

    case ONYRION_ACTION_SPLIT_ACTIVE: {
        OnyrionSplitOrientation orientation;

        return
            action_split_to_layout(
                request.split_orientation,
                &orientation
            ) &&
            onyrion_layout_split_active(
                server,
                orientation
            );
    }

    case ONYRION_ACTION_FOCUS_DIRECTION:
    case ONYRION_ACTION_MOVE_DIRECTION:
    case ONYRION_ACTION_RESIZE_DIRECTION: {
        OnyrionFocusDirection direction;

        if (!action_direction_to_layout(
                request.direction,
                &direction)) {
            return false;
        }

        switch (request.kind) {
        case ONYRION_ACTION_FOCUS_DIRECTION:
            return onyrion_layout_focus_direction(
                server,
                direction
            );

        case ONYRION_ACTION_MOVE_DIRECTION:
            return onyrion_layout_swap_direction(
                server,
                direction
            );

        case ONYRION_ACTION_RESIZE_DIRECTION:
            return onyrion_layout_resize_direction(
                server,
                direction
            );

        default:
            return false;
        }
    }

    case ONYRION_ACTION_FLIP_ACTIVE_SPLIT:
        return onyrion_layout_flip_active_split(server);

    case ONYRION_ACTION_TOGGLE_FULLSCREEN_ACTIVE:
        return onyrion_window_toggle_fullscreen_active(
            server
        );

    case ONYRION_ACTION_CLOSE_ACTIVE:
        return onyrion_window_close_active(server);

    case ONYRION_ACTION_WORKSPACE_ACTIVATE:
        return onyrion_workspace_activate_id(
            server,
            request.object_id
        );

    case ONYRION_ACTION_GROUP_FOCUS:
        return onyrion_group_focus_id(
            server,
            request.object_id
        );

    case ONYRION_ACTION_WINDOW_FOCUS:
        return onyrion_window_focus_id(
            server,
            request.object_id
        );

    case ONYRION_ACTION_WINDOW_NEXT:
        return onyrion_group_window_next(server);

    case ONYRION_ACTION_WINDOW_PREVIOUS:
        return onyrion_group_window_previous(server);

    case ONYRION_ACTION_FLOAT_ACTIVE: {
        OnyrionWindow *window =
            onyrion_window_focused(server);

        return
            window &&
            onyrion_window_float_id(
                server,
                window->id
            );
    }

    case ONYRION_ACTION_TILE_ACTIVE: {
        OnyrionWindow *window =
            onyrion_window_focused(server);

        return
            window &&
            onyrion_window_tile_id(
                server,
                window->id
            );
    }

    case ONYRION_ACTION_GROUP_MERGE:
        return onyrion_group_merge_id(
            server,
            request.object_pair.source_id,
            request.object_pair.target_id
        );

    case ONYRION_ACTION_WINDOW_MOVE_TO_GROUP:
        return onyrion_group_move_window_id(
            server,
            request.object_pair.source_id,
            request.object_pair.target_id
        );

    case ONYRION_ACTION_WINDOW_SPLIT: {
        OnyrionSplitOrientation orientation;

        return
            action_split_to_layout(
                request.object_split.
                    split_orientation,
                &orientation
            ) &&
            onyrion_group_split_window_id(
                server,
                request.object_split.object_id,
                orientation
            );
    }

    case ONYRION_ACTION_WINDOW_RANGE_MOVE:
        return onyrion_group_move_window_range_ids(
            server,
            request.object_range.first_id,
            request.object_range.last_id,
            request.object_range.target_id
        );

    case ONYRION_ACTION_GROUP_MOVE_TO_WORKSPACE:
        return onyrion_group_move_to_workspace_id(
            server,
            request.object_pair.source_id,
            request.object_pair.target_id
        );

    case ONYRION_ACTION_WINDOW_MOVE_TO_WORKSPACE:
        return onyrion_group_move_window_to_workspace_id(
            server,
            request.object_pair.source_id,
            request.object_pair.target_id
        );

    case ONYRION_ACTION_GROUP_SPLIT_AT: {
        OnyrionSplitOrientation orientation;

        return
            action_split_to_layout(
                request.object_split.
                    split_orientation,
                &orientation
            ) &&
            onyrion_group_split_at_window_id(
                server,
                request.object_split.object_id,
                orientation
            );
    }

    case ONYRION_ACTION_WINDOW_FLOAT:
        return onyrion_window_float_id(
            server,
            request.object_id
        );

    case ONYRION_ACTION_WINDOW_TILE:
        return onyrion_window_tile_id(
            server,
            request.object_id
        );

    case ONYRION_ACTION_SHELL_INVOKE:
        return onyrion_shell_protocol_invoke(
            server,
            request.shell_invoke.capability,
            request.shell_invoke.action
        );

    case ONYRION_ACTION_SESSION_EXIT:
        return onyrion_server_request_exit(server);

    case ONYRION_ACTION_KIND_COUNT:
        return false;
    }

    return false;
}
