#include "group.h"
#include "group_surface.h"
#include "input.h"
#include "layer_shell.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "shell_protocol.h"
#include "window.h"
#include "workspace.h"

#include <inttypes.h>
#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static OnyrionWindow *find_last_mapped_window(
        OnyrionGroup *group) {
    OnyrionWindow *window;
    OnyrionWindow *result = NULL;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        if (window->mapped) {
            result = window;
        }
    }

    return result;
}

static OnyrionGroup *find_last_group_in_workspace(
        OnyrionServer *server,
        OnyrionWorkspace *workspace) {
    OnyrionGroup *group;
    OnyrionGroup *result = NULL;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (group->workspace == workspace) {
            result = group;
        }
    }

    return result;
}

OnyrionGroup *onyrion_group_find_id(
        OnyrionServer *server,
        uint64_t id) {
    if (!server ||
            id == 0) {
        return NULL;
    }

    OnyrionGroup *group;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (group->id == id) {
            return group;
        }
    }

    return NULL;
}

static OnyrionWindow *find_window_by_id(
        OnyrionServer *server,
        uint64_t id) {
    if (!server ||
            id == 0) {
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

static OnyrionWindow *find_adjacent_mapped_window(
        OnyrionGroup *group,
        bool forward) {
    if (!group ||
            !group->active ||
            group->window_count < 2) {
        return NULL;
    }

    struct wl_list *cursor =
        &group->active->group_link;

    for (size_t i = 0;
            i < group->window_count;
            i++) {
        cursor =
            forward
                ? cursor->next
                : cursor->prev;

        if (cursor == &group->windows) {
            cursor =
                forward
                    ? cursor->next
                    : cursor->prev;
        }

        OnyrionWindow *candidate =
            wl_container_of(
                cursor,
                candidate,
                group_link
            );

        if (candidate != group->active &&
                candidate->mapped) {
            return candidate;
        }
    }

    return NULL;
}

static size_t mapped_window_count(
        const OnyrionGroup *group) {
    if (!group) {
        return 0;
    }

    size_t count = 0;
    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        if (window->mapped) {
            count++;
        }
    }

    return count;
}

bool onyrion_group_box(
        const OnyrionGroup *group,
        struct wlr_box *box) {
    if (!group || !box) {
        return false;
    }

    if (group->placement ==
            ONYRION_GROUP_PLACEMENT_TILED) {
        if (!group->tile ||
                group->tile->width <= 0 ||
                group->tile->height <= 0) {
            return false;
        }

        *box = (struct wlr_box){
            .x = group->tile->x,
            .y = group->tile->y,
            .width = group->tile->width,
            .height = group->tile->height,
        };

        return true;
    }

    if (group->placement ==
            ONYRION_GROUP_PLACEMENT_FLOATING &&
            group->floating_width > 0 &&
            group->floating_height > 0) {
        *box = (struct wlr_box){
            .x = group->floating_x,
            .y = group->floating_y,
            .width = group->floating_width,
            .height = group->floating_height,
        };

        return true;
    }

    return false;
}

static void effective_content_insets(
        const OnyrionGroup *group,
        int *top,
        int *right,
        int *bottom,
        int *left) {
    *top = 0;
    *right = 0;
    *bottom = 0;
    *left = 0;

    if (!group) {
        return;
    }

    if (group->shell_content_insets_set) {
        *top = group->content_inset_top;
        *right = group->content_inset_right;
        *bottom = group->content_inset_bottom;
        *left = group->content_inset_left;
        return;
    }

    *top = ONYRION_GROUP_CHROME_HEIGHT;
}

void onyrion_group_content_overhead(
        const OnyrionGroup *group,
        int *width,
        int *height) {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;

    effective_content_insets(
        group,
        &top,
        &right,
        &bottom,
        &left
    );

    if (width) {
        *width = left + right;
    }

    if (height) {
        *height = top + bottom;
    }
}

bool onyrion_group_content_box(
        const OnyrionGroup *group,
        struct wlr_box *box) {
    struct wlr_box outer = {0};

    if (!box || !onyrion_group_box(group, &outer) ||
            outer.width <= 0 || outer.height <= 0) {
        return false;
    }

    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;

    effective_content_insets(
        group,
        &top,
        &right,
        &bottom,
        &left
    );

    if (left < 0) left = 0;
    if (right < 0) right = 0;
    if (top < 0) top = 0;
    if (bottom < 0) bottom = 0;

    if (left >= outer.width) left = outer.width - 1;
    int remaining_width = outer.width - left;
    if (right >= remaining_width) right = remaining_width - 1;

    if (top >= outer.height) top = outer.height - 1;
    int remaining_height = outer.height - top;
    if (bottom >= remaining_height) bottom = remaining_height - 1;

    *box = (struct wlr_box){
        .x = outer.x + left,
        .y = outer.y + top,
        .width = outer.width - left - right,
        .height = outer.height - top - bottom,
    };

    return box->width > 0 && box->height > 0;
}

bool onyrion_group_set_content_insets(
        OnyrionGroup *group,
        struct wl_resource *owner_resource,
        int top,
        int right,
        int bottom,
        int left) {
    if (!group || !owner_resource ||
            top < 0 || right < 0 || bottom < 0 || left < 0) {
        return false;
    }

    group->shell_content_insets_set = true;
    group->content_insets_owner = owner_resource;
    group->content_inset_top = top;
    group->content_inset_right = right;
    group->content_inset_bottom = bottom;
    group->content_inset_left = left;

    onyrion_group_apply_geometry(group);
    return true;
}

static bool floating_default_box(
        const OnyrionGroup *group,
        struct wlr_box *box) {
    if (!group ||
            !group->workspace ||
            !box) {
        return false;
    }

    struct wlr_box usable = {0};

    onyrion_layer_shell_get_output_usable_box(
        group->server,
        group->workspace->output
            ? group->workspace->output->wlr_output
            : NULL,
        &usable
    );

    if (usable.width <= 0 ||
            usable.height <= 0) {
        return false;
    }

    int width = usable.width * 2 / 3;
    int height = usable.height * 2 / 3;

    if (width <= 0) {
        width = usable.width;
    }

    if (height <= 0) {
        height = usable.height;
    }

    *box = (struct wlr_box){
        .x = usable.x + (usable.width - width) / 2,
        .y = usable.y + (usable.height - height) / 2,
        .width = width,
        .height = height,
    };

    return true;
}

void onyrion_group_apply_geometry(
        OnyrionGroup *group) {
    if (!group) {
        return;
    }

    if (group->placement ==
            ONYRION_GROUP_PLACEMENT_TILED) {
        onyrion_layout_apply_group(group);
        return;
    }

    if (group->placement !=
                ONYRION_GROUP_PLACEMENT_FLOATING ||
            !group->active ||
            !group->active->scene_tree ||
            group->floating_width <= 0 ||
            group->floating_height <= 0) {
        onyrion_group_refresh_chrome(group);
        return;
    }

    OnyrionWindow *window = group->active;

    if (window->fullscreen) {
        struct wlr_box box = {0};

        wlr_output_layout_get_box(
            group->server->output_layout,
            group->workspace &&
                    group->workspace->output
                ? group->workspace->output->wlr_output
                : NULL,
            &box
        );

        if (box.width > 0 &&
                box.height > 0) {
            window->x = box.x;
            window->y = box.y;
            window->width = box.width;
            window->height = box.height;

            wlr_scene_node_set_position(
                &window->scene_tree->node,
                box.x,
                box.y
            );

            wlr_scene_node_raise_to_top(
                &window->scene_tree->node
            );

            if (window->toplevel->base->initialized) {
                wlr_xdg_toplevel_set_size(
                    window->toplevel,
                    box.width,
                    box.height
                );
            }

            onyrion_window_reflow_transients(window);
            onyrion_group_refresh_chrome(group);
            return;
        }
    }

    struct wlr_box content = {0};

    if (!onyrion_group_content_box(group, &content)) {
        onyrion_group_refresh_chrome(group);
        return;
    }

    window->x = content.x;
    window->y = content.y;
    window->width = content.width;
    window->height = content.height;

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
            window->width,
            window->height
        );
    }

    onyrion_window_reflow_transients(window);
    onyrion_group_refresh_chrome(group);
}

static int chrome_height(
        const OnyrionGroup *group) {
    struct wlr_box box = {0};

    if (!onyrion_group_box(group, &box) ||
            box.height <= 1) {
        return 0;
    }

    const int maximum =
        box.height - 1;

    return
        ONYRION_GROUP_CHROME_HEIGHT < maximum
            ? ONYRION_GROUP_CHROME_HEIGHT
            : maximum;
}

static int chrome_handle_width(
        const OnyrionGroup *group) {
    struct wlr_box box = {0};

    if (!onyrion_group_box(group, &box) ||
            box.width <= 1) {
        return 0;
    }

    const int maximum =
        box.width - 1;

    return
        ONYRION_GROUP_HANDLE_WIDTH < maximum
            ? ONYRION_GROUP_HANDLE_WIDTH
            : maximum;
}

static void destroy_chrome(
        OnyrionGroup *group) {
    if (!group ||
            !group->chrome_tree) {
        return;
    }

    wlr_scene_node_destroy(
        &group->chrome_tree->node
    );

    group->chrome_tree = NULL;
}

void onyrion_group_refresh_chrome(
        OnyrionGroup *group) {
    if (!group) {
        return;
    }

    destroy_chrome(group);
    onyrion_group_surface_refresh(group);

    if (group->shell_content_insets_set) {
        return;
    }

    const size_t count =
        mapped_window_count(group);

    const int height =
        chrome_height(group);

    struct wlr_box box = {0};

    if (!group->workspace ||
            !group->workspace->scene_tree ||
            !group->active ||
            group->active->fullscreen ||
            count == 0 ||
            !onyrion_group_box(group, &box) ||
            box.width <= 0 ||
            height <= 0) {
        return;
    }

    group->chrome_tree =
        wlr_scene_tree_create(
            group->workspace->scene_tree
        );

    if (!group->chrome_tree) {
        wlr_log(
            WLR_ERROR,
            "Failed to create Group chrome: id=%" PRIu64,
            group->id
        );
        return;
    }

    wlr_scene_node_set_position(
        &group->chrome_tree->node,
        box.x,
        box.y
    );

    const int handle_width =
        chrome_handle_width(group);

    const int tabs_width =
        box.width -
        handle_width;

    if (handle_width > 0) {
        const float handle_color[4] = {
            0.18f,
            0.18f,
            0.18f,
            1.0f,
        };

        struct wlr_scene_rect *handle =
            wlr_scene_rect_create(
                group->chrome_tree,
                handle_width,
                height,
                handle_color
            );

        if (!handle) {
            wlr_log(
                WLR_ERROR,
                "Failed to create Group handle:"
                " group=%" PRIu64,
                group->id
            );

            destroy_chrome(group);
            return;
        }
    }

    const int base_width =
        tabs_width /
        (int)count;

    const int remainder =
        tabs_width %
        (int)count;

    int offset_x =
        handle_width;
    size_t index = 0;
    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        if (!window->mapped) {
            continue;
        }

        const int width =
            base_width +
            ((int)index < remainder ? 1 : 0);

        if (width > 0) {
            const float *color =
                window == group->active
                    ? (const float[4]){
                        0.28f,
                        0.28f,
                        0.28f,
                        1.0f,
                    }
                    : (const float[4]){
                        0.12f,
                        0.12f,
                        0.12f,
                        1.0f,
                    };

            struct wlr_scene_rect *rect =
                wlr_scene_rect_create(
                    group->chrome_tree,
                    width,
                    height,
                    color
                );

            if (!rect) {
                wlr_log(
                    WLR_ERROR,
                    "Failed to create Group tab chrome:"
                    " group=%" PRIu64
                    " window=%" PRIu64,
                    group->id,
                    window->id
                );

                destroy_chrome(group);
                return;
            }

            wlr_scene_node_set_position(
                &rect->node,
                offset_x,
                0
            );
        }

        offset_x += width;
        index++;
    }

    wlr_scene_node_raise_to_top(
        &group->chrome_tree->node
    );

    wlr_log(
        WLR_DEBUG,
        "Group chrome refreshed:"
        " group=%" PRIu64
        " tabs=%zu"
        " handle=%d"
        " box=%dx%d+%d+%d",
        group->id,
        count,
        handle_width,
        box.width,
        height,
        box.x,
        box.y
    );
}

OnyrionGroup *onyrion_group_chrome_at(
        struct onyrion_server *server,
        double x,
        double y) {
    if (!server) {
        return NULL;
    }

    OnyrionGroup *group;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (!group->workspace ||
                !onyrion_workspace_is_visible(
                    group->workspace) ||
                !group->active ||
                group->active->fullscreen) {
            continue;
        }

        struct wlr_box box = {0};
        const int height =
            chrome_height(group);

        if (!onyrion_group_box(group, &box) ||
                height <= 0 ||
                x < (double)box.x ||
                x >= (double)(box.x + box.width) ||
                y < (double)box.y ||
                y >= (double)(box.y + height)) {
            continue;
        }

        return group;
    }

    return NULL;
}

OnyrionGroup *onyrion_group_handle_at(
        struct onyrion_server *server,
        double x,
        double y) {
    OnyrionGroup *group =
        onyrion_group_chrome_at(
            server,
            x,
            y
        );

    if (!group) {
        return NULL;
    }

    const int handle_width =
        chrome_handle_width(group);

    if (handle_width <= 0) {
        return NULL;
    }

    struct wlr_box box = {0};

    if (!onyrion_group_box(group, &box)) {
        return NULL;
    }

    const int local_x =
        (int)x - box.x;

    return
        local_x >= 0 &&
        local_x < handle_width
            ? group
            : NULL;
}

static bool group_tab_slot_at(
        OnyrionGroup *group,
        double x,
        OnyrionWindow **window_out,
        int *start_out,
        int *width_out) {
    struct wlr_box box = {0};

    if (!group ||
            !onyrion_group_box(group, &box)) {
        return false;
    }

    const size_t count =
        mapped_window_count(group);

    if (count == 0) {
        return false;
    }

    const int handle_width =
        chrome_handle_width(group);

    const int local_x =
        (int)x -
        box.x -
        handle_width;

    if (local_x < 0) {
        return false;
    }

    const int tabs_width =
        box.width -
        handle_width;

    const int base_width =
        tabs_width /
        (int)count;

    const int remainder =
        tabs_width %
        (int)count;

    int offset_x = 0;
    size_t index = 0;
    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        if (!window->mapped) {
            continue;
        }

        const int width =
            base_width +
            ((int)index < remainder ? 1 : 0);

        if (width > 0 &&
                local_x >= offset_x &&
                local_x < offset_x + width) {
            if (window_out) {
                *window_out = window;
            }

            if (start_out) {
                *start_out = offset_x;
            }

            if (width_out) {
                *width_out = width;
            }

            return true;
        }

        offset_x += width;
        index++;
    }

    return false;
}

struct onyrion_window *onyrion_group_tab_at(
        struct onyrion_server *server,
        double x,
        double y) {
    OnyrionGroup *group =
        onyrion_group_chrome_at(
            server,
            x,
            y
        );

    if (!group) {
        return NULL;
    }

    OnyrionWindow *window = NULL;

    return group_tab_slot_at(
            group,
            x,
            &window,
            NULL,
            NULL)
        ? window
        : NULL;
}

bool onyrion_group_insert_target_at(
        struct onyrion_server *server,
        double x,
        double y,
        OnyrionGroupInsertTarget *target) {
    if (!target) {
        return false;
    }

    OnyrionGroup *group =
        onyrion_group_chrome_at(
            server,
            x,
            y
        );

    if (!group) {
        return false;
    }

    OnyrionWindow *reference = NULL;
    int start = 0;
    int width = 0;

    if (!group_tab_slot_at(
            group,
            x,
            &reference,
            &start,
            &width) ||
            !reference ||
            width <= 0) {
        return false;
    }

    const int handle_width =
        chrome_handle_width(group);

    struct wlr_box box = {0};

    if (!onyrion_group_box(group, &box)) {
        return false;
    }

    const int local_x =
        (int)x -
        box.x -
        handle_width;

    *target =
        (OnyrionGroupInsertTarget){
            .group_id = group->id,
            .reference_window_id = reference->id,
            .side =
                local_x - start < width / 2
                    ? ONYRION_GROUP_INSERT_BEFORE
                    : ONYRION_GROUP_INSERT_AFTER,
        };

    return true;
}

static void set_active_window(
        OnyrionGroup *group,
        OnyrionWindow *active) {
    OnyrionWindow *window;
    OnyrionWindow *previous =
        group->active;

    group->active = active;

    onyrion_group_apply_geometry(group);

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        const bool is_active =
            window == active &&
            window->mapped;

        wlr_scene_node_set_enabled(
            &window->scene_tree->node,
            is_active
        );

        wlr_xdg_toplevel_set_activated(
            window->toplevel,
            is_active &&
                group->server->active_group == group
        );
    }

    if (group->server->active_group == group) {
        onyrion_input_focus_window(
            group->server,
            active
        );
    }

    if (previous != active) {
        onyrion_shell_protocol_mark_changed(
            group->server
        );
    }

    if (!active) {
        wlr_log(
            WLR_INFO,
            "Group has no active mapped window"
        );
        return;
    }

    wlr_log(
        WLR_INFO,
        "Group active window: %s",
        onyrion_window_title(active)
    );
}

static void destroy_group(OnyrionGroup *group) {
    OnyrionServer *server = group->server;
    OnyrionWorkspace *workspace =
        group->workspace;

    onyrion_input_cancel_chrome_drag_group(
        server,
        group->id
    );

    const bool was_workspace_active =
        workspace &&
        workspace->active_group == group;

    const bool was_server_active =
        server->active_group == group;

    destroy_chrome(group);
    onyrion_group_surface_group_destroyed(group);
    onyrion_layout_remove_group(group);

    wl_list_remove(&group->link);

    OnyrionGroup *fallback = NULL;

    if (was_workspace_active) {
        fallback =
            find_last_group_in_workspace(
                server,
                workspace
            );

        workspace->active_group =
            fallback;
    }

    if (was_server_active) {
        server->active_group =
            fallback;

        if (fallback) {
            set_active_window(
                fallback,
                fallback->active
            );
        } else {
            onyrion_input_focus_window(
                server,
                NULL
            );
        }
    }

    wlr_log(
        WLR_INFO,
        "Group destroyed: id=%" PRIu64,
        group->id
    );

    onyrion_shell_protocol_mark_changed(
        server
    );

    free(group);
}

void onyrion_group_init(struct onyrion_server *server) {
    wl_list_init(&server->groups);
    server->active_group = NULL;
    server->next_group_id = 1;
}

OnyrionGroup *onyrion_group_create_in_workspace(
        struct onyrion_server *server,
        struct onyrion_workspace *workspace,
        OnyrionSplitOrientation split_orientation) {
    if (!server || !workspace) {
        return NULL;
    }

    OnyrionGroup *group =
        calloc(1, sizeof(*group));

    if (!group) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate OnyrionGroup"
        );
        return NULL;
    }

    group->server = server;
    group->workspace = workspace;
    group->placement =
        ONYRION_GROUP_PLACEMENT_TILED;

    wl_list_init(&group->windows);

    wl_list_insert(
        server->groups.prev,
        &group->link
    );

    if (!onyrion_layout_add_group(
            group,
            split_orientation)) {
        wl_list_remove(
            &group->link
        );

        free(group);
        return NULL;
    }

    group->id =
        server->next_group_id++;

    workspace->active_group =
        group;

    if (workspace ==
            onyrion_workspace_focused(server)) {
        server->active_group = group;
    }

    wlr_log(
        WLR_INFO,
        "Group created: id=%" PRIu64
        " workspace_id=%" PRIu64,
        group->id,
        workspace->id
    );

    return group;
}

OnyrionGroup *onyrion_group_create(
        struct onyrion_server *server,
        OnyrionSplitOrientation split_orientation) {
    return onyrion_group_create_in_workspace(
        server,
        onyrion_workspace_focused(server),
        split_orientation
    );
}

void onyrion_group_add_window(
        OnyrionGroup *group,
        struct onyrion_window *window) {
    window->group = group;
    window->workspace =
        group->workspace;
    window->placement =
        group->placement ==
                ONYRION_GROUP_PLACEMENT_FLOATING
            ? ONYRION_WINDOW_PLACEMENT_FLOATING
            : ONYRION_WINDOW_PLACEMENT_TILED;

    if (group->workspace &&
            group->workspace->scene_tree) {
        wlr_scene_node_reparent(
            &window->scene_tree->node,
            group->workspace->scene_tree
        );
    }

    wl_list_insert(
        group->windows.prev,
        &window->group_link
    );

    group->window_count++;

    wlr_log(
        WLR_INFO,
        "Window joined group: window_id=%" PRIu64
        " group_id=%" PRIu64
        " members=%zu",
        window->id,
        group->id,
        group->window_count
    );

    onyrion_shell_protocol_mark_changed(
        group->server
    );
}

void onyrion_group_window_mapped(
        struct onyrion_window *window) {
    OnyrionGroup *group = window->group;

    window->mapped = true;

    if (!group) {
        return;
    }

    onyrion_group_focus_window(
        window
    );
}

void onyrion_group_window_unmapped(
        struct onyrion_window *window) {
    OnyrionGroup *group = window->group;

    window->mapped = false;

    if (!group) {
        return;
    }

    if (group->active != window) {
        wlr_scene_node_set_enabled(
            &window->scene_tree->node,
            false
        );

        wlr_xdg_toplevel_set_activated(
            window->toplevel,
            false
        );

        return;
    }

    set_active_window(
        group,
        find_last_mapped_window(group)
    );
}

void onyrion_group_focus_window(
        struct onyrion_window *window) {
    if (!window ||
            !window->group ||
            !window->mapped) {
        return;
    }

    OnyrionGroup *group =
        window->group;

    OnyrionServer *server =
        group->server;

    if (!onyrion_workspace_is_visible(
            group->workspace)) {
        return;
    }

    onyrion_output_focus(
        server,
        group->workspace->output
    );

    OnyrionWindow *previous_window =
        server->active_group
            ? server->active_group->active
            : NULL;

    if (previous_window &&
            previous_window != window &&
            previous_window->fullscreen &&
            previous_window->group &&
            previous_window->group->workspace ==
                group->workspace) {
        (void)onyrion_window_set_fullscreen(
            previous_window,
            false
        );
    }

    OnyrionGroup *previous =
        server->active_group;

    if (previous &&
            previous != group &&
            previous->active) {
        wlr_xdg_toplevel_set_activated(
            previous->active->toplevel,
            false
        );
    }

    group->workspace->active_group =
        group;

    server->active_group = group;

    set_active_window(
        group,
        window
    );

    if (previous != group) {
        onyrion_shell_protocol_mark_changed(
            server
        );
    }
}

bool onyrion_group_focus_id(
        struct onyrion_server *server,
        uint64_t id) {
    OnyrionGroup *group =
        onyrion_group_find_id(
            server,
            id
        );

    if (!group ||
            !group->workspace ||
            !group->active ||
            !group->active->mapped) {
        return false;
    }

    if (!onyrion_workspace_activate_id(
            server,
            group->workspace->id)) {
        return false;
    }

    onyrion_group_focus_window(
        group->active
    );

    return server->active_group == group;
}

bool onyrion_group_focus_window_id(
        struct onyrion_server *server,
        uint64_t id) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            id
        );

    if (!window ||
            !window->group ||
            !window->group->workspace ||
            !window->mapped) {
        return false;
    }

    if (!onyrion_workspace_activate_id(
            server,
            window->group->workspace->id)) {
        return false;
    }

    onyrion_group_focus_window(
        window
    );

    return
        server->active_group ==
            window->group &&
        window->group->active ==
            window;
}

bool onyrion_group_window_next(
        struct onyrion_server *server) {
    if (!server ||
            !server->active_group) {
        return false;
    }

    OnyrionWindow *target =
        find_adjacent_mapped_window(
            server->active_group,
            true
        );

    if (!target) {
        return false;
    }

    onyrion_group_focus_window(target);

    return true;
}

bool onyrion_group_window_previous(
        struct onyrion_server *server) {
    if (!server ||
            !server->active_group) {
        return false;
    }

    OnyrionWindow *target =
        find_adjacent_mapped_window(
            server->active_group,
            false
        );

    if (!target) {
        return false;
    }

    onyrion_group_focus_window(target);

    return true;
}

void onyrion_group_move_window(
        struct onyrion_window *window,
        OnyrionGroup *target) {
    if (!window ||
            !window->group ||
            !target ||
            window->group == target ||
            window->group->server != target->server) {
        return;
    }

    OnyrionGroup *source =
        window->group;

    const bool was_active =
        source->active == window;

    wl_list_remove(
        &window->group_link
    );

    wl_list_init(
        &window->group_link
    );

    window->group = NULL;
    source->window_count--;

    onyrion_group_add_window(
        target,
        window
    );

    if (source->window_count == 0) {
        destroy_group(source);
    } else if (was_active) {
        set_active_window(
            source,
            find_last_mapped_window(source)
        );
    }

    if (window->mapped) {
        onyrion_group_focus_window(
            window
        );
    }

    onyrion_layout_reflow_workspace(
        target->workspace
    );
}

bool onyrion_group_move_window_id(
        struct onyrion_server *server,
        uint64_t window_id,
        uint64_t target_group_id) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            window_id
        );

    OnyrionGroup *target =
        onyrion_group_find_id(
            server,
            target_group_id
        );

    if (!window ||
            !window->group ||
            !target ||
            window->group == target ||
            window->group->workspace !=
                target->workspace) {
        return false;
    }

    onyrion_group_move_window(
        window,
        target
    );

    return window->group == target;
}

bool onyrion_group_move_window_relative_id(
        struct onyrion_server *server,
        uint64_t window_id,
        uint64_t reference_window_id,
        OnyrionGroupInsertSide side) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            window_id
        );

    OnyrionWindow *reference =
        find_window_by_id(
            server,
            reference_window_id
        );

    if (!window ||
            !reference ||
            window == reference ||
            !window->group ||
            !reference->group ||
            !window->mapped ||
            !reference->mapped ||
            window->group->workspace !=
                reference->group->workspace) {
        return false;
    }

    OnyrionGroup *target =
        reference->group;

    if (window->group == target) {
        const bool already_placed =
            side == ONYRION_GROUP_INSERT_BEFORE
                ? window->group_link.next ==
                    &reference->group_link
                : window->group_link.prev ==
                    &reference->group_link;

        if (already_placed) {
            return false;
        }
    } else {
        onyrion_group_move_window(
            window,
            target
        );

        if (window->group != target) {
            return false;
        }
    }

    wl_list_remove(
        &window->group_link
    );

    if (side == ONYRION_GROUP_INSERT_BEFORE) {
        wl_list_insert(
            reference->group_link.prev,
            &window->group_link
        );
    } else {
        wl_list_insert(
            &reference->group_link,
            &window->group_link
        );
    }

    onyrion_group_refresh_chrome(target);
    onyrion_shell_protocol_mark_changed(server);

    wlr_log(
        WLR_INFO,
        "Window positioned in Group:"
        " window=%" PRIu64
        " target_group=%" PRIu64
        " reference=%" PRIu64
        " side=%s",
        window->id,
        target->id,
        reference->id,
        side == ONYRION_GROUP_INSERT_BEFORE
            ? "before"
            : "after"
    );

    return true;
}

bool onyrion_group_move_window_to_layout_zone_id(
        struct onyrion_server *server,
        uint64_t window_id,
        uint64_t target_group_id,
        OnyrionFocusDirection direction) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            window_id
        );

    OnyrionGroup *target =
        onyrion_group_find_id(
            server,
            target_group_id
        );

    if (!window ||
            !window->mapped ||
            !window->group ||
            !target ||
            window->group->workspace !=
                target->workspace) {
        return false;
    }

    OnyrionGroup *source =
        window->group;

    if (source == target &&
            source->window_count < 2) {
        return false;
    }

    OnyrionSplitOrientation orientation;

    switch (direction) {
    case ONYRION_FOCUS_LEFT:
    case ONYRION_FOCUS_RIGHT:
        orientation =
            ONYRION_SPLIT_HORIZONTAL;
        break;

    case ONYRION_FOCUS_UP:
    case ONYRION_FOCUS_DOWN:
        orientation =
            ONYRION_SPLIT_VERTICAL;
        break;

    default:
        return false;
    }

    OnyrionGroup *created =
        onyrion_group_create_in_workspace(
            server,
            source->workspace,
            orientation
        );

    if (!created) {
        return false;
    }

    if (!onyrion_layout_move_group_relative(
            created,
            target,
            direction)) {
        destroy_group(created);
        return false;
    }

    onyrion_group_move_window(
        window,
        created
    );

    if (window->group != created) {
        destroy_group(created);
        return false;
    }

    wlr_log(
        WLR_INFO,
        "Window tiled by layout drop:"
        " window=%" PRIu64
        " target=%" PRIu64
        " group=%" PRIu64,
        window_id,
        target_group_id,
        created->id
    );

    return true;
}

bool onyrion_group_merge_id(
        struct onyrion_server *server,
        uint64_t source_group_id,
        uint64_t target_group_id) {
    OnyrionGroup *source =
        onyrion_group_find_id(
            server,
            source_group_id
        );

    OnyrionGroup *target =
        onyrion_group_find_id(
            server,
            target_group_id
        );

    if (!source ||
            !target ||
            source == target ||
            source->workspace != target->workspace ||
            source->window_count == 0) {
        return false;
    }

    const uint64_t source_id =
        source->id;

    const size_t move_count =
        source->window_count;

    for (size_t i = 0;
            i < move_count;
            i++) {
        OnyrionWindow *window =
            wl_container_of(
                source->windows.next,
                window,
                group_link
            );

        onyrion_group_move_window(
            window,
            target
        );

        if (i + 1 < move_count &&
                window->group != target) {
            return false;
        }
    }

    wlr_log(
        WLR_INFO,
        "Groups merged: source=%" PRIu64
        " target=%" PRIu64,
        source_id,
        target->id
    );

    return true;
}

bool onyrion_group_split_window_id(
        struct onyrion_server *server,
        uint64_t window_id,
        OnyrionSplitOrientation split_orientation) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            window_id
        );

    if (!window ||
            !window->group ||
            !window->mapped ||
            window->group->placement !=
                ONYRION_GROUP_PLACEMENT_TILED ||
            !window->group->tile ||
            window->group->window_count < 2) {
        return false;
    }

    if (!onyrion_group_focus_window_id(
            server,
            window_id)) {
        return false;
    }

    return onyrion_layout_split_active(
        server,
        split_orientation
    );
}

bool onyrion_group_split_at_window_id(
        struct onyrion_server *server,
        uint64_t window_id,
        OnyrionSplitOrientation split_orientation) {
    OnyrionWindow *first =
        find_window_by_id(
            server,
            window_id
        );

    if (!first ||
            !first->group ||
            !first->mapped) {
        return false;
    }

    OnyrionGroup *source =
        first->group;

    if (source->placement !=
                ONYRION_GROUP_PLACEMENT_TILED ||
            !source->tile ||
            source->window_count < 2 ||
            source->windows.next ==
                &first->group_link) {
        return false;
    }

    OnyrionWindow *last =
        wl_container_of(
            source->windows.prev,
            last,
            group_link
        );

    const uint64_t source_group_id =
        source->id;
    const uint64_t last_window_id =
        last->id;

    if (!onyrion_group_focus_window_id(
            server,
            window_id)) {
        return false;
    }

    OnyrionGroup *target =
        onyrion_group_create_in_workspace(
            server,
            source->workspace,
            split_orientation
        );

    if (!target) {
        return false;
    }

    const uint64_t target_group_id =
        target->id;

    if (!onyrion_group_move_window_range_ids(
            server,
            window_id,
            last_window_id,
            target_group_id)) {
        destroy_group(target);
        return false;
    }

    wlr_log(
        WLR_INFO,
        "Group split at window:"
        " source=%" PRIu64
        " first_new=%" PRIu64
        " target=%" PRIu64,
        source_group_id,
        window_id,
        target_group_id
    );

    return true;
}

bool onyrion_group_move_window_range_ids(
        struct onyrion_server *server,
        uint64_t first_window_id,
        uint64_t last_window_id,
        uint64_t target_group_id) {
    OnyrionWindow *first =
        find_window_by_id(
            server,
            first_window_id
        );

    OnyrionWindow *last =
        find_window_by_id(
            server,
            last_window_id
        );

    OnyrionGroup *target =
        onyrion_group_find_id(
            server,
            target_group_id
        );

    if (!first ||
            !last ||
            !target ||
            !first->group ||
            first->group != last->group ||
            first->group == target ||
            first->group->workspace !=
                target->workspace) {
        return false;
    }

    OnyrionGroup *source =
        first->group;

    size_t count = 0;
    bool in_range = false;
    bool found_last = false;
    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &source->windows,
            group_link) {
        if (window == first) {
            in_range = true;
        }

        if (!in_range) {
            continue;
        }

        count++;

        if (window == last) {
            found_last = true;
            break;
        }
    }

    if (!found_last ||
            count == 0) {
        return false;
    }

    OnyrionWindow **windows =
        calloc(
            count,
            sizeof(*windows)
        );

    if (!windows) {
        return false;
    }

    size_t index = 0;
    in_range = false;

    wl_list_for_each(
            window,
            &source->windows,
            group_link) {
        if (window == first) {
            in_range = true;
        }

        if (!in_range) {
            continue;
        }

        windows[index++] =
            window;

        if (window == last) {
            break;
        }
    }

    for (size_t i = 0;
            i < count;
            i++) {
        onyrion_group_move_window(
            windows[i],
            target
        );
    }

    free(windows);

    wlr_log(
        WLR_INFO,
        "Window range moved: first=%" PRIu64
        " last=%" PRIu64
        " target_group=%" PRIu64,
        first_window_id,
        last_window_id,
        target_group_id
    );

    return true;
}

bool onyrion_group_float_id(
        struct onyrion_server *server,
        uint64_t group_id) {
    OnyrionGroup *group =
        onyrion_group_find_id(server, group_id);

    if (!group ||
            !group->workspace ||
            group->placement !=
                ONYRION_GROUP_PLACEMENT_TILED ||
            !group->tile ||
            (group->active &&
                group->active->fullscreen)) {
        return false;
    }

    struct wlr_box box = {0};

    if (!floating_default_box(group, &box)) {
        return false;
    }

    onyrion_layout_remove_group(group);

    group->placement =
        ONYRION_GROUP_PLACEMENT_FLOATING;
    group->pinned_output = NULL;
    group->floating_x = box.x;
    group->floating_y = box.y;
    group->floating_width = box.width;
    group->floating_height = box.height;

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        window->placement =
            ONYRION_WINDOW_PLACEMENT_FLOATING;
        window->workspace = group->workspace;
    }

    onyrion_group_apply_geometry(group);

    if (group->active &&
            group->active->mapped) {
        onyrion_group_focus_window(group->active);
    }

    onyrion_shell_protocol_mark_changed(server);

    wlr_log(
        WLR_INFO,
        "Group floating: group=%" PRIu64
        " workspace=%" PRIu64
        " geometry=%dx%d+%d+%d",
        group->id,
        group->workspace->id,
        group->floating_width,
        group->floating_height,
        group->floating_x,
        group->floating_y
    );

    return true;
}

bool onyrion_group_tile_id(
        struct onyrion_server *server,
        uint64_t group_id) {
    OnyrionGroup *group =
        onyrion_group_find_id(server, group_id);

    if (!group ||
            !group->workspace ||
            group->placement !=
                ONYRION_GROUP_PLACEMENT_FLOATING ||
            group->tile ||
            (group->active &&
                group->active->fullscreen)) {
        return false;
    }

    group->placement =
        ONYRION_GROUP_PLACEMENT_TILED;

    if (!onyrion_layout_add_group(
            group,
            ONYRION_SPLIT_HORIZONTAL)) {
        group->placement =
            ONYRION_GROUP_PLACEMENT_FLOATING;
        onyrion_group_refresh_chrome(group);
        return false;
    }

    group->pinned_output = NULL;

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        window->placement =
            ONYRION_WINDOW_PLACEMENT_TILED;
        window->workspace = group->workspace;
    }

    onyrion_group_apply_geometry(group);

    if (group->active &&
            group->active->mapped) {
        onyrion_group_focus_window(group->active);
    }

    onyrion_shell_protocol_mark_changed(server);

    wlr_log(
        WLR_INFO,
        "Group tiled: group=%" PRIu64
        " workspace=%" PRIu64,
        group->id,
        group->workspace->id
    );

    return true;
}

static void move_group_scene_to_workspace(
        OnyrionGroup *group,
        OnyrionWorkspace *target) {
    if (!group || !target) {
        return;
    }

    destroy_chrome(group);
    group->workspace = target;

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        window->workspace = target;

        if (target->scene_tree &&
                window->scene_tree) {
            wlr_scene_node_reparent(
                &window->scene_tree->node,
                target->scene_tree
            );
        }

        onyrion_window_move_transients_to_workspace(
            window,
            target
        );
    }

    /*
     * Group surfaces stack relative to the active window. Move every Group
     * window first so the surface and its sibling share the target parent
     * before onyrion_group_surface_reparent() reapplies z-order.
     */
    if (target->scene_tree) {
        onyrion_group_surface_reparent(group, target->scene_tree);
    }
}

static bool move_floating_group_to_workspace(
        OnyrionGroup *group,
        OnyrionWorkspace *target,
        bool pin_follow) {
    if (!group ||
            group->placement !=
                ONYRION_GROUP_PLACEMENT_FLOATING ||
            !group->workspace ||
            !target ||
            !target->output ||
            group->workspace == target) {
        return false;
    }

    OnyrionServer *server = group->server;
    OnyrionWorkspace *source = group->workspace;
    OnyrionOutput *source_output = source->output;
    const bool was_source_active =
        source->active_group == group;
    const bool was_server_active =
        server->active_group == group;

    move_group_scene_to_workspace(group, target);

    if (source_output != target->output) {
        struct wlr_box box = {0};

        if (floating_default_box(group, &box)) {
            group->floating_x = box.x;
            group->floating_y = box.y;
            group->floating_width = box.width;
            group->floating_height = box.height;
        }

        if (group->pinned_output != target->output) {
            group->pinned_output = NULL;
        }
    }

    if (was_source_active) {
        source->active_group =
            find_last_group_in_workspace(
                server,
                source
            );
    }

    if (!target->active_group) {
        target->active_group = group;
    }

    if (was_server_active && !pin_follow) {
        OnyrionGroup *fallback =
            source->active_group;

        server->active_group = fallback;

        if (fallback && fallback->active) {
            set_active_window(
                fallback,
                fallback->active
            );
        } else {
            onyrion_input_focus_window(
                server,
                NULL
            );
        }
    }

    onyrion_group_apply_geometry(group);

    return group->workspace == target;
}

bool onyrion_group_set_pinned_id(
        struct onyrion_server *server,
        uint64_t group_id,
        bool pinned) {
    OnyrionGroup *group =
        onyrion_group_find_id(server, group_id);

    if (!group ||
            group->placement !=
                ONYRION_GROUP_PLACEMENT_FLOATING ||
            !group->workspace ||
            !group->workspace->output) {
        return false;
    }

    OnyrionOutput *output =
        group->workspace->output;

    group->pinned_output =
        pinned ? output : NULL;

    if (pinned &&
            output->visible_workspace &&
            output->visible_workspace !=
                group->workspace) {
        (void)move_floating_group_to_workspace(
            group,
            output->visible_workspace,
            true
        );
    }

    onyrion_shell_protocol_mark_changed(server);

    wlr_log(
        WLR_INFO,
        "Group pin: group=%" PRIu64
        " pinned=%u output=%s",
        group->id,
        (unsigned)pinned,
        pinned && output->wlr_output &&
                output->wlr_output->name
            ? output->wlr_output->name
            : "<none>"
    );

    return (group->pinned_output != NULL) == pinned;
}

void onyrion_group_follow_pinned_workspace(
        struct onyrion_server *server,
        struct onyrion_output *output,
        struct onyrion_workspace *workspace) {
    if (!server ||
            !output ||
            !workspace ||
            workspace->output != output) {
        return;
    }

    OnyrionGroup *group;
    OnyrionGroup *tmp;

    wl_list_for_each_safe(
            group,
            tmp,
            &server->groups,
            link) {
        if (group->placement !=
                    ONYRION_GROUP_PLACEMENT_FLOATING ||
                group->pinned_output != output ||
                group->workspace == workspace) {
            continue;
        }

        (void)move_floating_group_to_workspace(
            group,
            workspace,
            true
        );
    }
}

void onyrion_group_output_removed(
        struct onyrion_server *server,
        struct onyrion_output *output) {
    if (!server || !output) {
        return;
    }

    OnyrionGroup *group;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (group->pinned_output == output) {
            group->pinned_output = NULL;
        }
    }
}

bool onyrion_group_move_to_workspace_id(
        struct onyrion_server *server,
        uint64_t group_id,
        uint64_t workspace_id) {
    OnyrionGroup *group =
        onyrion_group_find_id(
            server,
            group_id
        );

    OnyrionWorkspace *target =
        onyrion_workspace_find_id(
            server,
            workspace_id
        );

    if (!group ||
            !group->workspace ||
            !target ||
            !target->output ||
            group->workspace == target) {
        return false;
    }

    if (group->placement ==
            ONYRION_GROUP_PLACEMENT_FLOATING) {
        if (group->pinned_output) {
            wlr_log(
                WLR_DEBUG,
                "Pinned Group move ignored: group=%" PRIu64
                " workspace=%" PRIu64,
                group_id,
                workspace_id
            );
            return false;
        }

        const bool moved =
            move_floating_group_to_workspace(
                group,
                target,
                false
            );

        if (!moved) {
            return false;
        }

        onyrion_shell_protocol_mark_changed(server);

        wlr_log(
            WLR_INFO,
            "Floating Group moved to workspace:"
            " group=%" PRIu64
            " workspace=%" PRIu64,
            group_id,
            workspace_id
        );

        return true;
    }

    if (group->placement !=
                ONYRION_GROUP_PLACEMENT_TILED ||
            !group->tile) {
        return false;
    }

    OnyrionWorkspace *source =
        group->workspace;

    const bool was_source_active =
        source->active_group == group;

    const bool was_server_active =
        server->active_group == group;

    if (!onyrion_layout_move_group_to_workspace(
            group,
            target,
            ONYRION_SPLIT_HORIZONTAL)) {
        return false;
    }

    OnyrionGroup *source_fallback =
        find_last_group_in_workspace(
            server,
            source
        );

    if (was_source_active) {
        source->active_group =
            source_fallback;
    }

    if (!target->active_group) {
        target->active_group =
            group;
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &group->windows,
            group_link) {
        window->workspace = target;

        if (target->scene_tree) {
            wlr_scene_node_reparent(
                &window->scene_tree->node,
                target->scene_tree
            );
        }

        onyrion_window_move_transients_to_workspace(
            window,
            target
        );
    }

    if (was_server_active) {
        server->active_group =
            source_fallback;

        if (source_fallback &&
                source_fallback->active) {
            set_active_window(
                source_fallback,
                source_fallback->active
            );
        } else {
            onyrion_input_focus_window(
                server,
                NULL
            );
        }
    }

    set_active_window(
        group,
        group->active
    );

    onyrion_shell_protocol_mark_changed(
        server
    );

    wlr_log(
        WLR_INFO,
        "Group moved to workspace: group=%" PRIu64
        " workspace=%" PRIu64,
        group_id,
        workspace_id
    );

    return group->workspace == target;
}

bool onyrion_group_move_window_to_workspace_id(
        struct onyrion_server *server,
        uint64_t window_id,
        uint64_t workspace_id) {
    OnyrionWindow *window =
        find_window_by_id(
            server,
            window_id
        );

    OnyrionWorkspace *target_workspace =
        onyrion_workspace_find_id(
            server,
            workspace_id
        );

    if (!window ||
            !window->group ||
            !target_workspace ||
            !target_workspace->output ||
            window->group->workspace ==
                target_workspace) {
        return false;
    }

    OnyrionGroup *target_group =
        target_workspace->active_group;

    if (!target_group) {
        target_group =
            onyrion_group_create_in_workspace(
                server,
                target_workspace,
                ONYRION_SPLIT_HORIZONTAL
            );
    }

    if (!target_group) {
        return false;
    }

    onyrion_group_move_window(
        window,
        target_group
    );

    onyrion_window_move_transients_to_workspace(
        window,
        target_workspace
    );

    wlr_log(
        WLR_INFO,
        "Window moved to workspace: window=%" PRIu64
        " workspace=%" PRIu64
        " group=%" PRIu64,
        window_id,
        workspace_id,
        target_group->id
    );

    return window->group == target_group;
}

void onyrion_group_remove_window(
        struct onyrion_window *window) {
    OnyrionGroup *group = window->group;

    if (!group) {
        return;
    }

    const bool was_active =
        group->active == window;

    wl_list_remove(&window->group_link);
    wl_list_init(&window->group_link);

    window->group = NULL;
    group->window_count--;

    if (group->window_count == 0) {
        destroy_group(group);
        return;
    }

    if (was_active) {
        set_active_window(
            group,
            find_last_mapped_window(group)
        );
    } else {
        onyrion_group_refresh_chrome(group);
    }

    onyrion_shell_protocol_mark_changed(
        group->server
    );
}

void onyrion_group_finish(struct onyrion_server *server) {
    OnyrionGroup *group;
    OnyrionGroup *group_tmp;

    wl_list_for_each_safe(
            group,
            group_tmp,
            &server->groups,
            link) {
        OnyrionWindow *window;
        OnyrionWindow *window_tmp;

        wl_list_for_each_safe(
                window,
                window_tmp,
                &group->windows,
                group_link) {
            wl_list_remove(&window->group_link);
            wl_list_init(&window->group_link);
            window->group = NULL;
        }

        group->window_count = 0;
        group->active = NULL;

        destroy_group(group);
    }

    server->active_group = NULL;
}
