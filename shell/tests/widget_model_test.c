#include "widget_model.h"

#include <stdio.h>
#include <string.h>

#include <json-glib/json-glib.h>

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    static const char snapshot[] =
        "{"
        "\"generation\":42,"
        "\"outputs\":[{\"name\":\"eDP-1\",\"visible_workspace_id\":\"2\"}],"
        "\"workspaces\":["
          "{\"id\":\"1\",\"output_name\":\"eDP-1\",\"visible\":false,\"name\":\"1\",\"icon\":\"\"},"
          "{\"id\":\"2\",\"output_name\":\"eDP-1\",\"visible\":true,\"name\":\"2\",\"icon\":\"◆\"},"
          "{\"id\":\"3\",\"output_name\":\"eDP-1\",\"visible\":false,\"name\":\"3\",\"icon\":\"\"}"
        "],"
        "\"groups\":["
          "{\"id\":\"14\",\"workspace_id\":\"2\",\"active\":true,\"placement\":\"tiled\",\"pinned\":false,\"pinned_output_name\":\"\",\"placement_seen\":true},"
          "{\"id\":\"15\",\"workspace_id\":\"2\",\"active\":false,\"placement\":\"floating\",\"pinned\":true,\"pinned_output_name\":\"eDP-1\",\"placement_seen\":true}"
        "],"
        "\"windows\":["
          "{\"id\":\"16\",\"group_id\":\"14\",\"workspace_id\":\"2\",\"placement\":\"tiled\",\"active\":true,\"fullscreen\":false,\"app_id\":\"kitty\",\"title\":\"term\"},"
          "{\"id\":\"17\",\"group_id\":\"15\",\"workspace_id\":\"2\",\"placement\":\"floating\",\"active\":false,\"fullscreen\":false,\"app_id\":\"org.mozilla.zen\",\"title\":\"a\"},"
          "{\"id\":\"18\",\"group_id\":\"15\",\"workspace_id\":\"2\",\"placement\":\"floating\",\"active\":false,\"fullscreen\":false,\"app_id\":\"org.mozilla.zen\",\"title\":\"b\"}"
        "]"
        "}";

    GError *error = NULL;
    g_autofree char *projected = widget_block_project_json("bar", snapshot, &error);
    if (!projected) {
        fprintf(stderr, "FAIL: projection: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        return 1;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, projected, -1, &error)) {
        fprintf(stderr, "FAIL: projected JSON: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        return 1;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (strcmp(json_object_get_string_member(root, "block"), "bar") != 0)
        return fail("block id");

    JsonArray *outputs = json_object_get_array_member(root, "outputs");
    if (!outputs || json_array_get_length(outputs) != 1)
        return fail("output count");

    JsonObject *output = json_array_get_object_element(outputs, 0);
    if (!output || !json_object_get_boolean_member(output, "bar_visible"))
        return fail("bar visibility");

    JsonArray *workspaces = json_object_get_array_member(output, "workspaces");
    if (!workspaces || json_array_get_length(workspaces) != 3)
        return fail("workspace projection count");
    JsonObject *active_workspace = json_array_get_object_element(workspaces, 1);
    if (strcmp(json_object_get_string_member(active_workspace, "label"), "◆") != 0)
        return fail("workspace icon projection");
    if (!json_object_get_boolean_member(active_workspace, "active"))
        return fail("active workspace state");

    JsonArray *groups = json_object_get_array_member(output, "groups");
    if (!groups || json_array_get_length(groups) != 2)
        return fail("group projection count");

    JsonObject *single = json_array_get_object_element(groups, 0);
    JsonObject *multi = json_array_get_object_element(groups, 1);
    if (strcmp(json_object_get_string_member(single, "context_kind"), "window") != 0 ||
        strcmp(json_object_get_string_member(single, "context_id"), "16") != 0)
        return fail("single-window context target");
    if (strcmp(json_object_get_string_member(multi, "context_kind"), "group") != 0 ||
        strcmp(json_object_get_string_member(multi, "context_id"), "15") != 0)
        return fail("multi-window context target");
    if (strcmp(json_object_get_string_member(multi, "placement"), "floating") != 0 ||
        !json_object_get_boolean_member(multi, "pinned") ||
        strcmp(json_object_get_string_member(multi, "pinned_output_name"), "eDP-1") != 0)
        return fail("floating pinned group projection");

    JsonArray *single_windows = json_object_get_array_member(single, "windows");
    JsonArray *multi_windows = json_object_get_array_member(multi, "windows");
    if (!single_windows || json_array_get_length(single_windows) != 1)
        return fail("single group tab count");
    if (!multi_windows || json_array_get_length(multi_windows) != 2)
        return fail("multi group tab count");

    JsonObject *first_multi = json_array_get_object_element(multi_windows, 0);
    if (strcmp(json_object_get_string_member(first_multi, "id"), "17") != 0 ||
        strcmp(json_object_get_string_member(first_multi, "group_id"), "15") != 0 ||
        strcmp(json_object_get_string_member(first_multi, "label"), "Zen") != 0)
        return fail("window tab projection");

    puts("PASS: widget_model bar projection");
    return 0;
}
