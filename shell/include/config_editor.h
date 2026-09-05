#pragma once

#include <stdbool.h>

#include <glib.h>

typedef enum shell_config_edit_field {
    SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER,
    SHELL_CONFIG_EDIT_FALLBACK_ENABLED,
    SHELL_CONFIG_EDIT_FALLBACK_TITLE,
    SHELL_CONFIG_EDIT_PROVIDER_PRIORITY,
    SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART,
    SHELL_CONFIG_EDIT_PROVIDER_REQUIRED,
} ShellConfigEditField;

typedef struct shell_config_edit_request {
    ShellConfigEditField field;
    const char *provider_id;
    const char *value;
} ShellConfigEditRequest;

typedef struct shell_config_edit_result {
    bool changed;
    char *path;
    char *old_value;
    char *new_value;
} ShellConfigEditResult;

void shell_config_edit_result_finish(
    ShellConfigEditResult *result
);

bool shell_config_edit_persist(
    const char *path,
    const ShellConfigEditRequest *request,
    ShellConfigEditResult *result,
    GError **error
);
