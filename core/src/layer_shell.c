#include "layer_shell.h"

#include "group.h"
#include "input.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "window.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

typedef struct onyrion_layer_surface {
    OnyrionServer *server;
    struct wlr_layer_surface_v1 *layer_surface;
    struct wlr_scene_layer_surface_v1 *scene_layer;

    struct wl_list link;
    struct wl_list popups;

    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener new_popup;
} OnyrionLayerSurface;

typedef struct onyrion_layer_popup {
    OnyrionLayerSurface *surface;
    struct wlr_xdg_popup *popup;
    struct wlr_scene_tree *scene_tree;

    struct onyrion_layer_popup *parent;
    struct wl_list children;
    struct wl_list parent_link;

    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener new_popup;
    struct wl_listener destroy;
    struct wl_listener scene_destroy;
} OnyrionLayerPopup;

static void destroy_layer_popup(
    OnyrionLayerPopup *popup
);

static void handle_layer_popup_new_popup(
    struct wl_listener *listener,
    void *data
);

static void handle_layer_popup_destroy(
    struct wl_listener *listener,
    void *data
);

static const char *layer_name(
        enum zwlr_layer_shell_v1_layer layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return "background";

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return "bottom";

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return "top";

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return "overlay";
    }

    return "<invalid>";
}

static struct wlr_scene_tree *scene_tree_for_layer(
        OnyrionServer *server,
        enum zwlr_layer_shell_v1_layer layer) {
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return server->scene_background;

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return server->scene_bottom;

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return server->scene_top;

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return server->scene_overlay;
    }

    return NULL;
}

static struct wlr_output *select_output(
        OnyrionServer *server) {
    if (server->cursor) {
        struct wlr_output *output =
            wlr_output_layout_output_at(
                server->output_layout,
                server->cursor->x,
                server->cursor->y
            );

        if (output) {
            return output;
        }
    }

    return wlr_output_layout_get_center_output(
        server->output_layout
    );
}

static void arrange_output(
        OnyrionServer *server,
        struct wlr_output *output,
        struct wlr_box *usable_area) {
    struct wlr_box full_area = {0};

    wlr_output_layout_get_box(
        server->output_layout,
        output,
        &full_area
    );

    *usable_area = full_area;

    if (full_area.width <= 0 ||
            full_area.height <= 0) {
        return;
    }

    static const enum zwlr_layer_shell_v1_layer
    layer_order[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };

    for (int exclusive = 1;
            exclusive >= 0;
            exclusive--) {
        for (size_t i = 0;
                i < sizeof(layer_order) /
                    sizeof(layer_order[0]);
                i++) {
            OnyrionLayerSurface *surface;

            wl_list_for_each(
                    surface,
                    &server->layer_surfaces,
                    link) {
                struct wlr_layer_surface_v1 *layer_surface =
                    surface->layer_surface;

                if (!layer_surface->initialized ||
                        layer_surface->output != output ||
                        layer_surface->current.layer !=
                            layer_order[i]) {
                    continue;
                }

                const bool has_exclusive_zone =
                    layer_surface->current.exclusive_zone > 0;

                if (has_exclusive_zone !=
                        (exclusive != 0)) {
                    continue;
                }

                wlr_scene_layer_surface_v1_configure(
                    surface->scene_layer,
                    &full_area,
                    usable_area
                );
            }
        }
    }
}

bool onyrion_layer_shell_arrange(
        struct onyrion_server *server) {
    bool usable_changed = false;

    struct wlr_box full_layout = {0};

    wlr_output_layout_get_box(
        server->output_layout,
        NULL,
        &full_layout
    );

    server->usable_x =
        full_layout.x;
    server->usable_y =
        full_layout.y;
    server->usable_width =
        full_layout.width;
    server->usable_height =
        full_layout.height;

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        const struct wlr_box previous_usable =
            output->usable_box;

        struct wlr_box usable_area = {0};

        arrange_output(
            server,
            output->wlr_output,
            &usable_area
        );

        if (previous_usable.x != usable_area.x ||
                previous_usable.y != usable_area.y ||
                previous_usable.width != usable_area.width ||
                previous_usable.height != usable_area.height) {
            usable_changed = true;
        }

        output->usable_box =
            usable_area;
    }

    if (wl_list_length(
            &server->outputs) == 1) {
        OnyrionOutput *single =
            onyrion_output_first(server);

        if (single &&
                single->usable_box.width > 0 &&
                single->usable_box.height > 0) {
            server->usable_x =
                single->usable_box.x;
            server->usable_y =
                single->usable_box.y;
            server->usable_width =
                single->usable_box.width;
            server->usable_height =
                single->usable_box.height;
        }
    }

    return usable_changed;
}

void onyrion_layer_shell_get_usable_box(
        struct onyrion_server *server,
        struct wlr_box *box) {
    if (!box) {
        return;
    }

    if (server->usable_width > 0 &&
            server->usable_height > 0) {
        *box = (struct wlr_box){
            .x = server->usable_x,
            .y = server->usable_y,
            .width = server->usable_width,
            .height = server->usable_height,
        };

        return;
    }

    wlr_output_layout_get_box(
        server->output_layout,
        NULL,
        box
    );
}

void onyrion_layer_shell_get_output_usable_box(
        struct onyrion_server *server,
        struct wlr_output *wlr_output,
        struct wlr_box *box) {
    if (!box) {
        return;
    }

    *box = (struct wlr_box){0};

    OnyrionOutput *output =
        onyrion_output_from_wlr(
            server,
            wlr_output
        );

    if (output &&
            output->usable_box.width > 0 &&
            output->usable_box.height > 0) {
        *box =
            output->usable_box;
        return;
    }

    wlr_output_layout_get_box(
        server->output_layout,
        wlr_output,
        box
    );
}

static OnyrionLayerSurface *focused_layer_surface(
        OnyrionServer *server) {
    if (!server->seat ||
            !server->seat->keyboard_state.focused_surface) {
        return NULL;
    }

    struct wlr_surface *focused =
        server->seat->keyboard_state.focused_surface;

    OnyrionLayerSurface *surface;

    wl_list_for_each(
            surface,
            &server->layer_surfaces,
            link) {
        if (surface->layer_surface->surface ==
                focused) {
            return surface;
        }
    }

    return NULL;
}

static OnyrionLayerSurface *top_exclusive_surface(
        OnyrionServer *server) {
    static const enum zwlr_layer_shell_v1_layer
    focus_layers[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
    };

    for (size_t i = 0;
            i < sizeof(focus_layers) /
                sizeof(focus_layers[0]);
            i++) {
        OnyrionLayerSurface *surface;

        wl_list_for_each_reverse(
                surface,
                &server->layer_surfaces,
                link) {
            struct wlr_layer_surface_v1 *layer_surface =
                surface->layer_surface;

            if (!layer_surface->surface->mapped ||
                    layer_surface->current.layer !=
                        focus_layers[i] ||
                    layer_surface->current.keyboard_interactive !=
                        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                continue;
            }

            return surface;
        }
    }

    return NULL;
}

bool onyrion_layer_shell_has_exclusive_keyboard(
        struct onyrion_server *server) {
    return
        server &&
        top_exclusive_surface(server);
}

bool onyrion_layer_shell_has_keyboard_focus(
        struct onyrion_server *server) {
    return
        server &&
        focused_layer_surface(server);
}

static void restore_window_focus(
        OnyrionServer *server) {
    OnyrionWindow *window =
        server->active_group
            ? server->active_group->active
            : NULL;

    onyrion_input_focus_window(
        server,
        window
    );
}

void onyrion_layer_shell_refresh_keyboard_focus(
        OnyrionServer *server) {
    OnyrionLayerSurface *exclusive =
        top_exclusive_surface(server);

    if (exclusive) {
        onyrion_input_focus_surface(
            server,
            exclusive->layer_surface->surface
        );

        return;
    }

    OnyrionLayerSurface *focused =
        focused_layer_surface(server);

    if (!focused) {
        return;
    }

    struct wlr_layer_surface_v1 *layer_surface =
        focused->layer_surface;

    if (!layer_surface->surface->mapped ||
            layer_surface->current.keyboard_interactive ==
                ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        restore_window_focus(server);
    }
}

static void rearrange_and_reflow(
        OnyrionServer *server) {
    if (onyrion_layer_shell_arrange(server)) {
        onyrion_layout_reflow(server);
    }
}

static void handle_surface_commit(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerSurface *surface =
        wl_container_of(
            listener,
            surface,
            commit
        );

    struct wlr_layer_surface_v1 *layer_surface =
        surface->layer_surface;

    if (layer_surface->initial_commit) {
        struct wlr_layer_surface_v1_state old_state =
            layer_surface->current;

        layer_surface->current =
            layer_surface->pending;

        struct wlr_scene_tree *initial_target =
            scene_tree_for_layer(
                surface->server,
                layer_surface->current.layer
            );

        if (initial_target &&
                surface->scene_layer->tree->node.parent !=
                    initial_target) {
            wlr_scene_node_reparent(
                &surface->scene_layer->tree->node,
                initial_target
            );
        }

        rearrange_and_reflow(
            surface->server
        );

        layer_surface->current =
            old_state;

        wlr_log(
            WLR_INFO,
            "Layer surface initial configure:"
            " namespace=%s layer=%s",
            layer_surface->namespace
                ? layer_surface->namespace
                : "<unset>",
            layer_name(
                layer_surface->pending.layer
            )
        );

        return;
    }

    struct wlr_scene_tree *target =
        scene_tree_for_layer(
            surface->server,
            layer_surface->current.layer
        );

    if (target &&
            surface->scene_layer->tree->node.parent !=
                target) {
        wlr_scene_node_reparent(
            &surface->scene_layer->tree->node,
            target
        );
    }

    rearrange_and_reflow(
        surface->server
    );

    onyrion_layer_shell_refresh_keyboard_focus(
        surface->server
    );
}

static void handle_surface_map(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerSurface *surface =
        wl_container_of(
            listener,
            surface,
            map
        );

    wlr_log(
        WLR_INFO,
        "Layer surface mapped:"
        " namespace=%s layer=%s",
        surface->layer_surface->namespace
            ? surface->layer_surface->namespace
            : "<unset>",
        layer_name(
            surface->layer_surface->current.layer
        )
    );

    rearrange_and_reflow(
        surface->server
    );

    onyrion_layer_shell_refresh_keyboard_focus(
        surface->server
    );
}

static void handle_surface_unmap(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerSurface *surface =
        wl_container_of(
            listener,
            surface,
            unmap
        );

    wlr_log(
        WLR_INFO,
        "Layer surface unmapped:"
        " namespace=%s",
        surface->layer_surface->namespace
            ? surface->layer_surface->namespace
            : "<unset>"
    );

    rearrange_and_reflow(
        surface->server
    );

    onyrion_layer_shell_refresh_keyboard_focus(
        surface->server
    );
}

static void remove_listener(
        struct wl_listener *listener) {
    if (!listener->notify) {
        return;
    }

    wl_list_remove(
        &listener->link
    );

    listener->notify = NULL;
}

static void handle_layer_popup_scene_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerPopup *popup =
        wl_container_of(
            listener,
            popup,
            scene_destroy
        );

    wl_list_remove(
        &popup->scene_destroy.link
    );

    popup->scene_destroy.notify = NULL;
    popup->scene_tree = NULL;
}

static void destroy_layer_popup(
        OnyrionLayerPopup *popup) {
    OnyrionLayerPopup *child;
    OnyrionLayerPopup *tmp;

    wl_list_for_each_safe(
            child,
            tmp,
            &popup->children,
            parent_link) {
        destroy_layer_popup(child);
    }

    remove_listener(&popup->commit);
    remove_listener(&popup->reposition);
    remove_listener(&popup->new_popup);
    remove_listener(&popup->destroy);
    remove_listener(&popup->scene_destroy);

    if (popup->scene_tree) {
        wlr_scene_node_destroy(
            &popup->scene_tree->node
        );
        popup->scene_tree = NULL;
    }

    wl_list_remove(
        &popup->parent_link
    );

    free(popup);
}

static bool layer_popup_constraint_box(
        OnyrionLayerSurface *surface,
        struct wlr_box *box) {
    if (!surface ||
            !surface->layer_surface ||
            !surface->layer_surface->output ||
            !surface->scene_layer ||
            !surface->scene_layer->tree) {
        return false;
    }

    struct wlr_box output_box = {0};

    wlr_output_layout_get_box(
        surface->server->output_layout,
        surface->layer_surface->output,
        &output_box
    );

    if (output_box.width <= 0 ||
            output_box.height <= 0) {
        return false;
    }

    int layer_lx = 0;
    int layer_ly = 0;

    if (!wlr_scene_node_coords(
            &surface->scene_layer->tree->node,
            &layer_lx,
            &layer_ly)) {
        return false;
    }

    box->x = output_box.x - layer_lx;
    box->y = output_box.y - layer_ly;
    box->width = output_box.width;
    box->height = output_box.height;

    return true;
}

static bool configure_layer_popup(
        OnyrionLayerPopup *popup) {
    struct wlr_box constraint;

    if (!layer_popup_constraint_box(
            popup->surface,
            &constraint)) {
        return false;
    }

    wlr_xdg_popup_unconstrain_from_box(
        popup->popup,
        &constraint
    );

    return true;
}

static void handle_layer_popup_commit(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerPopup *popup =
        wl_container_of(
            listener,
            popup,
            commit
        );

    if (!popup->popup->base->initial_commit) {
        return;
    }

    if (!configure_layer_popup(popup)) {
        wlr_log(
            WLR_ERROR,
            "Failed to configure layer-shell popup"
        );
    }
}

static void handle_layer_popup_reposition(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerPopup *popup =
        wl_container_of(
            listener,
            popup,
            reposition
        );

    if (!popup->popup->base->initialized) {
        return;
    }

    if (!configure_layer_popup(popup)) {
        wlr_log(
            WLR_ERROR,
            "Failed to reposition layer-shell popup"
        );
    }
}

static OnyrionLayerPopup *create_layer_popup(
        OnyrionLayerSurface *surface,
        OnyrionLayerPopup *parent,
        struct wlr_scene_tree *parent_tree,
        struct wlr_xdg_popup *wlr_popup) {
    OnyrionLayerPopup *popup =
        calloc(1, sizeof(*popup));

    if (!popup) {
        return NULL;
    }

    popup->surface = surface;
    popup->popup = wlr_popup;
    popup->parent = parent;

    wl_list_init(
        &popup->children
    );

    popup->scene_tree =
        wlr_scene_xdg_surface_create(
            parent_tree,
            wlr_popup->base
        );

    if (!popup->scene_tree) {
        free(popup);
        return NULL;
    }

    popup->scene_destroy.notify =
        handle_layer_popup_scene_destroy;

    wl_signal_add(
        &popup->scene_tree->node.events.destroy,
        &popup->scene_destroy
    );

    popup->commit.notify =
        handle_layer_popup_commit;

    wl_signal_add(
        &wlr_popup->base->surface->events.commit,
        &popup->commit
    );

    popup->reposition.notify =
        handle_layer_popup_reposition;

    wl_signal_add(
        &wlr_popup->events.reposition,
        &popup->reposition
    );

    popup->new_popup.notify =
        handle_layer_popup_new_popup;

    wl_signal_add(
        &wlr_popup->base->events.new_popup,
        &popup->new_popup
    );

    popup->destroy.notify =
        handle_layer_popup_destroy;

    wl_signal_add(
        &wlr_popup->events.destroy,
        &popup->destroy
    );

    wl_list_insert(
        parent
            ? &parent->children
            : &surface->popups,
        &popup->parent_link
    );

    wlr_log(
        WLR_INFO,
        "New layer-shell xdg_popup"
    );

    return popup;
}

static void handle_layer_popup_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerPopup *popup =
        wl_container_of(
            listener,
            popup,
            destroy
        );

    destroy_layer_popup(popup);
}

static void handle_layer_popup_new_popup(
        struct wl_listener *listener,
        void *data) {
    OnyrionLayerPopup *parent =
        wl_container_of(
            listener,
            parent,
            new_popup
        );

    struct wlr_xdg_popup *wlr_popup =
        data;

    if (!create_layer_popup(
            parent->surface,
            parent,
            parent->scene_tree,
            wlr_popup)) {
        wlr_xdg_popup_destroy(
            wlr_popup
        );
    }
}

static void handle_layer_surface_new_popup(
        struct wl_listener *listener,
        void *data) {
    OnyrionLayerSurface *surface =
        wl_container_of(
            listener,
            surface,
            new_popup
        );

    struct wlr_xdg_popup *wlr_popup =
        data;

    if (!create_layer_popup(
            surface,
            NULL,
            surface->scene_layer->tree,
            wlr_popup)) {
        wlr_xdg_popup_destroy(
            wlr_popup
        );
    }
}

static void handle_surface_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionLayerSurface *surface =
        wl_container_of(
            listener,
            surface,
            destroy
        );

    OnyrionServer *server =
        surface->server;

    remove_listener(
        &surface->commit
    );

    remove_listener(
        &surface->map
    );

    remove_listener(
        &surface->unmap
    );

    remove_listener(
        &surface->destroy
    );

    remove_listener(
        &surface->new_popup
    );

    OnyrionLayerPopup *popup;
    OnyrionLayerPopup *popup_tmp;

    wl_list_for_each_safe(
            popup,
            popup_tmp,
            &surface->popups,
            parent_link) {
        destroy_layer_popup(
            popup
        );
    }

    wl_list_remove(
        &surface->link
    );

    free(surface);

    rearrange_and_reflow(server);
    onyrion_layer_shell_refresh_keyboard_focus(server);
}

static void handle_new_surface(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            new_layer_surface
        );

    struct wlr_layer_surface_v1 *layer_surface =
        data;

    if (!layer_surface->output) {
        layer_surface->output =
            select_output(server);
    }

    if (!layer_surface->output) {
        wlr_log(
            WLR_ERROR,
            "Layer surface rejected: no output available"
        );

        wlr_layer_surface_v1_destroy(
            layer_surface
        );

        return;
    }

    struct wlr_scene_tree *parent =
        scene_tree_for_layer(
            server,
            layer_surface->pending.layer
        );

    if (!parent) {
        wlr_log(
            WLR_ERROR,
            "Layer surface rejected: invalid layer"
        );

        wlr_layer_surface_v1_destroy(
            layer_surface
        );

        return;
    }

    OnyrionLayerSurface *surface =
        calloc(1, sizeof(*surface));

    if (!surface) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate layer surface"
        );

        wlr_layer_surface_v1_destroy(
            layer_surface
        );

        return;
    }

    surface->server = server;
    surface->layer_surface =
        layer_surface;

    wl_list_init(
        &surface->popups
    );

    surface->scene_layer =
        wlr_scene_layer_surface_v1_create(
            parent,
            layer_surface
        );

    if (!surface->scene_layer) {
        wlr_log(
            WLR_ERROR,
            "Failed to create layer surface scene"
        );

        free(surface);

        wlr_layer_surface_v1_destroy(
            layer_surface
        );

        return;
    }

    surface->commit.notify =
        handle_surface_commit;

    wl_signal_add(
        &layer_surface->surface->events.commit,
        &surface->commit
    );

    surface->map.notify =
        handle_surface_map;

    wl_signal_add(
        &layer_surface->surface->events.map,
        &surface->map
    );

    surface->unmap.notify =
        handle_surface_unmap;

    wl_signal_add(
        &layer_surface->surface->events.unmap,
        &surface->unmap
    );

    surface->destroy.notify =
        handle_surface_destroy;

    wl_signal_add(
        &layer_surface->events.destroy,
        &surface->destroy
    );

    surface->new_popup.notify =
        handle_layer_surface_new_popup;

    wl_signal_add(
        &layer_surface->events.new_popup,
        &surface->new_popup
    );

    wl_list_insert(
        server->layer_surfaces.prev,
        &surface->link
    );

    wlr_log(
        WLR_INFO,
        "New layer surface:"
        " namespace=%s layer=%s output=%s",
        layer_surface->namespace
            ? layer_surface->namespace
            : "<unset>",
        layer_name(
            layer_surface->pending.layer
        ),
        layer_surface->output->name
            ? layer_surface->output->name
            : "<unnamed>"
    );
}

bool onyrion_layer_shell_focus_at_cursor(
        struct onyrion_server *server) {
    if (!server ||
            !server->cursor) {
        return false;
    }

    double sx;
    double sy;

    struct wlr_scene_node *node =
        wlr_scene_node_at(
            &server->scene->tree.node,
            server->cursor->x,
            server->cursor->y,
            &sx,
            &sy
        );

    (void)sx;
    (void)sy;

    if (!node) {
        return false;
    }

    OnyrionLayerSurface *surface;

    wl_list_for_each_reverse(
            surface,
            &server->layer_surfaces,
            link) {
        struct wlr_scene_node *ancestor =
            node;

        while (ancestor) {
            if (ancestor ==
                    &surface->scene_layer->tree->node) {
                const uint32_t mode =
                    surface->layer_surface->
                        current.keyboard_interactive;

                if (mode !=
                        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
                    onyrion_input_focus_surface(
                        server,
                        surface->layer_surface->surface
                    );
                }

                return true;
            }

            if (!ancestor->parent) {
                break;
            }

            ancestor =
                &ancestor->parent->node;
        }
    }

    return false;
}

void onyrion_layer_shell_output_destroy(
        struct onyrion_server *server,
        struct wlr_output *output) {
    OnyrionLayerSurface *surface;
    OnyrionLayerSurface *tmp;

    wl_list_for_each_safe(
            surface,
            tmp,
            &server->layer_surfaces,
            link) {
        if (surface->layer_surface->output !=
                output) {
            continue;
        }

        wlr_layer_surface_v1_destroy(
            surface->layer_surface
        );
    }
}

bool onyrion_layer_shell_init(
        struct onyrion_server *server) {
    wl_list_init(
        &server->layer_surfaces
    );

    server->scene_background =
        wlr_scene_tree_create(
            &server->scene->tree
        );

    server->scene_bottom =
        wlr_scene_tree_create(
            &server->scene->tree
        );

    server->scene_content =
        wlr_scene_tree_create(
            &server->scene->tree
        );

    server->scene_top =
        wlr_scene_tree_create(
            &server->scene->tree
        );

    server->scene_overlay =
        wlr_scene_tree_create(
            &server->scene->tree
        );

    if (!server->scene_background ||
            !server->scene_bottom ||
            !server->scene_content ||
            !server->scene_top ||
            !server->scene_overlay) {
        wlr_log(
            WLR_ERROR,
            "Failed to create layer scene trees"
        );

        return false;
    }

    server->layer_shell =
        wlr_layer_shell_v1_create(
            server->display,
            5
        );

    if (!server->layer_shell) {
        wlr_log(
            WLR_ERROR,
            "Failed to create layer shell"
        );

        return false;
    }

    server->new_layer_surface.notify =
        handle_new_surface;

    wl_signal_add(
        &server->layer_shell->events.new_surface,
        &server->new_layer_surface
    );

    onyrion_layer_shell_arrange(server);

    return true;
}

void onyrion_layer_shell_finish(
        struct onyrion_server *server) {
    if (server->new_layer_surface.notify) {
        wl_list_remove(
            &server->new_layer_surface.link
        );

        server->new_layer_surface.notify =
            NULL;
    }

    OnyrionLayerSurface *surface;
    OnyrionLayerSurface *tmp;

    wl_list_for_each_safe(
            surface,
            tmp,
            &server->layer_surfaces,
            link) {
        wlr_layer_surface_v1_destroy(
            surface->layer_surface
        );
    }

    server->layer_shell = NULL;
}
