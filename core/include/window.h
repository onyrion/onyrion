#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct onyrion_group;
struct onyrion_server;
struct onyrion_workspace;

struct wlr_scene_tree;
struct wlr_xdg_toplevel;

typedef enum onyrion_window_placement {
    ONYRION_WINDOW_PLACEMENT_TILED,
    ONYRION_WINDOW_PLACEMENT_TRANSIENT,
    ONYRION_WINDOW_PLACEMENT_FLOATING,
} OnyrionWindowPlacement;

typedef struct onyrion_window {
    struct onyrion_server *server;
    struct onyrion_group *group;
    struct onyrion_workspace *workspace;

    uint64_t id;
    OnyrionWindowPlacement placement;

    int x;
    int y;
    int width;
    int height;

    struct wlr_xdg_toplevel *toplevel;
    struct wlr_scene_tree *scene_tree;

    bool mapped;
    bool fullscreen;

    struct wl_list link;
    struct wl_list group_link;
    struct wl_list popups;

    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_fullscreen;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener set_parent;
    struct wl_listener new_popup;
} OnyrionWindow;

[[nodiscard]]
const char *onyrion_window_title(
    const OnyrionWindow *window
);

[[nodiscard]]
OnyrionWindow *onyrion_window_focused(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_window_focus(
    OnyrionWindow *window
);

[[nodiscard]]
bool onyrion_window_focus_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
bool onyrion_window_float_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
bool onyrion_window_tile_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
bool onyrion_window_set_floating_geometry(
    OnyrionWindow *window,
    int x,
    int y,
    int width,
    int height
);

[[nodiscard]]
bool onyrion_window_resize_floating_from_edges(
    OnyrionWindow *window,
    int origin_x,
    int origin_y,
    int origin_width,
    int origin_height,
    uint32_t edges,
    int dx,
    int dy
);

void onyrion_window_reflow_transients(
    OnyrionWindow *parent
);

void onyrion_window_move_transients_to_workspace(
    OnyrionWindow *parent,
    struct onyrion_workspace *workspace
);

[[nodiscard]]
bool onyrion_window_set_fullscreen(
    OnyrionWindow *window,
    bool fullscreen
);

[[nodiscard]]
bool onyrion_window_toggle_fullscreen_active(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_window_close_active(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_window_init(struct onyrion_server *server);

void onyrion_window_finish(struct onyrion_server *server);
