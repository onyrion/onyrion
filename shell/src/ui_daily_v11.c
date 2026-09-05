#include "ui_daily_v11.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <glib.h>
#include <json-glib/json-glib.h>

enum {
    DAILY_UI_MAX_OUTPUTS = 8,
    WORKSPACE_PAGE_SIZE = 5,
};

typedef struct daily_v11_runtime {
    char *config_dir;
    GHashTable *workspace_parent;
    GHashTable *group_parent;
} DailyV11Runtime;

static DailyV11Runtime runtime = {0};

static int daily_ui_lock_acquire(
        const char *config_dir) {
    const char *runtime_dir =
        g_get_user_runtime_dir();

    if (!runtime_dir ||
            runtime_dir[0] == '\0' ||
            !config_dir ||
            config_dir[0] == '\0') {
        fprintf(
            stderr,
            "FAIL: daily UI lock runtime/config missing\n"
        );
        return -1;
    }

    g_autofree char *digest =
        g_compute_checksum_for_string(
            G_CHECKSUM_SHA256,
            config_dir,
            -1
        );
    g_autofree char *lock_dir =
        g_build_filename(
            runtime_dir,
            "onyrion",
            NULL
        );

    if (!digest ||
            !lock_dir ||
            (g_mkdir_with_parents(
                lock_dir,
                0700
            ) != 0 &&
            errno != EEXIST)) {
        fprintf(
            stderr,
            "FAIL: daily UI lock directory: %s\n",
            strerror(errno)
        );
        return -1;
    }

    g_autofree char *basename =
        g_strdup_printf(
            "daily-ui-%s.lock",
            digest
        );
    g_autofree char *path =
        basename
            ? g_build_filename(
                lock_dir,
                basename,
                NULL
            )
            : NULL;

    if (!path) {
        return -1;
    }

    const int fd =
        open(
            path,
            O_CREAT | O_RDWR,
            0600
        );

    if (fd < 0) {
        fprintf(
            stderr,
            "FAIL: daily UI lock open: %s\n",
            strerror(errno)
        );
        return -1;
    }

    const int fd_flags =
        fcntl(
            fd,
            F_GETFD
        );

    if (fd_flags < 0 ||
            fcntl(
                fd,
                F_SETFD,
                fd_flags | FD_CLOEXEC
            ) < 0) {
        fprintf(
            stderr,
            "FAIL: daily UI lock cloexec: %s\n",
            strerror(errno)
        );
        close(fd);
        return -1;
    }

    for (;;) {
        if (flock(
                fd,
                LOCK_EX) == 0) {
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        fprintf(
            stderr,
            "FAIL: daily UI lock acquire: %s\n",
            strerror(errno)
        );
        close(fd);
        return -1;
    }

    fprintf(
        stderr,
        "DAILY UI LOCK acquired key=%s\n",
        digest
    );

    return fd;
}

static void daily_ui_lock_release(
        int fd) {
    if (fd < 0) {
        return;
    }

    if (flock(
            fd,
            LOCK_UN) != 0) {
        fprintf(
            stderr,
            "FAIL: daily UI lock release: %s\n",
            strerror(errno)
        );
    }

    close(fd);
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
        if (*cursor < '0' ||
                *cursor > '9') {
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

static bool ensure_runtime(
        const char *config_dir) {
    if (runtime.config_dir &&
            strcmp(
                runtime.config_dir,
                config_dir
            ) == 0) {
        return true;
    }

    g_clear_pointer(&runtime.config_dir, g_free);

    if (runtime.workspace_parent) {
        g_hash_table_unref(runtime.workspace_parent);
    }

    if (runtime.group_parent) {
        g_hash_table_unref(runtime.group_parent);
    }

    runtime = (DailyV11Runtime){0};
    runtime.config_dir = g_strdup(config_dir);
    runtime.workspace_parent =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            g_free
        );
    runtime.group_parent =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            g_free
        );

    return runtime.config_dir &&
        runtime.workspace_parent &&
        runtime.group_parent;
}

static bool ewwii_run(
        const char *config_dir,
        const char * const *tail,
        char **stdout_out,
        bool quiet_failure) {
    GPtrArray *argv =
        g_ptr_array_new();

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
                "FAIL: ewwii command status: %s\n",
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else if (!quiet_failure) {
        fprintf(
            stderr,
            "FAIL: cannot spawn ewwii: %s\n",
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

    if (stdout_out) {
        *stdout_out = captured_stdout;
        captured_stdout = NULL;
    }

    g_free(captured_stdout);
    g_free(captured_stderr);
    g_clear_error(&error);

    return success;
}

static bool window_active(
        const char *config_dir,
        const char *window_name) {
    const char *args[] = {
        "active-windows",
        NULL,
    };

    g_autofree char *output = NULL;

    if (!ewwii_run(
            config_dir,
            args,
            &output,
            true) ||
            !output) {
        return false;
    }

    g_auto(GStrv) lines =
        g_strsplit(output, "\n", -1);
    g_autofree char *prefix =
        g_strdup_printf(
            "%s:",
            window_name
        );

    if (!lines || !prefix) {
        return false;
    }

    for (size_t i = 0; lines[i]; i++) {
        if (g_str_has_prefix(
                lines[i],
                prefix)) {
            return true;
        }
    }

    return false;
}

static bool set_bar_visible(
        const char *config_dir,
        guint output_index,
        bool visible) {
    g_autofree char *window_name =
        g_strdup_printf(
            "onyrion-bar-%u",
            output_index
        );

    if (!window_name) {
        return false;
    }

    const bool active =
        window_active(
            config_dir,
            window_name
        );

    if (visible == active) {
        return true;
    }

    const char *args[] = {
        visible ? "open" : "close",
        window_name,
        NULL,
    };

    if (!ewwii_run(
            config_dir,
            args,
            NULL,
            false)) {
        return false;
    }

    for (unsigned attempt = 0;
            attempt < 40;
            attempt++) {
        if (window_active(
                config_dir,
                window_name) == visible) {
            return true;
        }

        g_usleep(50000);
    }

    fprintf(
        stderr,
        "FAIL: bar visibility did not converge output=%u visible=%u\n",
        output_index,
        visible ? 1U : 0U
    );

    return false;
}

static char *nbcl_quote(
        const char *value) {
    g_autofree char *escaped =
        g_strescape(
            value ? value : "",
            NULL
        );

    return escaped
        ? g_strdup_printf(
            "\"%s\"",
            escaped
        )
        : NULL;
}

static char *workspace_widget_name(
        const char *id) {
    return g_strdup_printf(
        "onyrion-workspace-%s",
        id
    );
}

static char *group_widget_name(
        const char *id) {
    return g_strdup_printf(
        "onyrion-group-%s",
        id
    );
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
        NULL,
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
            ? g_strdup_printf(
                "label=%s",
                quoted
            )
            : NULL;

    if (!quoted || !property) {
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
        NULL,
        quiet_failure
    );
}

static bool widget_create_button(
        const char *config_dir,
        const char *parent,
        const char *name,
        const char *label,
        const char *onclick,
        const char *css_class) {
    g_autofree char *name_q = nbcl_quote(name);
    g_autofree char *label_q = nbcl_quote(label);
    g_autofree char *onclick_q = nbcl_quote(onclick);
    g_autofree char *class_q = nbcl_quote(css_class);

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
                NULL,
                true)) {
            return true;
        }

        g_usleep(50000);
    }

    fprintf(
        stderr,
        "FAIL: cannot create widget=%s parent=%s\n",
        name,
        parent
    );

    return false;
}

static bool ensure_button(
        GHashTable *parents,
        const char *config_dir,
        const char *parent,
        const char *name,
        const char *label,
        const char *onclick,
        const char *css_class) {
    const char *old_parent =
        g_hash_table_lookup(
            parents,
            name
        );

    if (old_parent &&
            strcmp(
                old_parent,
                parent
            ) == 0 &&
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
            parents,
            name
        );
    }

    if (!widget_create_button(
            config_dir,
            parent,
            name,
            label,
            onclick,
            css_class)) {
        return false;
    }

    g_hash_table_replace(
        parents,
        g_strdup(name),
        g_strdup(parent)
    );

    return true;
}

static void remove_missing(
        GHashTable *parents,
        GHashTable *desired,
        const char *config_dir) {
    GHashTableIter iterator;
    gpointer key = NULL;

    g_hash_table_iter_init(
        &iterator,
        parents
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

        (void)widget_remove(
            config_dir,
            key,
            true
        );

        g_hash_table_iter_remove(
            &iterator
        );
    }
}

static JsonObject *array_object(
        JsonArray *array,
        guint index) {
    JsonNode *node =
        json_array_get_element(
            array,
            index
        );

    return node &&
        JSON_NODE_HOLDS_OBJECT(node)
            ? json_node_get_object(node)
            : NULL;
}

static const char *required_string(
        JsonObject *object,
        const char *member) {
    if (!object ||
            !json_object_has_member(
                object,
                member)) {
        return NULL;
    }

    JsonNode *node =
        json_object_get_member(
            object,
            member
        );

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
            !json_object_has_member(
                object,
                member)) {
        return false;
    }

    JsonNode *node =
        json_object_get_member(
            object,
            member
        );

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

    if (strstr(lower, "ayugram") ||
            strstr(lower, "telegram")) {
        return g_strdup("Telegram");
    }

    const char *base =
        strrchr(app_id, '.');

    base = base ? base + 1 : app_id;

    g_autofree char *clean =
        g_strdup(base);

    if (!clean) {
        return NULL;
    }

    for (char *cursor = clean;
            *cursor;
            cursor++) {
        if (*cursor == '-' ||
                *cursor == '_') {
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
            array_object(
                windows,
                i
            );

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
                strcmp(
                    window_group,
                    group_id
                ) != 0 ||
                strcmp(
                    placement,
                    "tiled"
                ) != 0) {
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

static bool output_has_fullscreen_window(
        JsonArray *windows,
        const char *workspace_id) {
    for (guint i = 0;
            i < json_array_get_length(windows);
            i++) {
        JsonObject *window =
            array_object(
                windows,
                i
            );
        const char *window_workspace =
            required_string(
                window,
                "workspace_id"
            );
        bool fullscreen = false;

        if (!window_workspace ||
                strcmp(
                    window_workspace,
                    workspace_id
                ) != 0 ||
                !required_boolean(
                    window,
                    "fullscreen",
                    &fullscreen)) {
            continue;
        }

        if (fullscreen) {
            return true;
        }
    }

    return false;
}

static char *workspace_ordinal_label(
        guint ordinal,
        bool visible) {
    return g_strdup_printf(
        visible
            ? "[%u]"
            : "%u",
        ordinal
    );
}

static bool sync_output_workspaces(
        const char *config_dir,
        guint output_index,
        const char *output_name,
        const char *visible_workspace_id,
        JsonArray *workspaces,
        GHashTable *desired) {
    if (!positive_decimal_id(
            visible_workspace_id,
            NULL)) {
        return false;
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
            array_object(
                workspaces,
                i
            );
        const char *id =
            required_string(
                workspace,
                "id"
            );
        const char *workspace_output =
            required_string(
                workspace,
                "output_name"
            );

        if (!id ||
                !workspace_output ||
                !positive_decimal_id(
                    id,
                    NULL)) {
            return false;
        }

        if (strcmp(
                workspace_output,
                output_name
            ) != 0) {
            continue;
        }

        g_ptr_array_add(
            assigned,
            workspace
        );
    }

    g_ptr_array_sort(
        assigned,
        compare_object_id
    );

    guint visible_index = 0;
    bool visible_found = false;

    for (guint i = 0;
            i < assigned->len;
            i++) {
        JsonObject *workspace =
            g_ptr_array_index(
                assigned,
                i
            );
        const char *id =
            required_string(
                workspace,
                "id"
            );
        bool visible = false;

        if (!id ||
                !required_boolean(
                    workspace,
                    "visible",
                    &visible)) {
            return false;
        }

        const bool id_is_visible =
            strcmp(
                id,
                visible_workspace_id
            ) == 0;

        if (visible != id_is_visible) {
            return false;
        }

        if (id_is_visible) {
            visible_index = i;
            visible_found = true;
        }
    }

    if (!visible_found) {
        return false;
    }

    const guint page_start =
        (visible_index /
            WORKSPACE_PAGE_SIZE) *
        WORKSPACE_PAGE_SIZE;
    const guint page_end =
        MIN(
            page_start +
                WORKSPACE_PAGE_SIZE,
            assigned->len
        );

    g_autofree char *parent =
        g_strdup_printf(
            "onyrion-workspaces-%u",
            output_index
        );

    if (!parent) {
        return false;
    }

    for (guint i = page_start;
            i < page_end;
            i++) {
        JsonObject *workspace =
            g_ptr_array_index(
                assigned,
                i
            );
        const char *id =
            required_string(
                workspace,
                "id"
            );
        const bool visible =
            i == visible_index;
        const guint ordinal =
            i + 1;

        g_autofree char *name =
            workspace_widget_name(id);
        g_autofree char *label =
            workspace_ordinal_label(
                ordinal,
                visible
            );
        g_autofree char *onclick =
            g_strdup_printf(
                "onyrionctl workspace activate %s",
                id
            );

        if (!name || !label || !onclick) {
            return false;
        }

        g_hash_table_add(
            desired,
            g_strdup(name)
        );

        if (!ensure_button(
                runtime.workspace_parent,
                config_dir,
                parent,
                name,
                label,
                onclick,
                "onyrion-workspace")) {
            return false;
        }
    }

    return true;
}

static bool sync_output_groups(
        const char *config_dir,
        guint output_index,
        const char *visible_workspace_id,
        JsonArray *groups,
        JsonArray *windows,
        GHashTable *desired) {
    g_autoptr(GPtrArray) selected =
        g_ptr_array_new();

    if (!selected) {
        return false;
    }

    for (guint i = 0;
            i < json_array_get_length(groups);
            i++) {
        JsonObject *group =
            array_object(
                groups,
                i
            );
        const char *workspace_id =
            required_string(
                group,
                "workspace_id"
            );

        if (!workspace_id) {
            return false;
        }

        if (strcmp(
                workspace_id,
                visible_workspace_id
            ) == 0) {
            g_ptr_array_add(
                selected,
                group
            );
        }
    }

    g_ptr_array_sort(
        selected,
        compare_object_id
    );

    g_autofree char *parent =
        g_strdup_printf(
            "onyrion-groups-%u",
            output_index
        );

    if (!parent) {
        return false;
    }

    for (guint i = 0;
            i < selected->len;
            i++) {
        JsonObject *group =
            g_ptr_array_index(
                selected,
                i
            );
        const char *id =
            required_string(
                group,
                "id"
            );
        bool group_active = false;

        if (!id ||
                !positive_decimal_id(
                    id,
                    NULL) ||
                !required_boolean(
                    group,
                    "active",
                    &group_active)) {
            return false;
        }

        JsonObject *window =
            find_active_window(
                windows,
                id
            );
        const char *app_id =
            window
                ? required_string(
                    window,
                    "app_id"
                )
                : NULL;

        g_autofree char *identity =
            compact_app_identity(
                app_id
            );
        g_autofree char *label =
            identity
                ? g_strdup_printf(
                    group_active
                        ? "[%s]"
                        : "%s",
                    identity
                )
                : NULL;
        g_autofree char *name =
            group_widget_name(id);
        g_autofree char *onclick =
            g_strdup_printf(
                "onyrionctl group focus %s",
                id
            );

        if (!label || !name || !onclick) {
            return false;
        }

        g_hash_table_add(
            desired,
            g_strdup(name)
        );

        if (!ensure_button(
                runtime.group_parent,
                config_dir,
                parent,
                name,
                label,
                onclick,
                "onyrion-tab")) {
            return false;
        }
    }

    return true;
}


static bool ewwii_background_instance_active(
        const char *config_dir,
        guint output_index,
        bool *active) {
    g_autofree char *instance =
        g_strdup_printf(
            "onyrion-background-%u",
            output_index
        );

    if (!instance) {
        return false;
    }

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
                "FAIL: background active-windows status: %s\n",
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else {
        fprintf(
            stderr,
            "FAIL: cannot query background instances: %s\n",
            error
                ? error->message
                : "unknown"
        );
    }

    if (success) {
        g_autofree char *prefix =
            g_strdup_printf(
                "%s:",
                instance
            );

        if (!prefix) {
            success = false;
        } else {
            *active = false;

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
        fputs(
            captured_stderr,
            stderr
        );
    }

    g_free(captured_stdout);
    g_free(captured_stderr);
    g_clear_error(&error);

    return success;
}

static bool set_background_visible(
        const char *config_dir,
        guint output_index,
        bool visible) {
    bool active = false;

    if (!ewwii_background_instance_active(
            config_dir,
            output_index,
            &active)) {
        return false;
    }

    if (active == visible) {
        return true;
    }

    g_autofree char *instance =
        g_strdup_printf(
            "onyrion-background-%u",
            output_index
        );

    if (!instance) {
        return false;
    }

    g_autofree char *screen = NULL;

    if (visible) {
        screen =
            g_strdup_printf(
                "%u",
                output_index
            );

        if (!screen) {
            return false;
        }
    }

    const char *open_argv[] = {
        "ewwii",
        "--config",
        config_dir,
        "open",
        "--id",
        instance,
        "--screen",
        screen,
        "onyrion-background",
        NULL,
    };

    const char *close_argv[] = {
        "ewwii",
        "--config",
        config_dir,
        "close",
        instance,
        NULL,
    };

    const char * const *argv =
        visible
            ? open_argv
            : close_argv;

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
                "FAIL: background %s output=%u status: %s\n",
                visible
                    ? "open"
                    : "close",
                output_index,
                wait_error
                    ? wait_error->message
                    : "unknown"
            );
        }

        g_clear_error(&wait_error);
    } else {
        fprintf(
            stderr,
            "FAIL: cannot %s background output=%u: %s\n",
            visible
                ? "open"
                : "close",
            output_index,
            error
                ? error->message
                : "unknown"
        );
    }

    if (!success &&
            captured_stderr &&
            captured_stderr[0] != '\0') {
        fputs(
            captured_stderr,
            stderr
        );
    }

    g_free(captured_stdout);
    g_free(captured_stderr);
    g_clear_error(&error);

    if (success) {
        fprintf(
            stderr,
            "DAILY UI background=%u state=%s screen=%u\n",
            output_index,
            visible
                ? "visible"
                : "hidden",
            output_index
        );
    }

    return success;
}
static bool daily_ui_v11_sync_apply_unlocked(
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
            "FAIL: v11 daily UI JSON: %s\n",
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

    if (!outputs || !workspaces ||
            !groups || !windows) {
        return false;
    }

    const guint output_count =
        json_array_get_length(outputs);

    if (output_count >
            DAILY_UI_MAX_OUTPUTS) {
        fprintf(
            stderr,
            "FAIL: v11 daily UI outputs=%u exceeds max=%u\n",
            output_count,
            DAILY_UI_MAX_OUTPUTS
        );
        return false;
    }

    g_autoptr(GHashTable) desired_workspaces =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );
    g_autoptr(GHashTable) desired_groups =
        g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            NULL
        );

    if (!desired_workspaces ||
            !desired_groups) {
        return false;
    }

    for (guint output_index = 0;
            output_index < output_count;
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
                    NULL)) {
            return false;
        }

        if (!set_background_visible(
                config_dir,
                output_index,
                true)) {
            return false;
        }

        const bool fullscreen =
            output_has_fullscreen_window(
                windows,
                visible_workspace_id
            );

        if (!set_bar_visible(
                config_dir,
                output_index,
                !fullscreen)) {
            return false;
        }

        if (fullscreen) {
            continue;
        }

        if (!sync_output_workspaces(
                config_dir,
                output_index,
                output_name,
                visible_workspace_id,
                workspaces,
                desired_workspaces) ||
                !sync_output_groups(
                    config_dir,
                    output_index,
                    visible_workspace_id,
                    groups,
                    windows,
                    desired_groups)) {
            return false;
        }
    }

    for (guint output_index = output_count;
            output_index < DAILY_UI_MAX_OUTPUTS;
            output_index++) {
        if (!set_background_visible(
                config_dir,
                output_index,
                false) ||
                !set_bar_visible(
                    config_dir,
                    output_index,
                    false)) {
            return false;
        }
    }

    remove_missing(
        runtime.workspace_parent,
        desired_workspaces,
        config_dir
    );
    remove_missing(
        runtime.group_parent,
        desired_groups,
        config_dir
    );

    return true;
}

bool daily_ui_v11_sync_apply(
        const char *config_dir,
        const char *json) {
    const int lock_fd =
        daily_ui_lock_acquire(
            config_dir
        );

    if (lock_fd < 0) {
        return false;
    }

    const bool success =
        daily_ui_v11_sync_apply_unlocked(
            config_dir,
            json
        );

    daily_ui_lock_release(
        lock_fd
    );

    return success;
}
