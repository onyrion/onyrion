#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct onyrion_server;
struct onyrion_workspace;
struct wlr_output;
struct wlr_scene_output;

typedef struct onyrion_output {
    struct onyrion_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;

    struct wl_list assigned_workspaces;
    struct onyrion_workspace *visible_workspace;
    struct wlr_box usable_box;

    struct wl_list link;
    struct wl_listener frame;
    struct wl_listener present;
    struct wl_listener destroy;
} OnyrionOutput;

[[nodiscard]]
bool onyrion_output_init(struct onyrion_server *server);

void onyrion_output_finish(struct onyrion_server *server);

[[nodiscard]]
OnyrionOutput *onyrion_output_first(
    struct onyrion_server *server
);

[[nodiscard]]
OnyrionOutput *onyrion_output_from_wlr(
    struct onyrion_server *server,
    struct wlr_output *wlr_output
);

[[nodiscard]]
OnyrionOutput *onyrion_output_at(
    struct onyrion_server *server,
    double lx,
    double ly
);

void onyrion_output_focus(
    struct onyrion_server *server,
    OnyrionOutput *output
);
