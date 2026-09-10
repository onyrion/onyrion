#include "ui_actions_v11.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

enum {
    UI_ACTIONS_MAX_OUTPUTS = 8,
};

typedef enum ui_actions_scope {
    UI_ACTIONS_SCOPE_ACTIVE = 0,
    UI_ACTIONS_SCOPE_WINDOW,
    UI_ACTIONS_SCOPE_GROUP,
} UiActionsScope;

typedef struct ui_actions_runtime {
    char *config_dir;
    GHashTable *parent_by_widget;
    UiActionsScope context_scope[UI_ACTIONS_MAX_OUTPUTS];
    char *context_subject_id[UI_ACTIONS_MAX_OUTPUTS];
} UiActionsRuntime;

static UiActionsRuntime runtime = {0};

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

static bool ensure_runtime(
        const char *config_dir) {
    if (runtime.config_dir &&
            strcmp(runtime.config_dir, config_dir) == 0) {
        return true;
    }

    g_clear_pointer(&runtime.config_dir, g_free);

    if (runtime.parent_by_widget) {
        g_hash_table_unref(runtime.parent_by_widget);
    }

    for (guint i = 0; i < UI_ACTIONS_MAX_OUTPUTS; i++) {
        g_clear_pointer(
            &runtime.context_subject_id[i],
            g_free
        );
    }

    runtime = (UiActionsRuntime){0};
    runtime.config_dir = g_strdup(config_dir);
    runtime.parent_by_widget =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            g_free
        );

    return runtime.config_dir &&
        runtime.parent_by_widget;
}

static bool ewwii_run(
        const char *config_dir,
        const char * const *tail,
        bool quiet_failure) {
    GPtrArray *argv = g_ptr_array_new();

    if (!argv) {
        return false;
    }

    g_ptr_array_add(argv, (gpointer)"ewwii");
    g_ptr_array_add(argv, (gpointer)"--config");
    g_ptr_array_add(argv, (gpointer)config_dir);

    for (size_t i = 0; tail[i]; i++) {
        g_ptr_array_add(argv, (gpointer)tail[i]);
    }

    g_ptr_array_add(argv, NULL);

    char *captured_stdout = NULL;
    char *captured_stderr = NULL;
    int wait_status = 0;
    GError *error = NULL;

    const bool spawned =
        g_spawn_sync(
            NULL,
            (char **)argv->pdata,
            NULL,
            G_SPAWN_SEARCH_PATH,
            NULL,
            NULL,
            &captured_stdout,
            &captured_stderr,
            &wait_status,
            &error
        );

    g_ptr_array_free(argv, true);

    bool success = spawned;

    if (success) {
        GError *wait_error = NULL;
        success =
            g_spawn_check_wait_status(
                wait_status,
                &wait_error
            );

        if (!success && !quiet_failure) {
            fprintf(
                stderr,
                "FAIL: ewwii action-ui command status: %s\n",
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else if (!quiet_failure) {
        fprintf(
            stderr,
            "FAIL: cannot spawn ewwii action-ui: %s\n",
            error
                ? error->message
                : "unknown"
        );
    }

    if (!success &&
            !quiet_failure &&
            captured_stderr &&
            captured_stderr[0] != '\0') {
        fprintf(stderr, "%s", captured_stderr);
    }

    g_free(captured_stdout);
    g_free(captured_stderr);
    g_clear_error(&error);

    return success;
}

static bool ewwii_window_active(
        const char *config_dir,
        const char *window_name,
        bool *active) {
    const char *argv[] = {
        "ewwii",
        "--config",
        config_dir,
        "active-windows",
        NULL,
    };

    char *captured_stdout = NULL;
    char *captured_stderr = NULL;
    int wait_status = 0;
    GError *error = NULL;

    const bool spawned =
        g_spawn_sync(
            NULL,
            (char **)argv,
            NULL,
            G_SPAWN_SEARCH_PATH,
            NULL,
            NULL,
            &captured_stdout,
            &captured_stderr,
            &wait_status,
            &error
        );

    bool success = spawned;

    if (success) {
        GError *wait_error = NULL;
        success =
            g_spawn_check_wait_status(
                wait_status,
                &wait_error
            );

        if (!success) {
            fprintf(
                stderr,
                "FAIL: ewwii active-windows status: %s\n",
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else {
        fprintf(
            stderr,
            "FAIL: cannot query Ewwii active windows: %s\n",
            error
                ? error->message
                : "unknown"
        );
    }

    if (success) {
        *active = false;

        g_autofree char *prefix =
            g_strdup_printf(
                "%s:",
                window_name
            );

        if (!prefix) {
            success = false;
        } else {
            g_auto(GStrv) lines =
                g_strsplit(
                    captured_stdout
                        ? captured_stdout
                        : "",
                    "\n",
                    -1
                );

            for (size_t i = 0;
                    lines && lines[i];
                    i++) {
                if (g_str_has_prefix(
                        lines[i],
                        prefix)) {
                    *active = true;
                    break;
                }
            }
        }
    }

    if (!success &&
            captured_stderr &&
            captured_stderr[0] != '\0') {
        fprintf(stderr, "%s", captured_stderr);
    }

    g_free(captured_stdout);
    g_free(captured_stderr);
    g_clear_error(&error);

    return success;
}

static bool ewwii_active_windows_capture(
        const char *config_dir,
        char **active_windows) {
    const char *argv[] = {
        "ewwii",
        "--config",
        config_dir,
        "active-windows",
        NULL,
    };
    char *captured_stdout = NULL;
    char *captured_stderr = NULL;
    int wait_status = 0;
    GError *error = NULL;

    const bool spawned =
        g_spawn_sync(
            NULL,
            (char **)argv,
            NULL,
            G_SPAWN_SEARCH_PATH,
            NULL,
            NULL,
            &captured_stdout,
            &captured_stderr,
            &wait_status,
            &error
        );

    bool success = spawned;

    if (success) {
        GError *wait_error = NULL;
        success =
            g_spawn_check_wait_status(
                wait_status,
                &wait_error
            );

        if (!success) {
            fprintf(
                stderr,
                "FAIL: ewwii active-windows status: %s\n",
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else {
        fprintf(
            stderr,
            "FAIL: cannot query Ewwii active windows: %s\n",
            error ? error->message : "unknown"
        );
    }

    if (!success &&
            captured_stderr &&
            captured_stderr[0] != '\0') {
        fputs(captured_stderr, stderr);
    }

    g_free(captured_stderr);
    g_clear_error(&error);

    if (!success) {
        g_free(captured_stdout);
        return false;
    }

    *active_windows =
        captured_stdout
            ? captured_stdout
            : g_strdup("");

    return *active_windows != NULL;
}

static bool active_windows_contains(
        const char *active_windows,
        const char *window_name) {
    if (!active_windows || !window_name) {
        return false;
    }

    g_autofree char *prefix =
        g_strdup_printf(
            "%s:",
            window_name
        );

    if (!prefix) {
        return false;
    }

    g_auto(GStrv) lines =
        g_strsplit(
            active_windows,
            "\n",
            -1
        );

    for (size_t i = 0;
            lines && lines[i];
            i++) {
        if (g_str_has_prefix(
                lines[i],
                prefix)) {
            return true;
        }
    }

    return false;
}

static bool close_ewwii_window_if_active(
        const char *config_dir,
        const char *active_windows,
        const char *window_name) {
    if (!active_windows_contains(
            active_windows,
            window_name)) {
        return true;
    }

    const char *args[] = {
        "close",
        window_name,
        NULL,
    };

    return ewwii_run(
        config_dir,
        args,
        false
    );
}

static bool launcher_close_transient(void) {
    const char *launcher =
        g_getenv("ONYRION_LAUNCHER_BIN");

    if (!launcher || launcher[0] == '\0') {
        launcher =
            "/usr/libexec/onyrion/onyrion-launcher";
    }

    const char *argv[] = {
        launcher,
        "close",
        NULL,
    };
    int wait_status = 0;
    GError *error = NULL;

    const bool spawned =
        g_spawn_sync(
            NULL,
            (char **)argv,
            NULL,
            0,
            NULL,
            NULL,
            NULL,
            NULL,
            &wait_status,
            &error
        );

    bool success = spawned;

    if (success) {
        GError *wait_error = NULL;
        success =
            g_spawn_check_wait_status(
                wait_status,
                &wait_error
            );

        if (!success) {
            fprintf(
                stderr,
                "FAIL: launcher close status: %s\n",
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else {
        fprintf(
            stderr,
            "FAIL: cannot close launcher: %s\n",
            error ? error->message : "unknown"
        );
    }

    g_clear_error(&error);
    return success;
}

bool ui_actions_v11_dismiss_transients(
        const char *config_dir) {
    if (!config_dir || config_dir[0] == '\0') {
        return false;
    }

    bool success = launcher_close_transient();
    g_autofree char *active_windows = NULL;

    if (!ewwii_active_windows_capture(
            config_dir,
            &active_windows)) {
        return false;
    }

    static const char *shared_windows[] = {
        "onyrion-powermenu",
        "onyrion-audio-mixer",
        "onyrion-wifi-networks",
        "onyrion-bluetooth-devices",
        NULL,
    };

    for (size_t i = 0;
            shared_windows[i];
            i++) {
        if (!close_ewwii_window_if_active(
                config_dir,
                active_windows,
                shared_windows[i])) {
            success = false;
        }
    }

    for (guint output = 0;
            output < 8U;
            output++) {
        static const char *prefixes[] = {
            "onyrion-actions-menu",
            "onyrion-quicksettings",
            "onyrion-calendar",
            NULL,
        };

        for (size_t i = 0;
                prefixes[i];
                i++) {
            g_autofree char *window_name =
                g_strdup_printf(
                    "%s-%u",
                    prefixes[i],
                    output
                );

            if (!window_name ||
                    !close_ewwii_window_if_active(
                        config_dir,
                        active_windows,
                        window_name)) {
                success = false;
            }
        }
    }

    fprintf(
        stderr,
        "TRANSIENT UI dismiss result=%s\n",
        success ? "yes" : "no"
    );

    return success;
}

static char *nbcl_quote(
        const char *value) {
    g_autofree char *escaped =
        g_strescape(value ? value : "", NULL);

    return escaped
        ? g_strdup_printf("\"%s\"", escaped)
        : NULL;
}

static bool widget_remove(
        const char *config_dir,
        const char *name,
        bool quiet_failure) {
    const char *args[] = {
        "widget-control",
        "remove",
        name,
        NULL,
    };

    return ewwii_run(
        config_dir,
        args,
        quiet_failure
    );
}

static bool widget_update_label(
        const char *config_dir,
        const char *name,
        const char *label,
        bool quiet_failure) {
    g_autofree char *quoted =
        nbcl_quote(label);
    g_autofree char *property =
        quoted
            ? g_strdup_printf("label=%s", quoted)
            : NULL;

    if (!property) {
        return false;
    }

    const char *args[] = {
        "widget-control",
        "property-update",
        "--widget",
        name,
        property,
        NULL,
    };

    return ewwii_run(
        config_dir,
        args,
        quiet_failure
    );
}

static bool widget_update_class(
        const char *config_dir,
        const char *name,
        const char *class_name,
        bool quiet_failure) {
    g_autofree char *quoted =
        nbcl_quote(class_name);
    g_autofree char *property =
        quoted
            ? g_strdup_printf("class=%s", quoted)
            : NULL;

    if (!property) {
        return false;
    }

    const char *args[] = {
        "widget-control",
        "property-update",
        "--widget",
        name,
        property,
        NULL,
    };

    return ewwii_run(
        config_dir,
        args,
        quiet_failure
    );
}

static bool widget_create_button(
        const char *config_dir,
        const char *parent,
        const char *name,
        const char *label,
        const char *onclick) {
    g_autofree char *name_q =
        nbcl_quote(name);
    g_autofree char *label_q =
        nbcl_quote(label);
    g_autofree char *onclick_q =
        nbcl_quote(onclick);
    g_autofree char *class_q =
        nbcl_quote("onyrion-shell-control");

    g_autofree char *definition =
        name_q && label_q &&
        onclick_q && class_q
            ? g_strdup_printf(
                "Button %s { label = %s onclick = %s class = %s }",
                name_q,
                label_q,
                onclick_q,
                class_q
            )
            : NULL;

    if (!definition) {
        return false;
    }

    const char *args[] = {
        "widget-control",
        "create",
        "--parent",
        parent,
        definition,
        NULL,
    };

    for (unsigned attempt = 0;
            attempt < 30;
            attempt++) {
        if (ewwii_run(
                config_dir,
                args,
                true)) {
            return true;
        }

        g_usleep(50000);
    }

    fprintf(
        stderr,
        "FAIL: cannot create action widget=%s parent=%s\n",
        name,
        parent
    );

    return false;
}

static bool ensure_button(
        const char *config_dir,
        const char *parent,
        const char *name,
        const char *label,
        const char *onclick) {
    const char *old_parent =
        g_hash_table_lookup(
            runtime.parent_by_widget,
            name
        );

    if (old_parent &&
            strcmp(old_parent, parent) == 0 &&
            widget_update_label(
                config_dir,
                name,
                label,
                true)) {
        return true;
    }

    if (old_parent) {
        (void)widget_remove(
            config_dir,
            name,
            true
        );

        g_hash_table_remove(
            runtime.parent_by_widget,
            name
        );
    }

    if (!widget_create_button(
            config_dir,
            parent,
            name,
            label,
            onclick)) {
        return false;
    }

    g_hash_table_replace(
        runtime.parent_by_widget,
        g_strdup(name),
        g_strdup(parent)
    );

    return true;
}

static void remove_missing(
        GHashTable *desired,
        const char *config_dir) {
    GHashTableIter iterator;
    gpointer key = NULL;

    g_hash_table_iter_init(
        &iterator,
        runtime.parent_by_widget
    );

    while (g_hash_table_iter_next(
            &iterator,
            &key,
            NULL)) {
        if (g_hash_table_contains(desired, key)) {
            continue;
        }

        (void)widget_remove(
            config_dir,
            key,
            true
        );

        g_hash_table_iter_remove(&iterator);
    }
}

static JsonObject *array_object(
        JsonArray *array,
        guint index) {
    JsonNode *node =
        json_array_get_element(array, index);

    return node &&
        JSON_NODE_HOLDS_OBJECT(node)
            ? json_node_get_object(node)
            : NULL;
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

    if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) !=
                G_TYPE_STRING) {
        return NULL;
    }

    return json_object_get_string_member(
        object,
        member
    );
}

static bool required_boolean(
        JsonObject *object,
        const char *member,
        bool *value) {
    if (!object ||
            !json_object_has_member(object, member)) {
        return false;
    }

    JsonNode *node =
        json_object_get_member(object, member);

    if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) !=
                G_TYPE_BOOLEAN) {
        return false;
    }

    *value =
        json_object_get_boolean_member(
            object,
            member
        );

    return true;
}

static JsonObject *find_object_by_id(
        JsonArray *array,
        const char *id) {
    if (!array ||
            !positive_decimal_id(id, NULL)) {
        return NULL;
    }

    for (guint i = 0;
            i < json_array_get_length(array);
            i++) {
        JsonObject *object =
            array_object(array, i);
        const char *candidate_id =
            required_string(object, "id");

        if (candidate_id &&
                strcmp(candidate_id, id) == 0) {
            return object;
        }
    }

    return NULL;
}

static void clear_context_subject(
        guint output_index) {
    if (output_index >= UI_ACTIONS_MAX_OUTPUTS) {
        return;
    }

    runtime.context_scope[output_index] =
        UI_ACTIONS_SCOPE_ACTIVE;
    g_clear_pointer(
        &runtime.context_subject_id[output_index],
        g_free
    );
}

static bool set_context_subject(
        guint output_index,
        UiActionsScope scope,
        const char *subject_id) {
    if (output_index >= UI_ACTIONS_MAX_OUTPUTS ||
            scope == UI_ACTIONS_SCOPE_ACTIVE ||
            !positive_decimal_id(subject_id, NULL)) {
        return false;
    }

    g_autofree char *copy =
        g_strdup(subject_id);

    if (!copy) {
        return false;
    }

    clear_context_subject(output_index);
    runtime.context_scope[output_index] = scope;
    runtime.context_subject_id[output_index] =
        g_steal_pointer(&copy);

    return true;
}

static bool open_context_menu(
        const char *config_dir,
        guint output_index) {
    if (output_index >= UI_ACTIONS_MAX_OUTPUTS) {
        return false;
    }

    for (guint i = 0;
            i < UI_ACTIONS_MAX_OUTPUTS;
            i++) {
        g_autofree char *name =
            g_strdup_printf(
                "onyrion-actions-menu-%u",
                i
            );
        const char *close_args[] = {
            "close",
            name,
            NULL,
        };

        if (!name) {
            return false;
        }

        (void)ewwii_run(
            config_dir,
            close_args,
            true
        );

        if (i != output_index) {
            clear_context_subject(i);
        }
    }

    g_autofree char *menu_window =
        g_strdup_printf(
            "onyrion-actions-menu-%u",
            output_index
        );
    const char *open_args[] = {
        "open",
        menu_window,
        NULL,
    };

    return menu_window &&
        ewwii_run(
            config_dir,
            open_args,
            false
        );
}

static gint compare_object_id(
        gconstpointer left,
        gconstpointer right) {
    JsonObject *a =
        *(JsonObject * const *)left;
    JsonObject *b =
        *(JsonObject * const *)right;

    guint64 a_id = 0;
    guint64 b_id = 0;

    (void)positive_decimal_id(
        required_string(a, "id"),
        &a_id
    );
    (void)positive_decimal_id(
        required_string(b, "id"),
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

static JsonObject *find_active_group(
        JsonArray *groups,
        const char *workspace_id) {
    for (guint i = 0;
            i < json_array_get_length(groups);
            i++) {
        JsonObject *group =
            array_object(groups, i);
        const char *group_workspace =
            required_string(
                group,
                "workspace_id"
            );
        bool active = false;

        if (!group_workspace ||
                strcmp(
                    group_workspace,
                    workspace_id
                ) != 0 ||
                !required_boolean(
                    group,
                    "active",
                    &active)) {
            continue;
        }

        if (active) {
            return group;
        }
    }

    return NULL;
}

static JsonObject *find_group_active_window(
        JsonArray *windows,
        const char *group_id) {
    JsonObject *fallback = NULL;

    for (guint i = 0;
            i < json_array_get_length(windows);
            i++) {
        JsonObject *window =
            array_object(windows, i);
        const char *window_group =
            required_string(
                window,
                "group_id"
            );
        const char *placement =
            required_string(
                window,
                "placement"
            );

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

        if (required_boolean(
                window,
                "active",
                &active) &&
                active) {
            return window;
        }
    }

    return fallback;
}

static bool create_action(
        const char *config_dir,
        const char *parent,
        GHashTable *desired,
        const char *name,
        const char *label,
        const char *command) {
    static const char parent_prefix[] =
        "onyrion-action-menu-";

    if (!parent ||
            !g_str_has_prefix(
                parent,
                parent_prefix)) {
        return false;
    }

    const char *output_suffix =
        parent +
        (sizeof(parent_prefix) - 1);

    if (output_suffix[0] == '\0' ||
            strspn(
                output_suffix,
                "0123456789"
            ) != strlen(output_suffix)) {
        return false;
    }

    g_autofree char *menu_window =
        g_strdup_printf(
            "onyrion-actions-menu-%s",
            output_suffix
        );
    g_autofree char *wrapped_command =
        menu_window
            ? g_strdup_printf(
                "ewwii --config ${ONYRION_UI_DIR:-/usr/share/onyrion/ui/ewwii} close %s && %s",
                menu_window,
                command
            )
            : NULL;

    if (!menu_window ||
            !wrapped_command) {
        return false;
    }

    g_hash_table_add(
        desired,
        g_strdup(name)
    );

    return ensure_button(
        config_dir,
        parent,
        name,
        label,
        wrapped_command
    );
}

static bool sync_visible_floating_tile_actions(
        const char *config_dir,
        guint output_index,
        const char *visible_workspace_id,
        JsonArray *windows,
        GHashTable *desired) {
    g_autofree char *parent =
        g_strdup_printf(
            "onyrion-action-menu-%u",
            output_index
        );

    if (!parent) {
        return false;
    }

    for (guint i = 0;
            i < json_array_get_length(windows);
            i++) {
        JsonObject *window =
            array_object(windows, i);
        const char *window_id =
            required_string(
                window,
                "id"
            );
        const char *window_group =
            required_string(
                window,
                "group_id"
            );
        const char *window_workspace =
            required_string(
                window,
                "workspace_id"
            );
        const char *placement =
            required_string(
                window,
                "placement"
            );

        if (!window_id ||
                !window_group ||
                !window_workspace ||
                !placement ||
                !positive_decimal_id(
                    window_id,
                    NULL)) {
            return false;
        }

        if (strcmp(
                window_workspace,
                visible_workspace_id
            ) != 0 ||
                strcmp(
                    placement,
                    "floating"
                ) != 0 ||
                window_group[0] != '\0') {
            continue;
        }

        g_autofree char *tile_name =
            g_strdup_printf(
                "onyrion-action-tile-w%s",
                window_id
            );
        g_autofree char *tile_label =
            g_strdup_printf(
                "Tile F%s",
                window_id
            );
        g_autofree char *tile_cmd =
            g_strdup_printf(
                "onyrionctl window tile %s",
                window_id
            );

        if (!tile_name ||
                !tile_label ||
                !tile_cmd ||
                !create_action(
                    config_dir,
                    parent,
                    desired,
                    tile_name,
                    tile_label,
                    tile_cmd)) {
            return false;
        }
    }

    return true;
}

static bool sync_output_actions(
        const char *config_dir,
        guint output_index,
        const char *output_name,
        const char *visible_workspace_id,
        JsonArray *workspaces,
        JsonArray *groups,
        JsonArray *windows,
        GHashTable *desired,
        UiActionsScope scope,
        const char *subject_id) {
    g_autofree char *menu_window =
        g_strdup_printf(
            "onyrion-actions-menu-%u",
            output_index
        );
    bool menu_active = false;

    if (!menu_window ||
            !ewwii_window_active(
                config_dir,
                menu_window,
                &menu_active)) {
        return false;
    }

    if (!menu_active) {
        clear_context_subject(output_index);
        return true;
    }

    if (scope == UI_ACTIONS_SCOPE_ACTIVE &&
            !sync_visible_floating_tile_actions(
                config_dir,
                output_index,
                visible_workspace_id,
                windows,
                desired)) {
        return false;
    }

    JsonObject *active_group = NULL;
    JsonObject *active_window = NULL;
    const char *group_id = NULL;
    const char *window_id = NULL;
    const char *window_workspace = NULL;
    const char *placement = NULL;

    if (scope == UI_ACTIONS_SCOPE_GROUP) {
        active_group =
            find_object_by_id(
                groups,
                subject_id
            );

        group_id =
            active_group
                ? required_string(
                    active_group,
                    "id"
                )
                : NULL;
        window_workspace =
            active_group
                ? required_string(
                    active_group,
                    "workspace_id"
                )
                : NULL;
    } else if (scope == UI_ACTIONS_SCOPE_WINDOW) {
        active_window =
            find_object_by_id(
                windows,
                subject_id
            );

        if (active_window) {
            window_id =
                required_string(
                    active_window,
                    "id"
                );
            window_workspace =
                required_string(
                    active_window,
                    "workspace_id"
                );
            placement =
                required_string(
                    active_window,
                    "placement"
                );

            const char *candidate_group_id =
                required_string(
                    active_window,
                    "group_id"
                );

            group_id =
                candidate_group_id &&
                candidate_group_id[0] != '\0'
                    ? candidate_group_id
                    : NULL;
        }
    } else {
        active_group =
            find_active_group(
                groups,
                visible_workspace_id
            );

        group_id =
            active_group
                ? required_string(
                    active_group,
                    "id"
                )
                : NULL;

        active_window =
            group_id
                ? find_group_active_window(
                    windows,
                    group_id
                )
                : NULL;

        if (active_window) {
            window_id =
                required_string(
                    active_window,
                    "id"
                );
            window_workspace =
                required_string(
                    active_window,
                    "workspace_id"
                );
            placement =
                required_string(
                    active_window,
                    "placement"
                );
        }
    }

    if (scope == UI_ACTIONS_SCOPE_GROUP) {
        if (!group_id ||
                !window_workspace ||
                !positive_decimal_id(group_id, NULL) ||
                !positive_decimal_id(
                    window_workspace,
                    NULL) ||
                strcmp(
                    window_workspace,
                    visible_workspace_id
                ) != 0) {
            return false;
        }
    } else {
        if (!active_window) {
            return true;
        }

        if (!window_id ||
                !window_workspace ||
                !placement ||
                !positive_decimal_id(window_id, NULL) ||
                !positive_decimal_id(
                    window_workspace,
                    NULL) ||
                (group_id &&
                    !positive_decimal_id(
                        group_id,
                        NULL)) ||
                (scope == UI_ACTIONS_SCOPE_WINDOW &&
                    strcmp(
                        window_workspace,
                        visible_workspace_id
                    ) != 0)) {
            return false;
        }
    }

    g_autofree char *parent =
        g_strdup_printf(
            "onyrion-action-menu-%u",
            output_index
        );

    if (!parent) {
        return false;
    }

    if (scope != UI_ACTIONS_SCOPE_GROUP) {
        g_autofree char *close_name =
            g_strdup_printf(
                "onyrion-action-close-w%s",
                window_id
            );
        g_autofree char *close_cmd =
            g_strdup_printf(
                "onyrionctl window focus %s && onyrionctl window close",
                window_id
            );
        g_autofree char *fullscreen_name =
            g_strdup_printf(
                "onyrion-action-fullscreen-w%s",
                window_id
            );
        g_autofree char *fullscreen_cmd =
            g_strdup_printf(
                "onyrionctl window focus %s && onyrionctl window fullscreen",
                window_id
            );
        const char *close_label =
            scope == UI_ACTIONS_SCOPE_WINDOW
                ? "Close"
                : "Close";
        const char *fullscreen_label =
            scope == UI_ACTIONS_SCOPE_WINDOW
                ? "Fullscreen"
                : "Fullscreen";

        if (!close_name ||
                !close_cmd ||
                !fullscreen_name ||
                !fullscreen_cmd ||
                !create_action(
                    config_dir,
                    parent,
                    desired,
                    close_name,
                    close_label,
                    close_cmd) ||
                !create_action(
                    config_dir,
                    parent,
                    desired,
                    fullscreen_name,
                    fullscreen_label,
                    fullscreen_cmd)) {
            return false;
        }

        if (strcmp(placement, "tiled") == 0) {
            g_autofree char *float_name =
                g_strdup_printf(
                    "onyrion-action-float-w%s",
                    window_id
                );
            g_autofree char *float_cmd =
                g_strdup_printf(
                    "onyrionctl window float %s",
                    window_id
                );
            const char *float_label =
                scope == UI_ACTIONS_SCOPE_WINDOW
                    ? "Float"
                    : "Float";

            if (!float_name ||
                    !float_cmd ||
                    !create_action(
                        config_dir,
                        parent,
                        desired,
                        float_name,
                        float_label,
                        float_cmd)) {
                return false;
            }

            if (group_id) {
                g_autofree char *split_h_name =
                    g_strdup_printf(
                        "onyrion-action-split-h-w%s",
                        window_id
                    );
                g_autofree char *split_h_cmd =
                    g_strdup_printf(
                        "onyrionctl window split %s horizontal",
                        window_id
                    );
                g_autofree char *split_v_name =
                    g_strdup_printf(
                        "onyrion-action-split-v-w%s",
                        window_id
                    );
                g_autofree char *split_v_cmd =
                    g_strdup_printf(
                        "onyrionctl window split %s vertical",
                        window_id
                    );
                const char *split_h_label =
                    scope == UI_ACTIONS_SCOPE_WINDOW
                        ? "Split H"
                        : "Split H";
                const char *split_v_label =
                    scope == UI_ACTIONS_SCOPE_WINDOW
                        ? "Split V"
                        : "Split V";

                if (!split_h_name ||
                        !split_h_cmd ||
                        !split_v_name ||
                        !split_v_cmd ||
                        !create_action(
                            config_dir,
                            parent,
                            desired,
                            split_h_name,
                            split_h_label,
                            split_h_cmd) ||
                        !create_action(
                            config_dir,
                            parent,
                            desired,
                            split_v_name,
                            split_v_label,
                            split_v_cmd)) {
                    return false;
                }
            }
        } else if (strcmp(placement, "floating") == 0) {
            g_autofree char *tile_name =
                g_strdup_printf(
                    "onyrion-action-tile-w%s",
                    window_id
                );
            g_autofree char *tile_cmd =
                g_strdup_printf(
                    "onyrionctl window tile %s",
                    window_id
                );
            const char *tile_label =
                scope == UI_ACTIONS_SCOPE_WINDOW
                    ? "Tile"
                    : "Tile";

            if (!tile_name ||
                    !tile_cmd ||
                    !create_action(
                        config_dir,
                        parent,
                        desired,
                        tile_name,
                        tile_label,
                        tile_cmd)) {
                return false;
            }
        }
    }

    g_autoptr(GPtrArray) assigned =
        g_ptr_array_new();

    if (!assigned) {
        return false;
    }

    for (guint i = 0;
            i < json_array_get_length(workspaces);
            i++) {
        JsonObject *workspace =
            array_object(workspaces, i);
        const char *workspace_output =
            required_string(
                workspace,
                "output_name"
            );
        const char *workspace_id =
            required_string(
                workspace,
                "id"
            );

        if (!workspace_output ||
                !workspace_id ||
                !positive_decimal_id(
                    workspace_id,
                    NULL)) {
            return false;
        }

        if (strcmp(
                workspace_output,
                output_name
            ) == 0) {
            g_ptr_array_add(
                assigned,
                workspace
            );
        }
    }

    g_ptr_array_sort(
        assigned,
        compare_object_id
    );

    for (guint i = 0;
            i < assigned->len;
            i++) {
        JsonObject *workspace =
            g_ptr_array_index(
                assigned,
                i
            );
        const char *target_workspace =
            required_string(
                workspace,
                "id"
            );
        const guint ordinal = i + 1;

        if (!target_workspace ||
                strcmp(
                    target_workspace,
                    window_workspace
                ) == 0) {
            continue;
        }

        if (scope == UI_ACTIONS_SCOPE_ACTIVE) {
            g_autofree char *window_move_name =
                g_strdup_printf(
                    "onyrion-action-window-w%s-to-ws%s",
                    window_id,
                    target_workspace
                );
            g_autofree char *window_move_label =
                g_strdup_printf(
                    scope == UI_ACTIONS_SCOPE_WINDOW
                        ? "W>%u"
                        : "W>%u",
                    ordinal
                );
            g_autofree char *window_move_cmd =
                g_strdup_printf(
                    "onyrionctl window move-to-workspace %s %s",
                    window_id,
                    target_workspace
                );

            if (!window_move_name ||
                    !window_move_label ||
                    !window_move_cmd ||
                    !create_action(
                        config_dir,
                        parent,
                        desired,
                        window_move_name,
                        window_move_label,
                        window_move_cmd)) {
                return false;
            }
        }

        /* Whole-group workspace target expansion intentionally omitted.
         * Group movement between workspaces is not a context-menu primitive. */

    }

    for (guint i = 0;
            i < json_array_get_length(groups);
            i++) {
        JsonObject *target_group =
            array_object(groups, i);
        const char *target_group_id =
            required_string(
                target_group,
                "id"
            );
        const char *target_workspace =
            required_string(
                target_group,
                "workspace_id"
            );

        if (!target_group_id ||
                !target_workspace ||
                !positive_decimal_id(
                    target_group_id,
                    NULL) ||
                strcmp(
                    target_workspace,
                    visible_workspace_id
                ) != 0 ||
                (group_id &&
                    strcmp(
                        target_group_id,
                        group_id
                    ) == 0)) {
            continue;
        }

        if (scope == UI_ACTIONS_SCOPE_ACTIVE) {
            g_autofree char *window_group_name =
                g_strdup_printf(
                    "onyrion-action-window-w%s-to-g%s",
                    window_id,
                    target_group_id
                );
            g_autofree char *window_group_label =
                g_strdup_printf(
                    scope == UI_ACTIONS_SCOPE_WINDOW
                        ? "W>G%s"
                        : "W>G%s",
                    target_group_id
                );
            g_autofree char *window_group_cmd =
                g_strdup_printf(
                    "onyrionctl window move-to-group %s %s",
                    window_id,
                    target_group_id
                );

            if (!window_group_name ||
                    !window_group_label ||
                    !window_group_cmd ||
                    !create_action(
                        config_dir,
                        parent,
                        desired,
                        window_group_name,
                        window_group_label,
                        window_group_cmd)) {
                return false;
            }
        }

        if (scope == UI_ACTIONS_SCOPE_ACTIVE &&
                group_id) {
            g_autofree char *merge_name =
                g_strdup_printf(
                    "onyrion-action-merge-g%s-to-g%s",
                    group_id,
                    target_group_id
                );
            g_autofree char *merge_label =
                g_strdup_printf(
                    scope == UI_ACTIONS_SCOPE_GROUP
                        ? "Merge group>G%s"
                        : "Merge>G%s",
                    target_group_id
                );
            g_autofree char *merge_cmd =
                g_strdup_printf(
                    "onyrionctl group merge %s %s",
                    group_id,
                    target_group_id
                );

            if (!merge_name ||
                    !merge_label ||
                    !merge_cmd ||
                    !create_action(
                        config_dir,
                        parent,
                        desired,
                        merge_name,
                        merge_label,
                        merge_cmd)) {
                return false;
            }
        }
    }

    return true;
}


bool ui_actions_v11_context_open(
        const char *config_dir,
        const char *json,
        const char *subject_kind,
        const char *subject_id) {
    if (!config_dir ||
            config_dir[0] == '\0' ||
            !json ||
            !subject_kind ||
            !positive_decimal_id(subject_id, NULL) ||
            !ensure_runtime(config_dir)) {
        return false;
    }

    /* One RMB gesture owns the context menu until it closes. Repeated RMB
     * presses must not tear down/rebuild the current popup. */
    for (guint i = 0; i < UI_ACTIONS_MAX_OUTPUTS; i++) {
        g_autofree char *menu_name =
            g_strdup_printf("onyrion-actions-menu-%u", i);
        bool active = false;
        if (!menu_name ||
                !ewwii_window_active(config_dir, menu_name, &active)) {
            return false;
        }
        if (active) {
            return true;
        }
    }

    UiActionsScope scope = UI_ACTIONS_SCOPE_ACTIVE;

    if (strcmp(subject_kind, "window") == 0) {
        scope = UI_ACTIONS_SCOPE_WINDOW;
    } else if (strcmp(subject_kind, "group") == 0) {
        scope = UI_ACTIONS_SCOPE_GROUP;
    } else {
        return false;
    }

    g_autoptr(JsonParser) parser =
        json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(
            parser,
            json,
            -1,
            &error)) {
        fprintf(
            stderr,
            "FAIL: v11 context action JSON: %s\n",
            error
                ? error->message
                : "parse error"
        );
        g_clear_error(&error);
        return false;
    }

    JsonNode *root =
        json_parser_get_root(parser);

    if (!root ||
            !JSON_NODE_HOLDS_OBJECT(root)) {
        return false;
    }

    JsonObject *object =
        json_node_get_object(root);

    if (!json_object_has_member(object, "outputs") ||
            !json_object_has_member(object, "workspaces") ||
            !json_object_has_member(object, "groups") ||
            !json_object_has_member(object, "windows")) {
        return false;
    }

    JsonArray *outputs =
        json_object_get_array_member(object, "outputs");
    JsonArray *workspaces =
        json_object_get_array_member(object, "workspaces");
    JsonArray *groups =
        json_object_get_array_member(object, "groups");
    JsonArray *windows =
        json_object_get_array_member(object, "windows");

    if (!outputs ||
            !workspaces ||
            !groups ||
            !windows ||
            json_array_get_length(outputs) >
                UI_ACTIONS_MAX_OUTPUTS) {
        return false;
    }

    JsonObject *subject =
        find_object_by_id(
            scope == UI_ACTIONS_SCOPE_WINDOW
                ? windows
                : groups,
            subject_id
        );
    const char *workspace_id =
        subject
            ? required_string(
                subject,
                "workspace_id"
            )
            : NULL;

    if (!workspace_id ||
            !positive_decimal_id(
                workspace_id,
                NULL)) {
        return false;
    }

    guint output_index = UI_ACTIONS_MAX_OUTPUTS;
    const char *output_name = NULL;

    for (guint i = 0;
            i < json_array_get_length(outputs);
            i++) {
        JsonObject *output =
            array_object(outputs, i);
        const char *candidate_name =
            required_string(output, "name");
        const char *visible_workspace_id =
            required_string(
                output,
                "visible_workspace_id"
            );

        if (!candidate_name ||
                !visible_workspace_id) {
            return false;
        }

        if (strcmp(
                visible_workspace_id,
                workspace_id
            ) == 0) {
            output_index = i;
            output_name = candidate_name;
            break;
        }
    }

    if (output_index >= UI_ACTIONS_MAX_OUTPUTS ||
            !output_name ||
            !set_context_subject(
                output_index,
                scope,
                subject_id)) {
        if (output_index < UI_ACTIONS_MAX_OUTPUTS) {
            clear_context_subject(output_index);
        }
        return false;
    }

    g_autoptr(GHashTable) desired =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    if (!desired ||
            !sync_output_actions(
                config_dir,
                output_index,
                output_name,
                workspace_id,
                workspaces,
                groups,
                windows,
                desired,
                scope,
                subject_id)) {
        clear_context_subject(output_index);
        return false;
    }

    remove_missing(
        desired,
        config_dir
    );

    g_autofree char *parent =
        g_strdup_printf("onyrion-action-menu-%u", output_index);
    if (!parent ||
            !widget_update_class(
                config_dir,
                parent,
                "onyrion-powermenu onyrion-actions-menu onyrion-actions-ready",
                false) ||
            !open_context_menu(
                config_dir,
                output_index)) {
        clear_context_subject(output_index);
        return false;
    }

    return true;
}

bool ui_actions_v11_sync_apply(
        const char *config_dir,
        const char *json) {
    if (!config_dir ||
            config_dir[0] == '\0' ||
            !json ||
            !ensure_runtime(config_dir)) {
        return false;
    }

    g_autoptr(JsonParser) parser =
        json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(
            parser,
            json,
            -1,
            &error)) {
        fprintf(
            stderr,
            "FAIL: v11 action UI JSON: %s\n",
            error
                ? error->message
                : "parse error"
        );
        g_clear_error(&error);
        return false;
    }

    JsonNode *root =
        json_parser_get_root(parser);

    if (!root ||
            !JSON_NODE_HOLDS_OBJECT(root)) {
        return false;
    }

    JsonObject *object =
        json_node_get_object(root);

    if (!json_object_has_member(object, "outputs") ||
            !json_object_has_member(object, "workspaces") ||
            !json_object_has_member(object, "groups") ||
            !json_object_has_member(object, "windows")) {
        return false;
    }

    JsonArray *outputs =
        json_object_get_array_member(
            object,
            "outputs"
        );
    JsonArray *workspaces =
        json_object_get_array_member(
            object,
            "workspaces"
        );
    JsonArray *groups =
        json_object_get_array_member(
            object,
            "groups"
        );
    JsonArray *windows =
        json_object_get_array_member(
            object,
            "windows"
        );

    if (!outputs ||
            !workspaces ||
            !groups ||
            !windows ||
            json_array_get_length(outputs) >
                UI_ACTIONS_MAX_OUTPUTS) {
        return false;
    }

    g_autoptr(GHashTable) desired =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    if (!desired) {
        return false;
    }

    for (guint output_index = 0;
            output_index < json_array_get_length(outputs);
            output_index++) {
        JsonObject *output =
            array_object(
                outputs,
                output_index
            );
        const char *output_name =
            required_string(
                output,
                "name"
            );
        const char *visible_workspace_id =
            required_string(
                output,
                "visible_workspace_id"
            );

        if (!output_name ||
                !visible_workspace_id ||
                !positive_decimal_id(
                    visible_workspace_id,
                    NULL) ||
                !sync_output_actions(
                    config_dir,
                    output_index,
                    output_name,
                    visible_workspace_id,
                    workspaces,
                    groups,
                    windows,
                    desired,
                    runtime.context_scope[output_index],
                    runtime.context_subject_id[output_index])) {
            return false;
        }
    }

    remove_missing(
        desired,
        config_dir
    );

    return true;
}
