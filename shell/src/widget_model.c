#include "widget_model.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <json-glib/json-glib.h>

enum {
    BAR_MAX_OUTPUTS = 8,
    WORKSPACE_PAGE_SIZE = 5,
};

typedef struct widget_projection_context {
    JsonObject *root;
    JsonArray *outputs;
    JsonArray *workspaces;
    JsonArray *groups;
    JsonArray *windows;
} WidgetProjectionContext;

typedef bool (*WidgetModuleProjectOutputFn)(
    const WidgetProjectionContext *context,
    JsonObject *output,
    guint output_index,
    JsonBuilder *builder,
    GError **error
);

typedef struct widget_module_runtime_definition {
    WidgetModuleDefinition public;
    const char *member_name;
    WidgetModuleProjectOutputFn project_output;
} WidgetModuleRuntimeDefinition;

static bool project_workspaces(
    const WidgetProjectionContext *context,
    JsonObject *output,
    guint output_index,
    JsonBuilder *builder,
    GError **error
);

static bool project_groups(
    const WidgetProjectionContext *context,
    JsonObject *output,
    guint output_index,
    JsonBuilder *builder,
    GError **error
);

static const WidgetModuleRuntimeDefinition module_workspaces = {
    .public = {
        .id = "workspaces",
    },
    .member_name = "workspaces",
    .project_output = project_workspaces,
};

static const WidgetModuleRuntimeDefinition module_groups = {
    .public = {
        .id = "groups",
    },
    .member_name = "groups",
    .project_output = project_groups,
};

static const WidgetModuleDefinition * const bar_modules[] = {
    &module_workspaces.public,
    &module_groups.public,
};

static const WidgetBlockDefinition block_bar = {
    .id = "bar",
    .modules = bar_modules,
    .module_count = G_N_ELEMENTS(bar_modules),
};

static const WidgetBlockDefinition * const blocks[] = {
    &block_bar,
};

static void set_projection_error(
        GError **error,
        const char *message) {
    g_set_error_literal(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_INVAL,
        message
    );
}

static JsonObject *array_object(
        JsonArray *array,
        guint index) {
    if (!array || index >= json_array_get_length(array)) {
        return NULL;
    }

    JsonNode *node =
        json_array_get_element(array, index);

    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) {
        return NULL;
    }

    return json_node_get_object(node);
}

static const char *required_string(
        JsonObject *object,
        const char *member) {
    if (!object ||
            !json_object_has_member(object, member)) {
        return NULL;
    }

    JsonNode *node =
        json_object_get_member(object, member);

    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
        return NULL;
    }

    return json_node_get_string(node);
}

static const char *optional_string(
        JsonObject *object,
        const char *member) {
    if (!object || !json_object_has_member(object, member)) {
        return NULL;
    }

    JsonNode *node = json_object_get_member(object, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
        return NULL;
    }

    return json_node_get_string(node);
}

static bool required_boolean(
        JsonObject *object,
        const char *member,
        bool *value) {
    if (!object || !value ||
            !json_object_has_member(object, member)) {
        return false;
    }

    JsonNode *node =
        json_object_get_member(object, member);

    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
        return false;
    }

    *value = json_node_get_boolean(node);
    return true;
}

static bool positive_decimal_id(
        const char *value,
        guint64 *parsed) {
    if (!value || value[0] == '\0') {
        return false;
    }

    for (const unsigned char *cursor =
            (const unsigned char *)value;
            *cursor;
            cursor++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
    }

    errno = 0;
    char *end = NULL;
    const guint64 number =
        g_ascii_strtoull(value, &end, 10);

    if (errno != 0 ||
            end == value ||
            !end ||
            *end != '\0' ||
            number == 0) {
        return false;
    }

    if (parsed) {
        *parsed = number;
    }

    return true;
}

static gint compare_object_id(
        gconstpointer left,
        gconstpointer right) {
    const JsonObject *a = *(JsonObject * const *)left;
    const JsonObject *b = *(JsonObject * const *)right;
    guint64 a_id = 0;
    guint64 b_id = 0;

    (void)positive_decimal_id(
        required_string((JsonObject *)a, "id"),
        &a_id
    );
    (void)positive_decimal_id(
        required_string((JsonObject *)b, "id"),
        &b_id
    );

    if (a_id < b_id) {
        return -1;
    }

    if (a_id > b_id) {
        return 1;
    }

    return 0;
}

static char *compact_app_identity(
        const char *app_id) {
    if (!app_id || app_id[0] == '\0') {
        return g_strdup("App");
    }

    g_autofree char *lower =
        g_ascii_strdown(app_id, -1);

    if (!lower) {
        return NULL;
    }

    if (strstr(lower, "zen")) {
        return g_strdup("Zen");
    }

    if (strstr(lower, "kitty")) {
        return g_strdup("Kitty");
    }

    if (strstr(lower, "ayugram") ||
            strstr(lower, "telegram")) {
        return g_strdup("Telegram");
    }

    const char *base = strrchr(app_id, '.');
    base = base ? base + 1 : app_id;

    g_autofree char *clean = g_strdup(base);
    if (!clean) {
        return NULL;
    }

    for (char *cursor = clean; *cursor; cursor++) {
        if (*cursor == '-' || *cursor == '_') {
            *cursor = ' ';
        }
    }

    g_strstrip(clean);

    if (clean[0] == '\0') {
        return g_strdup("App");
    }

    if (strlen(clean) > 18) {
        clean[18] = '\0';
    }

    return g_strdup(clean);
}

static JsonObject *find_active_window(
        JsonArray *windows,
        const char *group_id) {
    JsonObject *fallback = NULL;

    for (guint i = 0;
            i < json_array_get_length(windows);
            i++) {
        JsonObject *window =
            array_object(windows, i);
        const char *window_group =
            required_string(window, "group_id");
        const char *placement =
            required_string(window, "placement");

        if (!window_group ||
                !placement ||
                strcmp(window_group, group_id) != 0 ||
                strcmp(placement, "tiled") != 0) {
            continue;
        }

        if (!fallback) {
            fallback = window;
        }

        bool active = false;
        if (required_boolean(window, "active", &active) && active) {
            return window;
        }
    }

    return fallback;
}

static const char *single_group_window_id(
        JsonArray *windows,
        const char *group_id) {
    const char *single_id = NULL;
    guint count = 0;

    if (!windows || !positive_decimal_id(group_id, NULL)) {
        return NULL;
    }

    for (guint i = 0;
            i < json_array_get_length(windows);
            i++) {
        JsonObject *window =
            array_object(windows, i);
        const char *window_id =
            required_string(window, "id");
        const char *window_group_id =
            required_string(window, "group_id");

        if (!window_id ||
                !window_group_id ||
                !positive_decimal_id(window_id, NULL) ||
                strcmp(window_group_id, group_id) != 0) {
            continue;
        }

        count++;
        single_id = window_id;

        if (count > 1U) {
            return NULL;
        }
    }

    return count == 1U ? single_id : NULL;
}

static bool output_has_fullscreen_window(
        JsonArray *windows,
        const char *workspace_id) {
    for (guint i = 0;
            i < json_array_get_length(windows);
            i++) {
        JsonObject *window =
            array_object(windows, i);
        const char *window_workspace =
            required_string(window, "workspace_id");
        bool fullscreen = false;

        if (!window_workspace ||
                strcmp(window_workspace, workspace_id) != 0 ||
                !required_boolean(window, "fullscreen", &fullscreen)) {
            continue;
        }

        if (fullscreen) {
            return true;
        }
    }

    return false;
}

static bool project_workspaces(
        const WidgetProjectionContext *context,
        JsonObject *output,
        guint output_index,
        JsonBuilder *builder,
        GError **error) {
    (void)output_index;

    const char *output_name =
        required_string(output, "name");
    const char *visible_workspace_id =
        required_string(output, "visible_workspace_id");

    if (!output_name ||
            !visible_workspace_id ||
            !positive_decimal_id(visible_workspace_id, NULL)) {
        set_projection_error(error, "invalid output workspace identity");
        return false;
    }

    g_autoptr(GPtrArray) assigned = g_ptr_array_new();
    if (!assigned) {
        set_projection_error(error, "cannot allocate workspace projection");
        return false;
    }

    for (guint i = 0;
            i < json_array_get_length(context->workspaces);
            i++) {
        JsonObject *workspace =
            array_object(context->workspaces, i);
        const char *id =
            required_string(workspace, "id");
        const char *workspace_output =
            required_string(workspace, "output_name");

        if (!id ||
                !workspace_output ||
                !positive_decimal_id(id, NULL)) {
            set_projection_error(error, "invalid workspace state");
            return false;
        }

        if (strcmp(workspace_output, output_name) == 0) {
            g_ptr_array_add(assigned, workspace);
        }
    }

    g_ptr_array_sort(assigned, compare_object_id);

    guint visible_index = 0;
    bool visible_found = false;

    for (guint i = 0; i < assigned->len; i++) {
        JsonObject *workspace =
            g_ptr_array_index(assigned, i);
        const char *id =
            required_string(workspace, "id");
        bool visible = false;

        if (!id ||
                !required_boolean(workspace, "visible", &visible)) {
            set_projection_error(error, "invalid workspace visibility state");
            return false;
        }

        const bool id_is_visible =
            strcmp(id, visible_workspace_id) == 0;

        if (visible != id_is_visible) {
            set_projection_error(error, "workspace visibility contract mismatch");
            return false;
        }

        if (id_is_visible) {
            visible_index = i;
            visible_found = true;
        }
    }

    if (!visible_found) {
        set_projection_error(error, "visible workspace not assigned to output");
        return false;
    }

    const guint page_start =
        (visible_index / WORKSPACE_PAGE_SIZE) * WORKSPACE_PAGE_SIZE;
    const guint page_end =
        MIN(page_start + WORKSPACE_PAGE_SIZE, assigned->len);

    json_builder_set_member_name(builder, "workspaces");
    json_builder_begin_array(builder);

    for (guint i = page_start; i < page_end; i++) {
        JsonObject *workspace =
            g_ptr_array_index(assigned, i);
        const char *id =
            required_string(workspace, "id");
        const char *icon =
            optional_string(workspace, "icon");
        const char *name =
            optional_string(workspace, "name");
        const bool active = i == visible_index;
        g_autofree char *fallback = NULL;
        const char *label =
            icon && icon[0] != '\0'
                ? icon
                : (name && name[0] != '\0'
                    ? name
                    : (fallback = g_strdup_printf("%u", i + 1)));

        if (!id || !label) {
            set_projection_error(error, "cannot format workspace projection");
            return false;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "label");
        json_builder_add_string_value(builder, label);
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, active);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    return true;
}

static bool project_groups(
        const WidgetProjectionContext *context,
        JsonObject *output,
        guint output_index,
        JsonBuilder *builder,
        GError **error) {
    (void)output_index;

    const char *visible_workspace_id =
        required_string(output, "visible_workspace_id");

    if (!visible_workspace_id ||
            !positive_decimal_id(visible_workspace_id, NULL)) {
        set_projection_error(error, "invalid visible workspace for group projection");
        return false;
    }

    g_autoptr(GPtrArray) selected = g_ptr_array_new();
    if (!selected) {
        set_projection_error(error, "cannot allocate group projection");
        return false;
    }

    for (guint i = 0;
            i < json_array_get_length(context->groups);
            i++) {
        JsonObject *group =
            array_object(context->groups, i);
        const char *workspace_id =
            required_string(group, "workspace_id");

        if (!workspace_id) {
            set_projection_error(error, "invalid group workspace identity");
            return false;
        }

        if (strcmp(workspace_id, visible_workspace_id) == 0) {
            g_ptr_array_add(selected, group);
        }
    }

    g_ptr_array_sort(selected, compare_object_id);

    json_builder_set_member_name(builder, "groups");
    json_builder_begin_array(builder);

    for (guint i = 0; i < selected->len; i++) {
        JsonObject *group =
            g_ptr_array_index(selected, i);
        const char *id =
            required_string(group, "id");
        const char *placement =
            required_string(group, "placement");
        const char *pinned_output_name =
            optional_string(group, "pinned_output_name");
        bool active = false;
        bool pinned = false;
        bool placement_seen = false;

        if (!id ||
                !placement ||
                !positive_decimal_id(id, NULL) ||
                !required_boolean(group, "active", &active) ||
                !required_boolean(group, "pinned", &pinned) ||
                !required_boolean(group, "placement_seen", &placement_seen) ||
                (placement_seen &&
                 strcmp(placement, "tiled") != 0 &&
                 strcmp(placement, "floating") != 0) ||
                (!placement_seen && strcmp(placement, "unknown") != 0) ||
                (pinned && strcmp(placement, "floating") != 0) ||
                (pinned && (!pinned_output_name || pinned_output_name[0] == '\0'))) {
            set_projection_error(error, "invalid group state");
            return false;
        }

        JsonObject *window =
            find_active_window(context->windows, id);
        const char *app_id =
            window ? required_string(window, "app_id") : NULL;
        g_autofree char *identity =
            compact_app_identity(app_id);
        g_autofree char *label =
            identity
                ? g_strdup_printf(active ? "[%s]" : "%s", identity)
                : NULL;
        const char *single_window_id =
            single_group_window_id(context->windows, id);
        const char *context_kind =
            single_window_id ? "window" : "group";
        const char *context_id =
            single_window_id ? single_window_id : id;

        if (!label || !context_id) {
            set_projection_error(error, "cannot format group projection");
            return false;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_set_member_name(builder, "label");
        json_builder_add_string_value(builder, label);
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, active);
        json_builder_set_member_name(builder, "context_kind");
        json_builder_add_string_value(builder, context_kind);
        json_builder_set_member_name(builder, "context_id");
        json_builder_add_string_value(builder, context_id);
        json_builder_set_member_name(builder, "placement");
        json_builder_add_string_value(builder, placement);
        json_builder_set_member_name(builder, "pinned");
        json_builder_add_boolean_value(builder, pinned);
        json_builder_set_member_name(builder, "pinned_output_name");
        json_builder_add_string_value(builder, pinned_output_name ? pinned_output_name : "");
        json_builder_set_member_name(builder, "placement_seen");
        json_builder_add_boolean_value(builder, placement_seen);

        json_builder_set_member_name(builder, "windows");
        json_builder_begin_array(builder);

        guint projected_windows = 0;
        for (guint window_index = 0;
                window_index < json_array_get_length(context->windows);
                window_index++) {
            JsonObject *group_window =
                array_object(context->windows, window_index);
            const char *window_id =
                required_string(group_window, "id");
            const char *window_group_id =
                required_string(group_window, "group_id");
            const char *window_workspace_id =
                required_string(group_window, "workspace_id");
            const char *placement =
                required_string(group_window, "placement");
            const char *window_app_id =
                optional_string(group_window, "app_id");
            const char *title =
                optional_string(group_window, "title");
            bool window_active = false;

            if (!window_id ||
                    !window_group_id ||
                    !window_workspace_id ||
                    !placement ||
                    !positive_decimal_id(window_id, NULL) ||
                    !required_boolean(group_window, "active", &window_active)) {
                set_projection_error(error, "invalid window state for group projection");
                return false;
            }

            if (strcmp(window_group_id, id) != 0 ||
                    strcmp(window_workspace_id, visible_workspace_id) != 0 ||
                    (strcmp(placement, "tiled") != 0 &&
                     strcmp(placement, "floating") != 0)) {
                continue;
            }

            g_autofree char *window_identity =
                compact_app_identity(window_app_id);
            if (!window_identity) {
                set_projection_error(error, "cannot format window identity");
                return false;
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, window_id);
            json_builder_set_member_name(builder, "group_id");
            json_builder_add_string_value(builder, id);
            json_builder_set_member_name(builder, "label");
            json_builder_add_string_value(builder, window_identity);
            json_builder_set_member_name(builder, "app_id");
            json_builder_add_string_value(builder, window_app_id ? window_app_id : "");
            json_builder_set_member_name(builder, "title");
            json_builder_add_string_value(builder, title ? title : "");
            json_builder_set_member_name(builder, "placement");
            json_builder_add_string_value(builder, placement);
            json_builder_set_member_name(builder, "active");
            json_builder_add_boolean_value(builder, window_active);
            json_builder_end_object(builder);
            projected_windows++;
        }

        json_builder_end_array(builder);

        if (projected_windows == 0) {
            set_projection_error(error, "group has no owned windows in visible workspace");
            return false;
        }

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    return true;
}

static const WidgetModuleRuntimeDefinition *module_runtime_find(
        const WidgetModuleDefinition *definition) {
    if (definition == &module_workspaces.public) {
        return &module_workspaces;
    }

    if (definition == &module_groups.public) {
        return &module_groups;
    }

    return NULL;
}

const WidgetBlockDefinition *widget_block_definition_find(
        const char *block_id) {
    if (!block_id || block_id[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(blocks); i++) {
        if (strcmp(blocks[i]->id, block_id) == 0) {
            return blocks[i];
        }
    }

    return NULL;
}

char *widget_block_project_json(
        const char *block_id,
        const char *snapshot_json,
        GError **error) {
    const WidgetBlockDefinition *block =
        widget_block_definition_find(block_id);

    if (!block) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unknown widget block '%s'",
            block_id ? block_id : ""
        );
        return NULL;
    }

    if (!snapshot_json || snapshot_json[0] == '\0') {
        set_projection_error(error, "empty shell snapshot JSON");
        return NULL;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    GError *parse_error = NULL;

    if (!json_parser_load_from_data(
            parser,
            snapshot_json,
            -1,
            &parse_error)) {
        g_propagate_prefixed_error(
            error,
            parse_error,
            "cannot parse shell snapshot: "
        );
        return NULL;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
        set_projection_error(error, "shell snapshot root must be an object");
        return NULL;
    }

    JsonObject *root = json_node_get_object(root_node);
    if (!json_object_has_member(root, "generation") ||
            !json_object_has_member(root, "outputs") ||
            !json_object_has_member(root, "workspaces") ||
            !json_object_has_member(root, "groups") ||
            !json_object_has_member(root, "windows")) {
        set_projection_error(error, "shell snapshot missing widget projection members");
        return NULL;
    }

    JsonArray *outputs = json_object_get_array_member(root, "outputs");
    JsonArray *workspaces = json_object_get_array_member(root, "workspaces");
    JsonArray *groups = json_object_get_array_member(root, "groups");
    JsonArray *windows = json_object_get_array_member(root, "windows");

    if (!outputs || !workspaces || !groups || !windows) {
        set_projection_error(error, "shell snapshot projection members must be arrays");
        return NULL;
    }

    if (json_array_get_length(outputs) > BAR_MAX_OUTPUTS) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "bar Ewwii adapter supports at most %u outputs, got %u",
            BAR_MAX_OUTPUTS,
            json_array_get_length(outputs)
        );
        return NULL;
    }

    const WidgetProjectionContext context = {
        .root = root,
        .outputs = outputs,
        .workspaces = workspaces,
        .groups = groups,
        .windows = windows,
    };

    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "generation");
    json_builder_add_int_value(
        builder,
        json_object_get_int_member(root, "generation")
    );

    json_builder_set_member_name(builder, "block");
    json_builder_add_string_value(builder, block->id);

    json_builder_set_member_name(builder, "modules");
    json_builder_begin_array(builder);
    for (size_t i = 0; i < block->module_count; i++) {
        json_builder_add_string_value(builder, block->modules[i]->id);
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "outputs");
    json_builder_begin_array(builder);

    for (guint output_index = 0;
            output_index < json_array_get_length(outputs);
            output_index++) {
        JsonObject *output = array_object(outputs, output_index);
        const char *output_name = required_string(output, "name");
        const char *visible_workspace_id =
            required_string(output, "visible_workspace_id");

        if (!output ||
                !output_name ||
                !visible_workspace_id ||
                !positive_decimal_id(visible_workspace_id, NULL)) {
            set_projection_error(error, "invalid output state for bar projection");
            return NULL;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "index");
        json_builder_add_int_value(builder, output_index);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, output_name);
        json_builder_set_member_name(builder, "bar_visible");
        json_builder_add_boolean_value(
            builder,
            !output_has_fullscreen_window(
                windows,
                visible_workspace_id
            )
        );

        for (size_t module_index = 0;
                module_index < block->module_count;
                module_index++) {
            const WidgetModuleRuntimeDefinition *module =
                module_runtime_find(block->modules[module_index]);

            if (!module || !module->project_output) {
                set_projection_error(error, "widget block contains unknown module definition");
                return NULL;
            }

            if (!module->project_output(
                    &context,
                    output,
                    output_index,
                    builder,
                    error)) {
                return NULL;
            }
        }

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) projected_root = json_builder_get_root(builder);
    json_generator_set_root(generator, projected_root);
    json_generator_set_pretty(generator, false);

    return json_generator_to_data(generator, NULL);
}
