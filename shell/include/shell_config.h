#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <glib.h>

#include "provider.h"

typedef struct fallback_action_config {
    char *label;
    char *shortcut;
    char *desktop_id;
} FallbackActionConfig;

typedef struct fallback_config {
    bool enabled;
    char *title;

    char **show_when_capabilities_missing;
    size_t show_when_capabilities_missing_count;

    FallbackActionConfig *actions;
    size_t action_count;
} FallbackConfig;

typedef struct appearance_config {
    char *wallpaper;
} AppearanceConfig;

typedef struct shell_config {
    unsigned version;
    bool loaded_from_file;

    ProviderConfig *providers;
    size_t provider_count;

    AppearanceConfig appearance;
    FallbackConfig fallback;
} ShellConfig;

void shell_config_init(ShellConfig *config);
void shell_config_finish(ShellConfig *config);

bool shell_config_load(
    ShellConfig *config,
    const char *path,
    bool allow_missing,
    GError **error
);

const ProviderConfig *shell_config_provider_for_capability_action(
    const ShellConfig *config,
    const char *capability,
    const char *action
);
