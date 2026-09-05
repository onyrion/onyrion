#include "workspace.h"

#include "config.h"
#include "group.h"
#include "input.h"
#include "layout.h"
#include "output.h"
#include "server.h"
#include "shell_protocol.h"
#include "window.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void apply_workspace_metadata(
        OnyrionWorkspace *workspace,
        const OnyrionWorkspacePolicy *policy) {
    if (!workspace) {
        return;
    }

    workspace->icon[0] = '\0';
    workspace->persistent = false;
    workspace->startup = false;
    workspace->output_affinity[0] = '\0';

    if (!policy) {
        snprintf(
            workspace->name,
            sizeof(workspace->name),
            "%" PRIu64,
            workspace->id
        );
        return;
    }

    if (policy->name[0] != '\0') {
        snprintf(
            workspace->name,
            sizeof(workspace->name),
            "%s",
            policy->name
        );
    } else {
        snprintf(
            workspace->name,
            sizeof(workspace->name),
            "%" PRIu64,
            workspace->id
        );
    }

    snprintf(
        workspace->icon,
        sizeof(workspace->icon),
        "%s",
        policy->icon
    );

    workspace->persistent =
        policy->persistent;

    workspace->startup =
        policy->startup;

    snprintf(
        workspace->output_affinity,
        sizeof(workspace->output_affinity),
        "%s",
        policy->output_affinity
    );
}

static OnyrionWorkspace *create_workspace_with_policy(
        OnyrionServer *server,
        uint64_t id,
        const OnyrionWorkspacePolicy *policy) {
    OnyrionWorkspace *workspace =
        calloc(1, sizeof(*workspace));

    if (!workspace) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate OnyrionWorkspace"
        );
        return NULL;
    }

    workspace->server = server;

    if (id == 0) {
        workspace->id =
            server->next_workspace_id++;
    } else {
        workspace->id = id;

        if (id >= server->next_workspace_id) {
            server->next_workspace_id =
                id + 1;
        }
    }

    apply_workspace_metadata(
        workspace,
        policy
    );

    wl_list_init(&workspace->output_link);
    wl_list_init(&workspace->tiles);

    workspace->scene_tree =
        wlr_scene_tree_create(
            server->scene_content
        );

    if (!workspace->scene_tree) {
        wlr_log(
            WLR_ERROR,
            "Failed to create workspace scene tree"
        );

        free(workspace);
        return NULL;
    }

    wlr_scene_node_set_enabled(
        &workspace->scene_tree->node,
        false
    );

    wl_list_insert(
        server->workspaces.prev,
        &workspace->link
    );

    wlr_log(
        WLR_INFO,
        "Workspace created: id=%" PRIu64
        " name=%s affinity=%s startup=%u persistent=%u",
        workspace->id,
        workspace->name,
        workspace->output_affinity[0]
            ? workspace->output_affinity
            : "<none>",
        workspace->startup,
        workspace->persistent
    );

    return workspace;
}

static OnyrionWorkspace *create_workspace(
        OnyrionServer *server) {
    return create_workspace_with_policy(
        server,
        0,
        NULL
    );
}

OnyrionWorkspace *onyrion_workspace_find_id(
        OnyrionServer *server,
        uint64_t id) {
    if (!server || id == 0) {
        return NULL;
    }

    OnyrionWorkspace *workspace;

    wl_list_for_each(
            workspace,
            &server->workspaces,
            link) {
        if (workspace->id == id) {
            return workspace;
        }
    }

    return NULL;
}

static const OnyrionWorkspacePolicy *find_workspace_policy(
        const OnyrionWorkspacePolicySet *policy,
        uint64_t id) {
    if (!policy || id == 0) {
        return NULL;
    }

    for (size_t i = 0;
            i < policy->count;
            i++) {
        if (policy->items[i].id == id) {
            return &policy->items[i];
        }
    }

    return NULL;
}

static OnyrionOutput *find_output_by_name(
        OnyrionServer *server,
        const char *name) {
    if (!server ||
            !name ||
            name[0] == '\0') {
        return NULL;
    }

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        if (output->wlr_output &&
                output->wlr_output->name &&
                strcmp(
                    output->wlr_output->name,
                    name) == 0) {
            return output;
        }
    }

    return NULL;
}

static OnyrionOutput *workspace_policy_target_output(
        OnyrionServer *server,
        OnyrionWorkspace *workspace,
        const OnyrionWorkspacePolicy *policy) {
    if (!server || !workspace || !policy) {
        return workspace
            ? workspace->output
            : NULL;
    }

    if (policy->output_affinity[0] != '\0') {
        return find_output_by_name(
            server,
            policy->output_affinity
        );
    }

    if (workspace->output) {
        return workspace->output;
    }

    if (server->focused_output) {
        return server->focused_output;
    }

    return onyrion_output_first(server);
}

static OnyrionWorkspace *find_unassigned_workspace(
        OnyrionServer *server) {
    OnyrionWorkspace *workspace;

    wl_list_for_each(
            workspace,
            &server->workspaces,
            link) {
        if (!workspace->output &&
                workspace->output_affinity[0] == '\0') {
            return workspace;
        }
    }

    return NULL;
}

static void assign_workspace(
        OnyrionOutput *output,
        OnyrionWorkspace *workspace) {
    if (!output || !workspace || workspace->output) {
        return;
    }

    workspace->output = output;

    wl_list_insert(
        output->assigned_workspaces.prev,
        &workspace->output_link
    );

    wlr_log(
        WLR_INFO,
        "Workspace assigned: id=%" PRIu64 " output=%s",
        workspace->id,
        output->wlr_output && output->wlr_output->name
            ? output->wlr_output->name
            : "<unnamed>"
    );
}

static void focus_visible_workspace(
        OnyrionServer *server,
        OnyrionOutput *output) {
    if (!server || !output || !output->visible_workspace) {
        return;
    }

    OnyrionWorkspace *workspace =
        output->visible_workspace;

    onyrion_output_focus(server, output);

    if (workspace->active_group &&
            workspace->active_group->active &&
            workspace->active_group->active->mapped) {
        onyrion_group_focus_window(
            workspace->active_group->active
        );
        return;
    }

    if (server->active_group &&
            server->active_group->active) {
        wlr_xdg_toplevel_set_activated(
            server->active_group->active->toplevel,
            false
        );
    }

    server->active_group =
        workspace->active_group;

    onyrion_input_focus_window(
        server,
        NULL
    );
}

static bool switch_output_workspace(
        OnyrionServer *server,
        OnyrionOutput *output,
        OnyrionWorkspace *target,
        bool focus_output) {
    if (!server ||
            !output ||
            !target ||
            target->output != output) {
        return false;
    }

    OnyrionWorkspace *previous =
        output->visible_workspace;

    if (previous == target) {
        if (focus_output) {
            focus_visible_workspace(
                server,
                output
            );
        }

        return true;
    }

    if (previous && previous->scene_tree) {
        wlr_scene_node_set_enabled(
            &previous->scene_tree->node,
            false
        );
    }

    output->visible_workspace = target;

    wlr_scene_node_set_enabled(
        &target->scene_tree->node,
        true
    );

    onyrion_layout_reflow_workspace(target);

    if (focus_output) {
        onyrion_input_clear_pointer_focus(server);
        focus_visible_workspace(server, output);
    }

    wlr_log(
        WLR_INFO,
        "Workspace visible: id=%" PRIu64 " output=%s",
        target->id,
        output->wlr_output && output->wlr_output->name
            ? output->wlr_output->name
            : "<unnamed>"
    );

    onyrion_shell_protocol_mark_changed(server);

    return true;
}

bool onyrion_workspace_init(
        struct onyrion_server *server) {
    wl_list_init(&server->workspaces);
    server->next_workspace_id = 1;
    server->focused_output = NULL;

    const OnyrionWorkspacePolicySet *policy =
        server->policy
            ? &server->policy->workspaces
            : NULL;

    if (!policy || policy->count == 0) {
        return create_workspace(server) != NULL;
    }

    for (size_t i = 0;
            i < policy->count;
            i++) {
        if (!create_workspace_with_policy(
                server,
                policy->items[i].id,
                &policy->items[i])) {
            return false;
        }
    }

    return true;
}

void onyrion_workspace_output_added(
        struct onyrion_server *server,
        struct onyrion_output *output) {
    if (!server ||
            !output ||
            !output->wlr_output) {
        return;
    }

    const char *output_name =
        output->wlr_output->name
            ? output->wlr_output->name
            : "";

    const bool first_output =
        server->focused_output == NULL;

    OnyrionWorkspace *workspace;

    wl_list_for_each(
            workspace,
            &server->workspaces,
            link) {
        if (workspace->output ||
                workspace->output_affinity[0] == '\0' ||
                strcmp(
                    workspace->output_affinity,
                    output_name) != 0) {
            continue;
        }

        assign_workspace(
            output,
            workspace
        );
    }

    if (first_output) {
        wl_list_for_each(
                workspace,
                &server->workspaces,
                link) {
            if (!workspace->output &&
                    workspace->output_affinity[0] == '\0') {
                assign_workspace(
                    output,
                    workspace
                );
            }
        }
    }

    if (wl_list_empty(
            &output->assigned_workspaces)) {
        workspace =
            find_unassigned_workspace(
                server
            );

        if (!workspace) {
            workspace =
                create_workspace(server);
        }

        if (!workspace) {
            return;
        }

        assign_workspace(
            output,
            workspace
        );
    }

    OnyrionWorkspace *first =
        NULL;

    OnyrionWorkspace *startup =
        NULL;

    wl_list_for_each(
            workspace,
            &output->assigned_workspaces,
            output_link) {
        if (!first) {
            first = workspace;
        }

        if (workspace->startup) {
            startup = workspace;
            break;
        }
    }

    OnyrionWorkspace *target =
        startup
            ? startup
            : first;

    if (!target) {
        return;
    }

    (void)switch_output_workspace(
        server,
        output,
        target,
        first_output
    );
}

void onyrion_workspace_output_removed(
        struct onyrion_server *server,
        struct onyrion_output *output) {
    if (!server || !output) {
        return;
    }

    if (output->visible_workspace &&
            output->visible_workspace->scene_tree) {
        wlr_scene_node_set_enabled(
            &output->visible_workspace->scene_tree->node,
            false
        );
    }

    OnyrionWorkspace *workspace;
    OnyrionWorkspace *tmp;

    wl_list_for_each_safe(
            workspace,
            tmp,
            &output->assigned_workspaces,
            output_link) {
        wl_list_remove(&workspace->output_link);
        wl_list_init(&workspace->output_link);
        workspace->output = NULL;
    }

    output->visible_workspace = NULL;

    if (server->focused_output == output) {
        server->focused_output = NULL;

        OnyrionOutput *fallback =
            onyrion_output_first(server);

        if (fallback && fallback != output) {
            focus_visible_workspace(
                server,
                fallback
            );
        } else {
            server->active_group = NULL;
            onyrion_input_focus_window(
                server,
                NULL
            );
        }
    }

    onyrion_shell_protocol_mark_changed(server);
}

OnyrionWorkspace *onyrion_workspace_focused(
        struct onyrion_server *server) {
    if (!server || !server->focused_output) {
        return NULL;
    }

    return server->focused_output->visible_workspace;
}

bool onyrion_workspace_is_visible(
        const OnyrionWorkspace *workspace) {
    return workspace &&
        workspace->output &&
        workspace->output->visible_workspace == workspace;
}

bool onyrion_workspace_next(
        struct onyrion_server *server) {
    OnyrionOutput *output =
        server ? server->focused_output : NULL;

    OnyrionWorkspace *current =
        onyrion_workspace_focused(server);

    if (!output || !current) {
        return false;
    }

    struct wl_list *next =
        current->output_link.next;

    OnyrionWorkspace *target = NULL;

    if (next == &output->assigned_workspaces) {
        target = create_workspace(server);

        if (!target) {
            return false;
        }

        assign_workspace(output, target);
    } else {
        target =
            wl_container_of(
                next,
                target,
                output_link
            );
    }

    return switch_output_workspace(
        server,
        output,
        target,
        true
    );
}

bool onyrion_workspace_previous(
        struct onyrion_server *server) {
    OnyrionOutput *output =
        server ? server->focused_output : NULL;

    OnyrionWorkspace *current =
        onyrion_workspace_focused(server);

    if (!output || !current) {
        return false;
    }

    struct wl_list *previous =
        current->output_link.prev;

    if (previous == &output->assigned_workspaces) {
        return false;
    }

    OnyrionWorkspace *target =
        wl_container_of(
            previous,
            target,
            output_link
        );

    return switch_output_workspace(
        server,
        output,
        target,
        true
    );
}

bool onyrion_workspace_activate_id(
        struct onyrion_server *server,
        uint64_t id) {
    OnyrionWorkspace *target =
        onyrion_workspace_find_id(
            server,
            id
        );

    if (!target || !target->output) {
        return false;
    }

    return switch_output_workspace(
        server,
        target->output,
        target,
        true
    );
}

bool onyrion_workspace_policy_validate_runtime(
        struct onyrion_server *server,
        const struct onyrion_workspace_policy_set *policy,
        char *error,
        size_t error_size) {
    if (!server ||
            !server->policy ||
            !policy) {
        if (error && error_size > 0) {
            snprintf(
                error,
                error_size,
                "invalid workspace runtime policy"
            );
        }

        return false;
    }

    const OnyrionWorkspacePolicySet *current =
        &server->policy->workspaces;

    if (current->count != policy->count) {
        if (error && error_size > 0) {
            snprintf(
                error,
                error_size,
                "workspace declaration set change requires restart"
            );
        }

        return false;
    }

    for (size_t i = 0;
            i < current->count;
            i++) {
        if (!find_workspace_policy(
                policy,
                current->items[i].id)) {
            if (error && error_size > 0) {
                snprintf(
                    error,
                    error_size,
                    "workspace declaration set change requires restart"
                );
            }

            return false;
        }
    }

    for (size_t i = 0;
            i < policy->count;
            i++) {
        if (!onyrion_workspace_find_id(
                server,
                policy->items[i].id)) {
            if (error && error_size > 0) {
                snprintf(
                    error,
                    error_size,
                    "workspace %" PRIu64
                    " is not live; restart required",
                    policy->items[i].id
                );
            }

            return false;
        }
    }

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        size_t target_count = 0;

        OnyrionWorkspace *workspace;

        wl_list_for_each(
                workspace,
                &server->workspaces,
                link) {
            const OnyrionWorkspacePolicy *candidate =
                find_workspace_policy(
                    policy,
                    workspace->id
                );

            OnyrionOutput *target =
                candidate
                    ? workspace_policy_target_output(
                        server,
                        workspace,
                        candidate
                    )
                    : workspace->output;

            if (target == output) {
                target_count++;
            }
        }

        if (target_count == 0) {
            if (error && error_size > 0) {
                snprintf(
                    error,
                    error_size,
                    "workspace policy would leave output '%s' empty",
                    output->wlr_output &&
                            output->wlr_output->name
                        ? output->wlr_output->name
                        : "<unnamed>"
                );
            }

            return false;
        }
    }

    return true;
}

void onyrion_workspace_policy_apply_runtime(
        struct onyrion_server *server,
        const struct onyrion_workspace_policy_set *policy) {
    if (!server || !policy) {
        return;
    }

    for (size_t i = 0;
            i < policy->count;
            i++) {
        const OnyrionWorkspacePolicy *candidate =
            &policy->items[i];

        OnyrionWorkspace *workspace =
            onyrion_workspace_find_id(
                server,
                candidate->id
            );

        if (!workspace) {
            continue;
        }

        OnyrionOutput *target =
            workspace_policy_target_output(
                server,
                workspace,
                candidate
            );

        if (workspace->output == target) {
            continue;
        }

        OnyrionOutput *previous =
            workspace->output;

        if (previous &&
                previous->visible_workspace ==
                    workspace) {
            if (workspace->scene_tree) {
                wlr_scene_node_set_enabled(
                    &workspace->scene_tree->node,
                    false
                );
            }

            previous->visible_workspace =
                NULL;
        }

        if (workspace->output) {
            wl_list_remove(
                &workspace->output_link
            );

            wl_list_init(
                &workspace->output_link
            );

            workspace->output = NULL;
        }

        if (target) {
            assign_workspace(
                target,
                workspace
            );
        }
    }

    for (size_t i = 0;
            i < policy->count;
            i++) {
        OnyrionWorkspace *workspace =
            onyrion_workspace_find_id(
                server,
                policy->items[i].id
            );

        if (!workspace) {
            continue;
        }

        apply_workspace_metadata(
            workspace,
            &policy->items[i]
        );
    }

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        if (output->visible_workspace &&
                output->visible_workspace->output ==
                    output) {
            continue;
        }

        if (wl_list_empty(
                &output->assigned_workspaces)) {
            continue;
        }

        OnyrionWorkspace *replacement =
            wl_container_of(
                output->assigned_workspaces.next,
                replacement,
                output_link
            );

        (void)switch_output_workspace(
            server,
            output,
            replacement,
            output == server->focused_output
        );
    }

    wlr_log(
        WLR_INFO,
        "Workspace policy applied:"
        " declarations=%zu",
        policy->count
    );

    onyrion_shell_protocol_mark_changed(
        server
    );
}

void onyrion_workspace_finish(
        struct onyrion_server *server) {
    OnyrionWorkspace *workspace;
    OnyrionWorkspace *tmp;

    wl_list_for_each_safe(
            workspace,
            tmp,
            &server->workspaces,
            link) {
        if (!wl_list_empty(&workspace->tiles)) {
            wlr_log(
                WLR_ERROR,
                "Workspace id=%" PRIu64
                " still owns tiles during finish",
                workspace->id
            );
        }

        if (workspace->layout_root) {
            wlr_log(
                WLR_ERROR,
                "Workspace id=%" PRIu64
                " still owns layout tree during finish",
                workspace->id
            );
        }

        if (workspace->output) {
            if (workspace->output->visible_workspace ==
                    workspace) {
                workspace->output->visible_workspace =
                    NULL;
            }

            wl_list_remove(&workspace->output_link);
            wl_list_init(&workspace->output_link);
            workspace->output = NULL;
        }

        wl_list_remove(&workspace->link);

        if (workspace->scene_tree) {
            wlr_scene_node_destroy(
                &workspace->scene_tree->node
            );

            workspace->scene_tree = NULL;
        }

        free(workspace);
    }

    server->focused_output = NULL;
}
