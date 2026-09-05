#include "group.h"
#include "input.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "shell_protocol.h"
#include "window.h"
#include "workspace.h"

#include <inttypes.h>
#include <stdlib.h>

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

static OnyrionGroup *find_group_by_id(
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

static int chrome_height(
        const OnyrionGroup *group) {
    if (!group ||
            !group->tile ||
            group->tile->height <= 1) {
        return 0;
    }

    const int maximum =
        group->tile->height - 1;

    return
        ONYRION_GROUP_CHROME_HEIGHT < maximum
            ? ONYRION_GROUP_CHROME_HEIGHT
            : maximum;
}

static int chrome_handle_width(
        const OnyrionGroup *group) {
    if (!group ||
            !group->tile ||
            group->tile->width <= 1) {
        return 0;
    }

    const int maximum =
        group->tile->width - 1;

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

    const size_t count =
        mapped_window_count(group);

    const int height =
        chrome_height(group);

    if (!group->tile ||
            !group->workspace ||
            !group->workspace->scene_tree ||
            !group->active ||
            group->active->fullscreen ||
            count == 0 ||
            group->tile->width <= 0 ||
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
        group->tile->x,
        group->tile->y
    );

    const int handle_width =
        chrome_handle_width(group);

    const int tabs_width =
        group->tile->width -
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
        group->tile->width,
        height,
        group->tile->x,
        group->tile->y
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
        if (!group->tile ||
                !group->workspace ||
                !onyrion_workspace_is_visible(
                    group->workspace) ||
                !group->active ||
                group->active->fullscreen) {
            continue;
        }

        const int height =
            chrome_height(group);

        if (height <= 0 ||
                x < (double)group->tile->x ||
                x >= (double)(
                    group->tile->x +
                    group->tile->width
                ) ||
                y < (double)group->tile->y ||
                y >= (double)(
                    group->tile->y +
                    height
                )) {
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

    const int local_x =
        (int)x - group->tile->x;

    return
        local_x >= 0 &&
        local_x < handle_width
            ? group
            : NULL;
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

    const size_t count =
        mapped_window_count(group);

    if (count == 0) {
        return NULL;
    }

    const int handle_width =
        chrome_handle_width(group);

    const int local_x =
        (int)x -
        group->tile->x -
        handle_width;

    if (local_x < 0) {
        return NULL;
    }

    const int tabs_width =
        group->tile->width -
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
            return window;
        }

        offset_x += width;
        index++;
    }

    return NULL;
}

static void set_active_window(
        OnyrionGroup *group,
        OnyrionWindow *active) {
    OnyrionWindow *window;
    OnyrionWindow *previous =
        group->active;

    group->active = active;

    onyrion_layout_apply_group(group);

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

    const bool was_workspace_active =
        workspace &&
        workspace->active_group == group;

    const bool was_server_active =
        server->active_group == group;

    destroy_chrome(group);
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
        ONYRION_WINDOW_PLACEMENT_TILED;

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
        find_group_by_id(
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
        find_group_by_id(
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
        find_group_by_id(
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
        find_group_by_id(
            server,
            source_group_id
        );

    OnyrionGroup *target =
        find_group_by_id(
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

    if (source->window_count < 2 ||
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
        find_group_by_id(
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

bool onyrion_group_move_to_workspace_id(
        struct onyrion_server *server,
        uint64_t group_id,
        uint64_t workspace_id) {
    OnyrionGroup *group =
        find_group_by_id(
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
        window->workspace =
            target;

        if (target->scene_tree) {
            wlr_scene_node_reparent(
                &window->scene_tree->node,
                target->scene_tree
            );
        }
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
