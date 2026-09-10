#pragma once

#include "compiler.h"
#include "group.h"

#include <stdbool.h>
#include <stdint.h>

struct onyrion_server;
struct wl_client;
struct wl_resource;
struct wlr_scene_tree;

[[nodiscard]]
bool onyrion_group_surface_init(
    struct onyrion_server *server
);

void onyrion_group_surface_finish(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_group_surface_create(
    struct onyrion_server *server,
    struct wl_resource *owner_resource,
    struct wl_client *client,
    uint32_t id,
    struct wl_resource *surface_resource,
    uint64_t group_id,
    const char *name_space
);

void onyrion_group_surface_owner_destroyed(
    struct onyrion_server *server,
    struct wl_resource *owner_resource
);

void onyrion_group_surface_group_destroyed(
    OnyrionGroup *group
);

void onyrion_group_surface_refresh(
    OnyrionGroup *group
);

void onyrion_group_surface_reparent(
    OnyrionGroup *group,
    struct wlr_scene_tree *parent
);

[[nodiscard]]
OnyrionGroup *onyrion_group_surface_at(
    struct onyrion_server *server,
    double x,
    double y
);


[[nodiscard]]
bool onyrion_group_surface_motion_target(
    struct onyrion_server *server,
    double x,
    double y,
    uint64_t *group_id,
    const char **name_space,
    double *surface_x,
    double *surface_y
);
