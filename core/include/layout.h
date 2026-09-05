#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct onyrion_group;
struct onyrion_layout_node;
struct onyrion_server;
struct onyrion_workspace;

typedef enum onyrion_layout_node_type {
    ONYRION_LAYOUT_NODE_TILE,
    ONYRION_LAYOUT_NODE_SPLIT,
} OnyrionLayoutNodeType;

typedef enum onyrion_split_orientation {
    ONYRION_SPLIT_HORIZONTAL,
    ONYRION_SPLIT_VERTICAL,
} OnyrionSplitOrientation;

typedef struct onyrion_layout_resize_target {
    uint64_t first_group_id;
    uint64_t second_group_id;
    OnyrionSplitOrientation orientation;
} OnyrionLayoutResizeTarget;

#define ONYRION_FOCUS_DIRECTIONS(X) \
    X(LEFT,  "left",  ONYRION_SPLIT_HORIZONTAL, -1) \
    X(RIGHT, "right", ONYRION_SPLIT_HORIZONTAL, +1) \
    X(UP,    "up",    ONYRION_SPLIT_VERTICAL,   -1) \
    X(DOWN,  "down",  ONYRION_SPLIT_VERTICAL,   +1)

#define ONYRION_FOCUS_DIRECTION_ENUM( \
        name, label, orientation, ratio_sign) \
    ONYRION_FOCUS_##name,

typedef enum onyrion_focus_direction {
    ONYRION_FOCUS_DIRECTIONS(
        ONYRION_FOCUS_DIRECTION_ENUM
    )

    ONYRION_FOCUS_DIRECTION_COUNT,
} OnyrionFocusDirection;

#undef ONYRION_FOCUS_DIRECTION_ENUM

typedef struct onyrion_tile {
    struct onyrion_workspace *workspace;
    struct onyrion_group *group;
    struct onyrion_layout_node *node;

    /*
     * Registry link.
     *
     * The layout tree is authoritative for geometry.
     * This list remains useful for enumeration/counting.
     */
    struct wl_list link;

    int x;
    int y;
    int width;
    int height;
} OnyrionTile;

typedef struct onyrion_layout_node {
    OnyrionLayoutNodeType type;

    struct onyrion_workspace *workspace;
    struct onyrion_layout_node *parent;

    union {
        struct {
            OnyrionTile *tile;
        } leaf;

        struct {
            OnyrionSplitOrientation orientation;
            double ratio;

            struct onyrion_layout_node *first;
            struct onyrion_layout_node *second;
        } split;
    };
} OnyrionLayoutNode;

[[nodiscard]]
bool onyrion_layout_init(
    struct onyrion_server *server
);

void onyrion_layout_finish(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_layout_add_group(
    struct onyrion_group *group,
    OnyrionSplitOrientation split_orientation
);

void onyrion_layout_remove_group(
    struct onyrion_group *group
);

[[nodiscard]]
bool onyrion_layout_move_group_to_workspace(
    struct onyrion_group *group,
    struct onyrion_workspace *target_workspace,
    OnyrionSplitOrientation split_orientation
);

[[nodiscard]]
bool onyrion_layout_move_group_relative(
    struct onyrion_group *group,
    struct onyrion_group *target,
    OnyrionFocusDirection direction
);

void onyrion_layout_apply_group(
    struct onyrion_group *group
);

void onyrion_layout_reflow(
    struct onyrion_server *server
);

void onyrion_layout_reflow_workspace(
    struct onyrion_workspace *workspace
);

[[nodiscard]]
bool onyrion_layout_focus_direction(
    struct onyrion_server *server,
    OnyrionFocusDirection direction
);

[[nodiscard]]
bool onyrion_layout_swap_direction(
    struct onyrion_server *server,
    OnyrionFocusDirection direction
);

[[nodiscard]]
bool onyrion_layout_resize_direction(
    struct onyrion_server *server,
    OnyrionFocusDirection direction
);

[[nodiscard]]
bool onyrion_layout_resize_border_at(
    struct onyrion_server *server,
    double x,
    double y,
    double tolerance,
    OnyrionLayoutResizeTarget *target
);

[[nodiscard]]
bool onyrion_layout_resize_active_target_at(
    struct onyrion_server *server,
    double x,
    double y,
    OnyrionLayoutResizeTarget *target
);

[[nodiscard]]
bool onyrion_layout_resize_target_to_position(
    struct onyrion_server *server,
    const OnyrionLayoutResizeTarget *target,
    double x,
    double y,
    double *ratio_out
);

[[nodiscard]]
bool onyrion_layout_flip_active_split(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_layout_split_active(
    struct onyrion_server *server,
    OnyrionSplitOrientation split_orientation
);
