#include "layout.h"

#include "config.h"
#include "group.h"
#include "layer_shell.h"
#include "output.h"
#include "server.h"
#include "window.h"
#include "workspace.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

static size_t count_tiles(
        OnyrionWorkspace *workspace) {
    size_t count = 0;
    OnyrionTile *tile;

    wl_list_for_each(
            tile,
            &workspace->tiles,
            link) {
        count++;
    }

    return count;
}

static const char *split_orientation_name(
        OnyrionSplitOrientation orientation) {
    switch (orientation) {
    case ONYRION_SPLIT_HORIZONTAL:
        return "horizontal";

    case ONYRION_SPLIT_VERTICAL:
        return "vertical";
    }

    return "<invalid>";
}

typedef struct onyrion_focus_direction_meta {
    const char *label;
    OnyrionSplitOrientation orientation;
    int ratio_sign;
} OnyrionFocusDirectionMeta;

#define ONYRION_FOCUS_DIRECTION_META( \
        name, label, orientation, ratio_sign) \
    [ONYRION_FOCUS_##name] = { \
        label, \
        orientation, \
        ratio_sign, \
    },

static const OnyrionFocusDirectionMeta
focus_direction_meta[] = {
    ONYRION_FOCUS_DIRECTIONS(
        ONYRION_FOCUS_DIRECTION_META
    )
};

#undef ONYRION_FOCUS_DIRECTION_META

static_assert(
    sizeof(focus_direction_meta) /
        sizeof(focus_direction_meta[0]) ==
    ONYRION_FOCUS_DIRECTION_COUNT
);

static const OnyrionFocusDirectionMeta *
focus_direction_metadata(
        OnyrionFocusDirection direction) {
    if ((unsigned)direction >=
            (unsigned)ONYRION_FOCUS_DIRECTION_COUNT) {
        return NULL;
    }

    return &focus_direction_meta[direction];
}

static const char *focus_direction_name(
        OnyrionFocusDirection direction) {
    const OnyrionFocusDirectionMeta *meta =
        focus_direction_metadata(direction);

    return meta
        ? meta->label
        : "<invalid>";
}

static size_t tree_depth(
        const OnyrionLayoutNode *node) {
    if (!node) {
        return 0;
    }

    if (node->type ==
            ONYRION_LAYOUT_NODE_TILE) {
        return 1;
    }

    const size_t first =
        tree_depth(
            node->split.first
        );

    const size_t second =
        tree_depth(
            node->split.second
        );

    return 1 +
        (first > second
            ? first
            : second);
}

static OnyrionTile *find_anchor_tile(
        OnyrionWorkspace *workspace) {
    if (workspace->active_group &&
            workspace->active_group->tile) {
        return workspace->active_group->tile;
    }

    if (wl_list_empty(
            &workspace->tiles)) {
        return NULL;
    }

    OnyrionTile *tile =
        wl_container_of(
            workspace->tiles.prev,
            tile,
            link
        );

    return tile;
}

static OnyrionLayoutNode *create_tile_leaf(
        OnyrionGroup *group) {
    OnyrionTile *tile =
        calloc(1, sizeof(*tile));

    if (!tile) {
        return NULL;
    }

    OnyrionLayoutNode *node =
        calloc(1, sizeof(*node));

    if (!node) {
        free(tile);
        return NULL;
    }

    tile->workspace =
        group->workspace;

    tile->group =
        group;

    tile->node =
        node;

    node->type =
        ONYRION_LAYOUT_NODE_TILE;

    node->workspace =
        group->workspace;

    node->leaf.tile =
        tile;

    wl_list_insert(
        group->workspace->tiles.prev,
        &tile->link
    );

    group->tile =
        tile;

    return node;
}

static void destroy_unattached_leaf(
        OnyrionLayoutNode *node) {
    if (!node ||
            node->type !=
                ONYRION_LAYOUT_NODE_TILE) {
        return;
    }

    OnyrionTile *tile =
        node->leaf.tile;

    if (tile) {
        if (tile->group &&
                tile->group->tile == tile) {
            tile->group->tile = NULL;
        }

        wl_list_remove(
            &tile->link
        );

        free(tile);
    }

    free(node);
}

static bool replace_node(
        OnyrionWorkspace *workspace,
        OnyrionLayoutNode *old_node,
        OnyrionLayoutNode *new_node) {
    OnyrionLayoutNode *parent =
        old_node->parent;

    new_node->parent =
        parent;

    if (!parent) {
        if (workspace->layout_root !=
                old_node) {
            return false;
        }

        workspace->layout_root =
            new_node;

        return true;
    }

    if (parent->type !=
            ONYRION_LAYOUT_NODE_SPLIT) {
        return false;
    }

    if (parent->split.first ==
            old_node) {
        parent->split.first =
            new_node;

        return true;
    }

    if (parent->split.second ==
            old_node) {
        parent->split.second =
            new_node;

        return true;
    }

    return false;
}

void onyrion_layout_apply_group(
        struct onyrion_group *group) {
    if (!group ||
            !group->tile ||
            !group->active ||
            !group->active->mapped) {
        return;
    }

    OnyrionWindow *window =
        group->active;

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

            wlr_xdg_toplevel_set_size(
                window->toplevel,
                box.width,
                box.height
            );

            onyrion_window_reflow_transients(
                window
            );

            onyrion_group_refresh_chrome(
                group
            );

            return;
        }
    }

    struct wlr_box content = {0};

    if (!onyrion_group_content_box(group, &content)) {
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

    wlr_xdg_toplevel_set_size(
        window->toplevel,
        window->width,
        window->height
    );

    onyrion_window_reflow_transients(
        window
    );

    onyrion_group_refresh_chrome(
        group
    );
}

static int bounded_inner_gap(
        int requested,
        int span) {
    if (requested <= 0 ||
            span <= 2) {
        return 0;
    }

    const int maximum =
        span - 2;

    return
        requested < maximum
            ? requested
            : maximum;
}

static void inset_outer_gap(
        const OnyrionLayoutPolicy *policy,
        struct wlr_box *box) {
    if (!policy ||
            !box ||
            policy->outer_gap <= 0 ||
            box->width <= 1 ||
            box->height <= 1) {
        return;
    }

    const int max_x =
        (box->width - 1) / 2;

    const int max_y =
        (box->height - 1) / 2;

    const int inset_x =
        policy->outer_gap < max_x
            ? policy->outer_gap
            : max_x;

    const int inset_y =
        policy->outer_gap < max_y
            ? policy->outer_gap
            : max_y;

    box->x += inset_x;
    box->y += inset_y;
    box->width -= inset_x * 2;
    box->height -= inset_y * 2;
}

static void layout_node(
        OnyrionLayoutNode *node,
        const struct wlr_box *box) {
    if (!node) {
        return;
    }

    if (node->type ==
            ONYRION_LAYOUT_NODE_TILE) {
        OnyrionTile *tile =
            node->leaf.tile;

        tile->x = box->x;
        tile->y = box->y;
        tile->width = box->width;
        tile->height = box->height;

        onyrion_layout_apply_group(
            tile->group
        );

        return;
    }

    OnyrionLayoutNode *first =
        node->split.first;

    OnyrionLayoutNode *second =
        node->split.second;

    if (!first || !second) {
        wlr_log(
            WLR_ERROR,
            "Layout split node has missing child"
        );
        return;
    }

    double ratio =
        node->split.ratio;

    if (ratio <= 0.0 ||
            ratio >= 1.0) {
        ratio =
            node->workspace->server->
                policy->layout.initial_split_ratio;
    }

    struct wlr_box first_box =
        *box;

    struct wlr_box second_box =
        *box;

    if (node->split.orientation ==
            ONYRION_SPLIT_HORIZONTAL) {
        const int gap =
            bounded_inner_gap(
                node->workspace->server->
                    policy->layout.inner_gap,
                box->width
            );

        const int available =
            box->width - gap;

        int first_width =
            (int)(
                (double)available *
                ratio
            );

        if (available > 1) {
            if (first_width < 1) {
                first_width = 1;
            }

            if (first_width >=
                    available) {
                first_width =
                    available - 1;
            }
        }

        first_box.width =
            first_width;

        second_box.x =
            box->x +
            first_width +
            gap;

        second_box.width =
            available - first_width;
    } else {
        const int gap =
            bounded_inner_gap(
                node->workspace->server->
                    policy->layout.inner_gap,
                box->height
            );

        const int available =
            box->height - gap;

        int first_height =
            (int)(
                (double)available *
                ratio
            );

        if (available > 1) {
            if (first_height < 1) {
                first_height = 1;
            }

            if (first_height >=
                    available) {
                first_height =
                    available - 1;
            }
        }

        first_box.height =
            first_height;

        second_box.y =
            box->y +
            first_height +
            gap;

        second_box.height =
            available - first_height;
    }

    layout_node(
        first,
        &first_box
    );

    layout_node(
        second,
        &second_box
    );
}

void onyrion_layout_reflow_workspace(
        struct onyrion_workspace *workspace) {
    if (!workspace ||
            !workspace->layout_root) {
        return;
    }

    OnyrionServer *server =
        workspace->server;

    struct wlr_box box = {0};

    onyrion_layer_shell_get_output_usable_box(
        server,
        workspace->output
            ? workspace->output->wlr_output
            : NULL,
        &box
    );

    if (box.width <= 0 ||
            box.height <= 0) {
        return;
    }

    inset_outer_gap(
        &server->policy->layout,
        &box
    );

    layout_node(
        workspace->layout_root,
        &box
    );

    wlr_log(
        WLR_INFO,
        "Layout reflow: workspace=%" PRIu64
        " tiles=%zu depth=%zu"
        " box=%dx%d+%d+%d"
        " outer_gap=%d inner_gap=%d",
        workspace->id,
        count_tiles(workspace),
        tree_depth(
            workspace->layout_root
        ),
        box.width,
        box.height,
        box.x,
        box.y,
        server->policy->layout.outer_gap,
        server->policy->layout.inner_gap
    );
}

void onyrion_layout_reflow(
        struct onyrion_server *server) {
    if (!server) {
        return;
    }

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        if (output->visible_workspace) {
            onyrion_layout_reflow_workspace(
                output->visible_workspace
            );
        }
    }
}

static OnyrionTile *find_directional_tile(
        struct onyrion_server *server,
        OnyrionFocusDirection direction) {
    if (!server ||
            !onyrion_workspace_focused(server) ||
            !server->active_group ||
            !server->active_group->tile) {
        return NULL;
    }

    OnyrionWorkspace *workspace =
        onyrion_workspace_focused(server);

    OnyrionGroup *source_group =
        server->active_group;

    if (source_group->workspace !=
            workspace) {
        return NULL;
    }

    OnyrionTile *source =
        source_group->tile;

    const int64_t source_left =
        source->x;
    const int64_t source_right =
        (int64_t)source->x +
        source->width;
    const int64_t source_top =
        source->y;
    const int64_t source_bottom =
        (int64_t)source->y +
        source->height;

    const int64_t source_x2 =
        source_left +
        source_right;
    const int64_t source_y2 =
        source_top +
        source_bottom;

    OnyrionTile *best = NULL;

    /*
     * Geometry ranking, lexicographically:
     *
     * 1. Prefer a candidate overlapping the source on the axis
     *    perpendicular to movement ("same lane").
     * 2. Prefer the nearest edge in the requested direction.
     * 3. Prefer the closest perpendicular center.
     * 4. Prefer the closest directional center.
     *
     * If there is no same-lane candidate, diagonal navigation
     * remains possible as a fallback.
     */
    int best_lane_penalty = 2;
    int64_t best_primary = INT64_MAX;
    int64_t best_secondary = INT64_MAX;
    int64_t best_center_primary = INT64_MAX;

    OnyrionTile *candidate;

    wl_list_for_each(
            candidate,
            &workspace->tiles,
            link) {
        if (candidate == source ||
                !candidate->group ||
                !candidate->group->active ||
                !candidate->group->active->mapped) {
            continue;
        }

        const int64_t candidate_left =
            candidate->x;
        const int64_t candidate_right =
            (int64_t)candidate->x +
            candidate->width;
        const int64_t candidate_top =
            candidate->y;
        const int64_t candidate_bottom =
            (int64_t)candidate->y +
            candidate->height;

        const int64_t candidate_x2 =
            candidate_left +
            candidate_right;
        const int64_t candidate_y2 =
            candidate_top +
            candidate_bottom;

        const int64_t dx =
            candidate_x2 - source_x2;
        const int64_t dy =
            candidate_y2 - source_y2;

        int lane_penalty;
        int64_t primary;
        int64_t secondary;
        int64_t center_primary;

        switch (direction) {
        case ONYRION_FOCUS_LEFT: {
            if (dx >= 0) {
                continue;
            }

            const int64_t overlap =
                (source_bottom < candidate_bottom
                    ? source_bottom
                    : candidate_bottom) -
                (source_top > candidate_top
                    ? source_top
                    : candidate_top);

            lane_penalty =
                overlap > 0 ? 0 : 1;

            primary =
                source_left > candidate_right
                    ? source_left - candidate_right
                    : 0;

            secondary =
                dy < 0 ? -dy : dy;

            center_primary = -dx;
            break;
        }

        case ONYRION_FOCUS_RIGHT: {
            if (dx <= 0) {
                continue;
            }

            const int64_t overlap =
                (source_bottom < candidate_bottom
                    ? source_bottom
                    : candidate_bottom) -
                (source_top > candidate_top
                    ? source_top
                    : candidate_top);

            lane_penalty =
                overlap > 0 ? 0 : 1;

            primary =
                candidate_left > source_right
                    ? candidate_left - source_right
                    : 0;

            secondary =
                dy < 0 ? -dy : dy;

            center_primary = dx;
            break;
        }

        case ONYRION_FOCUS_UP: {
            if (dy >= 0) {
                continue;
            }

            const int64_t overlap =
                (source_right < candidate_right
                    ? source_right
                    : candidate_right) -
                (source_left > candidate_left
                    ? source_left
                    : candidate_left);

            lane_penalty =
                overlap > 0 ? 0 : 1;

            primary =
                source_top > candidate_bottom
                    ? source_top - candidate_bottom
                    : 0;

            secondary =
                dx < 0 ? -dx : dx;

            center_primary = -dy;
            break;
        }

        case ONYRION_FOCUS_DOWN: {
            if (dy <= 0) {
                continue;
            }

            const int64_t overlap =
                (source_right < candidate_right
                    ? source_right
                    : candidate_right) -
                (source_left > candidate_left
                    ? source_left
                    : candidate_left);

            lane_penalty =
                overlap > 0 ? 0 : 1;

            primary =
                candidate_top > source_bottom
                    ? candidate_top - source_bottom
                    : 0;

            secondary =
                dx < 0 ? -dx : dx;

            center_primary = dy;
            break;
        }

        default:
            return NULL;
        }

        if (!best ||
                lane_penalty < best_lane_penalty ||
                (lane_penalty == best_lane_penalty &&
                    primary < best_primary) ||
                (lane_penalty == best_lane_penalty &&
                    primary == best_primary &&
                    secondary < best_secondary) ||
                (lane_penalty == best_lane_penalty &&
                    primary == best_primary &&
                    secondary == best_secondary &&
                    center_primary <
                        best_center_primary)) {
            best = candidate;
            best_lane_penalty =
                lane_penalty;
            best_primary =
                primary;
            best_secondary =
                secondary;
            best_center_primary =
                center_primary;
        }
    }

    return best;
}

bool onyrion_layout_focus_direction(
        struct onyrion_server *server,
        OnyrionFocusDirection direction) {
    OnyrionTile *target =
        find_directional_tile(
            server,
            direction
        );

    if (!target) {
        wlr_log(
            WLR_DEBUG,
            "Directional focus ignored: direction=%s no target",
            focus_direction_name(direction)
        );

        return false;
    }

    onyrion_group_focus_window(
        target->group->active
    );

    wlr_log(
        WLR_INFO,
        "Directional focus: %s",
        focus_direction_name(direction)
    );

    return true;
}

bool onyrion_layout_swap_direction(
        struct onyrion_server *server,
        OnyrionFocusDirection direction) {
    if (!server ||
            !server->active_group ||
            !server->active_group->tile) {
        return false;
    }

    OnyrionGroup *active_group =
        server->active_group;

    OnyrionTile *source =
        active_group->tile;

    OnyrionTile *target =
        find_directional_tile(
            server,
            direction
        );

    if (!target) {
        wlr_log(
            WLR_DEBUG,
            "Directional move ignored: direction=%s no target",
            focus_direction_name(direction)
        );

        return false;
    }

    OnyrionGroup *target_group =
        target->group;

    if (!target_group ||
            target_group == active_group) {
        return false;
    }

    source->group =
        target_group;

    target->group =
        active_group;

    target_group->tile =
        source;

    active_group->tile =
        target;

    onyrion_layout_apply_group(
        target_group
    );

    onyrion_layout_apply_group(
        active_group
    );

    wlr_log(
        WLR_INFO,
        "Directional move: %s",
        focus_direction_name(direction)
    );

    return true;
}

static OnyrionLayoutNode *find_resize_split(
        OnyrionTile *tile,
        OnyrionSplitOrientation orientation) {
    if (!tile ||
            !tile->node) {
        return NULL;
    }

    OnyrionLayoutNode *node =
        tile->node->parent;

    while (node) {
        if (node->type ==
                    ONYRION_LAYOUT_NODE_SPLIT &&
                node->split.orientation ==
                    orientation) {
            return node;
        }

        node = node->parent;
    }

    return NULL;
}

static bool layout_node_bounds(
        const OnyrionLayoutNode *node,
        struct wlr_box *box) {
    if (!node || !box) {
        return false;
    }

    if (node->type ==
            ONYRION_LAYOUT_NODE_TILE) {
        const OnyrionTile *tile =
            node->leaf.tile;

        if (!tile ||
                tile->width <= 0 ||
                tile->height <= 0) {
            return false;
        }

        *box = (struct wlr_box){
            .x = tile->x,
            .y = tile->y,
            .width = tile->width,
            .height = tile->height,
        };

        return true;
    }

    if (node->type !=
            ONYRION_LAYOUT_NODE_SPLIT) {
        return false;
    }

    struct wlr_box first;
    struct wlr_box second;

    if (!layout_node_bounds(
            node->split.first,
            &first) ||
        !layout_node_bounds(
            node->split.second,
            &second)) {
        return false;
    }

    const int left =
        first.x < second.x
            ? first.x
            : second.x;

    const int top =
        first.y < second.y
            ? first.y
            : second.y;

    const int first_right =
        first.x + first.width;

    const int second_right =
        second.x + second.width;

    const int right =
        first_right > second_right
            ? first_right
            : second_right;

    const int first_bottom =
        first.y + first.height;

    const int second_bottom =
        second.y + second.height;

    const int bottom =
        first_bottom > second_bottom
            ? first_bottom
            : second_bottom;

    *box = (struct wlr_box){
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };

    return
        box->width > 0 &&
        box->height > 0;
}

static uint64_t first_group_id_in_node(
        const OnyrionLayoutNode *node) {
    if (!node) {
        return 0;
    }

    if (node->type ==
            ONYRION_LAYOUT_NODE_TILE) {
        return
            node->leaf.tile &&
                node->leaf.tile->group
                ? node->leaf.tile->group->id
                : 0;
    }

    if (node->type !=
            ONYRION_LAYOUT_NODE_SPLIT) {
        return 0;
    }

    const uint64_t first =
        first_group_id_in_node(
            node->split.first
        );

    return
        first != 0
            ? first
            : first_group_id_in_node(
                node->split.second
            );
}

static void find_resize_border_in_node(
        const OnyrionLayoutNode *node,
        double x,
        double y,
        double tolerance,
        size_t depth,
        double *best_distance,
        size_t *best_depth,
        OnyrionLayoutResizeTarget *best) {
    if (!node ||
            node->type !=
                ONYRION_LAYOUT_NODE_SPLIT) {
        return;
    }

    find_resize_border_in_node(
        node->split.first,
        x,
        y,
        tolerance,
        depth + 1,
        best_distance,
        best_depth,
        best
    );

    find_resize_border_in_node(
        node->split.second,
        x,
        y,
        tolerance,
        depth + 1,
        best_distance,
        best_depth,
        best
    );

    struct wlr_box first;
    struct wlr_box second;

    if (!layout_node_bounds(
            node->split.first,
            &first) ||
        !layout_node_bounds(
            node->split.second,
            &second)) {
        return;
    }

    double distance;
    bool in_span;

    if (node->split.orientation ==
            ONYRION_SPLIT_HORIZONTAL) {
        const double first_edge =
            (double)(
                first.x +
                first.width
            );

        const double second_edge =
            (double)second.x;

        const double boundary =
            (first_edge + second_edge) *
            0.5;

        distance = x - boundary;

        if (distance < 0.0) {
            distance = -distance;
        }

        const int top =
            first.y < second.y
                ? first.y
                : second.y;

        const int bottom =
            first.y + first.height >
                    second.y + second.height
                ? first.y + first.height
                : second.y + second.height;

        in_span =
            y >= (double)top &&
            y < (double)bottom;
    } else {
        const double first_edge =
            (double)(
                first.y +
                first.height
            );

        const double second_edge =
            (double)second.y;

        const double boundary =
            (first_edge + second_edge) *
            0.5;

        distance = y - boundary;

        if (distance < 0.0) {
            distance = -distance;
        }

        const int left =
            first.x < second.x
                ? first.x
                : second.x;

        const int right =
            first.x + first.width >
                    second.x + second.width
                ? first.x + first.width
                : second.x + second.width;

        in_span =
            x >= (double)left &&
            x < (double)right;
    }

    if (!in_span ||
            distance > tolerance) {
        return;
    }

    const double epsilon = 1e-9;

    if (*best_distance >= 0.0 &&
            distance >
                *best_distance +
                    epsilon) {
        return;
    }

    if (*best_distance >= 0.0 &&
            distance >=
                *best_distance -
                    epsilon &&
            depth <= *best_depth) {
        return;
    }

    const uint64_t first_group_id =
        first_group_id_in_node(
            node->split.first
        );

    const uint64_t second_group_id =
        first_group_id_in_node(
            node->split.second
        );

    if (first_group_id == 0 ||
            second_group_id == 0) {
        return;
    }

    *best_distance = distance;
    *best_depth = depth;
    *best = (OnyrionLayoutResizeTarget){
        .first_group_id =
            first_group_id,
        .second_group_id =
            second_group_id,
        .orientation =
            node->split.orientation,
    };
}

bool onyrion_layout_resize_border_at(
        struct onyrion_server *server,
        double x,
        double y,
        double tolerance,
        OnyrionLayoutResizeTarget *target) {
    if (!server ||
            !target ||
            tolerance < 0.0) {
        return false;
    }

    OnyrionWorkspace *workspace =
        onyrion_workspace_focused(
            server
        );

    if (!workspace ||
            !workspace->layout_root) {
        return false;
    }

    double best_distance = -1.0;
    size_t best_depth = 0;
    OnyrionLayoutResizeTarget best = {0};

    find_resize_border_in_node(
        workspace->layout_root,
        x,
        y,
        tolerance,
        0,
        &best_distance,
        &best_depth,
        &best
    );

    if (best_distance < 0.0) {
        return false;
    }

    *target = best;
    return true;
}

bool onyrion_layout_resize_active_target_at(
        struct onyrion_server *server,
        double x,
        double y,
        OnyrionLayoutResizeTarget *target) {
    if (!server ||
            !server->active_group ||
            !server->active_group->tile ||
            !server->active_group->tile->node ||
            !target) {
        return false;
    }

    OnyrionTile *tile =
        server->active_group->tile;

    if (tile->width <= 0 ||
            tile->height <= 0) {
        return false;
    }

    const double center_x =
        (double)tile->x +
        (double)tile->width * 0.5;

    const double center_y =
        (double)tile->y +
        (double)tile->height * 0.5;

    double dx = x - center_x;
    double dy = y - center_y;

    if (dx < 0.0) {
        dx = -dx;
    }

    if (dy < 0.0) {
        dy = -dy;
    }

    const double normalized_x =
        dx / (double)tile->width;

    const double normalized_y =
        dy / (double)tile->height;

    const OnyrionSplitOrientation primary =
        normalized_x >= normalized_y
            ? ONYRION_SPLIT_HORIZONTAL
            : ONYRION_SPLIT_VERTICAL;

    const OnyrionSplitOrientation secondary =
        primary == ONYRION_SPLIT_HORIZONTAL
            ? ONYRION_SPLIT_VERTICAL
            : ONYRION_SPLIT_HORIZONTAL;

    const OnyrionSplitOrientation order[2] = {
        primary,
        secondary,
    };

    for (size_t i = 0; i < 2; i++) {
        OnyrionLayoutNode *split =
            find_resize_split(
                tile,
                order[i]
            );

        if (!split) {
            continue;
        }

        const uint64_t first_group_id =
            first_group_id_in_node(
                split->split.first
            );

        const uint64_t second_group_id =
            first_group_id_in_node(
                split->split.second
            );

        if (first_group_id == 0 ||
                second_group_id == 0) {
            continue;
        }

        *target = (OnyrionLayoutResizeTarget){
            .first_group_id = first_group_id,
            .second_group_id = second_group_id,
            .orientation = split->split.orientation,
        };

        return true;
    }

    return false;
}

static OnyrionGroup *layout_group_by_id(
        OnyrionServer *server,
        uint64_t id) {
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

static bool layout_node_is_ancestor(
        const OnyrionLayoutNode *ancestor,
        const OnyrionLayoutNode *node) {
    while (node) {
        if (node == ancestor) {
            return true;
        }

        node = node->parent;
    }

    return false;
}

static OnyrionLayoutNode *
resolve_resize_target(
        OnyrionServer *server,
        const OnyrionLayoutResizeTarget *target) {
    if (!server || !target) {
        return NULL;
    }

    OnyrionGroup *first_group =
        layout_group_by_id(
            server,
            target->first_group_id
        );

    OnyrionGroup *second_group =
        layout_group_by_id(
            server,
            target->second_group_id
        );

    if (!first_group ||
            !second_group ||
            !first_group->tile ||
            !second_group->tile ||
            !first_group->tile->node ||
            !second_group->tile->node ||
            first_group->workspace !=
                second_group->workspace) {
        return NULL;
    }

    OnyrionLayoutNode *node =
        first_group->tile->node->parent;

    while (node) {
        if (node->type ==
                    ONYRION_LAYOUT_NODE_SPLIT &&
                node->split.orientation ==
                    target->orientation &&
                layout_node_is_ancestor(
                    node->split.first,
                    first_group->tile->node) &&
                layout_node_is_ancestor(
                    node->split.second,
                    second_group->tile->node)) {
            return node;
        }

        node = node->parent;
    }

    return NULL;
}

bool onyrion_layout_resize_target_to_position(
        struct onyrion_server *server,
        const OnyrionLayoutResizeTarget *target,
        double x,
        double y,
        double *ratio_out) {
    OnyrionLayoutNode *split =
        resolve_resize_target(
            server,
            target
        );

    if (!split ||
            !split->workspace ||
            !split->workspace->server) {
        return false;
    }

    struct wlr_box first;
    struct wlr_box second;

    if (!layout_node_bounds(
            split->split.first,
            &first) ||
        !layout_node_bounds(
            split->split.second,
            &second)) {
        return false;
    }

    double next;

    if (split->split.orientation ==
            ONYRION_SPLIT_HORIZONTAL) {
        const int gap =
            second.x -
            (first.x + first.width);

        const int available =
            first.width +
            second.width;

        if (available <= 0) {
            return false;
        }

        next =
            (
                x -
                (double)first.x -
                (double)gap * 0.5
            ) /
            (double)available;
    } else {
        const int gap =
            second.y -
            (first.y + first.height);

        const int available =
            first.height +
            second.height;

        if (available <= 0) {
            return false;
        }

        next =
            (
                y -
                (double)first.y -
                (double)gap * 0.5
            ) /
            (double)available;
    }

    const double minimum =
        server->policy->
            layout.resize_min;

    const double maximum =
        server->policy->
            layout.resize_max;

    if (next < minimum) {
        next = minimum;
    }

    if (next > maximum) {
        next = maximum;
    }

    double previous =
        split->split.ratio;

    if (previous <= 0.0 ||
            previous >= 1.0) {
        previous =
            server->policy->
                layout.initial_split_ratio;
    }

    if (ratio_out) {
        *ratio_out = next;
    }

    const double delta =
        next - previous;

    if (delta < 1e-9 &&
            delta > -1e-9) {
        return true;
    }

    split->split.ratio = next;

    onyrion_layout_reflow_workspace(
        split->workspace
    );

    wlr_log(
        WLR_DEBUG,
        "Pointer border resize:"
        " first_group=%" PRIu64
        " second_group=%" PRIu64
        " orientation=%s"
        " ratio=%.4f",
        target->first_group_id,
        target->second_group_id,
        split_orientation_name(
            split->split.orientation
        ),
        next
    );

    return true;
}

bool onyrion_layout_resize_direction(
        struct onyrion_server *server,
        OnyrionFocusDirection direction) {
    if (!server ||
            !server->active_group ||
            !server->active_group->tile) {
        return false;
    }

    const OnyrionFocusDirectionMeta *meta =
        focus_direction_metadata(direction);

    if (!meta) {
        return false;
    }

    OnyrionGroup *group =
        server->active_group;

    OnyrionLayoutNode *split =
        find_resize_split(
            group->tile,
            meta->orientation
        );

    if (!split) {
        wlr_log(
            WLR_DEBUG,
            "Directional resize ignored: direction=%s no split",
            meta->label
        );

        return false;
    }

    const double step =
        server->policy->
            layout.resize_step;

    const double minimum =
        server->policy->
            layout.resize_min;

    const double maximum =
        server->policy->
            layout.resize_max;

    double ratio =
        split->split.ratio;

    if (ratio <= 0.0 ||
            ratio >= 1.0) {
        ratio =
            server->policy->
                layout.initial_split_ratio;
    }

    double next =
        ratio +
        (double)meta->ratio_sign *
            step;

    if (next < minimum) {
        next = minimum;
    }

    if (next > maximum) {
        next = maximum;
    }

    const double boundary_epsilon =
        1e-12;

    const bool at_limit =
        (meta->ratio_sign < 0 &&
            ratio <= minimum + boundary_epsilon) ||
        (meta->ratio_sign > 0 &&
            ratio >= maximum - boundary_epsilon);

    if (at_limit) {
        wlr_log(
            WLR_DEBUG,
            "Directional resize ignored: direction=%s limit=%.2f",
            meta->label,
            ratio
        );

        return false;
    }

    split->split.ratio =
        next;

    onyrion_layout_reflow_workspace(
        group->workspace
    );

    wlr_log(
        WLR_INFO,
        "Directional resize: direction=%s ratio=%.2f",
        meta->label,
        next
    );

    return true;
}

static void handle_layout_change(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            layout_change
        );

    onyrion_layer_shell_arrange(server);
    onyrion_layout_reflow(server);
}

bool onyrion_layout_add_group(
        struct onyrion_group *group,
        OnyrionSplitOrientation split_orientation) {
    if (!group ||
            !group->workspace ||
            group->tile) {
        return false;
    }

    OnyrionWorkspace *workspace =
        group->workspace;

    if (!workspace->layout_root) {
        OnyrionLayoutNode *leaf =
            create_tile_leaf(group);

        if (!leaf) {
            wlr_log(
                WLR_ERROR,
                "Failed to allocate root tile"
            );
            return false;
        }

        workspace->layout_root =
            leaf;

        onyrion_layout_reflow_workspace(
            workspace
        );

        wlr_log(
            WLR_INFO,
            "Tile created: workspace=%" PRIu64
            " root=leaf",
            workspace->id
        );

        return true;
    }

    OnyrionTile *anchor =
        find_anchor_tile(workspace);

    if (!anchor ||
            !anchor->node) {
        wlr_log(
            WLR_ERROR,
            "Cannot place tile without layout anchor"
        );
        return false;
    }

    OnyrionLayoutNode *anchor_node =
        anchor->node;

    OnyrionLayoutNode *split =
        calloc(1, sizeof(*split));

    if (!split) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate layout split"
        );
        return false;
    }

    OnyrionLayoutNode *leaf =
        create_tile_leaf(group);

    if (!leaf) {
        free(split);

        wlr_log(
            WLR_ERROR,
            "Failed to allocate split tile"
        );
        return false;
    }

    split->type =
        ONYRION_LAYOUT_NODE_SPLIT;

    split->workspace =
        workspace;

    split->split.orientation =
        split_orientation;

    split->split.ratio =
        group->server->policy->
            layout.initial_split_ratio;

    split->split.first =
        anchor_node;

    split->split.second =
        leaf;

    if (!replace_node(
            workspace,
            anchor_node,
            split)) {
        destroy_unattached_leaf(
            leaf
        );

        free(split);

        wlr_log(
            WLR_ERROR,
            "Failed to attach layout split"
        );

        return false;
    }

    anchor_node->parent =
        split;

    leaf->parent =
        split;

    onyrion_layout_reflow_workspace(
        workspace
    );

    wlr_log(
        WLR_INFO,
        "Tile created: workspace=%" PRIu64
        " split=%s depth=%zu",
        workspace->id,
        split_orientation_name(
            split_orientation
        ),
        tree_depth(
            workspace->layout_root
        )
    );

    return true;
}

void onyrion_layout_remove_group(
        struct onyrion_group *group) {
    if (!group ||
            !group->tile ||
            !group->workspace) {
        return;
    }

    OnyrionWorkspace *workspace =
        group->workspace;

    OnyrionTile *tile =
        group->tile;

    OnyrionLayoutNode *node =
        tile->node;

    if (!node ||
            node->type !=
                ONYRION_LAYOUT_NODE_TILE) {
        wlr_log(
            WLR_ERROR,
            "Layout tile has invalid leaf node"
        );
        abort();
    }

    OnyrionLayoutNode *parent =
        node->parent;

    if (!parent) {
        if (workspace->layout_root !=
                node) {
            wlr_log(
                WLR_ERROR,
                "Layout root invariant violated"
            );
            abort();
        }

        workspace->layout_root =
            NULL;
    } else {
        if (parent->type !=
                ONYRION_LAYOUT_NODE_SPLIT) {
            wlr_log(
                WLR_ERROR,
                "Layout parent invariant violated"
            );
            abort();
        }

        OnyrionLayoutNode *sibling;

        if (parent->split.first ==
                node) {
            sibling =
                parent->split.second;
        } else if (parent->split.second ==
                node) {
            sibling =
                parent->split.first;
        } else {
            wlr_log(
                WLR_ERROR,
                "Layout child invariant violated"
            );
            abort();
        }

        if (!sibling ||
                !replace_node(
                    workspace,
                    parent,
                    sibling)) {
            wlr_log(
                WLR_ERROR,
                "Failed to collapse layout split"
            );
            abort();
        }

        free(parent);
    }

    group->tile = NULL;

    wl_list_remove(
        &tile->link
    );

    free(node);
    free(tile);

    onyrion_layout_reflow_workspace(
        workspace
    );

    wlr_log(
        WLR_INFO,
        "Tile destroyed: workspace=%" PRIu64
        " remaining=%zu depth=%zu",
        workspace->id,
        count_tiles(workspace),
        tree_depth(
            workspace->layout_root
        )
    );
}

bool onyrion_layout_move_group_to_workspace(
        struct onyrion_group *group,
        struct onyrion_workspace *target_workspace,
        OnyrionSplitOrientation split_orientation) {
    if (!group ||
            !group->workspace ||
            !group->tile ||
            !group->tile->node ||
            !target_workspace ||
            group->workspace == target_workspace) {
        return false;
    }

    OnyrionWorkspace *source_workspace =
        group->workspace;

    OnyrionTile *tile =
        group->tile;

    OnyrionLayoutNode *leaf =
        tile->node;

    if (leaf->type !=
            ONYRION_LAYOUT_NODE_TILE ||
            leaf->leaf.tile != tile) {
        return false;
    }

    OnyrionLayoutNode *source_parent =
        leaf->parent;

    OnyrionLayoutNode *source_sibling =
        NULL;

    if (!source_parent) {
        if (source_workspace->layout_root !=
                leaf) {
            return false;
        }
    } else {
        if (source_parent->type !=
                ONYRION_LAYOUT_NODE_SPLIT) {
            return false;
        }

        if (source_parent->split.first ==
                leaf) {
            source_sibling =
                source_parent->split.second;
        } else if (source_parent->split.second ==
                leaf) {
            source_sibling =
                source_parent->split.first;
        } else {
            return false;
        }

        if (!source_sibling) {
            return false;
        }
    }

    OnyrionTile *target_anchor =
        NULL;

    OnyrionLayoutNode *target_anchor_node =
        NULL;

    OnyrionLayoutNode *target_parent =
        NULL;

    OnyrionLayoutNode *target_split =
        NULL;

    if (target_workspace->layout_root) {
        target_anchor =
            find_anchor_tile(
                target_workspace
            );

        if (!target_anchor ||
                !target_anchor->node ||
                target_anchor->workspace !=
                    target_workspace) {
            return false;
        }

        target_anchor_node =
            target_anchor->node;

        target_parent =
            target_anchor_node->parent;

        if (!target_parent) {
            if (target_workspace->layout_root !=
                    target_anchor_node) {
                return false;
            }
        } else {
            if (target_parent->type !=
                    ONYRION_LAYOUT_NODE_SPLIT ||
                    (target_parent->split.first !=
                            target_anchor_node &&
                        target_parent->split.second !=
                            target_anchor_node)) {
                return false;
            }
        }

        target_split =
            calloc(
                1,
                sizeof(*target_split)
            );

        if (!target_split) {
            return false;
        }

        target_split->type =
            ONYRION_LAYOUT_NODE_SPLIT;

        target_split->workspace =
            target_workspace;

        target_split->parent =
            target_parent;

        target_split->split.orientation =
            split_orientation;

        target_split->split.ratio =
            group->server->policy->
                layout.initial_split_ratio;

        target_split->split.first =
            target_anchor_node;

        target_split->split.second =
            leaf;
    }

    if (!source_parent) {
        source_workspace->layout_root =
            NULL;
    } else {
        source_sibling->parent =
            source_parent->parent;

        if (!source_parent->parent) {
            source_workspace->layout_root =
                source_sibling;
        } else if (source_parent->parent->
                split.first == source_parent) {
            source_parent->parent->
                split.first =
                    source_sibling;
        } else {
            source_parent->parent->
                split.second =
                    source_sibling;
        }

        free(source_parent);
    }

    wl_list_remove(
        &tile->link
    );

    wl_list_init(
        &tile->link
    );

    group->workspace =
        target_workspace;

    tile->workspace =
        target_workspace;

    leaf->workspace =
        target_workspace;

    leaf->parent =
        NULL;

    wl_list_insert(
        target_workspace->tiles.prev,
        &tile->link
    );

    if (!target_workspace->layout_root) {
        target_workspace->layout_root =
            leaf;
    } else {
        if (!target_parent) {
            target_workspace->layout_root =
                target_split;
        } else if (target_parent->split.first ==
                target_anchor_node) {
            target_parent->split.first =
                target_split;
        } else {
            target_parent->split.second =
                target_split;
        }

        target_anchor_node->parent =
            target_split;

        leaf->parent =
            target_split;
    }

    onyrion_layout_reflow_workspace(
        source_workspace
    );

    onyrion_layout_reflow_workspace(
        target_workspace
    );

    wlr_log(
        WLR_INFO,
        "Group tile moved: group=%" PRIu64
        " workspace=%" PRIu64 "->%" PRIu64,
        group->id,
        source_workspace->id,
        target_workspace->id
    );

    return true;
}

bool onyrion_layout_move_group_relative(
        struct onyrion_group *group,
        struct onyrion_group *target,
        OnyrionFocusDirection direction) {
    if (!group ||
            !target ||
            group == target ||
            !group->workspace ||
            group->workspace != target->workspace ||
            !group->tile ||
            !target->tile ||
            !group->tile->node ||
            !target->tile->node) {
        return false;
    }

    OnyrionLayoutNode *source_leaf =
        group->tile->node;

    OnyrionLayoutNode *target_leaf =
        target->tile->node;

    if (source_leaf->type !=
            ONYRION_LAYOUT_NODE_TILE ||
            target_leaf->type !=
                ONYRION_LAYOUT_NODE_TILE ||
            source_leaf->leaf.tile != group->tile ||
            target_leaf->leaf.tile != target->tile) {
        return false;
    }

    OnyrionSplitOrientation orientation;
    bool source_first;

    switch (direction) {
    case ONYRION_FOCUS_LEFT:
        orientation =
            ONYRION_SPLIT_HORIZONTAL;
        source_first = true;
        break;

    case ONYRION_FOCUS_RIGHT:
        orientation =
            ONYRION_SPLIT_HORIZONTAL;
        source_first = false;
        break;

    case ONYRION_FOCUS_UP:
        orientation =
            ONYRION_SPLIT_VERTICAL;
        source_first = true;
        break;

    case ONYRION_FOCUS_DOWN:
        orientation =
            ONYRION_SPLIT_VERTICAL;
        source_first = false;
        break;

    default:
        return false;
    }

    OnyrionLayoutNode *split =
        calloc(1, sizeof(*split));

    if (!split) {
        return false;
    }

    OnyrionWorkspace *workspace =
        group->workspace;

    OnyrionLayoutNode *source_parent =
        source_leaf->parent;

    if (!source_parent) {
        if (workspace->layout_root !=
                source_leaf) {
            free(split);
            return false;
        }

        workspace->layout_root = NULL;
    } else {
        if (source_parent->type !=
                ONYRION_LAYOUT_NODE_SPLIT) {
            free(split);
            return false;
        }

        OnyrionLayoutNode *sibling;

        if (source_parent->split.first ==
                source_leaf) {
            sibling =
                source_parent->split.second;
        } else if (source_parent->split.second ==
                source_leaf) {
            sibling =
                source_parent->split.first;
        } else {
            free(split);
            return false;
        }

        if (!sibling) {
            free(split);
            return false;
        }

        OnyrionLayoutNode *source_grandparent =
            source_parent->parent;

        if (source_grandparent &&
                (source_grandparent->type !=
                        ONYRION_LAYOUT_NODE_SPLIT ||
                    (source_grandparent->split.first !=
                            source_parent &&
                        source_grandparent->split.second !=
                            source_parent))) {
            free(split);
            return false;
        }

        sibling->parent =
            source_grandparent;

        if (!source_grandparent) {
            workspace->layout_root =
                sibling;
        } else if (source_grandparent->
                split.first == source_parent) {
            source_grandparent->split.first =
                sibling;
        } else {
            source_grandparent->split.second =
                sibling;
        }

        free(source_parent);
    }

    source_leaf->parent = NULL;

    OnyrionLayoutNode *target_parent =
        target_leaf->parent;

    split->type =
        ONYRION_LAYOUT_NODE_SPLIT;
    split->workspace =
        workspace;
    split->parent =
        target_parent;
    split->split.orientation =
        orientation;
    split->split.ratio =
        group->server->policy->
            layout.initial_split_ratio;

    split->split.first =
        source_first
            ? source_leaf
            : target_leaf;

    split->split.second =
        source_first
            ? target_leaf
            : source_leaf;

    if (!target_parent) {
        if (workspace->layout_root !=
                target_leaf) {
            abort();
        }

        workspace->layout_root =
            split;
    } else if (target_parent->split.first ==
            target_leaf) {
        target_parent->split.first =
            split;
    } else if (target_parent->split.second ==
            target_leaf) {
        target_parent->split.second =
            split;
    } else {
        abort();
    }

    source_leaf->parent = split;
    target_leaf->parent = split;

    onyrion_layout_reflow_workspace(
        workspace
    );

    wlr_log(
        WLR_INFO,
        "Group tile repositioned:"
        " group=%" PRIu64
        " target=%" PRIu64
        " direction=%s",
        group->id,
        target->id,
        focus_direction_meta[direction].label
    );

    return true;
}

bool onyrion_layout_flip_active_split(
        struct onyrion_server *server) {
    if (!server ||
            !server->active_group ||
            !server->active_group->tile ||
            !server->active_group->tile->node) {
        return false;
    }

    OnyrionGroup *group =
        server->active_group;

    OnyrionLayoutNode *split =
        group->tile->node->parent;

    if (!split ||
            split->type !=
                ONYRION_LAYOUT_NODE_SPLIT) {
        wlr_log(
            WLR_DEBUG,
            "Split orientation flip ignored: no parent split"
        );

        return false;
    }

    const OnyrionSplitOrientation previous =
        split->split.orientation;

    switch (previous) {
    case ONYRION_SPLIT_HORIZONTAL:
        split->split.orientation =
            ONYRION_SPLIT_VERTICAL;
        break;

    case ONYRION_SPLIT_VERTICAL:
        split->split.orientation =
            ONYRION_SPLIT_HORIZONTAL;
        break;

    default:
        wlr_log(
            WLR_ERROR,
            "Split orientation flip failed: invalid orientation"
        );

        return false;
    }

    onyrion_layout_reflow_workspace(
        group->workspace
    );

    wlr_log(
        WLR_INFO,
        "Split orientation flipped: %s -> %s ratio=%.2f",
        split_orientation_name(previous),
        split_orientation_name(
            split->split.orientation
        ),
        split->split.ratio
    );

    return true;
}

bool onyrion_layout_split_active(
        struct onyrion_server *server,
        OnyrionSplitOrientation split_orientation) {
    OnyrionGroup *source =
        server->active_group;

    if (!source ||
            !source->active ||
            source->window_count < 2) {
        wlr_log(
            WLR_DEBUG,
            "Split ignored: active group has fewer than two windows"
        );
        return false;
    }

    OnyrionWindow *window =
        source->active;

    OnyrionGroup *target =
        onyrion_group_create(
            server,
            split_orientation
        );

    if (!target) {
        wlr_log(
            WLR_ERROR,
            "Split failed: cannot create target group"
        );
        return false;
    }

    onyrion_group_move_window(
        window,
        target
    );

    onyrion_layout_reflow_workspace(
        target->workspace
    );

    wlr_log(
        WLR_INFO,
        "Split active window into new %s tile",
        split_orientation_name(
            split_orientation
        )
    );

    return true;
}

bool onyrion_layout_init(
        struct onyrion_server *server) {
    server->layout_change.notify =
        handle_layout_change;

    wl_signal_add(
        &server->output_layout->events.change,
        &server->layout_change
    );

    return true;
}

void onyrion_layout_finish(
        struct onyrion_server *server) {
    if (server->layout_change.notify) {
        wl_list_remove(
            &server->layout_change.link
        );

        server->layout_change.notify =
            NULL;
    }
}
