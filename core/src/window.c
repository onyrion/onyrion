#include "window.h"
#include "config.h"
#include "fallback.h"
#include "group.h"
#include "input.h"
#include "layer_shell.h"
#include "output.h"
#include "layout.h"
#include "server.h"
#include "shell_protocol.h"
#include "workspace.h"

#include <inttypes.h>
#include <stdlib.h>
#include <wayland-server-core.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

typedef struct onyrion_xdg_popup {
    OnyrionWindow *window;
    struct wlr_xdg_popup *popup;
    struct wlr_scene_tree *scene_tree;

    struct onyrion_xdg_popup *parent;
    struct wl_list children;
    struct wl_list parent_link;

    struct wl_listener new_popup;
    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener destroy;
} OnyrionXdgPopup;

static void destroy_popup(
    OnyrionXdgPopup *popup
);

static void handle_popup_new_popup(
    struct wl_listener *listener,
    void *data
);

static void handle_popup_commit(
    struct wl_listener *listener,
    void *data
);

static void handle_popup_reposition(
    struct wl_listener *listener,
    void *data
);

static void handle_popup_destroy(
    struct wl_listener *listener,
    void *data
);

const char *onyrion_window_title(
        const OnyrionWindow *window) {
    if (!window->toplevel->title) {
        return "<untitled>";
    }

    return window->toplevel->title;
}

static OnyrionWindow *find_window_by_id(
        OnyrionServer *server,
        uint64_t id) {
    if (!server || id == 0) {
        return NULL;
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &server->windows,
            link) {
        if (window->id == id) {
            return window;
        }
    }

    return NULL;
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

static OnyrionWindow *managed_parent(
        OnyrionWindow *window) {
    if (!window ||
            !window->toplevel ||
            !window->toplevel->parent) {
        return NULL;
    }

    return find_window_by_toplevel(
        window->server,
        window->toplevel->parent
    );
}

static void reparent_to_workspace(
        OnyrionWindow *window,
        OnyrionWorkspace *workspace) {
    if (!window || !workspace) {
        return;
    }

    window->workspace = workspace;

    if (workspace->scene_tree &&
            window->scene_tree) {
        wlr_scene_node_reparent(
            &window->scene_tree->node,
            workspace->scene_tree
        );
    }
}

static bool floating_box(
        OnyrionWindow *window,
        struct wlr_box *box) {
    if (!window ||
            !window->workspace ||
            !box) {
        return false;
    }

    onyrion_layer_shell_get_output_usable_box(
        window->server,
        window->workspace->output
            ? window->workspace->output->wlr_output
            : NULL,
        box
    );

    return
        box->width > 0 &&
        box->height > 0;
}

static bool apply_floating_geometry(
        OnyrionWindow *window) {
    if (!window ||
            window->placement !=
                ONYRION_WINDOW_PLACEMENT_FLOATING ||
            !window->workspace ||
            !window->scene_tree) {
        return false;
    }

    struct wlr_box anchor = {0};

    if (!floating_box(
            window,
            &anchor)) {
        return false;
    }

    int width =
        anchor.width * 2 / 3;
    int height =
        anchor.height * 2 / 3;

    if (width <= 0) {
        width = anchor.width;
    }

    if (height <= 0) {
        height = anchor.height;
    }

    window->x =
        anchor.x +
        (anchor.width - width) / 2;
    window->y =
        anchor.y +
        (anchor.height - height) / 2;
    window->width = width;
    window->height = height;

    wlr_scene_node_set_position(
        &window->scene_tree->node,
        window->x,
        window->y
    );

    wlr_scene_node_raise_to_top(
        &window->scene_tree->node
    );

    if (window->toplevel->base->initialized) {
        wlr_xdg_toplevel_set_size(
            window->toplevel,
            width,
            height
        );
    }

    return true;
}

static void apply_transient_geometry(
        OnyrionWindow *window) {
    if (!window ||
            window->placement !=
                ONYRION_WINDOW_PLACEMENT_TRANSIENT ||
            !window->workspace ||
            !window->scene_tree) {
        return;
    }

    struct wlr_box anchor = {0};

    OnyrionWindow *parent =
        managed_parent(window);

    if (parent &&
            parent->width > 0 &&
            parent->height > 0) {
        anchor.x = parent->x;
        anchor.y = parent->y;
        anchor.width = parent->width;
        anchor.height = parent->height;
    } else {
        onyrion_layer_shell_get_output_usable_box(
            window->server,
            window->workspace->output
                ? window->workspace->output->wlr_output
                : NULL,
            &anchor
        );
    }

    if (anchor.width <= 0 ||
            anchor.height <= 0) {
        return;
    }

    int width =
        anchor.width * 2 / 3;
    int height =
        anchor.height * 2 / 3;

    if (width <= 0) {
        width = anchor.width;
    }

    if (height <= 0) {
        height = anchor.height;
    }

    window->x =
        anchor.x +
        (anchor.width - width) / 2;
    window->y =
        anchor.y +
        (anchor.height - height) / 2;
    window->width = width;
    window->height = height;

    wlr_scene_node_set_position(
        &window->scene_tree->node,
        window->x,
        window->y
    );

    wlr_scene_node_raise_to_top(
        &window->scene_tree->node
    );

    if (window->toplevel->base->initialized) {
        wlr_xdg_toplevel_set_size(
            window->toplevel,
            width,
            height
        );
    }
}

void onyrion_window_reflow_transients(
        OnyrionWindow *parent) {
    if (!parent ||
            !parent->server) {
        return;
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &parent->server->windows,
            link) {
        if (window->placement !=
                    ONYRION_WINDOW_PLACEMENT_TRANSIENT ||
                window->toplevel->parent !=
                    parent->toplevel) {
            continue;
        }

        apply_transient_geometry(
            window
        );

        onyrion_window_reflow_transients(
            window
        );
    }
}

OnyrionWindow *onyrion_window_focused(
        struct onyrion_server *server) {
    if (!server) {
        return NULL;
    }

    struct wlr_surface *focused =
        server->seat
            ? server->seat->keyboard_state.focused_surface
            : NULL;

    if (focused) {
        OnyrionWindow *window;

        wl_list_for_each(
                window,
                &server->windows,
                link) {
            if (window->mapped &&
                    window->toplevel &&
                    window->toplevel->base &&
                    window->toplevel->base->surface == focused) {
                return window;
            }
        }
    }

    return
        server->active_group
            ? server->active_group->active
            : NULL;
}

bool onyrion_window_focus(
        OnyrionWindow *window) {
    if (!window ||
            !window->mapped) {
        return false;
    }

    if (window->group) {
        onyrion_group_focus_window(
            window
        );

        return
            window->group->active ==
                window;
    }

    if ((window->placement !=
                ONYRION_WINDOW_PLACEMENT_TRANSIENT &&
            window->placement !=
                ONYRION_WINDOW_PLACEMENT_FLOATING) ||
            !window->workspace ||
            !onyrion_workspace_is_visible(
                window->workspace)) {
        return false;
    }

    onyrion_output_focus(
        window->server,
        window->workspace->output
    );

    OnyrionWindow *tiled_active =
        window->server->active_group
            ? window->server->active_group->active
            : NULL;

    if (tiled_active &&
            tiled_active != window &&
            tiled_active->mapped) {
        wlr_xdg_toplevel_set_activated(
            tiled_active->toplevel,
            false
        );
    }

    OnyrionWindow *parent =
        managed_parent(window);

    if (parent &&
            parent != tiled_active &&
            parent->mapped) {
        wlr_xdg_toplevel_set_activated(
            parent->toplevel,
            false
        );
    }

    wlr_scene_node_set_enabled(
        &window->scene_tree->node,
        true
    );

    wlr_scene_node_raise_to_top(
        &window->scene_tree->node
    );

    wlr_xdg_toplevel_set_activated(
        window->toplevel,
        true
    );

    onyrion_input_focus_window(
        window->server,
        window
    );

    onyrion_shell_protocol_mark_changed(
        window->server
    );

    return true;
}

bool onyrion_window_focus_id(
        OnyrionServer *server,
        uint64_t id) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            id
        );

    if (!window ||
            !window->workspace) {
        return false;
    }

    if (!onyrion_workspace_activate_id(
            server,
            window->workspace->id)) {
        return false;
    }

    return onyrion_window_focus(
        window
    );
}

static void make_transient(
        OnyrionWindow *window,
        OnyrionWindow *parent) {
    if (!window ||
            !parent ||
            !parent->workspace ||
            window == parent) {
        return;
    }

    if (window->group) {
        onyrion_group_remove_window(
            window
        );
    }

    window->placement =
        ONYRION_WINDOW_PLACEMENT_TRANSIENT;

    reparent_to_workspace(
        window,
        parent->workspace
    );

    apply_transient_geometry(
        window
    );

    if (window->mapped) {
        wlr_scene_node_set_enabled(
            &window->scene_tree->node,
            true
        );

        (void)onyrion_window_focus(
            window
        );
    }

    wlr_log(
        WLR_INFO,
        "Window transient relation: child=%" PRIu64
        " parent=%" PRIu64
        " workspace=%" PRIu64,
        window->id,
        parent->id,
        parent->workspace->id
    );

    onyrion_shell_protocol_mark_changed(
        window->server
    );
}

static bool make_tiled(
        OnyrionWindow *window) {
    if (!window ||
            window->group) {
        return false;
    }

    OnyrionWorkspace *workspace =
        window->workspace
            ? window->workspace
            : onyrion_workspace_focused(
                window->server
            );

    if (!workspace) {
        return false;
    }

    OnyrionGroup *group =
        workspace->active_group;

    if (!group) {
        group =
            onyrion_group_create_in_workspace(
                window->server,
                workspace,
                ONYRION_SPLIT_HORIZONTAL
            );
    }

    if (!group) {
        return false;
    }

    onyrion_group_add_window(
        group,
        window
    );

    if (window->mapped) {
        onyrion_group_window_mapped(
            window
        );
    }

    onyrion_shell_protocol_mark_changed(
        window->server
    );

    return
        window->group == group &&
        window->placement ==
            ONYRION_WINDOW_PLACEMENT_TILED;
}

static bool make_floating(
        OnyrionWindow *window) {
    if (!window ||
            !window->group ||
            window->placement !=
                ONYRION_WINDOW_PLACEMENT_TILED ||
            window->fullscreen) {
        return false;
    }

    struct wlr_box anchor = {0};

    if (!floating_box(
            window,
            &anchor)) {
        return false;
    }

    OnyrionWorkspace *workspace =
        window->workspace;

    onyrion_group_remove_window(
        window
    );

    window->placement =
        ONYRION_WINDOW_PLACEMENT_FLOATING;

    reparent_to_workspace(
        window,
        workspace
    );

    if (!apply_floating_geometry(
            window)) {
        return false;
    }

    if (window->mapped) {
        wlr_scene_node_set_enabled(
            &window->scene_tree->node,
            true
        );

        if (onyrion_workspace_is_visible(
                workspace)) {
            (void)onyrion_window_focus(
                window
            );
        }
    }

    onyrion_shell_protocol_mark_changed(
        window->server
    );

    return
        window->placement ==
            ONYRION_WINDOW_PLACEMENT_FLOATING &&
        window->group == NULL;
}

bool onyrion_window_float_id(
        OnyrionServer *server,
        uint64_t id) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            id
        );

    if (!window ||
            !window->mapped ||
            !window->group ||
            window->placement !=
                ONYRION_WINDOW_PLACEMENT_TILED ||
            window->fullscreen) {
        return false;
    }

    if (!make_floating(
            window)) {
        return false;
    }

    wlr_log(
        WLR_INFO,
        "Window floated: id=%" PRIu64
        " workspace=%" PRIu64
        " geometry=%dx%d+%d+%d",
        window->id,
        window->workspace
            ? window->workspace->id
            : 0,
        window->width,
        window->height,
        window->x,
        window->y
    );

    return true;
}

bool onyrion_window_tile_id(
        OnyrionServer *server,
        uint64_t id) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            id
        );

    if (!window ||
            !window->mapped ||
            window->placement !=
                ONYRION_WINDOW_PLACEMENT_FLOATING ||
            window->group) {
        return false;
    }

    if (!make_tiled(
            window)) {
        return false;
    }

    wlr_log(
        WLR_INFO,
        "Window tiled: id=%" PRIu64
        " workspace=%" PRIu64
        " group=%" PRIu64,
        window->id,
        window->workspace
            ? window->workspace->id
            : 0,
        window->group
            ? window->group->id
            : 0
    );

    return true;
}

bool onyrion_window_set_fullscreen(
        OnyrionWindow *window,
        bool fullscreen) {
    if (!window ||
            !window->toplevel) {
        return false;
    }

    const bool changed =
        window->fullscreen != fullscreen;

    window->fullscreen =
        fullscreen;

    if (window->toplevel->base->initialized) {
        wlr_xdg_toplevel_set_fullscreen(
            window->toplevel,
            fullscreen
        );
    }

    if (window->group &&
            window->group->active == window &&
            window->mapped) {
        onyrion_layout_apply_group(
            window->group
        );
    }

    if (changed) {
        wlr_log(
            WLR_INFO,
            "Window fullscreen: %s title=%s",
            fullscreen
                ? "enabled"
                : "disabled",
            onyrion_window_title(window)
        );

        onyrion_shell_protocol_mark_changed(
            window->server
        );
    }

    return changed;
}

bool onyrion_window_toggle_fullscreen_active(
        struct onyrion_server *server) {
    if (!server ||
            !server->active_group ||
            !server->active_group->active) {
        return false;
    }

    OnyrionWindow *window =
        server->active_group->active;

    if (!window->mapped) {
        return false;
    }

    return onyrion_window_set_fullscreen(
        window,
        !window->fullscreen
    );
}

bool onyrion_window_close_active(
        struct onyrion_server *server) {
    if (!server ||
            !server->active_group ||
            !server->active_group->active) {
        return false;
    }

    OnyrionWindow *window =
        server->active_group->active;

    if (!window->mapped ||
            !window->toplevel) {
        return false;
    }

    wlr_xdg_toplevel_send_close(
        window->toplevel
    );

    wlr_log(
        WLR_INFO,
        "Window close requested: %s",
        onyrion_window_title(window)
    );

    return true;
}


static void destroy_popup(
        OnyrionXdgPopup *popup) {
    OnyrionXdgPopup *child;
    OnyrionXdgPopup *tmp;

    wl_list_for_each_safe(
            child,
            tmp,
            &popup->children,
            parent_link) {
        destroy_popup(child);
    }

    if (popup->new_popup.notify) {
        wl_list_remove(&popup->new_popup.link);
        popup->new_popup.notify = NULL;
    }

    if (popup->commit.notify) {
        wl_list_remove(&popup->commit.link);
        popup->commit.notify = NULL;
    }

    if (popup->reposition.notify) {
        wl_list_remove(&popup->reposition.link);
        popup->reposition.notify = NULL;
    }

    if (popup->destroy.notify) {
        wl_list_remove(&popup->destroy.link);
        popup->destroy.notify = NULL;
    }

    if (popup->scene_tree) {
        wlr_scene_node_destroy(
            &popup->scene_tree->node
        );

        popup->scene_tree = NULL;
    }

    wl_list_remove(&popup->parent_link);

    free(popup);
}

static bool popup_constraint_box(
        OnyrionWindow *window,
        struct wlr_box *box) {
    int window_lx = 0;
    int window_ly = 0;

    if (!wlr_scene_node_coords(
            &window->scene_tree->node,
            &window_lx,
            &window_ly)) {
        return false;
    }

    /*
     * wlroots 0.20 exposes the root xdg-surface geometry
     * directly as wlr_xdg_surface.geometry.
     */
    const struct wlr_box geometry =
        window->toplevel->base->geometry;

    box->x =
        window->server->usable_x -
        window_lx +
        geometry.x;

    box->y =
        window->server->usable_y -
        window_ly +
        geometry.y;

    box->width =
        window->server->usable_width;

    box->height =
        window->server->usable_height;

    return
        box->width > 0 &&
        box->height > 0;
}

static OnyrionXdgPopup *create_popup(
        OnyrionWindow *window,
        OnyrionXdgPopup *parent,
        struct wlr_scene_tree *parent_tree,
        struct wlr_xdg_popup *wlr_popup) {
    OnyrionXdgPopup *popup =
        calloc(1, sizeof(*popup));

    if (!popup) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate xdg popup"
        );

        return NULL;
    }

    popup->window = window;
    popup->popup = wlr_popup;
    popup->parent = parent;

    wl_list_init(&popup->children);

    popup->scene_tree =
        wlr_scene_xdg_surface_create(
            parent_tree,
            wlr_popup->base
        );

    if (!popup->scene_tree) {
        wlr_log(
            WLR_ERROR,
            "Failed to create xdg popup scene tree"
        );

        free(popup);
        return NULL;
    }

    popup->new_popup.notify =
        handle_popup_new_popup;

    wl_signal_add(
        &wlr_popup->base->events.new_popup,
        &popup->new_popup
    );

    popup->commit.notify =
        handle_popup_commit;

    wl_signal_add(
        &wlr_popup->base->surface->events.commit,
        &popup->commit
    );

    popup->reposition.notify =
        handle_popup_reposition;

    wl_signal_add(
        &wlr_popup->events.reposition,
        &popup->reposition
    );

    popup->destroy.notify =
        handle_popup_destroy;

    wl_signal_add(
        &wlr_popup->events.destroy,
        &popup->destroy
    );

    wl_list_insert(
        parent
            ? &parent->children
            : &window->popups,
        &popup->parent_link
    );

    wlr_log(
        WLR_INFO,
        "New xdg_popup"
    );

    return popup;
}

static bool configure_popup(
        OnyrionXdgPopup *popup) {
    struct wlr_box constraint;

    if (!popup_constraint_box(
            popup->window,
            &constraint)) {
        wlr_log(
            WLR_ERROR,
            "Failed to resolve xdg popup constraint box"
        );

        return false;
    }

    wlr_xdg_popup_unconstrain_from_box(
        popup->popup,
        &constraint
    );

    return true;
}

static void handle_popup_commit(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionXdgPopup *popup =
        wl_container_of(
            listener,
            popup,
            commit
        );

    /*
     * wlroots >= 0.18 no longer sends the initial
     * xdg_surface.configure automatically. The compositor
     * must wait for the popup's initial wl_surface.commit,
     * then schedule/configure it.
     */
    if (!popup->popup->base->initial_commit) {
        return;
    }

    if (!configure_popup(popup)) {
        wlr_log(
            WLR_ERROR,
            "Failed to configure initial xdg popup"
        );
    }
}

static void handle_popup_reposition(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionXdgPopup *popup =
        wl_container_of(
            listener,
            popup,
            reposition
        );

    if (!popup->popup->base->initialized) {
        return;
    }

    if (!configure_popup(popup)) {
        wlr_log(
            WLR_ERROR,
            "Failed to reconfigure xdg popup"
        );
    }
}

static void handle_popup_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionXdgPopup *popup =
        wl_container_of(
            listener,
            popup,
            destroy
        );

    destroy_popup(popup);
}

static void handle_popup_new_popup(
        struct wl_listener *listener,
        void *data) {
    OnyrionXdgPopup *parent =
        wl_container_of(
            listener,
            parent,
            new_popup
        );

    struct wlr_xdg_popup *wlr_popup = data;

    if (!create_popup(
            parent->window,
            parent,
            parent->scene_tree,
            wlr_popup)) {
        wlr_xdg_popup_destroy(wlr_popup);
    }
}

static void handle_window_new_popup(
        struct wl_listener *listener,
        void *data) {
    OnyrionWindow *window =
        wl_container_of(
            listener,
            window,
            new_popup
        );

    struct wlr_xdg_popup *wlr_popup = data;

    if (!create_popup(
            window,
            NULL,
            window->scene_tree,
            wlr_popup)) {
        wlr_xdg_popup_destroy(wlr_popup);
    }
}

static void destroy_window(OnyrionWindow *window) {
    if (window->new_popup.notify) {
        wl_list_remove(&window->new_popup.link);
        window->new_popup.notify = NULL;
    }

    OnyrionXdgPopup *popup;
    OnyrionXdgPopup *popup_tmp;

    wl_list_for_each_safe(
            popup,
            popup_tmp,
            &window->popups,
            parent_link) {
        destroy_popup(popup);
    }

    if (window->commit.notify) {
        wl_list_remove(&window->commit.link);
        window->commit.notify = NULL;
    }

    if (window->map.notify) {
        wl_list_remove(&window->map.link);
        window->map.notify = NULL;
    }

    if (window->unmap.notify) {
        wl_list_remove(&window->unmap.link);
        window->unmap.notify = NULL;
    }

    if (window->destroy.notify) {
        wl_list_remove(&window->destroy.link);
        window->destroy.notify = NULL;
    }

    if (window->request_fullscreen.notify) {
        wl_list_remove(
            &window->request_fullscreen.link
        );

        window->request_fullscreen.notify =
            NULL;
    }

    if (window->set_title.notify) {
        wl_list_remove(&window->set_title.link);
        window->set_title.notify = NULL;
    }

    if (window->set_app_id.notify) {
        wl_list_remove(&window->set_app_id.link);
        window->set_app_id.notify = NULL;
    }

    if (window->set_parent.notify) {
        wl_list_remove(&window->set_parent.link);
        window->set_parent.notify = NULL;
    }

    onyrion_fallback_window_unavailable(
        window->server,
        window
    );

    onyrion_group_remove_window(window);

    if (window->scene_tree) {
        wlr_scene_node_destroy(
            &window->scene_tree->node
        );

        window->scene_tree = NULL;
    }

    wl_list_remove(&window->link);

    wlr_log(
        WLR_INFO,
        "Window destroyed: id=%" PRIu64,
        window->id
    );

    free(window);
}

static void handle_window_commit(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(listener, window, commit);

    if (!window->toplevel->base->initial_commit) {
        return;
    }

    if (window->fullscreen) {
        wlr_xdg_toplevel_set_fullscreen(
            window->toplevel,
            true
        );
    }

    if (window->placement ==
            ONYRION_WINDOW_PLACEMENT_TRANSIENT) {
        apply_transient_geometry(
            window
        );

        return;
    }

    if (window->placement ==
            ONYRION_WINDOW_PLACEMENT_FLOATING) {
        (void)apply_floating_geometry(
            window
        );

        return;
    }

    wlr_xdg_toplevel_set_size(
        window->toplevel,
        0,
        0
    );
}

static void apply_window_placement_rule(
        OnyrionWindow *window) {
    if (!window ||
            !window->server ||
            !window->server->policy ||
            window->placement ==
                ONYRION_WINDOW_PLACEMENT_TRANSIENT) {
        return;
    }

    const char *output_name = NULL;

    if (window->workspace &&
            window->workspace->output &&
            window->workspace->output->wlr_output) {
        output_name =
            window->workspace->
                output->wlr_output->name;
    }

    OnyrionWindowRulePlacement placement =
        ONYRION_WINDOW_RULE_PLACEMENT_COUNT;
    size_t rule_index = SIZE_MAX;

    if (!onyrion_core_policy_match_window_rule(
            window->server->policy,
            window->toplevel->app_id,
            window->toplevel->title,
            window->workspace
                ? window->workspace->id
                : 0,
            output_name,
            &placement,
            &rule_index)) {
        return;
    }

    const char *placement_name =
        placement ==
                ONYRION_WINDOW_RULE_PLACEMENT_FLOATING
            ? "floating"
            : "tiled";

    if (placement ==
            ONYRION_WINDOW_RULE_PLACEMENT_FLOATING &&
            window->placement ==
                ONYRION_WINDOW_PLACEMENT_TILED) {
        if (!make_floating(
                window)) {
            wlr_log(
                WLR_ERROR,
                "Window placement rule failed:"
                " id=%" PRIu64
                " rule=%zu placement=%s",
                window->id,
                rule_index,
                placement_name
            );

            return;
        }
    }

    wlr_log(
        WLR_INFO,
        "Window placement rule matched:"
        " id=%" PRIu64
        " rule=%zu placement=%s"
        " app_id=%s title=%s"
        " workspace=%" PRIu64
        " output=%s",
        window->id,
        rule_index,
        placement_name,
        window->toplevel->app_id
            ? window->toplevel->app_id
            : "<unset>",
        window->toplevel->title
            ? window->toplevel->title
            : "<unset>",
        window->workspace
            ? window->workspace->id
            : 0,
        output_name
            ? output_name
            : "<none>"
    );
}

static void handle_window_map(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(listener, window, map);

    apply_window_placement_rule(
        window
    );

    if (window->placement ==
            ONYRION_WINDOW_PLACEMENT_TRANSIENT ||
            window->placement ==
                ONYRION_WINDOW_PLACEMENT_FLOATING) {
        window->mapped = true;

        wlr_scene_node_set_enabled(
            &window->scene_tree->node,
            true
        );

        if (window->placement ==
                ONYRION_WINDOW_PLACEMENT_TRANSIENT) {
            apply_transient_geometry(
                window
            );
        } else {
            (void)apply_floating_geometry(
                window
            );
        }

        (void)onyrion_window_focus(
            window
        );
    } else {
        onyrion_group_window_mapped(
            window
        );
    }

    onyrion_fallback_window_mapped(
        window->server,
        window
    );

    wlr_log(
        WLR_INFO,
        "Window mapped: %s",
        window->toplevel->title
            ? window->toplevel->title
            : "<untitled>"
    );
}

static void handle_window_unmap(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(listener, window, unmap);

    if (window->placement ==
            ONYRION_WINDOW_PLACEMENT_TRANSIENT ||
            window->placement ==
                ONYRION_WINDOW_PLACEMENT_FLOATING) {
        const OnyrionWindowPlacement placement =
            window->placement;

        window->mapped = false;

        wlr_scene_node_set_enabled(
            &window->scene_tree->node,
            false
        );

        wlr_xdg_toplevel_set_activated(
            window->toplevel,
            false
        );

        if (placement ==
                ONYRION_WINDOW_PLACEMENT_TRANSIENT) {
            OnyrionWindow *parent =
                managed_parent(window);

            if (parent &&
                    parent->mapped) {
                (void)onyrion_window_focus(
                    parent
                );
            }
        } else if (window->server->active_group &&
                window->server->active_group->active &&
                window->server->active_group->workspace ==
                    window->workspace) {
            onyrion_group_focus_window(
                window->server->active_group->active
            );
        }
    } else {
        onyrion_group_window_unmapped(
            window
        );
    }

    onyrion_fallback_window_unavailable(
        window->server,
        window
    );

    wlr_log(
        WLR_INFO,
        "Window unmapped"
    );
}

static void handle_window_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(listener, window, destroy);

    destroy_window(window);
}

static void handle_window_set_title(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(listener, window, set_title);

    wlr_log(
        WLR_DEBUG,
        "Window title changed: id=%" PRIu64 " title=%s",
        window->id,
        window->toplevel->title
            ? window->toplevel->title
            : "<unset>"
    );

    onyrion_shell_protocol_mark_changed(
        window->server
    );
}

static void handle_window_set_app_id(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(listener, window, set_app_id);

    wlr_log(
        WLR_DEBUG,
        "Window app_id changed: id=%" PRIu64 " app_id=%s",
        window->id,
        window->toplevel->app_id
            ? window->toplevel->app_id
            : "<unset>"
    );

    onyrion_shell_protocol_mark_changed(
        window->server
    );
}

static void handle_window_set_parent(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(
            listener,
            window,
            set_parent
        );

    OnyrionWindow *parent =
        managed_parent(window);

    if (window->toplevel->parent &&
            !parent) {
        wlr_log(
            WLR_DEBUG,
            "Transient parent not managed: child=%" PRIu64,
            window->id
        );

        return;
    }

    if (parent) {
        make_transient(
            window,
            parent
        );

        return;
    }

    if (window->placement ==
            ONYRION_WINDOW_PLACEMENT_TRANSIENT) {
        make_tiled(
            window
        );
    }
}

static void handle_window_request_fullscreen(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionWindow *window =
        wl_container_of(
            listener,
            window,
            request_fullscreen
        );

    /*
     * Every xdg-shell fullscreen request receives a configure,
     * even when the requested state already matches ours.
     */
    (void)onyrion_window_set_fullscreen(
        window,
        window->toplevel->requested.fullscreen
    );
}

static void handle_request_activate(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            request_activate
        );

    struct wlr_xdg_activation_v1_request_activate_event *event =
        data;

    struct wlr_xdg_toplevel *target =
        wlr_xdg_toplevel_try_from_wlr_surface(
            event->surface
        );

    if (!target) {
        wlr_log(
            WLR_DEBUG,
            "XDG activation ignored: target is not an xdg_toplevel"
        );

        return;
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &server->windows,
            link) {
        if (window->toplevel != target) {
            continue;
        }

        (void)onyrion_window_focus(
            window
        );

        wlr_log(
            WLR_DEBUG,
            "XDG activation accepted: title=%s app_id=%s",
            target->title
                ? target->title
                : "<unset>",
            target->app_id
                ? target->app_id
                : "<unset>"
        );

        return;
    }

    wlr_log(
        WLR_DEBUG,
        "XDG activation ignored: target is not managed"
    );
}

static void handle_new_toplevel(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(listener, server, new_toplevel);

    struct wlr_xdg_toplevel *toplevel = data;

    OnyrionWindow *window =
        calloc(1, sizeof(*window));

    if (!window) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate OnyrionWindow"
        );
        return;
    }

    window->server = server;
    window->toplevel = toplevel;
    window->id =
        server->next_window_id++;
    window->placement =
        ONYRION_WINDOW_PLACEMENT_TILED;

    wl_list_init(&window->popups);

    window->scene_tree =
        wlr_scene_xdg_surface_create(
            &server->scene->tree,
            toplevel->base
        );

    if (!window->scene_tree) {
        wlr_log(
            WLR_ERROR,
            "Failed to create window scene tree"
        );

        free(window);
        return;
    }

    wlr_scene_node_set_enabled(
        &window->scene_tree->node,
        false
    );

    OnyrionWorkspace *workspace =
        onyrion_workspace_focused(server);

    window->workspace =
        workspace;

    OnyrionGroup *group =
        workspace
            ? workspace->active_group
            : NULL;

    if (!group) {
        group =
            onyrion_group_create(
                server,
                ONYRION_SPLIT_HORIZONTAL
            );
    }

    if (!group) {
        wlr_scene_node_destroy(
            &window->scene_tree->node
        );

        free(window);
        return;
    }

    onyrion_group_add_window(
        group,
        window
    );

    window->commit.notify =
        handle_window_commit;

    wl_signal_add(
        &toplevel->base->surface->events.commit,
        &window->commit
    );

    window->map.notify =
        handle_window_map;

    wl_signal_add(
        &toplevel->base->surface->events.map,
        &window->map
    );

    window->unmap.notify =
        handle_window_unmap;

    wl_signal_add(
        &toplevel->base->surface->events.unmap,
        &window->unmap
    );

    window->destroy.notify =
        handle_window_destroy;

    wl_signal_add(
        &toplevel->events.destroy,
        &window->destroy
    );

    window->request_fullscreen.notify =
        handle_window_request_fullscreen;

    wl_signal_add(
        &toplevel->events.request_fullscreen,
        &window->request_fullscreen
    );

    window->set_title.notify =
        handle_window_set_title;

    wl_signal_add(
        &toplevel->events.set_title,
        &window->set_title
    );

    window->set_app_id.notify =
        handle_window_set_app_id;

    wl_signal_add(
        &toplevel->events.set_app_id,
        &window->set_app_id
    );

    window->set_parent.notify =
        handle_window_set_parent;

    wl_signal_add(
        &toplevel->events.set_parent,
        &window->set_parent
    );

    window->new_popup.notify =
        handle_window_new_popup;

    wl_signal_add(
        &toplevel->base->events.new_popup,
        &window->new_popup
    );

    wl_list_insert(
        &server->windows,
        &window->link
    );

    wlr_log(
        WLR_INFO,
        "New xdg_toplevel: id=%" PRIu64
        " group_id=%" PRIu64
        " title=%s app_id=%s",
        window->id,
        window->group->id,
        toplevel->title
            ? toplevel->title
            : "<unset>",
        toplevel->app_id
            ? toplevel->app_id
            : "<unset>"
    );
}

bool onyrion_window_init(struct onyrion_server *server) {
    wl_list_init(&server->windows);
    server->next_window_id = 1;

    server->xdg_activation =
        wlr_xdg_activation_v1_create(
            server->display
        );

    if (!server->xdg_activation) {
        wlr_log(
            WLR_ERROR,
            "Failed to create XDG activation manager"
        );

        return false;
    }

    server->request_activate.notify =
        handle_request_activate;

    wl_signal_add(
        &server->xdg_activation->events.request_activate,
        &server->request_activate
    );

    server->xdg_shell =
        wlr_xdg_shell_create(
            server->display,
            6
        );

    if (!server->xdg_shell) {
        wlr_log(
            WLR_ERROR,
            "Failed to create xdg shell"
        );
        return false;
    }

    server->new_toplevel.notify =
        handle_new_toplevel;

    wl_signal_add(
        &server->xdg_shell->events.new_toplevel,
        &server->new_toplevel
    );

    return true;
}

void onyrion_window_finish(struct onyrion_server *server) {
    if (server->request_activate.notify) {
        wl_list_remove(
            &server->request_activate.link
        );

        server->request_activate.notify = NULL;
    }

    if (server->new_toplevel.notify) {
        wl_list_remove(&server->new_toplevel.link);
        server->new_toplevel.notify = NULL;
    }

    OnyrionWindow *window;
    OnyrionWindow *tmp;

    wl_list_for_each_safe(
            window,
            tmp,
            &server->windows,
            link) {
        destroy_window(window);
    }
}
