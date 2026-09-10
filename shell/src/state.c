#include "shell_state.h"

#include <stdlib.h>
#include <string.h>

static bool nonempty(const char *value) {
    return value && value[0] != '\0';
}

static bool grow_array(
        void **items,
        size_t *capacity,
        size_t needed,
        size_t item_size) {
    if (*capacity >= needed) {
        return true;
    }

    size_t next = *capacity ? *capacity * 2 : 8;
    while (next < needed) {
        next *= 2;
    }

    void *grown = realloc(*items, next * item_size);
    if (!grown) {
        return false;
    }

    *items = grown;
    *capacity = next;
    return true;
}

static char *copy_string(const char *value) {
    if (!value) {
        return NULL;
    }
    return strdup(value);
}

static void output_finish(ShellOutputState *output) {
    free(output->name);
    free(output->visible_workspace_id);
    *output = (ShellOutputState){0};
}

static void workspace_finish(ShellWorkspaceState *workspace) {
    free(workspace->id);
    free(workspace->output_name);
    free(workspace->name);
    free(workspace->icon);
    free(workspace->output_affinity);
    *workspace = (ShellWorkspaceState){0};
}

static void group_finish(ShellGroupState *group) {
    free(group->id);
    free(group->workspace_id);
    free(group->pinned_output_name);
    *group = (ShellGroupState){0};
}

static void window_finish(ShellWindowState *window) {
    free(window->id);
    free(window->group_id);
    free(window->app_id);
    free(window->title);
    free(window->workspace_id);
    free(window->parent_window_id);
    *window = (ShellWindowState){0};
}

static void snapshot_clear(ShellSnapshot *snapshot) {
    for (size_t i = 0; i < snapshot->output_count; i++) {
        output_finish(&snapshot->outputs[i]);
    }
    for (size_t i = 0; i < snapshot->workspace_count; i++) {
        workspace_finish(&snapshot->workspaces[i]);
    }
    for (size_t i = 0; i < snapshot->group_count; i++) {
        group_finish(&snapshot->groups[i]);
    }
    for (size_t i = 0; i < snapshot->window_count; i++) {
        window_finish(&snapshot->windows[i]);
    }

    free(snapshot->outputs);
    free(snapshot->workspaces);
    free(snapshot->groups);
    free(snapshot->windows);
    *snapshot = (ShellSnapshot){0};
}

static ShellOutputState *find_output_mut(
        ShellSnapshot *snapshot,
        const char *name) {
    for (size_t i = 0; i < snapshot->output_count; i++) {
        if (strcmp(snapshot->outputs[i].name, name) == 0) {
            return &snapshot->outputs[i];
        }
    }
    return NULL;
}

static const ShellOutputState *find_output(
        const ShellSnapshot *snapshot,
        const char *name) {
    return find_output_mut((ShellSnapshot *)snapshot, name);
}

static ShellWorkspaceState *find_workspace_mut(
        ShellSnapshot *snapshot,
        const char *id) {
    for (size_t i = 0; i < snapshot->workspace_count; i++) {
        if (strcmp(snapshot->workspaces[i].id, id) == 0) {
            return &snapshot->workspaces[i];
        }
    }
    return NULL;
}

static const ShellWorkspaceState *find_workspace(
        const ShellSnapshot *snapshot,
        const char *id) {
    return find_workspace_mut((ShellSnapshot *)snapshot, id);
}

static ShellGroupState *find_group_mut(
        ShellSnapshot *snapshot,
        const char *id) {
    for (size_t i = 0; i < snapshot->group_count; i++) {
        if (strcmp(snapshot->groups[i].id, id) == 0) {
            return &snapshot->groups[i];
        }
    }
    return NULL;
}

static const ShellGroupState *find_group(
        const ShellSnapshot *snapshot,
        const char *id) {
    return find_group_mut((ShellSnapshot *)snapshot, id);
}

static ShellWindowState *find_window_mut(
        ShellSnapshot *snapshot,
        const char *id) {
    for (size_t i = 0; i < snapshot->window_count; i++) {
        if (strcmp(snapshot->windows[i].id, id) == 0) {
            return &snapshot->windows[i];
        }
    }
    return NULL;
}

static const ShellWindowState *find_window(
        const ShellSnapshot *snapshot,
        const char *id) {
    return find_window_mut((ShellSnapshot *)snapshot, id);
}

void shell_state_init(ShellState *state) {
    *state = (ShellState){0};
}

void shell_state_finish(ShellState *state) {
    snapshot_clear(&state->committed);
    snapshot_clear(&state->staging);
    state->snapshot_in_progress = false;
}

bool shell_state_begin(
        ShellState *state,
        uint32_t generation,
        uint32_t workspace_count) {
    if (state->snapshot_in_progress) {
        return false;
    }

    snapshot_clear(&state->staging);
    state->staging.generation = generation;
    state->staging.expected_workspace_count = workspace_count;
    state->snapshot_in_progress = true;
    return true;
}

bool shell_state_add_output(
        ShellState *state,
        const char *name,
        bool focused,
        const char *visible_workspace_id,
        uint32_t assigned_workspace_count) {
    if (!state->snapshot_in_progress ||
            !nonempty(name) ||
            visible_workspace_id == NULL ||
            find_output_mut(&state->staging, name)) {
        return false;
    }

    if (!grow_array(
            (void **)&state->staging.outputs,
            &state->staging.output_capacity,
            state->staging.output_count + 1,
            sizeof(*state->staging.outputs))) {
        return false;
    }

    char *name_copy = copy_string(name);
    char *visible_copy = copy_string(visible_workspace_id);
    if (!name_copy || !visible_copy) {
        free(name_copy);
        free(visible_copy);
        return false;
    }

    state->staging.outputs[state->staging.output_count++] =
        (ShellOutputState){
            .name = name_copy,
            .focused = focused,
            .visible_workspace_id = visible_copy,
            .assigned_workspace_count = assigned_workspace_count,
        };
    return true;
}

bool shell_state_add_workspace(
        ShellState *state,
        const char *id,
        bool active,
        uint32_t group_count,
        uint32_t tile_count,
        uint32_t window_count) {
    if (!state->snapshot_in_progress ||
            !nonempty(id) ||
            state->staging.workspace_count >=
                state->staging.expected_workspace_count ||
            find_workspace_mut(&state->staging, id)) {
        return false;
    }

    if (!grow_array(
            (void **)&state->staging.workspaces,
            &state->staging.workspace_capacity,
            state->staging.workspace_count + 1,
            sizeof(*state->staging.workspaces))) {
        return false;
    }

    char *id_copy = copy_string(id);
    if (!id_copy) {
        return false;
    }

    state->staging.workspaces[state->staging.workspace_count++] =
        (ShellWorkspaceState){
            .id = id_copy,
            .active = active,
            .group_count = group_count,
            .tile_count = tile_count,
            .window_count = window_count,
        };
    return true;
}

bool shell_state_set_workspace_output(
        ShellState *state,
        const char *workspace_id,
        const char *output_name,
        bool visible) {
    if (!state->snapshot_in_progress ||
            !nonempty(workspace_id) ||
            output_name == NULL) {
        return false;
    }

    ShellWorkspaceState *workspace =
        find_workspace_mut(&state->staging, workspace_id);
    if (!workspace || workspace->output_seen) {
        return false;
    }

    workspace->output_name = copy_string(output_name);
    if (!workspace->output_name) {
        return false;
    }

    workspace->visible = visible;
    workspace->output_seen = true;
    return true;
}

bool shell_state_set_workspace_metadata(
        ShellState *state,
        const char *workspace_id,
        const char *name,
        const char *icon,
        bool persistent,
        bool startup,
        const char *output_affinity) {
    if (!state->snapshot_in_progress ||
            !nonempty(workspace_id) ||
            name == NULL ||
            icon == NULL ||
            output_affinity == NULL) {
        return false;
    }

    ShellWorkspaceState *workspace =
        find_workspace_mut(&state->staging, workspace_id);
    if (!workspace || workspace->metadata_seen) {
        return false;
    }

    workspace->name = copy_string(name);
    workspace->icon = copy_string(icon);
    workspace->output_affinity = copy_string(output_affinity);

    if (!workspace->name ||
            !workspace->icon ||
            !workspace->output_affinity) {
        free(workspace->name);
        free(workspace->icon);
        free(workspace->output_affinity);
        workspace->name = NULL;
        workspace->icon = NULL;
        workspace->output_affinity = NULL;
        return false;
    }

    workspace->persistent = persistent;
    workspace->startup = startup;
    workspace->metadata_seen = true;
    return true;
}

bool shell_state_add_group(
        ShellState *state,
        const char *id,
        const char *workspace_id,
        bool active,
        uint32_t window_count) {
    if (!state->snapshot_in_progress ||
            !nonempty(id) ||
            !nonempty(workspace_id) ||
            find_group_mut(&state->staging, id)) {
        return false;
    }

    if (!grow_array(
            (void **)&state->staging.groups,
            &state->staging.group_capacity,
            state->staging.group_count + 1,
            sizeof(*state->staging.groups))) {
        return false;
    }

    char *id_copy = copy_string(id);
    char *workspace_copy = copy_string(workspace_id);
    if (!id_copy || !workspace_copy) {
        free(id_copy);
        free(workspace_copy);
        return false;
    }

    state->staging.groups[state->staging.group_count++] =
        (ShellGroupState){
            .id = id_copy,
            .workspace_id = workspace_copy,
            .active = active,
            .window_count = window_count,
            .placement = SHELL_GROUP_PLACEMENT_UNKNOWN,
        };
    return true;
}

bool shell_state_set_group_placement(
        ShellState *state,
        const char *group_id,
        ShellGroupPlacement placement,
        bool pinned,
        const char *pinned_output_name) {
    if (!state->snapshot_in_progress ||
            !nonempty(group_id) ||
            pinned_output_name == NULL ||
            (placement != SHELL_GROUP_PLACEMENT_TILED &&
             placement != SHELL_GROUP_PLACEMENT_FLOATING)) {
        return false;
    }

    if ((placement == SHELL_GROUP_PLACEMENT_TILED && pinned) ||
            (pinned && !nonempty(pinned_output_name)) ||
            (!pinned && pinned_output_name[0] != '\0')) {
        return false;
    }

    ShellGroupState *group =
        find_group_mut(&state->staging, group_id);
    if (!group || group->placement_seen) {
        return false;
    }

    group->pinned_output_name = copy_string(pinned_output_name);
    if (!group->pinned_output_name) {
        return false;
    }

    group->placement = placement;
    group->pinned = pinned;
    group->placement_seen = true;
    return true;
}

const char *shell_group_placement_name(
        ShellGroupPlacement placement) {
    switch (placement) {
    case SHELL_GROUP_PLACEMENT_TILED:
        return "tiled";
    case SHELL_GROUP_PLACEMENT_FLOATING:
        return "floating";
    case SHELL_GROUP_PLACEMENT_UNKNOWN:
        break;
    }
    return "unknown";
}

bool shell_state_add_window(
        ShellState *state,
        const char *id,
        const char *group_id,
        bool active,
        bool fullscreen,
        const char *app_id,
        const char *title) {
    if (!state->snapshot_in_progress ||
            !nonempty(id) ||
            group_id == NULL ||
            app_id == NULL ||
            title == NULL ||
            find_window_mut(&state->staging, id)) {
        return false;
    }

    if (!grow_array(
            (void **)&state->staging.windows,
            &state->staging.window_capacity,
            state->staging.window_count + 1,
            sizeof(*state->staging.windows))) {
        return false;
    }

    char *id_copy = copy_string(id);
    char *group_copy = copy_string(group_id);
    char *app_copy = copy_string(app_id);
    char *title_copy = copy_string(title);

    if (!id_copy || !group_copy || !app_copy || !title_copy) {
        free(id_copy);
        free(group_copy);
        free(app_copy);
        free(title_copy);
        return false;
    }

    state->staging.windows[state->staging.window_count++] =
        (ShellWindowState){
            .id = id_copy,
            .group_id = group_copy,
            .active = active,
            .fullscreen = fullscreen,
            .app_id = app_copy,
            .title = title_copy,
        };
    return true;
}

bool shell_state_set_window_placement(
        ShellState *state,
        const char *window_id,
        const char *workspace_id,
        ShellWindowPlacement placement,
        const char *parent_window_id) {
    if (!state->snapshot_in_progress ||
            !nonempty(window_id) ||
            !nonempty(workspace_id) ||
            parent_window_id == NULL ||
            placement < SHELL_WINDOW_PLACEMENT_TILED ||
            placement > SHELL_WINDOW_PLACEMENT_FLOATING) {
        return false;
    }

    ShellWindowState *window =
        find_window_mut(&state->staging, window_id);
    if (!window || window->placement_seen) {
        return false;
    }

    window->workspace_id = copy_string(workspace_id);
    window->parent_window_id = copy_string(parent_window_id);
    if (!window->workspace_id || !window->parent_window_id) {
        free(window->workspace_id);
        free(window->parent_window_id);
        window->workspace_id = NULL;
        window->parent_window_id = NULL;
        return false;
    }

    window->placement = placement;
    window->placement_seen = true;
    return true;
}

const char *shell_window_placement_name(
        ShellWindowPlacement placement) {
    switch (placement) {
    case SHELL_WINDOW_PLACEMENT_TILED:
        return "tiled";
    case SHELL_WINDOW_PLACEMENT_TRANSIENT:
        return "transient";
    case SHELL_WINDOW_PLACEMENT_FLOATING:
        return "floating";
    }
    return "unknown";
}

static bool snapshot_validate(const ShellSnapshot *snapshot) {
    if (snapshot->workspace_count != snapshot->expected_workspace_count) {
        return false;
    }

    size_t expected_groups = 0;
    size_t expected_windows = 0;
    size_t focused_outputs = 0;

    for (size_t i = 0; i < snapshot->output_count; i++) {
        const ShellOutputState *output = &snapshot->outputs[i];
        if (output->focused) {
            focused_outputs++;
        }

        size_t assigned = 0;
        for (size_t j = 0; j < snapshot->workspace_count; j++) {
            const ShellWorkspaceState *workspace = &snapshot->workspaces[j];
            if (workspace->output_seen &&
                    strcmp(workspace->output_name, output->name) == 0) {
                assigned++;
            }
        }

        if (assigned != output->assigned_workspace_count) {
            return false;
        }

        if (output->visible_workspace_id[0] != '\0') {
            const ShellWorkspaceState *visible =
                find_workspace(snapshot, output->visible_workspace_id);
            if (!visible ||
                    !visible->output_seen ||
                    !visible->visible ||
                    strcmp(visible->output_name, output->name) != 0) {
                return false;
            }
        }
    }

    if (focused_outputs > 1) {
        return false;
    }

    for (size_t i = 0; i < snapshot->workspace_count; i++) {
        const ShellWorkspaceState *workspace = &snapshot->workspaces[i];

        if (!workspace->output_seen || !workspace->metadata_seen) {
            return false;
        }

        if (workspace->visible && workspace->output_name[0] == '\0') {
            return false;
        }

        if (workspace->output_name[0] != '\0' &&
                !find_output(snapshot, workspace->output_name)) {
            return false;
        }

        expected_groups += workspace->group_count;
        expected_windows += workspace->window_count;

        size_t groups = 0;
        size_t windows = 0;
        size_t active_groups = 0;

        for (size_t j = 0; j < snapshot->group_count; j++) {
            const ShellGroupState *group = &snapshot->groups[j];
            if (strcmp(group->workspace_id, workspace->id) != 0) {
                continue;
            }
            groups++;
            if (group->active) {
                active_groups++;
            }
        }

        for (size_t j = 0; j < snapshot->window_count; j++) {
            const ShellWindowState *window = &snapshot->windows[j];
            if (!window->placement_seen) {
                return false;
            }
            if (strcmp(window->workspace_id, workspace->id) == 0) {
                windows++;
            }
        }

        if (groups != workspace->group_count ||
                windows != workspace->window_count ||
                active_groups > 1) {
            return false;
        }
    }

    if (expected_groups != snapshot->group_count ||
            expected_windows != snapshot->window_count) {
        return false;
    }

    for (size_t i = 0; i < snapshot->group_count; i++) {
        const ShellGroupState *group = &snapshot->groups[i];
        if (!find_workspace(snapshot, group->workspace_id)) {
            return false;
        }

        if (group->placement_seen) {
            if ((group->placement != SHELL_GROUP_PLACEMENT_TILED &&
                 group->placement != SHELL_GROUP_PLACEMENT_FLOATING) ||
                    (group->placement == SHELL_GROUP_PLACEMENT_TILED && group->pinned) ||
                    (group->pinned && !nonempty(group->pinned_output_name)) ||
                    (!group->pinned && group->pinned_output_name[0] != '\0') ||
                    (group->pinned && !find_output(snapshot, group->pinned_output_name))) {
                return false;
            }
        }

        size_t group_windows = 0;
        size_t active_windows = 0;
        for (size_t j = 0; j < snapshot->window_count; j++) {
            const ShellWindowState *window = &snapshot->windows[j];
            if (window->group_id[0] != '\0' &&
                    strcmp(window->group_id, group->id) == 0) {
                group_windows++;
                if (window->active) {
                    active_windows++;
                }
            }
        }

        if (group_windows != group->window_count ||
                active_windows > 1) {
            return false;
        }
    }

    for (size_t i = 0; i < snapshot->window_count; i++) {
        const ShellWindowState *window = &snapshot->windows[i];
        if (!window->placement_seen ||
                !find_workspace(snapshot, window->workspace_id)) {
            return false;
        }

        switch (window->placement) {
        case SHELL_WINDOW_PLACEMENT_TILED: {
            if (!nonempty(window->group_id) ||
                    window->parent_window_id[0] != '\0') {
                return false;
            }

            const ShellGroupState *group =
                find_group(snapshot, window->group_id);
            if (!group ||
                    strcmp(group->workspace_id, window->workspace_id) != 0 ||
                    (group->placement_seen &&
                     group->placement != SHELL_GROUP_PLACEMENT_TILED)) {
                return false;
            }
            break;
        }

        case SHELL_WINDOW_PLACEMENT_FLOATING:
            if (window->parent_window_id[0] != '\0') {
                return false;
            }

            /* v12: floating windows remain owned by a floating Group.
             * v11 compatibility snapshots may still expose an empty group_id. */
            if (window->group_id[0] != '\0') {
                const ShellGroupState *group =
                    find_group(snapshot, window->group_id);
                if (!group ||
                        strcmp(group->workspace_id, window->workspace_id) != 0 ||
                        (group->placement_seen &&
                         group->placement != SHELL_GROUP_PLACEMENT_FLOATING)) {
                    return false;
                }
            }
            break;

        case SHELL_WINDOW_PLACEMENT_TRANSIENT:
            if (window->group_id[0] != '\0' ||
                    !nonempty(window->parent_window_id) ||
                    !find_window(snapshot, window->parent_window_id)) {
                return false;
            }
            break;
        }
    }

    return true;
}

bool shell_state_end(
        ShellState *state,
        uint32_t generation) {
    if (!state->snapshot_in_progress) {
        return false;
    }

    if (state->staging.generation != generation ||
            !snapshot_validate(&state->staging)) {
        snapshot_clear(&state->staging);
        state->snapshot_in_progress = false;
        return false;
    }

    snapshot_clear(&state->committed);
    state->committed = state->staging;
    state->staging = (ShellSnapshot){0};
    state->snapshot_in_progress = false;
    return true;
}

const ShellSnapshot *shell_state_snapshot(
        const ShellState *state) {
    return &state->committed;
}
