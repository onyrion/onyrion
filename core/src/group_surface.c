#include "group_surface.h"

#include "server.h"
#include "input.h"
#include "workspace.h"
#include "window.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include "onyrion-shell-unstable-v1-server-protocol.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

typedef struct onyrion_group_surface {
    OnyrionServer *server;
    OnyrionGroup *group;
    struct wl_resource *owner_resource;
    struct wl_resource *resource;
    struct wlr_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener surface_destroy;
    struct wl_list link;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int layer;
    bool layer_set;
    bool configure_sent;
    int32_t configure_width;
    int32_t configure_height;
    char *name_space;
} OnyrionGroupSurface;

static const struct wlr_surface_role group_surface_role = {
    .name = "onyrion_group_surface_v1",
};

static void destroy_group_surface_state(
        OnyrionGroupSurface *surface) {
    if (!surface) {
        return;
    }

    if (surface->surface) {
        wl_list_remove(&surface->surface_destroy.link);
        surface->surface = NULL;
    }

    if (surface->scene_tree) {
        wlr_scene_node_destroy(&surface->scene_tree->node);
        surface->scene_tree = NULL;
    }

    wl_list_remove(&surface->link);
    free(surface->name_space);
    free(surface);
}

static void make_group_surface_inert(
        OnyrionGroupSurface *surface,
        const char *reason) {
    if (!surface) {
        return;
    }

    struct wl_resource *resource = surface->resource;
    const uint32_t resource_id = resource
        ? wl_resource_get_id(resource)
        : 0;
    const uint64_t group_id = surface->group
        ? surface->group->id
        : 0;

    if (resource) {
        /*
         * Keep the client-owned protocol object ID valid until its normal
         * destructor/client teardown. Requests on the inert object become
         * no-ops because its implementation resolves NULL user_data.
         */
        wl_resource_set_user_data(resource, NULL);
    }

    wlr_log(
        WLR_INFO,
        "Shell Group surface made inert:"
        " resource=%" PRIu32
        " group=%" PRIu64
        " reason=%s",
        resource_id,
        group_id,
        reason ? reason : "unknown"
    );

    destroy_group_surface_state(surface);
}

static void handle_group_surface_resource_destroy(
        struct wl_resource *resource) {
    OnyrionGroupSurface *surface =
        wl_resource_get_user_data(resource);

    if (!surface) {
        return;
    }

    surface->resource = NULL;
    destroy_group_surface_state(surface);
}

static void handle_group_surface_surface_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionGroupSurface *surface =
        wl_container_of(listener, surface, surface_destroy);

    wl_list_remove(&surface->surface_destroy.link);
    surface->surface = NULL;

    if (surface->resource) {
        wl_resource_destroy(surface->resource);
    }
}

static void send_group_size_if_changed(
        OnyrionGroupSurface *surface) {
    if (!surface || !surface->resource || !surface->group ||
            wl_resource_get_version(surface->resource) < 14) {
        return;
    }

    struct wlr_box group_box = {0};

    if (!onyrion_group_box(surface->group, &group_box) ||
            group_box.width <= 0 ||
            group_box.height <= 0) {
        return;
    }

    if (surface->configure_sent &&
            surface->configure_width == group_box.width &&
            surface->configure_height == group_box.height) {
        return;
    }

    surface->configure_sent = true;
    surface->configure_width = group_box.width;
    surface->configure_height = group_box.height;

    onyrion_group_surface_v1_send_configure(
        surface->resource,
        group_box.width,
        group_box.height
    );
}

static void apply_surface_state(OnyrionGroupSurface *surface) {
    if (!surface || !surface->scene_tree || !surface->group ||
            !surface->group->workspace || !surface->group->workspace->scene_tree ||
            !surface->group->active || !surface->group->active->scene_tree ||
            !surface->layer_set) {
        if (surface && surface->scene_tree) {
            wlr_scene_node_set_enabled(&surface->scene_tree->node, false);
        }
        return;
    }

    struct wlr_box group_box = {0};

    if (!onyrion_group_box(surface->group, &group_box) ||
            group_box.width <= 0 || group_box.height <= 0 ||
            surface->width <= 0 || surface->height <= 0 ||
            (surface->group->active && surface->group->active->fullscreen)) {
        wlr_scene_node_set_enabled(&surface->scene_tree->node, false);
        return;
    }

    int clip_x = surface->x < 0 ? -surface->x : 0;
    int clip_y = surface->y < 0 ? -surface->y : 0;
    int right = surface->x + surface->width;
    int bottom = surface->y + surface->height;
    int visible_right = right < group_box.width ? right : group_box.width;
    int visible_bottom = bottom < group_box.height ? bottom : group_box.height;
    int clip_width = visible_right - (surface->x + clip_x);
    int clip_height = visible_bottom - (surface->y + clip_y);

    if (clip_width <= 0 || clip_height <= 0) {
        wlr_scene_node_set_enabled(&surface->scene_tree->node, false);
        return;
    }

    const struct wlr_box clip = {
        .x = clip_x,
        .y = clip_y,
        .width = clip_width,
        .height = clip_height,
    };

    wlr_scene_subsurface_tree_set_clip(&surface->scene_tree->node, &clip);
    wlr_scene_node_set_position(
        &surface->scene_tree->node,
        group_box.x + surface->x,
        group_box.y + surface->y
    );

    struct wlr_scene_node *surface_node =
        &surface->scene_tree->node;
    struct wlr_scene_node *active_node =
        &surface->group->active->scene_tree->node;

    /*
     * wlroots requires place_above/place_below siblings to be distinct and
     * to share one parent. A transient migration mismatch must never reach
     * the asserting primitive. Normal migration ordering keeps this true;
     * this guard makes the invariant explicit at the call site as well.
     */
    if (surface_node == active_node ||
            surface_node->parent != active_node->parent) {
        wlr_scene_node_set_enabled(surface_node, false);
        return;
    }

    wlr_scene_node_set_enabled(surface_node, true);

    if (surface->group->active && surface->group->active->scene_tree) {
        if (surface->layer == 0) {
            wlr_scene_node_place_below(
                &surface->scene_tree->node,
                &surface->group->active->scene_tree->node
            );
        } else {
            wlr_scene_node_place_above(
                &surface->scene_tree->node,
                &surface->group->active->scene_tree->node
            );
        }
    }
}

void onyrion_group_surface_refresh(OnyrionGroup *group) {
    if (!group || !group->server) {
        return;
    }

    OnyrionGroupSurface *surface;
    wl_list_for_each(surface, &group->server->group_surfaces, link) {
        if (surface->group == group) {
            send_group_size_if_changed(surface);
            apply_surface_state(surface);
        }
    }
}

void onyrion_group_surface_reparent(
        OnyrionGroup *group,
        struct wlr_scene_tree *parent) {
    if (!group || !group->server || !parent) {
        return;
    }

    OnyrionGroupSurface *surface;
    wl_list_for_each(surface, &group->server->group_surfaces, link) {
        if (surface->group == group && surface->scene_tree) {
            wlr_scene_node_reparent(&surface->scene_tree->node, parent);
            apply_surface_state(surface);
        }
    }
}

static void handle_group_surface_destroy_request(
        struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void handle_group_surface_set_rect(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t x,
        int32_t y,
        int32_t width,
        int32_t height) {
    (void)client;

    OnyrionGroupSurface *surface =
        wl_resource_get_user_data(resource);

    if (!surface) {
        return;
    }

    if (width <= 0 || height <= 0) {
        wl_resource_post_error(
            resource,
            ONYRION_GROUP_SURFACE_V1_ERROR_INVALID_RECT,
            "group surface rect must have positive size"
        );
        return;
    }

    surface->x = x;
    surface->y = y;
    surface->width = width;
    surface->height = height;
    apply_surface_state(surface);
}

static void handle_group_surface_set_layer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t layer) {
    (void)client;

    OnyrionGroupSurface *surface = wl_resource_get_user_data(resource);

    if (!surface) {
        return;
    }

    if (layer > 1) {
        wl_resource_post_error(
            resource,
            ONYRION_GROUP_SURFACE_V1_ERROR_INVALID_LAYER,
            "invalid Group surface layer"
        );
        return;
    }

    surface->layer = (int)layer;
    surface->layer_set = true;
    apply_surface_state(surface);
}

static const struct onyrion_group_surface_v1_interface
        group_surface_implementation = {
    .destroy = handle_group_surface_destroy_request,
    .set_rect = handle_group_surface_set_rect,
    .set_layer = handle_group_surface_set_layer,
};

bool onyrion_group_surface_create(
        struct onyrion_server *server,
        struct wl_resource *owner_resource,
        struct wl_client *client,
        uint32_t id,
        struct wl_resource *surface_resource,
        uint64_t group_id,
        const char *name_space) {
    if (!server || !owner_resource || !client || !surface_resource ||
            group_id == 0 || !name_space) {
        return false;
    }

    OnyrionGroup *group = onyrion_group_find_id(server, group_id);
    struct wlr_surface *wlr_surface =
        wlr_surface_from_resource(surface_resource);

    if (!group) {
        wl_resource_post_error(
            owner_resource,
            ONYRION_SHELL_UNSTABLE_V1_ERROR_INVALID_GROUP,
            "group id does not resolve"
        );
        return false;
    }

    if (!wlr_surface) {
        return false;
    }

    uint32_t version = wl_resource_get_version(owner_resource);

    if (version > 14) {
        version = 14;
    }

    struct wl_resource *resource = wl_resource_create(
        client,
        &onyrion_group_surface_v1_interface,
        version,
        id
    );

    if (!resource) {
        wl_client_post_no_memory(client);
        return false;
    }

    if (!wlr_surface_set_role(
            wlr_surface,
            &group_surface_role,
            owner_resource,
            ONYRION_SHELL_UNSTABLE_V1_ERROR_SURFACE_ROLE)) {
        wl_resource_destroy(resource);
        return false;
    }

    OnyrionGroupSurface *surface = calloc(1, sizeof(*surface));

    if (!surface) {
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return false;
    }

    surface->name_space = strdup(name_space);

    if (!surface->name_space) {
        free(surface);
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return false;
    }

    surface->server = server;
    surface->group = group;
    surface->owner_resource = owner_resource;
    surface->resource = resource;
    surface->surface = wlr_surface;
    surface->width = 1;
    surface->height = 1;
    surface->scene_tree = wlr_scene_subsurface_tree_create(
        group->workspace->scene_tree,
        wlr_surface
    );

    if (!surface->scene_tree) {
        free(surface->name_space);
        free(surface);
        wl_resource_destroy(resource);
        return false;
    }

    wl_list_insert(server->group_surfaces.prev, &surface->link);

    surface->surface_destroy.notify =
        handle_group_surface_surface_destroy;
    wl_signal_add(
        &wlr_surface->events.destroy,
        &surface->surface_destroy
    );

    wl_resource_set_implementation(
        resource,
        &group_surface_implementation,
        surface,
        handle_group_surface_resource_destroy
    );

    wlr_surface_set_role_object(wlr_surface, resource);
    send_group_size_if_changed(surface);
    apply_surface_state(surface);

    wlr_log(
        WLR_INFO,
        "Shell Group surface created: group=%" PRIu64 " namespace=%s",
        group->id,
        surface->name_space
    );

    return true;
}

void onyrion_group_surface_owner_destroyed(
        struct onyrion_server *server,
        struct wl_resource *owner_resource) {
    if (!server || !owner_resource) {
        return;
    }

    if (server->chrome_drag_shell_resource == owner_resource) {
        if (server->chrome_drag_subject == ONYRION_CHROME_DRAG_WINDOW) {
            onyrion_input_cancel_chrome_drag_window(
                server,
                server->chrome_drag_window_id
            );
        } else if (server->chrome_drag_subject == ONYRION_CHROME_DRAG_GROUP) {
            onyrion_input_cancel_chrome_drag_group(
                server,
                server->chrome_drag_group_id
            );
        }
    }

    OnyrionGroupSurface *surface;
    OnyrionGroupSurface *tmp;

    wl_list_for_each_safe(surface, tmp, &server->group_surfaces, link) {
        if (surface->owner_resource == owner_resource && surface->resource) {
            wl_resource_destroy(surface->resource);
        }
    }

    OnyrionGroup *group;
    wl_list_for_each(group, &server->groups, link) {
        if (group->content_insets_owner == owner_resource) {
            group->content_insets_owner = NULL;
            group->shell_content_insets_set = false;
            group->content_inset_top = 0;
            group->content_inset_right = 0;
            group->content_inset_bottom = 0;
            group->content_inset_left = 0;
            onyrion_group_apply_geometry(group);
        }
    }
}

void onyrion_group_surface_group_destroyed(OnyrionGroup *group) {
    if (!group || !group->server) {
        return;
    }

    OnyrionGroupSurface *surface;
    OnyrionGroupSurface *tmp;

    wl_list_for_each_safe(surface, tmp, &group->server->group_surfaces, link) {
        if (surface->group == group) {
            make_group_surface_inert(
                surface,
                "backing-group-destroyed"
            );
        }
    }
}

bool onyrion_group_surface_motion_target(
        struct onyrion_server *server,
        double x,
        double y,
        uint64_t *group_id,
        const char **name_space,
        double *surface_x,
        double *surface_y) {
    if (!server || !server->scene) {
        return false;
    }

    double sx = 0.0;
    double sy = 0.0;
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node,
        x,
        y,
        &sx,
        &sy
    );

    if (!node) {
        return false;
    }

    OnyrionGroupSurface *surface;
    wl_list_for_each(surface, &server->group_surfaces, link) {
        struct wlr_scene_node *ancestor = node;

        while (ancestor) {
            if (surface->scene_tree &&
                    ancestor == &surface->scene_tree->node) {
                struct wlr_box box = {0};

                if (!onyrion_group_box(surface->group, &box)) {
                    return false;
                }

                if (group_id) *group_id = surface->group->id;
                if (name_space) *name_space = surface->name_space;
                if (surface_x) *surface_x = x - box.x - surface->x;
                if (surface_y) *surface_y = y - box.y - surface->y;
                return true;
            }

            if (!ancestor->parent) {
                break;
            }

            ancestor = &ancestor->parent->node;
        }
    }

    return false;
}

OnyrionGroup *onyrion_group_surface_at(
        struct onyrion_server *server,
        double x,
        double y) {
    uint64_t group_id = 0;

    if (!onyrion_group_surface_motion_target(
            server, x, y, &group_id, NULL, NULL, NULL)) {
        return NULL;
    }

    return onyrion_group_find_id(server, group_id);
}

bool onyrion_group_surface_init(struct onyrion_server *server) {
    if (!server) {
        return false;
    }

    wl_list_init(&server->group_surfaces);
    return true;
}

void onyrion_group_surface_finish(struct onyrion_server *server) {
    if (!server) {
        return;
    }

    OnyrionGroupSurface *surface;
    OnyrionGroupSurface *tmp;

    wl_list_for_each_safe(surface, tmp, &server->group_surfaces, link) {
        if (surface->resource) {
            wl_resource_destroy(surface->resource);
        }
    }
}
