#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum shell_window_placement {
    SHELL_WINDOW_PLACEMENT_TILED = 0,
    SHELL_WINDOW_PLACEMENT_TRANSIENT = 1,
    SHELL_WINDOW_PLACEMENT_FLOATING = 2,
} ShellWindowPlacement;

typedef enum shell_group_placement {
    SHELL_GROUP_PLACEMENT_UNKNOWN = -1,
    SHELL_GROUP_PLACEMENT_TILED = 0,
    SHELL_GROUP_PLACEMENT_FLOATING = 1,
} ShellGroupPlacement;

typedef struct shell_output_state {
    char *name;
    bool focused;
    char *visible_workspace_id;
    uint32_t assigned_workspace_count;
} ShellOutputState;

typedef struct shell_workspace_state {
    char *id;
    bool active;
    uint32_t group_count;
    uint32_t tile_count;
    uint32_t window_count;

    char *output_name;
    bool visible;
    bool output_seen;

    char *name;
    char *icon;
    bool persistent;
    bool startup;
    char *output_affinity;
    bool metadata_seen;
} ShellWorkspaceState;

typedef struct shell_group_state {
    char *id;
    char *workspace_id;
    bool active;
    uint32_t window_count;

    ShellGroupPlacement placement;
    bool pinned;
    char *pinned_output_name;
    bool placement_seen;
} ShellGroupState;

typedef struct shell_window_state {
    char *id;
    char *group_id;
    bool active;
    bool fullscreen;
    char *app_id;
    char *title;

    char *workspace_id;
    ShellWindowPlacement placement;
    char *parent_window_id;
    bool placement_seen;
} ShellWindowState;

typedef struct shell_snapshot {
    uint32_t generation;
    uint32_t expected_workspace_count;

    ShellOutputState *outputs;
    size_t output_count;
    size_t output_capacity;

    ShellWorkspaceState *workspaces;
    size_t workspace_count;
    size_t workspace_capacity;

    ShellGroupState *groups;
    size_t group_count;
    size_t group_capacity;

    ShellWindowState *windows;
    size_t window_count;
    size_t window_capacity;
} ShellSnapshot;

typedef struct shell_state {
    ShellSnapshot committed;
    ShellSnapshot staging;
    bool snapshot_in_progress;
} ShellState;

void shell_state_init(ShellState *state);
void shell_state_finish(ShellState *state);

bool shell_state_begin(
    ShellState *state,
    uint32_t generation,
    uint32_t workspace_count
);

bool shell_state_add_output(
    ShellState *state,
    const char *name,
    bool focused,
    const char *visible_workspace_id,
    uint32_t assigned_workspace_count
);

bool shell_state_add_workspace(
    ShellState *state,
    const char *id,
    bool active,
    uint32_t group_count,
    uint32_t tile_count,
    uint32_t window_count
);

bool shell_state_set_workspace_output(
    ShellState *state,
    const char *workspace_id,
    const char *output_name,
    bool visible
);

bool shell_state_set_workspace_metadata(
    ShellState *state,
    const char *workspace_id,
    const char *name,
    const char *icon,
    bool persistent,
    bool startup,
    const char *output_affinity
);

bool shell_state_add_group(
    ShellState *state,
    const char *id,
    const char *workspace_id,
    bool active,
    uint32_t window_count
);

bool shell_state_set_group_placement(
    ShellState *state,
    const char *group_id,
    ShellGroupPlacement placement,
    bool pinned,
    const char *pinned_output_name
);

const char *shell_group_placement_name(
    ShellGroupPlacement placement
);

bool shell_state_add_window(
    ShellState *state,
    const char *id,
    const char *group_id,
    bool active,
    bool fullscreen,
    const char *app_id,
    const char *title
);

bool shell_state_set_window_placement(
    ShellState *state,
    const char *window_id,
    const char *workspace_id,
    ShellWindowPlacement placement,
    const char *parent_window_id
);

const char *shell_window_placement_name(
    ShellWindowPlacement placement
);

bool shell_state_end(
    ShellState *state,
    uint32_t generation
);

const ShellSnapshot *shell_state_snapshot(
    const ShellState *state
);
