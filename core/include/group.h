#pragma once

#include "compiler.h"
#include "layout.h"

#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct onyrion_server;
struct onyrion_tile;
struct onyrion_window;
struct onyrion_workspace;
struct wlr_scene_tree;

typedef struct onyrion_group {
    struct onyrion_server *server;
    struct onyrion_workspace *workspace;

    uint64_t id;
    struct onyrion_tile *tile;

    struct wl_list link;
    struct wl_list windows;

    struct onyrion_window *active;
    size_t window_count;

    struct wlr_scene_tree *chrome_tree;
} OnyrionGroup;

enum {
    ONYRION_GROUP_CHROME_HEIGHT = 24,
    ONYRION_GROUP_HANDLE_WIDTH = 24,
};

void onyrion_group_init(struct onyrion_server *server);
void onyrion_group_finish(struct onyrion_server *server);

[[nodiscard]]
OnyrionGroup *onyrion_group_create(
    struct onyrion_server *server,
    OnyrionSplitOrientation split_orientation
);

[[nodiscard]]
OnyrionGroup *onyrion_group_create_in_workspace(
    struct onyrion_server *server,
    struct onyrion_workspace *workspace,
    OnyrionSplitOrientation split_orientation
);

void onyrion_group_add_window(
    OnyrionGroup *group,
    struct onyrion_window *window
);

void onyrion_group_remove_window(
    struct onyrion_window *window
);

void onyrion_group_window_mapped(
    struct onyrion_window *window
);

void onyrion_group_window_unmapped(
    struct onyrion_window *window
);

void onyrion_group_focus_window(
    struct onyrion_window *window
);

void onyrion_group_refresh_chrome(
    OnyrionGroup *group
);

[[nodiscard]]
struct onyrion_window *onyrion_group_tab_at(
    struct onyrion_server *server,
    double x,
    double y
);

[[nodiscard]]
OnyrionGroup *onyrion_group_chrome_at(
    struct onyrion_server *server,
    double x,
    double y
);

[[nodiscard]]
OnyrionGroup *onyrion_group_handle_at(
    struct onyrion_server *server,
    double x,
    double y
);

[[nodiscard]]
bool onyrion_group_focus_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
bool onyrion_group_focus_window_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
bool onyrion_group_window_next(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_group_window_previous(
    struct onyrion_server *server
);

void onyrion_group_move_window(
    struct onyrion_window *window,
    OnyrionGroup *target
);


[[nodiscard]]
bool onyrion_group_merge_id(
    struct onyrion_server *server,
    uint64_t source_group_id,
    uint64_t target_group_id
);

[[nodiscard]]
bool onyrion_group_move_window_id(
    struct onyrion_server *server,
    uint64_t window_id,
    uint64_t target_group_id
);

[[nodiscard]]
bool onyrion_group_move_window_to_layout_zone_id(
    struct onyrion_server *server,
    uint64_t window_id,
    uint64_t target_group_id,
    OnyrionFocusDirection direction
);

[[nodiscard]]
bool onyrion_group_split_window_id(
    struct onyrion_server *server,
    uint64_t window_id,
    OnyrionSplitOrientation split_orientation
);

[[nodiscard]]
bool onyrion_group_split_at_window_id(
    struct onyrion_server *server,
    uint64_t window_id,
    OnyrionSplitOrientation split_orientation
);

[[nodiscard]]
bool onyrion_group_move_window_range_ids(
    struct onyrion_server *server,
    uint64_t first_window_id,
    uint64_t last_window_id,
    uint64_t target_group_id
);

[[nodiscard]]
bool onyrion_group_move_to_workspace_id(
    struct onyrion_server *server,
    uint64_t group_id,
    uint64_t workspace_id
);

[[nodiscard]]
bool onyrion_group_move_window_to_workspace_id(
    struct onyrion_server *server,
    uint64_t window_id,
    uint64_t workspace_id
);
