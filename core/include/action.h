#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>

#define ONYRION_ACTION_IDENTIFIER_CAPACITY 256

struct onyrion_server;

typedef enum onyrion_direction {
    ONYRION_DIRECTION_LEFT,
    ONYRION_DIRECTION_RIGHT,
    ONYRION_DIRECTION_UP,
    ONYRION_DIRECTION_DOWN,
    ONYRION_DIRECTION_COUNT,
} OnyrionDirection;

typedef enum onyrion_action_split_orientation {
    ONYRION_ACTION_SPLIT_HORIZONTAL,
    ONYRION_ACTION_SPLIT_VERTICAL,
    ONYRION_ACTION_SPLIT_ORIENTATION_COUNT,
} OnyrionActionSplitOrientation;

typedef enum onyrion_action_kind {
    ONYRION_ACTION_WORKSPACE_NEXT,
    ONYRION_ACTION_WORKSPACE_PREVIOUS,
    ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE,
    ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE,
    ONYRION_ACTION_SPLIT_ACTIVE,
    ONYRION_ACTION_FOCUS_DIRECTION,
    ONYRION_ACTION_MOVE_DIRECTION,
    ONYRION_ACTION_RESIZE_DIRECTION,
    ONYRION_ACTION_FLIP_ACTIVE_SPLIT,
    ONYRION_ACTION_TOGGLE_FULLSCREEN_ACTIVE,
    ONYRION_ACTION_CLOSE_ACTIVE,
    ONYRION_ACTION_WORKSPACE_ACTIVATE,
    ONYRION_ACTION_GROUP_FOCUS,
    ONYRION_ACTION_WINDOW_FOCUS,
    ONYRION_ACTION_WINDOW_NEXT,
    ONYRION_ACTION_WINDOW_PREVIOUS,
    ONYRION_ACTION_FLOAT_ACTIVE,
    ONYRION_ACTION_TILE_ACTIVE,
    ONYRION_ACTION_SHELL_INVOKE,
    ONYRION_ACTION_SESSION_EXIT,
    ONYRION_ACTION_GROUP_MERGE,
    ONYRION_ACTION_WINDOW_MOVE_TO_GROUP,
    ONYRION_ACTION_WINDOW_SPLIT,
    ONYRION_ACTION_WINDOW_RANGE_MOVE,
    ONYRION_ACTION_GROUP_MOVE_TO_WORKSPACE,
    ONYRION_ACTION_WINDOW_MOVE_TO_WORKSPACE,
    ONYRION_ACTION_GROUP_SPLIT_AT,
    ONYRION_ACTION_WINDOW_FLOAT,
    ONYRION_ACTION_WINDOW_TILE,
    ONYRION_ACTION_KIND_COUNT,
} OnyrionActionKind;

typedef struct onyrion_shell_invoke_action {
    char capability[ONYRION_ACTION_IDENTIFIER_CAPACITY];
    char action[ONYRION_ACTION_IDENTIFIER_CAPACITY];
} OnyrionShellInvokeAction;

typedef struct onyrion_action_object_pair {
    uint64_t source_id;
    uint64_t target_id;
} OnyrionActionObjectPair;

typedef struct onyrion_action_object_range {
    uint64_t first_id;
    uint64_t last_id;
    uint64_t target_id;
} OnyrionActionObjectRange;

typedef struct onyrion_action_object_split {
    uint64_t object_id;
    OnyrionActionSplitOrientation split_orientation;
} OnyrionActionObjectSplit;

typedef struct onyrion_action_request {
    OnyrionActionKind kind;

    union {
        OnyrionDirection direction;
        OnyrionActionSplitOrientation split_orientation;
        uint64_t object_id;
        OnyrionActionObjectPair object_pair;
        OnyrionActionObjectRange object_range;
        OnyrionActionObjectSplit object_split;
        OnyrionShellInvokeAction shell_invoke;
    };
} OnyrionActionRequest;

[[nodiscard]]
bool onyrion_action_execute(
    struct onyrion_server *server,
    OnyrionActionRequest request
);
