#include "session_lock.h"
#include "input.h"
#include "layer_shell.h"
#include "server.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

typedef struct lock_output {
    struct wlr_output *output;
    struct wlr_scene_rect *background;
    bool presented;
    bool secure_commit_pending;
    uint32_t secure_commit_seq;
    struct wl_list link;
} LockOutput;

typedef struct lock_surface {
    struct onyrion_session_lock_controller *controller;
    struct wlr_session_lock_surface_v1 *lock_surface;
    struct wlr_scene_tree *tree;
    bool active;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener tree_destroy;
} LockSurface;

typedef struct onyrion_session_lock_controller {
    OnyrionServer *server;
    struct wlr_session_lock_manager_v1 *manager;
    struct wlr_session_lock_v1 *lock;
    struct wlr_scene_tree *tree;
    struct wl_list outputs;
    struct wl_list surfaces;
    struct wl_listener new_lock;
    struct wl_listener manager_destroy;
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
    bool secure;
    bool locked_sent;
    bool normal_enabled[6];
} LockController;

static LockOutput *find_output(LockController *c, struct wlr_output *output) {
    LockOutput *entry;
    wl_list_for_each(entry, &c->outputs, link) {
        if (entry->output == output) return entry;
    }
    return NULL;
}

static void get_output_box(LockController *c, struct wlr_output *output, struct wlr_box *box) {
    *box = (struct wlr_box){0};
    wlr_output_layout_get_box(c->server->output_layout, output, box);
    if (box->width <= 0 || box->height <= 0) {
        wlr_output_effective_resolution(output, &box->width, &box->height);
    }
}

static void detach_lock_listeners(LockController *c) {
    if (c->new_surface.notify) { wl_list_remove(&c->new_surface.link); c->new_surface.notify = NULL; }
    if (c->unlock.notify) { wl_list_remove(&c->unlock.link); c->unlock.notify = NULL; }
    if (c->destroy.notify) { wl_list_remove(&c->destroy.link); c->destroy.notify = NULL; }
}

static void set_normal_scene(LockController *c, bool enabled) {
    struct wlr_scene_tree *trees[] = {
        c->server->scene_background, c->server->scene_bottom,
        c->server->scene_content, c->server->scene_top,
        c->server->scene_overlay, c->server->drag_icons,
    };
    for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); ++i) {
        if (!trees[i]) continue;
        if (!enabled) c->normal_enabled[i] = trees[i]->node.enabled;
        wlr_scene_node_set_enabled(&trees[i]->node, enabled ? c->normal_enabled[i] : false);
    }
}

static bool surface_is_current(const LockController *c, struct wlr_surface *surface) {
    if (!c || !surface) return false;
    LockSurface *entry;
    wl_list_for_each(entry, &c->surfaces, link) {
        if (entry->active && entry->lock_surface->surface == surface) return true;
    }
    return false;
}

static void focus_first(LockController *c) {
    LockSurface *entry;
    wl_list_for_each(entry, &c->surfaces, link) {
        if (entry->active) {
            onyrion_input_focus_surface(c->server, entry->lock_surface->surface);
            return;
        }
    }
    onyrion_input_focus_surface(c->server, NULL);
}

static void deactivate_surfaces(LockController *c) {
    LockSurface *entry;
    wl_list_for_each(entry, &c->surfaces, link) {
        if (!entry->active) continue;
        entry->active = false;
        if (entry->tree) wlr_scene_node_destroy(&entry->tree->node);
    }
}

static bool all_presented(LockController *c) {
    LockOutput *entry;
    wl_list_for_each(entry, &c->outputs, link) {
        if (!entry->presented) return false;
    }
    return true;
}

static void maybe_send_locked(LockController *c) {
    if (!c->lock || c->locked_sent || !c->secure || !all_presented(c)) return;
    wlr_session_lock_v1_send_locked(c->lock);
    c->locked_sent = true;
    wlr_log(WLR_INFO, "Session lock secure frame presented on all outputs");
}

static void schedule_locked_frames(LockController *c) {
    LockOutput *entry;
    wl_list_for_each(entry, &c->outputs, link) {
        entry->presented = false;
        entry->secure_commit_pending = false;
        wlr_output_schedule_frame(entry->output);
    }
    maybe_send_locked(c);
}

static void enter_secure(LockController *c) {
    if (!c->secure) {
        set_normal_scene(c, false);
        wlr_scene_node_set_enabled(&c->tree->node, true);
        wlr_scene_node_raise_to_top(&c->tree->node);
        c->secure = true;
        onyrion_input_clear_pointer_focus(c->server);
        onyrion_input_focus_surface(c->server, NULL);
    }
    c->locked_sent = false;
    schedule_locked_frames(c);
}

static void leave_secure(LockController *c) {
    if (!c->secure) return;
    deactivate_surfaces(c);
    wlr_scene_node_set_enabled(&c->tree->node, false);
    set_normal_scene(c, true);
    c->secure = false;
    c->locked_sent = false;
    onyrion_input_clear_pointer_focus(c->server);
    onyrion_layer_shell_refresh_keyboard_focus(c->server);
    wlr_log(WLR_INFO, "Session unlocked");
}

static void handle_tree_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    LockSurface *surface = wl_container_of(listener, surface, tree_destroy);
    surface->tree = NULL;
    wl_list_remove(&surface->tree_destroy.link);
    surface->tree_destroy.notify = NULL;
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    LockSurface *surface = wl_container_of(listener, surface, destroy);
    LockController *c = surface->controller;
    if (c->server->seat && c->server->seat->keyboard_state.focused_surface == surface->lock_surface->surface) {
        onyrion_input_focus_surface(c->server, NULL);
    }
    surface->active = false;
    if (surface->tree) wlr_scene_node_destroy(&surface->tree->node);
    wl_list_remove(&surface->destroy.link);
    surface->destroy.notify = NULL;
    wl_list_remove(&surface->link);
    free(surface);
    if (c->secure) focus_first(c);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
    LockController *c = wl_container_of(listener, c, new_surface);
    struct wlr_session_lock_surface_v1 *lock_surface = data;
    struct wlr_box box;
    get_output_box(c, lock_surface->output, &box);
    if (box.width <= 0 || box.height <= 0) {
        wlr_log(WLR_ERROR, "Session lock surface has invalid output geometry");
        return;
    }

    LockSurface *surface = calloc(1, sizeof(*surface));
    if (!surface) return;
    surface->controller = c;
    surface->lock_surface = lock_surface;
    surface->active = true;

    (void)wlr_session_lock_surface_v1_configure(lock_surface, (uint32_t)box.width, (uint32_t)box.height);
    surface->tree = wlr_scene_subsurface_tree_create(c->tree, lock_surface->surface);
    if (!surface->tree) { free(surface); return; }
    wlr_scene_node_set_position(&surface->tree->node, box.x, box.y);

    surface->tree_destroy.notify = handle_tree_destroy;
    wl_signal_add(&surface->tree->node.events.destroy, &surface->tree_destroy);
    surface->destroy.notify = handle_surface_destroy;
    wl_signal_add(&lock_surface->events.destroy, &surface->destroy);
    wl_list_insert(c->surfaces.prev, &surface->link);

    if (c->secure && c->server->seat &&
            !surface_is_current(c, c->server->seat->keyboard_state.focused_surface)) {
        onyrion_input_focus_surface(c->server, lock_surface->surface);
    }
    wlr_output_schedule_frame(lock_surface->output);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
    (void)data;
    LockController *c = wl_container_of(listener, c, unlock);
    if (c->locked_sent) leave_secure(c);
}

static void handle_lock_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    LockController *c = wl_container_of(listener, c, destroy);
    bool was_locked = c->locked_sent;
    detach_lock_listeners(c);
    c->lock = NULL;
    c->locked_sent = false;
    deactivate_surfaces(c);
    if (!was_locked) {
        leave_secure(c);
        return;
    }
    onyrion_input_clear_pointer_focus(c->server);
    onyrion_input_focus_surface(c->server, NULL);
    schedule_locked_frames(c);
    wlr_log(WLR_ERROR, "Session locker disappeared; keeping session securely locked");
}

static void attach_lock(LockController *c, struct wlr_session_lock_v1 *lock) {
    c->lock = lock;
    lock->data = c;
    c->new_surface.notify = handle_new_surface;
    wl_signal_add(&lock->events.new_surface, &c->new_surface);
    c->unlock.notify = handle_unlock;
    wl_signal_add(&lock->events.unlock, &c->unlock);
    c->destroy.notify = handle_lock_destroy;
    wl_signal_add(&lock->events.destroy, &c->destroy);
    enter_secure(c);
}

static void handle_new_lock(struct wl_listener *listener, void *data) {
    LockController *c = wl_container_of(listener, c, new_lock);
    struct wlr_session_lock_v1 *lock = data;
    if (c->lock) {
        wlr_log(WLR_ERROR, "Rejecting concurrent session lock request");
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    attach_lock(c, lock);
    wlr_log(WLR_INFO, "Session lock request accepted");
}

static void handle_manager_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    LockController *c = wl_container_of(listener, c, manager_destroy);
    if (c->new_lock.notify) { wl_list_remove(&c->new_lock.link); c->new_lock.notify = NULL; }
    wl_list_remove(&c->manager_destroy.link);
    c->manager_destroy.notify = NULL;
    c->manager = NULL;
}

bool onyrion_session_lock_init(struct onyrion_server *server) {
    if (!server || !server->display || !server->scene || !server->seat) return false;
    LockController *c = calloc(1, sizeof(*c));
    if (!c) return false;
    c->server = server;
    wl_list_init(&c->outputs);
    wl_list_init(&c->surfaces);
    c->tree = wlr_scene_tree_create(&server->scene->tree);
    if (!c->tree) { free(c); return false; }
    wlr_scene_node_set_enabled(&c->tree->node, false);
    c->manager = wlr_session_lock_manager_v1_create(server->display);
    if (!c->manager) { wlr_scene_node_destroy(&c->tree->node); free(c); return false; }
    c->new_lock.notify = handle_new_lock;
    wl_signal_add(&c->manager->events.new_lock, &c->new_lock);
    c->manager_destroy.notify = handle_manager_destroy;
    wl_signal_add(&c->manager->events.destroy, &c->manager_destroy);
    server->session_lock = c;
    return true;
}

void onyrion_session_lock_finish(struct onyrion_server *server) {
    if (!server || !server->session_lock) return;
    LockController *c = server->session_lock;
    detach_lock_listeners(c);
    if (c->new_lock.notify) wl_list_remove(&c->new_lock.link);
    if (c->manager_destroy.notify) wl_list_remove(&c->manager_destroy.link);

    LockSurface *surface, *surface_tmp;
    wl_list_for_each_safe(surface, surface_tmp, &c->surfaces, link) {
        if (surface->destroy.notify) wl_list_remove(&surface->destroy.link);
        if (surface->tree) wlr_scene_node_destroy(&surface->tree->node);
        wl_list_remove(&surface->link);
        free(surface);
    }
    LockOutput *output, *output_tmp;
    wl_list_for_each_safe(output, output_tmp, &c->outputs, link) {
        if (output->background) wlr_scene_node_destroy(&output->background->node);
        wl_list_remove(&output->link);
        free(output);
    }
    if (c->tree) wlr_scene_node_destroy(&c->tree->node);
    free(c);
    server->session_lock = NULL;
}

bool onyrion_session_lock_active(const struct onyrion_server *server) {
    return server && server->session_lock && server->session_lock->secure;
}

bool onyrion_session_lock_surface_allowed(const struct onyrion_server *server, struct wlr_surface *surface) {
    if (!onyrion_session_lock_active(server)) return true;
    return surface_is_current(server->session_lock, surface);
}

bool onyrion_session_lock_focus_at_cursor(struct onyrion_server *server) {
    if (!onyrion_session_lock_active(server) || !server->cursor) return false;
    double sx, sy;
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, server->cursor->x, server->cursor->y, &sx, &sy);
    (void)sx; (void)sy;
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return false;
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!scene_surface || !surface_is_current(server->session_lock, scene_surface->surface)) return false;
    onyrion_input_focus_surface(server, scene_surface->surface);
    return true;
}

void onyrion_session_lock_output_add(struct onyrion_server *server, struct wlr_output *output) {
    if (!server || !server->session_lock || !output) return;
    LockController *c = server->session_lock;
    if (find_output(c, output)) return;
    struct wlr_box box;
    get_output_box(c, output, &box);
    if (box.width <= 0 || box.height <= 0) return;
    LockOutput *entry = calloc(1, sizeof(*entry));
    if (!entry) return;
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    entry->output = output;
    entry->background = wlr_scene_rect_create(c->tree, box.width, box.height, black);
    if (!entry->background) { free(entry); return; }
    wlr_scene_node_set_position(&entry->background->node, box.x, box.y);
    entry->presented = !c->secure;
    entry->secure_commit_pending = false;
    wl_list_insert(c->outputs.prev, &entry->link);
    if (c->secure) { wlr_scene_node_raise_to_top(&c->tree->node); wlr_output_schedule_frame(output); }
}

void onyrion_session_lock_output_remove(struct onyrion_server *server, struct wlr_output *output) {
    if (!server || !server->session_lock || !output) return;
    LockController *c = server->session_lock;
    LockOutput *entry = find_output(c, output);
    if (!entry) return;
    if (entry->background) wlr_scene_node_destroy(&entry->background->node);
    wl_list_remove(&entry->link);
    free(entry);
    maybe_send_locked(c);
}

void onyrion_session_lock_output_commit_pending(
        struct onyrion_server *server,
        struct wlr_output *output,
        uint32_t commit_seq) {
    if (!onyrion_session_lock_active(server) || !output) return;

    LockController *c = server->session_lock;
    LockOutput *entry = find_output(c, output);

    if (!entry || entry->presented || entry->secure_commit_pending) return;

    entry->secure_commit_seq = commit_seq;
    entry->secure_commit_pending = true;
}

void onyrion_session_lock_output_commit_failed(
        struct onyrion_server *server,
        struct wlr_output *output,
        uint32_t commit_seq) {
    if (!onyrion_session_lock_active(server) || !output) return;

    LockController *c = server->session_lock;
    LockOutput *entry = find_output(c, output);

    if (!entry || !entry->secure_commit_pending ||
            entry->secure_commit_seq != commit_seq) {
        return;
    }

    entry->secure_commit_pending = false;
}

void onyrion_session_lock_output_presented(
        struct onyrion_server *server,
        struct wlr_output *output,
        uint32_t commit_seq,
        bool presented) {
    if (!onyrion_session_lock_active(server) || !output || !presented) return;

    LockController *c = server->session_lock;
    LockOutput *entry = find_output(c, output);

    if (!entry || entry->presented || !entry->secure_commit_pending) return;

    if ((int32_t)(commit_seq - entry->secure_commit_seq) < 0) {
        return;
    }

    entry->presented = true;
    entry->secure_commit_pending = false;
    maybe_send_locked(c);
}
