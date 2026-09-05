#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct onyrion_group;
struct onyrion_layout_node;
struct onyrion_output;
struct onyrion_server;
struct onyrion_workspace_policy_set;
struct wlr_scene_tree;

typedef struct onyrion_workspace {
    struct onyrion_server *server;

    uint64_t id;
    char name[64];
    char icon[64];
    bool persistent;
    bool startup;
    char output_affinity[128];

    struct onyrion_output *output;
    struct wl_list output_link;
    struct wl_list link;

    /*
     * Tile registry for enumeration/counting.
     * Geometry is authoritative in layout_root.
     */
    struct wl_list tiles;
    struct onyrion_layout_node *layout_root;

    struct wlr_scene_tree *scene_tree;
    struct onyrion_group *active_group;
} OnyrionWorkspace;

[[nodiscard]]
bool onyrion_workspace_init(
    struct onyrion_server *server
);

void onyrion_workspace_finish(
    struct onyrion_server *server
);

void onyrion_workspace_output_added(
    struct onyrion_server *server,
    struct onyrion_output *output
);

void onyrion_workspace_output_removed(
    struct onyrion_server *server,
    struct onyrion_output *output
);

[[nodiscard]]
OnyrionWorkspace *onyrion_workspace_find_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
OnyrionWorkspace *onyrion_workspace_focused(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_workspace_is_visible(
    const OnyrionWorkspace *workspace
);

[[nodiscard]]
bool onyrion_workspace_next(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_workspace_previous(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_workspace_activate_id(
    struct onyrion_server *server,
    uint64_t id
);

[[nodiscard]]
bool onyrion_workspace_policy_validate_runtime(
    struct onyrion_server *server,
    const struct onyrion_workspace_policy_set *policy,
    char *error,
    size_t error_size
);

void onyrion_workspace_policy_apply_runtime(
    struct onyrion_server *server,
    const struct onyrion_workspace_policy_set *policy
);
