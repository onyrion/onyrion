#include "ewwii_adapter.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

enum {
    EWWII_ADAPTER_MAX_OUTPUTS = 8,
};

typedef struct ewwii_adapter_runtime {
    char *config_dir;
    bool initialized;
    bool background_visible[EWWII_ADAPTER_MAX_OUTPUTS];
    bool bar_visible[EWWII_ADAPTER_MAX_OUTPUTS];
} EwwiiAdapterRuntime;

static EwwiiAdapterRuntime runtime = {0};
static GHashTable *tabgroups_visible = NULL;

static bool ensure_tabgroup_set(void) {
    if (tabgroups_visible) {
        return true;
    }

    tabgroups_visible =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    return tabgroups_visible != NULL;
}

static bool positive_decimal_id(
        const char *value) {
    if (!value ||
            value[0] == '\0' ||
            value[0] == '0') {
        return false;
    }

    for (const unsigned char *p =
            (const unsigned char *)value;
            *p;
            p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }

    return true;
}

static char *tabgroup_id_from_active_line(
        const char *line) {
    static const char prefix[] =
        "onyrion-tabgroup:";
    static const char suffix[] =
        ": onyrion-tabgroup";

    if (!line ||
            !g_str_has_prefix(line, prefix) ||
            !g_str_has_suffix(line, suffix)) {
        return NULL;
    }

    const size_t line_length = strlen(line);
    const size_t prefix_length =
        sizeof(prefix) - 1;
    const size_t suffix_length =
        sizeof(suffix) - 1;

    if (line_length <=
            prefix_length +
            suffix_length) {
        return NULL;
    }

    g_autofree char *id =
        g_strndup(
            line + prefix_length,
            line_length -
                prefix_length -
                suffix_length
        );

    if (!positive_decimal_id(id)) {
        return NULL;
    }

    return g_steal_pointer(&id);
}

static bool ewwii_run(
        const char *config_dir,
        const char * const *tail,
        char **stdout_out) {
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
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

    bool success = spawned;

    if (success) {
        GError *wait_error = NULL;
        success = g_spawn_check_wait_status(wait_status, &wait_error);
        if (!success) {
            fprintf(
                stderr,
                "FAIL: Ewwii adapter command status: %s\n",
                wait_error ? wait_error->message : "unknown"
            );
        }
        g_clear_error(&wait_error);
    } else {
        fprintf(
            stderr,
            "FAIL: Ewwii adapter spawn: %s\n",
            error ? error->message : "unknown"
        );
    }

    if (!success && captured_stderr && captured_stderr[0] != '\0') {
        fputs(captured_stderr, stderr);
    }

    if (stdout_out) {
        *stdout_out = captured_stdout;
        captured_stdout = NULL;
    }

    g_free(captured_stdout);
    g_free(captured_stderr);
    g_clear_error(&error);
    return success;
}

static bool active_name_matches(
        const char *line,
        const char *name) {
    if (!line || !name) {
        return false;
    }

    g_autofree char *prefix = g_strdup_printf("%s:", name);
    return prefix && g_str_has_prefix(line, prefix);
}

static bool seed_runtime(
        const char *config_dir) {
    const char *args[] = {
        "active-windows",
        NULL,
    };
    g_autofree char *output = NULL;

    if (!ewwii_run(config_dir, args, &output) || !output) {
        return false;
    }

    memset(runtime.background_visible, 0, sizeof(runtime.background_visible));
    memset(runtime.bar_visible, 0, sizeof(runtime.bar_visible));

    if (!ensure_tabgroup_set()) {
        return false;
    }
    g_hash_table_remove_all(tabgroups_visible);

    g_auto(GStrv) lines = g_strsplit(output, "\n", -1);

    for (size_t line = 0; lines && lines[line]; line++) {
        g_autofree char *group_id =
            tabgroup_id_from_active_line(
                lines[line]
            );

        if (group_id) {
            g_hash_table_add(
                tabgroups_visible,
                g_steal_pointer(&group_id)
            );
        }
    }

    for (guint index = 0; index < EWWII_ADAPTER_MAX_OUTPUTS; index++) {
        g_autofree char *background =
            g_strdup_printf("onyrion-background-%u", index);
        g_autofree char *bar =
            g_strdup_printf("onyrion-bar-%u", index);

        if (!background || !bar) {
            return false;
        }

        for (size_t line = 0; lines && lines[line]; line++) {
            if (active_name_matches(lines[line], background)) {
                runtime.background_visible[index] = true;
            }
            if (active_name_matches(lines[line], bar)) {
                runtime.bar_visible[index] = true;
            }
        }
    }

    g_free(runtime.config_dir);
    runtime.config_dir = g_strdup(config_dir);
    runtime.initialized = runtime.config_dir != NULL;

    if (runtime.initialized) {
        fprintf(
            stderr,
            "EWWII ADAPTER seed=PASS mode=single-active-windows-query\n"
        );
    }

    return runtime.initialized;
}

static bool ensure_runtime(
        const char *config_dir) {
    if (runtime.initialized &&
            runtime.config_dir &&
            strcmp(runtime.config_dir, config_dir) == 0) {
        return true;
    }

    g_free(runtime.config_dir);
    runtime = (EwwiiAdapterRuntime){0};
    return seed_runtime(config_dir);
}

static bool set_background(
        const char *config_dir,
        guint output_index,
        bool visible) {
    if (runtime.background_visible[output_index] == visible) {
        return true;
    }

    g_autofree char *instance =
        g_strdup_printf("onyrion-background-%u", output_index);
    g_autofree char *screen =
        g_strdup_printf("%u", output_index);

    if (!instance || !screen) {
        return false;
    }

    const char *open_args[] = {
        "open",
        "--id",
        instance,
        "--screen",
        screen,
        "onyrion-background",
        NULL,
    };
    const char *close_args[] = {
        "close",
        instance,
        NULL,
    };

    if (!ewwii_run(
            config_dir,
            visible ? open_args : close_args,
            NULL)) {
        return false;
    }

    runtime.background_visible[output_index] = visible;
    fprintf(
        stderr,
        "EWWII ADAPTER root=background output=%u visible=%s\n",
        output_index,
        visible ? "true" : "false"
    );
    return true;
}

static bool set_bar(
        const char *config_dir,
        guint output_index,
        bool visible) {
    if (runtime.bar_visible[output_index] == visible) {
        return true;
    }

    g_autofree char *name =
        g_strdup_printf("onyrion-bar-%u", output_index);

    if (!name) {
        return false;
    }

    const char *open_args[] = {
        "open",
        name,
        NULL,
    };
    const char *close_args[] = {
        "close",
        name,
        NULL,
    };

    if (!ewwii_run(
            config_dir,
            visible ? open_args : close_args,
            NULL)) {
        return false;
    }

    runtime.bar_visible[output_index] = visible;
    fprintf(
        stderr,
        "EWWII ADAPTER root=bar output=%u visible=%s\n",
        output_index,
        visible ? "true" : "false"
    );
    return true;
}

static bool set_tabgroup(
        const char *config_dir,
        const char *group_id,
        bool visible) {
    if (!ensure_tabgroup_set() ||
            !positive_decimal_id(group_id)) {
        return false;
    }

    const bool current =
        g_hash_table_contains(
            tabgroups_visible,
            group_id
        );

    if (current == visible) {
        return true;
    }

    g_autofree char *instance =
        g_strdup_printf(
            "onyrion-tabgroup:%s",
            group_id
        );

    if (!instance) {
        return false;
    }

    const char *open_args[] = {
        "open",
        "onyrion-tabgroup",
        "--id",
        instance,
        NULL,
    };

    const char *close_args[] = {
        "close",
        instance,
        NULL,
    };

    if (!ewwii_run(
            config_dir,
            visible ? open_args : close_args,
            NULL)) {
        return false;
    }

    if (visible) {
        g_hash_table_add(
            tabgroups_visible,
            g_strdup(group_id)
        );
    } else {
        g_hash_table_remove(
            tabgroups_visible,
            group_id
        );
    }

    fprintf(
        stderr,
        "EWWII ADAPTER root=tabgroup group=%s visible=%s\n",
        group_id,
        visible ? "true" : "false"
    );

    return true;
}

static bool reconcile_tabgroups(
        const char *config_dir,
        GHashTable *desired) {
    if (!config_dir ||
            !desired ||
            !ensure_tabgroup_set()) {
        return false;
    }

    g_autoptr(GPtrArray) stale =
        g_ptr_array_new_with_free_func(g_free);

    if (!stale) {
        return false;
    }

    GHashTableIter current_iter;
    gpointer key = NULL;

    g_hash_table_iter_init(
        &current_iter,
        tabgroups_visible
    );

    while (g_hash_table_iter_next(
            &current_iter,
            &key,
            NULL)) {
        const char *group_id = key;

        if (!g_hash_table_contains(
                desired,
                group_id)) {
            g_ptr_array_add(
                stale,
                g_strdup(group_id)
            );
        }
    }

    for (guint i = 0;
            i < stale->len;
            i++) {
        const char *group_id =
            g_ptr_array_index(stale, i);

        if (!set_tabgroup(
                config_dir,
                group_id,
                false)) {
            return false;
        }
    }

    GHashTableIter desired_iter;
    g_hash_table_iter_init(
        &desired_iter,
        desired
    );

    while (g_hash_table_iter_next(
            &desired_iter,
            &key,
            NULL)) {
        const char *group_id = key;

        if (!set_tabgroup(
                config_dir,
                group_id,
                true)) {
            return false;
        }
    }

    return true;
}

bool ewwii_adapter_reconcile_bar_roots(
        const char *config_dir,
        const char *projected_json) {
    if (!config_dir || config_dir[0] == '\0' ||
            !projected_json || projected_json[0] == '\0') {
        return false;
    }

    if (!ensure_runtime(config_dir)) {
        return false;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    GError *error = NULL;

    if (!json_parser_load_from_data(
            parser,
            projected_json,
            -1,
            &error)) {
        fprintf(
            stderr,
            "FAIL: Ewwii adapter projected state parse: %s\n",
            error ? error->message : "unknown"
        );
        g_clear_error(&error);
        return false;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
        fprintf(stderr, "FAIL: Ewwii adapter projected root is not object\n");
        return false;
    }

    JsonObject *root = json_node_get_object(root_node);
    if (!json_object_has_member(root, "block") ||
            strcmp(json_object_get_string_member(root, "block"), "bar") != 0 ||
            !json_object_has_member(root, "outputs")) {
        fprintf(stderr, "FAIL: Ewwii adapter requires bar projection\n");
        return false;
    }

    JsonArray *outputs = json_object_get_array_member(root, "outputs");
    if (!outputs || json_array_get_length(outputs) > EWWII_ADAPTER_MAX_OUTPUTS) {
        fprintf(stderr, "FAIL: Ewwii adapter invalid output count\n");
        return false;
    }

    bool desired_background[EWWII_ADAPTER_MAX_OUTPUTS] = {0};
    bool desired_bar[EWWII_ADAPTER_MAX_OUTPUTS] = {0};

    g_autoptr(GHashTable) desired_tabgroups =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    if (!desired_tabgroups) {
        return false;
    }

    for (guint i = 0; i < json_array_get_length(outputs); i++) {
        JsonNode *node = json_array_get_element(outputs, i);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) {
            return false;
        }

        JsonObject *output = json_node_get_object(node);
        if (!json_object_has_member(output, "index") ||
                !json_object_has_member(output, "bar_visible")) {
            return false;
        }

        const gint64 raw_index = json_object_get_int_member(output, "index");
        if (raw_index < 0 || raw_index >= EWWII_ADAPTER_MAX_OUTPUTS) {
            return false;
        }

        const guint index = (guint)raw_index;
        desired_background[index] = true;
        desired_bar[index] = json_object_get_boolean_member(output, "bar_visible");

        if (!json_object_has_member(
                output,
                "groups")) {
            return false;
        }

        JsonArray *groups =
            json_object_get_array_member(
                output,
                "groups"
            );

        if (!groups) {
            return false;
        }

        for (guint group_index = 0;
                group_index <
                    json_array_get_length(groups);
                group_index++) {
            JsonNode *group_node =
                json_array_get_element(
                    groups,
                    group_index
                );

            if (!group_node ||
                    !JSON_NODE_HOLDS_OBJECT(group_node)) {
                return false;
            }

            JsonObject *group =
                json_node_get_object(
                    group_node
                );

            if (!json_object_has_member(
                    group,
                    "id")) {
                return false;
            }

            const char *group_id =
                json_object_get_string_member(
                    group,
                    "id"
                );

            if (!positive_decimal_id(group_id)) {
                return false;
            }

            g_hash_table_add(
                desired_tabgroups,
                g_strdup(group_id)
            );
        }
    }

    if (!reconcile_tabgroups(
            config_dir,
            desired_tabgroups)) {
        return false;
    }

    for (guint index = 0; index < EWWII_ADAPTER_MAX_OUTPUTS; index++) {
        if (!set_background(config_dir, index, desired_background[index]) ||
                !set_bar(config_dir, index, desired_bar[index])) {
            return false;
        }
    }

    return true;
}
