#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <glib.h>

typedef struct widget_module_definition {
    const char *id;
} WidgetModuleDefinition;

typedef struct widget_block_definition {
    const char *id;
    const WidgetModuleDefinition * const *modules;
    size_t module_count;
} WidgetBlockDefinition;

const WidgetBlockDefinition *widget_block_definition_find(
    const char *block_id
);

char *widget_block_project_json(
    const char *block_id,
    const char *snapshot_json,
    GError **error
);
