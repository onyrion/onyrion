#include "ui_runtime.h"
#include "ui_daily_v11.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct ui_workspace {
    char *id;
    bool active;
} UiWorkspace;

typedef struct ui_window {
    char *id;
    bool active;
    char *label;
    char *tooltip;
} UiWindow;

typedef struct daily_ui_runtime {
    char *config_dir;
    GHashTable *workspace_created;
    GHashTable *tab_created;
    bool initialized;
} DailyUiRuntime;

static DailyUiRuntime daily_ui = {0};

static void ui_workspace_free(
        gpointer data) {
    UiWorkspace *workspace = data;

    if (!workspace) {
        return;
    }

    g_free(workspace->id);
    g_free(workspace);
}

static void ui_window_free(
        gpointer data) {
    UiWindow *window = data;

    if (!window) {
        return;
    }

    g_free(window->id);
    g_free(window->label);
    g_free(window->tooltip);
    g_free(window);
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
        g_ascii_strtoull(
            value,
            &end,
            10
        );

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

static gint compare_workspace_id(
        gconstpointer left,
        gconstpointer right) {
    const UiWorkspace *a =
        *(UiWorkspace * const *)left;
    const UiWorkspace *b =
        *(UiWorkspace * const *)right;

    guint64 a_id = 0;
    guint64 b_id = 0;

    (void)positive_decimal_id(a->id, &a_id);
    (void)positive_decimal_id(b->id, &b_id);

    if (a_id < b_id) {
        return -1;
    }

    if (a_id > b_id) {
        return 1;
    }

    return 0;
}

static gint compare_window_id(
        gconstpointer left,
        gconstpointer right) {
    const UiWindow *a =
        *(UiWindow * const *)left;
    const UiWindow *b =
        *(UiWindow * const *)right;

    guint64 a_id = 0;
    guint64 b_id = 0;

    (void)positive_decimal_id(a->id, &a_id);
    (void)positive_decimal_id(b->id, &b_id);

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
        g_ascii_strdown(
            app_id,
            -1
        );

    if (!lower) {
        return NULL;
    }

    if (strstr(lower, "zen")) {
        return g_strdup("Zen");
    }

    if (strstr(lower, "kitty")) {
        return g_strdup("Kitty");
    }

    if (strstr(lower, "ayugram")) {
        return g_strdup("AyuGram");
    }

    if (strcmp(lower, "foot") == 0 ||
            g_str_has_suffix(lower, ".foot")) {
        return g_strdup("Foot");
    }

    const char *start = app_id;
    const char *dot = strrchr(app_id, '.');

    if (dot && dot[1] != '\0') {
        start = dot + 1;
    }

    GString *label =
        g_string_new(NULL);

    if (!label) {
        return NULL;
    }

    bool capitalize = true;

    for (const unsigned char *cursor =
            (const unsigned char *)start;
            *cursor;
            cursor++) {
        const unsigned char value = *cursor;

        if (value == '-' ||
                value == '_' ||
                value == '.') {
            if (label->len > 0U &&
                    label->str[label->len - 1U] != ' ') {
                g_string_append_c(label, ' ');
            }

            capitalize = true;
            continue;
        }

        if (capitalize &&
                value >= 'a' &&
                value <= 'z') {
            g_string_append_c(
                label,
                (char)(value - ('a' - 'A'))
            );
        } else {
            g_string_append_c(
                label,
                (char)value
            );
        }

        capitalize = false;
    }

    g_strstrip(label->str);

    if (label->str[0] == '\0') {
        g_string_assign(label, "App");
    }

    return g_string_free(label, false);
}

static bool title_looks_like_command_or_path(
        const char *title) {
    if (!title || title[0] == '\0') {
        return false;
    }

    if (strchr(title, '/')) {
        return true;
    }

    static const char *const prefixes[] = {
        "bash ",
        "dash ",
        "fish ",
        "sh ",
        "ssh ",
        "zsh ",
        NULL,
    };

    for (size_t i = 0;
            prefixes[i];
            i++) {
        if (g_str_has_prefix(
                title,
                prefixes[i])) {
            return true;
        }
    }

    return false;
}

static char *normalize_ui_text(
        const char *value) {
    if (!value) {
        return g_strdup("");
    }

    GString *normalized =
        g_string_new(NULL);

    if (!normalized) {
        return NULL;
    }

    bool pending_space = false;

    for (const char *cursor = value;
            *cursor;) {
        const gunichar character =
            g_utf8_get_char_validated(
                cursor,
                -1
            );

        if (character == (gunichar)-1 ||
                character == (gunichar)-2) {
            cursor++;
            continue;
        }

        cursor =
            g_utf8_next_char(cursor);

        if (g_unichar_iscntrl(character) ||
                g_unichar_isspace(character)) {
            pending_space =
                normalized->len > 0U;
            continue;
        }

        if (pending_space) {
            g_string_append_c(
                normalized,
                ' '
            );
            pending_space = false;
        }

        g_string_append_unichar(
            normalized,
            character
        );
    }

    g_strstrip(normalized->str);
    return g_string_free(
        normalized,
        false
    );
}

static char *truncate_ui_label(
        const char *value,
        glong max_chars) {
    if (!value || max_chars < 2) {
        return g_strdup("");
    }

    const glong length =
        g_utf8_strlen(
            value,
            -1
        );

    if (length <= max_chars) {
        return g_strdup(value);
    }

    const char *cut =
        g_utf8_offset_to_pointer(
            value,
            max_chars - 1
        );

    return g_strdup_printf(
        "%.*s…",
        (int)(cut - value),
        value
    );
}

static char *window_presentation_label(
        const char *title,
        const char *app_id) {
    g_autofree char *normalized =
        normalize_ui_text(title);

    if (!normalized) {
        return NULL;
    }

    g_autofree char *candidate = NULL;

    if (normalized[0] == '\0' ||
            title_looks_like_command_or_path(
                normalized)) {
        candidate =
            compact_app_identity(
                app_id
            );
    } else {
        candidate =
            g_strdup(normalized);
    }

    if (!candidate) {
        return NULL;
    }

    g_strstrip(candidate);

    if (candidate[0] == '\0') {
        g_free(candidate);
        candidate = g_strdup("App");

        if (!candidate) {
            return NULL;
        }
    }

    return truncate_ui_label(
        candidate,
        14
    );
}

static JsonArray *object_array_member(
        JsonObject *object,
        const char *name) {
    JsonNode *node =
        json_object_get_member(
            object,
            name
        );

    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) {
        return NULL;
    }

    return json_node_get_array(node);
}

static const char *object_string_member(
        JsonObject *object,
        const char *name) {
    JsonNode *node =
        json_object_get_member(
            object,
            name
        );

    if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
        return NULL;
    }

    return json_node_get_string(node);
}

static bool object_boolean_member(
        JsonObject *object,
        const char *name,
        bool *value) {
    JsonNode *node =
        json_object_get_member(
            object,
            name
        );

    if (!node ||
            !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
        return false;
    }

    *value =
        json_node_get_boolean(node);

    return true;
}

static char *json_quote_string(
        const char *value) {
    JsonNode *node =
        json_node_new(JSON_NODE_VALUE);

    if (!node) {
        return NULL;
    }

    json_node_set_string(
        node,
        value ? value : ""
    );

    JsonGenerator *generator =
        json_generator_new();

    if (!generator) {
        json_node_free(node);
        return NULL;
    }

    json_generator_set_root(
        generator,
        node
    );

    char *quoted =
        json_generator_to_data(
            generator,
            NULL
        );

    g_object_unref(generator);
    json_node_free(node);

    return quoted;
}

static bool spawn_capture(
        char **argv,
        char **stdout_text,
        char **stderr_text) {
    GError *error = NULL;
    gint wait_status = 0;

    const gboolean spawned =
        g_spawn_sync(
            NULL,
            argv,
            NULL,
            G_SPAWN_SEARCH_PATH,
            NULL,
            NULL,
            stdout_text,
            stderr_text,
            &wait_status,
            &error
        );

    if (!spawned) {
        if (error) {
            fprintf(
                stderr,
                "FAIL: spawn %s: %s\n",
                argv[0],
                error->message
            );
        }

        g_clear_error(&error);
        return false;
    }

    const gboolean success =
        g_spawn_check_wait_status(
            wait_status,
            &error
        );

    if (!success) {
        g_clear_error(&error);
    }

    return success;
}

static bool ewwii_widget_control(
        const char *config_dir,
        const char *const args[],
        guint attempts,
        bool allow_failure) {
    const char *ewwii =
        g_getenv("EWWII_BIN");

    if (!ewwii || ewwii[0] == '\0') {
        ewwii = "ewwii";
    }

    GPtrArray *argv =
        g_ptr_array_new();

    if (!argv) {
        return false;
    }

    g_ptr_array_add(argv, (gpointer)ewwii);
    g_ptr_array_add(argv, "--config");
    g_ptr_array_add(argv, (gpointer)config_dir);
    g_ptr_array_add(argv, "widget-control");

    for (size_t i = 0; args[i]; i++) {
        g_ptr_array_add(
            argv,
            (gpointer)args[i]
        );
    }

    g_ptr_array_add(argv, NULL);

    char *last_stderr = NULL;

    for (guint attempt = 0;
            attempt < attempts;
            attempt++) {
        char *stdout_text = NULL;
        char *stderr_text = NULL;

        const bool success =
            spawn_capture(
                (char **)argv->pdata,
                &stdout_text,
                &stderr_text
            );

        g_free(stdout_text);
        g_free(last_stderr);
        last_stderr = stderr_text;

        if (success) {
            g_free(last_stderr);
            g_ptr_array_free(argv, true);
            return true;
        }

        if (allow_failure) {
            g_free(last_stderr);
            g_ptr_array_free(argv, true);
            return true;
        }

        if (attempt + 1U < attempts) {
            g_usleep(50U * 1000U);
        }
    }

    fprintf(
        stderr,
        "FAIL: ewwii widget-control"
    );

    for (size_t i = 0; args[i]; i++) {
        fprintf(
            stderr,
            " %s",
            args[i]
        );
    }

    fputc('\n', stderr);

    if (last_stderr && last_stderr[0] != '\0') {
        fputs(last_stderr, stderr);

        if (last_stderr[strlen(last_stderr) - 1U] != '\n') {
            fputc('\n', stderr);
        }
    }

    g_free(last_stderr);
    g_ptr_array_free(argv, true);
    return false;
}

static bool wait_for_parent_widget(
        const char *config_dir,
        const char *widget) {
    const char *args[] = {
        "property-get",
        "spacing",
        "--widget",
        widget,
        NULL,
    };

    return ewwii_widget_control(
        config_dir,
        args,
        100U,
        false
    );
}

static bool ewwii_window_active(
        const char *config_dir,
        const char *window,
        bool *active_out) {
    const char *ewwii =
        g_getenv("EWWII_BIN");

    if (!ewwii || ewwii[0] == '\0') {
        ewwii = "ewwii";
    }

    char *argv[] = {
        (char *)ewwii,
        "--config",
        (char *)config_dir,
        "active-windows",
        NULL,
    };
    char *stdout_text = NULL;
    char *stderr_text = NULL;

    const bool success =
        spawn_capture(
            argv,
            &stdout_text,
            &stderr_text
        );

    if (!success) {
        if (stderr_text &&
                stderr_text[0] != '\0') {
            fputs(stderr_text, stderr);
        }

        g_free(stdout_text);
        g_free(stderr_text);
        return false;
    }

    g_autofree char *needle =
        g_strdup_printf(
            "%s:",
            window
        );

    if (!needle) {
        g_free(stdout_text);
        g_free(stderr_text);
        return false;
    }

    *active_out =
        stdout_text &&
        strstr(
            stdout_text,
            needle
        ) != NULL;

    g_free(stdout_text);
    g_free(stderr_text);
    return true;
}

static bool ewwii_window_command(
        const char *config_dir,
        const char *command,
        const char *window) {
    const char *ewwii =
        g_getenv("EWWII_BIN");

    if (!ewwii || ewwii[0] == '\0') {
        ewwii = "ewwii";
    }

    char *argv[] = {
        (char *)ewwii,
        "--config",
        (char *)config_dir,
        (char *)command,
        (char *)window,
        NULL,
    };
    char *stdout_text = NULL;
    char *stderr_text = NULL;

    const bool success =
        spawn_capture(
            argv,
            &stdout_text,
            &stderr_text
        );

    g_free(stdout_text);

    if (!success &&
            stderr_text &&
            stderr_text[0] != '\0') {
        fputs(stderr_text, stderr);
    }

    g_free(stderr_text);
    return success;
}

static void reset_dynamic_ui_tracking(
        DailyUiRuntime *runtime) {
    if (runtime->workspace_created) {
        g_hash_table_remove_all(
            runtime->workspace_created
        );
    }

    if (runtime->tab_created) {
        g_hash_table_remove_all(
            runtime->tab_created
        );
    }

    runtime->initialized = false;
}

static bool set_daily_bar_visible(
        DailyUiRuntime *runtime,
        bool visible) {
    bool active = false;

    if (!ewwii_window_active(
            runtime->config_dir,
            "onyrion-bar",
            &active)) {
        return false;
    }

    if (visible == active) {
        return true;
    }

    if (!ewwii_window_command(
            runtime->config_dir,
            visible ? "open" : "close",
            "onyrion-bar")) {
        return false;
    }

    reset_dynamic_ui_tracking(
        runtime
    );

    if (!visible) {
        fprintf(
            stderr,
            "DAILY UI bar=hidden reason=active-fullscreen\n"
        );
        return true;
    }

    if (!wait_for_parent_widget(
            runtime->config_dir,
            "onyrion-workspaces") ||
            !wait_for_parent_widget(
                runtime->config_dir,
                "onyrion-tabs")) {
        fprintf(
            stderr,
            "FAIL: restored bar widgets not ready\n"
        );
        return false;
    }

    fprintf(
        stderr,
        "DAILY UI bar=visible reason=normal-layout\n"
    );
    return true;
}

static char *workspace_widget_name(
        const char *id) {
    return g_strdup_printf(
        "onyrion-workspace-%s",
        id
    );
}

static char *tab_widget_name(
        const char *id) {
    return g_strdup_printf(
        "onyrion-tab-%s",
        id
    );
}

static char *workspace_button_definition(
        const char *id) {
    const char *shell =
        g_getenv("ONYRION_SHELL_BIN");

    if (!shell || shell[0] == '\0') {
        shell = "onyrion-shell";
    }

    g_autofree char *name =
        workspace_widget_name(id);
    g_autofree char *quoted_label =
        json_quote_string(id);
    g_autofree char *quoted_shell =
        g_shell_quote(shell);
    g_autofree char *quoted_id =
        g_shell_quote(id);
    g_autofree char *command =
        g_strdup_printf(
            "%s workspace %s",
            quoted_shell,
            quoted_id
        );
    g_autofree char *quoted_command =
        json_quote_string(command);

    if (!name ||
            !quoted_label ||
            !quoted_shell ||
            !quoted_id ||
            !command ||
            !quoted_command) {
        return NULL;
    }

    return g_strdup_printf(
        "Button \"%s\" { "
        "label = %s "
        "onclick = %s "
        "class = \"onyrion-workspace\" "
        "}",
        name,
        quoted_label,
        quoted_command
    );
}

static char *tab_button_definition(
        const UiWindow *window) {
    const char *shell =
        g_getenv("ONYRION_SHELL_BIN");

    if (!shell || shell[0] == '\0') {
        shell = "onyrion-shell";
    }

    g_autofree char *name =
        tab_widget_name(window->id);
    g_autofree char *quoted_label =
        json_quote_string(window->label);
    g_autofree char *quoted_tooltip =
        json_quote_string(window->tooltip);
    g_autofree char *quoted_shell =
        g_shell_quote(shell);
    g_autofree char *quoted_id =
        g_shell_quote(window->id);
    g_autofree char *command =
        g_strdup_printf(
            "%s window %s",
            quoted_shell,
            quoted_id
        );
    g_autofree char *quoted_command =
        json_quote_string(command);

    if (!name ||
            !quoted_label ||
            !quoted_tooltip ||
            !quoted_shell ||
            !quoted_id ||
            !command ||
            !quoted_command) {
        return NULL;
    }

    return g_strdup_printf(
        "Button \"%s\" { "
        "label = %s "
        "tooltip = %s "
        "width = 104 "
        "onclick = %s "
        "class = \"onyrion-tab\" "
        "}",
        name,
        quoted_label,
        quoted_tooltip,
        quoted_command
    );
}

static bool parse_daily_ui_state(
        const char *json,
        GPtrArray **workspaces_out,
        GPtrArray **windows_out,
        bool *active_fullscreen_out) {
    *active_fullscreen_out = false;

    JsonParser *parser =
        json_parser_new();

    if (!parser) {
        return false;
    }

    GError *error = NULL;

    if (!json_parser_load_from_data(
            parser,
            json,
            -1,
            &error)) {
        fprintf(
            stderr,
            "FAIL: daily UI state JSON: %s\n",
            error ? error->message : "parse failed"
        );
        g_clear_error(&error);
        g_object_unref(parser);
        return false;
    }

    JsonNode *root =
        json_parser_get_root(parser);

    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        fprintf(
            stderr,
            "FAIL: daily UI state root is not an object\n"
        );
        g_object_unref(parser);
        return false;
    }

    JsonObject *object =
        json_node_get_object(root);
    JsonArray *workspace_array =
        object_array_member(object, "workspaces");
    JsonArray *group_array =
        object_array_member(object, "groups");
    JsonArray *window_array =
        object_array_member(object, "windows");

    if (!workspace_array ||
            !group_array ||
            !window_array) {
        fprintf(
            stderr,
            "FAIL: daily UI state arrays missing\n"
        );
        g_object_unref(parser);
        return false;
    }

    GPtrArray *workspaces =
        g_ptr_array_new_with_free_func(
            ui_workspace_free
        );
    GPtrArray *windows =
        g_ptr_array_new_with_free_func(
            ui_window_free
        );

    if (!workspaces || !windows) {
        if (workspaces) {
            g_ptr_array_free(workspaces, true);
        }

        if (windows) {
            g_ptr_array_free(windows, true);
        }

        g_object_unref(parser);
        return false;
    }

    const char *active_workspace_id = NULL;

    for (guint i = 0;
            i < json_array_get_length(workspace_array);
            i++) {
        JsonNode *entry =
            json_array_get_element(
                workspace_array,
                i
            );

        if (!entry || !JSON_NODE_HOLDS_OBJECT(entry)) {
            fprintf(
                stderr,
                "FAIL: invalid workspace entry\n"
            );
            goto fail;
        }

        JsonObject *workspace_object =
            json_node_get_object(entry);
        const char *id =
            object_string_member(
                workspace_object,
                "id"
            );
        bool active = false;

        if (!positive_decimal_id(id, NULL) ||
                !object_boolean_member(
                    workspace_object,
                    "active",
                    &active)) {
            fprintf(
                stderr,
                "FAIL: invalid workspace state\n"
            );
            goto fail;
        }

        if (active) {
            if (active_workspace_id) {
                fprintf(
                    stderr,
                    "FAIL: more than one active workspace\n"
                );
                goto fail;
            }

            active_workspace_id = id;
        }

        UiWorkspace *workspace =
            g_new0(UiWorkspace, 1);

        if (!workspace) {
            goto fail;
        }

        workspace->id = g_strdup(id);
        workspace->active = active;

        if (!workspace->id) {
            ui_workspace_free(workspace);
            goto fail;
        }

        g_ptr_array_add(
            workspaces,
            workspace
        );
    }

    const char *active_group_id = NULL;

    if (active_workspace_id) {
        for (guint i = 0;
                i < json_array_get_length(group_array);
                i++) {
            JsonNode *entry =
                json_array_get_element(
                    group_array,
                    i
                );

            if (!entry || !JSON_NODE_HOLDS_OBJECT(entry)) {
                fprintf(
                    stderr,
                    "FAIL: invalid group entry\n"
                );
                goto fail;
            }

            JsonObject *group_object =
                json_node_get_object(entry);
            const char *id =
                object_string_member(
                    group_object,
                    "id"
                );
            const char *workspace_id =
                object_string_member(
                    group_object,
                    "workspace_id"
                );
            bool active = false;

            if (!positive_decimal_id(id, NULL) ||
                    !positive_decimal_id(
                        workspace_id,
                        NULL
                    ) ||
                    !object_boolean_member(
                        group_object,
                        "active",
                        &active)) {
                fprintf(
                    stderr,
                    "FAIL: invalid group state\n"
                );
                goto fail;
            }

            if (active &&
                    strcmp(
                        workspace_id,
                        active_workspace_id
                    ) == 0) {
                if (active_group_id) {
                    fprintf(
                        stderr,
                        "FAIL: more than one active group\n"
                    );
                    goto fail;
                }

                active_group_id = id;
            }
        }
    }

    if (active_group_id) {
        for (guint i = 0;
                i < json_array_get_length(window_array);
                i++) {
            JsonNode *entry =
                json_array_get_element(
                    window_array,
                    i
                );

            if (!entry || !JSON_NODE_HOLDS_OBJECT(entry)) {
                fprintf(
                    stderr,
                    "FAIL: invalid window entry\n"
                );
                goto fail;
            }

            JsonObject *window_object =
                json_node_get_object(entry);
            const char *id =
                object_string_member(
                    window_object,
                    "id"
                );
            const char *group_id =
                object_string_member(
                    window_object,
                    "group_id"
                );
            const char *title =
                object_string_member(
                    window_object,
                    "title"
                );
            const char *app_id =
                object_string_member(
                    window_object,
                    "app_id"
                );
            bool active = false;
            bool fullscreen = false;

            if (!positive_decimal_id(id, NULL) ||
                    !positive_decimal_id(group_id, NULL) ||
                    !title ||
                    !app_id ||
                    !object_boolean_member(
                        window_object,
                        "active",
                        &active)) {
                fprintf(
                    stderr,
                    "FAIL: invalid window state\n"
                );
                goto fail;
            }

            if (json_object_has_member(
                    window_object,
                    "fullscreen") &&
                    !object_boolean_member(
                        window_object,
                        "fullscreen",
                        &fullscreen)) {
                fprintf(
                    stderr,
                    "FAIL: invalid window fullscreen state\n"
                );
                goto fail;
            }

            if (strcmp(group_id, active_group_id) != 0) {
                continue;
            }

            if (active && fullscreen) {
                *active_fullscreen_out = true;
            }

            g_autofree char *label_storage =
                window_presentation_label(
                    title,
                    app_id
                );
            g_autofree char *tooltip_storage =
                normalize_ui_text(title);

            if (!label_storage ||
                    !tooltip_storage) {
                goto fail;
            }

            if (tooltip_storage[0] == '\0') {
                g_free(tooltip_storage);
                tooltip_storage =
                    compact_app_identity(
                        app_id
                    );

                if (!tooltip_storage) {
                    goto fail;
                }
            }

            UiWindow *window =
                g_new0(UiWindow, 1);

            if (!window) {
                goto fail;
            }

            window->id = g_strdup(id);
            window->active = active;
            window->label = g_strdup(label_storage);
            window->tooltip =
                g_strdup(tooltip_storage);

            if (!window->id ||
                    !window->label ||
                    !window->tooltip) {
                ui_window_free(window);
                goto fail;
            }

            g_ptr_array_add(
                windows,
                window
            );
        }
    }

    g_ptr_array_sort(
        workspaces,
        compare_workspace_id
    );
    g_ptr_array_sort(
        windows,
        compare_window_id
    );

    *workspaces_out = workspaces;
    *windows_out = windows;

    g_object_unref(parser);
    return true;

fail:
    g_ptr_array_free(workspaces, true);
    g_ptr_array_free(windows, true);
    g_object_unref(parser);
    return false;
}

static bool sync_remove_missing(
        const char *config_dir,
        GHashTable *created,
        GHashTable *desired) {
    GHashTableIter iterator;
    gpointer key = NULL;

    g_hash_table_iter_init(
        &iterator,
        created
    );

    while (g_hash_table_iter_next(
            &iterator,
            &key,
            NULL)) {
        if (g_hash_table_contains(
                desired,
                key)) {
            continue;
        }

        const char *args[] = {
            "remove",
            key,
            NULL,
        };

        if (!ewwii_widget_control(
                config_dir,
                args,
                1U,
                true)) {
            return false;
        }

        g_hash_table_iter_remove(
            &iterator
        );
    }

    return true;
}

static bool sync_workspaces(
        DailyUiRuntime *runtime,
        GPtrArray *workspaces) {
    GHashTable *desired =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    if (!desired) {
        return false;
    }

    for (guint i = 0;
            i < workspaces->len;
            i++) {
        UiWorkspace *workspace =
            g_ptr_array_index(
                workspaces,
                i
            );
        g_autofree char *name =
            workspace_widget_name(
                workspace->id
            );

        if (!name) {
            g_hash_table_unref(desired);
            return false;
        }

        g_hash_table_add(
            desired,
            g_strdup(name)
        );
    }

    if (!sync_remove_missing(
            runtime->config_dir,
            runtime->workspace_created,
            desired)) {
        g_hash_table_unref(desired);
        return false;
    }

    for (guint i = 0;
            i < workspaces->len;
            i++) {
        UiWorkspace *workspace =
            g_ptr_array_index(
                workspaces,
                i
            );
        g_autofree char *name =
            workspace_widget_name(
                workspace->id
            );

        if (!name) {
            g_hash_table_unref(desired);
            return false;
        }

        if (!g_hash_table_contains(
                runtime->workspace_created,
                name)) {
            g_autofree char *definition =
                workspace_button_definition(
                    workspace->id
                );

            if (!definition) {
                g_hash_table_unref(desired);
                return false;
            }

            const char *create_args[] = {
                "create",
                "--parent",
                "onyrion-workspaces",
                definition,
                NULL,
            };

            if (!ewwii_widget_control(
                    runtime->config_dir,
                    create_args,
                    20U,
                    false)) {
                g_hash_table_unref(desired);
                return false;
            }

            g_hash_table_add(
                runtime->workspace_created,
                g_strdup(name)
            );
        }

        g_autofree char *label =
            g_strdup_printf(
                workspace->active
                    ? "[%s]"
                    : "%s",
                workspace->id
            );
        g_autofree char *quoted =
            json_quote_string(label);
        g_autofree char *property =
            quoted
                ? g_strdup_printf(
                    "label=%s",
                    quoted
                )
                : NULL;

        if (!label || !quoted || !property) {
            g_hash_table_unref(desired);
            return false;
        }

        const char *update_args[] = {
            "property-update",
            "--widget",
            name,
            property,
            NULL,
        };

        if (!ewwii_widget_control(
                runtime->config_dir,
                update_args,
                20U,
                false)) {
            g_hash_table_unref(desired);
            return false;
        }
    }

    g_hash_table_unref(desired);
    return true;
}

static bool sync_tabs(
        DailyUiRuntime *runtime,
        GPtrArray *windows) {
    GHashTable *desired =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    if (!desired) {
        return false;
    }

    for (guint i = 0;
            i < windows->len;
            i++) {
        UiWindow *window =
            g_ptr_array_index(
                windows,
                i
            );
        g_autofree char *name =
            tab_widget_name(
                window->id
            );

        if (!name) {
            g_hash_table_unref(desired);
            return false;
        }

        g_hash_table_add(
            desired,
            g_strdup(name)
        );
    }

    if (!sync_remove_missing(
            runtime->config_dir,
            runtime->tab_created,
            desired)) {
        g_hash_table_unref(desired);
        return false;
    }

    for (guint i = 0;
            i < windows->len;
            i++) {
        UiWindow *window =
            g_ptr_array_index(
                windows,
                i
            );
        g_autofree char *name =
            tab_widget_name(
                window->id
            );

        if (!name) {
            g_hash_table_unref(desired);
            return false;
        }

        if (!g_hash_table_contains(
                runtime->tab_created,
                name)) {
            g_autofree char *definition =
                tab_button_definition(window);

            if (!definition) {
                g_hash_table_unref(desired);
                return false;
            }

            const char *create_args[] = {
                "create",
                "--parent",
                "onyrion-tabs",
                definition,
                NULL,
            };

            if (!ewwii_widget_control(
                    runtime->config_dir,
                    create_args,
                    20U,
                    false)) {
                g_hash_table_unref(desired);
                return false;
            }

            g_hash_table_add(
                runtime->tab_created,
                g_strdup(name)
            );
        }

        g_autofree char *label =
            g_strdup_printf(
                window->active
                    ? "● %s"
                    : "%s",
                window->label
            );
        g_autofree char *quoted =
            json_quote_string(label);
        g_autofree char *quoted_tooltip =
            json_quote_string(window->tooltip);
        g_autofree char *properties =
            quoted && quoted_tooltip
                ? g_strdup_printf(
                    "label=%s,tooltip=%s",
                    quoted,
                    quoted_tooltip
                )
                : NULL;

        if (!label ||
                !quoted ||
                !quoted_tooltip ||
                !properties) {
            g_hash_table_unref(desired);
            return false;
        }

        const char *update_args[] = {
            "property-update",
            "--widget",
            name,
            properties,
            NULL,
        };

        if (!ewwii_widget_control(
                runtime->config_dir,
                update_args,
                20U,
                false)) {
            g_hash_table_unref(desired);
            return false;
        }
    }

    g_hash_table_unref(desired);
    return true;
}

static bool ensure_daily_ui_runtime(
        const char *config_dir) {
    if (daily_ui.config_dir &&
            strcmp(
                daily_ui.config_dir,
                config_dir
            ) == 0) {
        return true;
    }

    g_clear_pointer(
        &daily_ui.config_dir,
        g_free
    );

    if (daily_ui.workspace_created) {
        g_hash_table_unref(
            daily_ui.workspace_created
        );
    }

    if (daily_ui.tab_created) {
        g_hash_table_unref(
            daily_ui.tab_created
        );
    }

    daily_ui = (DailyUiRuntime){0};
    daily_ui.config_dir =
        g_strdup(config_dir);
    daily_ui.workspace_created =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );
    daily_ui.tab_created =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    return daily_ui.config_dir &&
        daily_ui.workspace_created &&
        daily_ui.tab_created;
}

bool daily_ui_sync_apply(
        const char *config_dir,
        const char *json) {
    if (json &&
            strstr(json, "\"outputs\"")) {
        return daily_ui_v11_sync_apply(
            config_dir,
            json
        );
    }

    if (!config_dir ||
            config_dir[0] == '\0' ||
            !json) {
        return false;
    }

    if (!ensure_daily_ui_runtime(
            config_dir)) {
        return false;
    }

    GPtrArray *workspaces = NULL;
    GPtrArray *windows = NULL;
    bool active_fullscreen = false;

    if (!parse_daily_ui_state(
            json,
            &workspaces,
            &windows,
            &active_fullscreen)) {
        return false;
    }

    if (!set_daily_bar_visible(
            &daily_ui,
            !active_fullscreen)) {
        g_ptr_array_free(workspaces, true);
        g_ptr_array_free(windows, true);
        fprintf(
            stderr,
            "FAIL: daily bar visibility transition failed\n"
        );
        return false;
    }

    if (active_fullscreen) {
        g_ptr_array_free(workspaces, true);
        g_ptr_array_free(windows, true);
        return true;
    }

    if (!daily_ui.initialized) {
        if (!wait_for_parent_widget(
                config_dir,
                "onyrion-workspaces") ||
                !wait_for_parent_widget(
                    config_dir,
                    "onyrion-tabs")) {
            g_ptr_array_free(workspaces, true);
            g_ptr_array_free(windows, true);
            fprintf(
                stderr,
                "FAIL: persistent bar widgets not ready before daily UI sync\n"
            );
            return false;
        }
        for (guint i = 0;
                i < workspaces->len;
                i++) {
            UiWorkspace *workspace =
                g_ptr_array_index(
                    workspaces,
                    i
                );
            g_autofree char *name =
                workspace_widget_name(
                    workspace->id
                );
            const char *args[] = {
                "remove",
                name,
                NULL,
            };

            if (!name ||
                    !ewwii_widget_control(
                        config_dir,
                        args,
                        1U,
                        true)) {
                g_ptr_array_free(workspaces, true);
                g_ptr_array_free(windows, true);
                return false;
            }
        }

        for (guint i = 0;
                i < windows->len;
                i++) {
            UiWindow *window =
                g_ptr_array_index(
                    windows,
                    i
                );
            g_autofree char *name =
                tab_widget_name(
                    window->id
                );
            const char *args[] = {
                "remove",
                name,
                NULL,
            };

            if (!name ||
                    !ewwii_widget_control(
                        config_dir,
                        args,
                        1U,
                        true)) {
                g_ptr_array_free(workspaces, true);
                g_ptr_array_free(windows, true);
                return false;
            }
        }

        daily_ui.initialized = true;
    }

    const bool success =
        sync_workspaces(
            &daily_ui,
            workspaces
        ) &&
        sync_tabs(
            &daily_ui,
            windows
        );

    g_ptr_array_free(workspaces, true);
    g_ptr_array_free(windows, true);

    if (!success) {
        fprintf(
            stderr,
            "FAIL: daily UI C sync failed\n"
        );
    }

    return success;
}

static char *read_trimmed_file(
        const char *path) {
    char *contents = NULL;

    if (!g_file_get_contents(
            path,
            &contents,
            NULL,
            NULL)) {
        return NULL;
    }

    g_strstrip(contents);
    return contents;
}

static char *battery_status(void) {
    const char *root =
        "/sys/class/power_supply";
    GDir *directory =
        g_dir_open(
            root,
            0,
            NULL
        );

    if (!directory) {
        return g_strdup("BAT --");
    }

    char *battery_name = NULL;
    const char *name = NULL;

    while ((name = g_dir_read_name(directory))) {
        if (!g_str_has_prefix(name, "BAT")) {
            continue;
        }

        if (!battery_name ||
                g_strcmp0(name, battery_name) < 0) {
            g_free(battery_name);
            battery_name = g_strdup(name);
        }
    }

    g_dir_close(directory);

    if (!battery_name) {
        return g_strdup("BAT --");
    }

    g_autofree char *capacity_path =
        g_build_filename(
            root,
            battery_name,
            "capacity",
            NULL
        );
    g_autofree char *status_path =
        g_build_filename(
            root,
            battery_name,
            "status",
            NULL
        );
    g_autofree char *capacity =
        read_trimmed_file(capacity_path);
    g_autofree char *state =
        read_trimmed_file(status_path);

    g_free(battery_name);

    if (!capacity ||
            !positive_decimal_id(capacity, NULL)) {
        if (capacity && strcmp(capacity, "0") == 0) {
            return g_strdup("BAT 0%");
        }

        return g_strdup("BAT --");
    }

    const char *suffix = "";

    if (state &&
            g_ascii_strncasecmp(
                state,
                "charging",
                8U
            ) == 0) {
        suffix = "+";
    } else if (state &&
            g_ascii_strncasecmp(
                state,
                "discharging",
                11U
            ) == 0) {
        suffix = "-";
    }

    return g_strdup_printf(
        "BAT %s%%%s",
        capacity,
        suffix
    );
}

static char *default_interface(void) {
    FILE *route =
        fopen(
            "/proc/net/route",
            "r"
        );

    if (route) {
        char line[512];

        (void)fgets(
            line,
            sizeof(line),
            route
        );

        while (fgets(
                line,
                sizeof(line),
                route)) {
            char interface[128];
            unsigned long destination = 0;
            unsigned long gateway = 0;
            unsigned long flags = 0;

            if (sscanf(
                    line,
                    "%127s %lx %lx %lx",
                    interface,
                    &destination,
                    &gateway,
                    &flags) == 4 &&
                    destination == 0 &&
                    (flags & 0x1UL) != 0) {
                fclose(route);
                return g_strdup(interface);
            }
        }

        fclose(route);
    }

    const char *net_root =
        "/sys/class/net";
    GDir *directory =
        g_dir_open(
            net_root,
            0,
            NULL
        );

    if (!directory) {
        return NULL;
    }

    char *selected = NULL;
    const char *name = NULL;

    while ((name = g_dir_read_name(directory))) {
        if (strcmp(name, "lo") == 0) {
            continue;
        }

        g_autofree char *operstate_path =
            g_build_filename(
                net_root,
                name,
                "operstate",
                NULL
            );
        g_autofree char *operstate =
            read_trimmed_file(
                operstate_path
            );

        if (!operstate ||
                strcmp(operstate, "up") != 0) {
            continue;
        }

        if (!selected ||
                g_strcmp0(name, selected) < 0) {
            g_free(selected);
            selected = g_strdup(name);
        }
    }

    g_dir_close(directory);
    return selected;
}

static char *network_status(void) {
    g_autofree char *interface =
        default_interface();

    if (!interface) {
        return g_strdup("NET offline");
    }

    g_autofree char *operstate_path =
        g_build_filename(
            "/sys/class/net",
            interface,
            "operstate",
            NULL
        );
    g_autofree char *operstate =
        read_trimmed_file(
            operstate_path
        );

    if (operstate &&
            operstate[0] != '\0' &&
            strcmp(operstate, "up") != 0) {
        return g_strdup_printf(
            "NET %s %s",
            interface,
            operstate
        );
    }

    return g_strdup_printf(
        "NET %s",
        interface
    );
}

static char *audio_status(
        const char **source_out) {
    *source_out = "none";
    return g_strdup("VOL --");
}

static char *status_json(void) {
    char clock_text[16] = "--:--";
    const time_t now = time(NULL);
    struct tm local_time;

    if (now != (time_t)-1 &&
            localtime_r(
                &now,
                &local_time)) {
        (void)strftime(
            clock_text,
            sizeof(clock_text),
            "%H:%M",
            &local_time
        );
    }

    g_autofree char *battery =
        battery_status();
    g_autofree char *network =
        network_status();
    const char *audio_source = "none";
    g_autofree char *audio =
        audio_status(&audio_source);

    JsonBuilder *builder =
        json_builder_new();

    if (!builder) {
        return NULL;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "clock");
    json_builder_add_string_value(builder, clock_text);

    json_builder_set_member_name(builder, "battery");
    json_builder_add_string_value(
        builder,
        battery ? battery : "BAT --"
    );

    json_builder_set_member_name(builder, "network");
    json_builder_add_string_value(
        builder,
        network ? network : "NET offline"
    );

    json_builder_set_member_name(builder, "audio");
    json_builder_add_string_value(
        builder,
        audio ? audio : "VOL --"
    );

    json_builder_set_member_name(
        builder,
        "audio_source"
    );
    json_builder_add_string_value(
        builder,
        audio_source
    );

    json_builder_end_object(builder);

    JsonNode *root =
        json_builder_get_root(builder);
    JsonGenerator *generator =
        json_generator_new();

    if (!root || !generator) {
        if (root) {
            json_node_free(root);
        }

        if (generator) {
            g_object_unref(generator);
        }

        g_object_unref(builder);
        return NULL;
    }

    json_generator_set_root(
        generator,
        root
    );

    char *json =
        json_generator_to_data(
            generator,
            NULL
        );

    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);

    return json;
}

bool watch_status_stream(void) {
    for (;;) {
        g_autofree char *json =
            status_json();

        if (!json) {
            fprintf(
                stderr,
                "FAIL: cannot build Shell status snapshot\n"
            );
            return false;
        }

        if (fputs(json, stdout) == EOF ||
                fputc('\n', stdout) == EOF ||
                fflush(stdout) == EOF) {
            if (errno == EPIPE) {
                return true;
            }

            fprintf(
                stderr,
                "FAIL: status stream write: %s\n",
                strerror(errno)
            );
            return false;
        }

        g_usleep(1000U * 1000U);
    }
}
