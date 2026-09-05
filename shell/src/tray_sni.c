#include "tray_sni.h"

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define WATCHER_SERVICE "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"

typedef struct item_ref {
    char *service;
    char *path;
    char *id;
} ItemRef;

static void item_ref_free(gpointer p) {
    ItemRef *item = p;
    if (!item) return;
    g_free(item->service);
    g_free(item->path);
    g_free(item->id);
    g_free(item);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ItemRef, item_ref_free)

static char *stable_id(const char *service, const char *path) {
    g_autofree char *key = g_strdup_printf("%s\n%s", service, path);
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
}

static ItemRef *parse_registered(const char *raw) {
    if (!raw || raw[0] == '\0') return NULL;
    ItemRef *item = g_new0(ItemRef, 1);
    const char *slash = strchr(raw, '/');
    if (slash) {
        item->service = g_strndup(raw, (gsize)(slash - raw));
        item->path = g_strdup(slash);
    } else {
        item->service = g_strdup(raw);
        item->path = g_strdup("/StatusNotifierItem");
    }
    if (!item->service || item->service[0] == '\0' ||
            !g_variant_is_object_path(item->path)) {
        item_ref_free(item);
        return NULL;
    }
    item->id = stable_id(item->service, item->path);
    return item;
}

static GVariant *call_sync(
        GDBusConnection *bus,
        const char *service,
        const char *path,
        const char *iface,
        const char *method,
        GVariant *params,
        const GVariantType *reply_type,
        GError **error) {
    return g_dbus_connection_call_sync(
        bus, service, path, iface, method, params, reply_type,
        G_DBUS_CALL_FLAGS_NONE, 8000, NULL, error
    );
}

static GPtrArray *registered_items(GDBusConnection *bus, GError **error) {
    g_autoptr(GVariant) reply = call_sync(
        bus, WATCHER_SERVICE, WATCHER_PATH, PROPS_IFACE, "Get",
        g_variant_new("(ss)", WATCHER_IFACE, "RegisteredStatusNotifierItems"),
        G_VARIANT_TYPE("(v)"), error
    );
    if (!reply) return NULL;

    g_autoptr(GVariant) boxed = NULL;
    g_variant_get(reply, "(@v)", &boxed);
    g_autoptr(GVariant) inner = g_variant_get_variant(boxed);
    if (!g_variant_is_of_type(inner, G_VARIANT_TYPE("as"))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "RegisteredStatusNotifierItems is not as");
        return NULL;
    }

    GPtrArray *items = g_ptr_array_new_with_free_func(item_ref_free);
    GVariantIter iter;
    const char *raw = NULL;
    g_variant_iter_init(&iter, inner);
    while (g_variant_iter_next(&iter, "&s", &raw)) {
        ItemRef *item = parse_registered(raw);
        if (item) g_ptr_array_add(items, item);
    }
    return items;
}

static const char *dict_string(GVariant *dict, const char *key) {
    const char *value = NULL;
    return g_variant_lookup(dict, key, "&s", &value) ? value : "";
}

static bool append_item_json(
        GDBusConnection *bus,
        JsonBuilder *builder,
        const ItemRef *item) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = call_sync(
        bus, item->service, item->path, PROPS_IFACE, "GetAll",
        g_variant_new("(s)", ITEM_IFACE),
        G_VARIANT_TYPE("(a{sv})"), &error
    );
    if (!reply) return false;

    g_autoptr(GVariant) props = NULL;
    g_variant_get(reply, "(@a{sv})", &props);

    const char *title = dict_string(props, "Title");
    const char *status = dict_string(props, "Status");
    const char *icon = dict_string(props, "IconName");
    const char *attention = dict_string(props, "AttentionIconName");
    const char *sni_id = dict_string(props, "Id");

    const char *chosen_icon =
        (g_strcmp0(status, "NeedsAttention") == 0 && attention[0] != '\0')
            ? attention
            : icon;
    if (chosen_icon[0] == '\0') chosen_icon = "application-x-executable-symbolic";

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, item->id);
    json_builder_set_member_name(builder, "title");
    json_builder_add_string_value(builder, title[0] ? title : sni_id);
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, status);
    json_builder_set_member_name(builder, "icon");
    json_builder_add_string_value(builder, chosen_icon);
    json_builder_end_object(builder);
    return true;
}

static char *state_json(GDBusConnection *bus, GError **error) {
    g_autoptr(GPtrArray) items = registered_items(bus, error);
    if (!items) return NULL;

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "items");
    json_builder_begin_array(builder);
    for (guint i = 0; i < items->len; i++) {
        append_item_json(bus, builder, g_ptr_array_index(items, i));
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    char *json = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(builder);
    return json;
}

static bool emit_state(GDBusConnection *bus) {
    g_autoptr(GError) error = NULL;
    g_autofree char *json = state_json(bus, &error);
    if (!json) {
        g_printerr("TRAY_FAIL state error=%s\n", error ? error->message : "unknown");
        return false;
    }
    g_print("%s\n", json);
    fflush(stdout);
    return true;
}

static ItemRef *find_by_id(GDBusConnection *bus, const char *id, GError **error) {
    g_autoptr(GPtrArray) items = registered_items(bus, error);
    if (!items) return NULL;
    for (guint i = 0; i < items->len; i++) {
        ItemRef *item = g_ptr_array_index(items, i);
        if (g_strcmp0(item->id, id) == 0) {
            ItemRef *copy = g_new0(ItemRef, 1);
            copy->service = g_strdup(item->service);
            copy->path = g_strdup(item->path);
            copy->id = g_strdup(item->id);
            return copy;
        }
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "tray item id not found");
    return NULL;
}

static int action_cli(GDBusConnection *bus, const char *action, const char *id) {
    g_autoptr(GError) error = NULL;
    g_autoptr(ItemRef) item = find_by_id(bus, id, &error);
    if (!item) {
        g_printerr("TRAY_FAIL action=%s id=%s error=%s\n",
            action, id, error ? error->message : "unknown");
        return 1;
    }

    const char *method =
        g_strcmp0(action, "activate") == 0 ? "Activate" :
        g_strcmp0(action, "context") == 0 ? "ContextMenu" : NULL;
    if (!method) return 64;

    g_clear_error(&error);
    g_autoptr(GVariant) reply = call_sync(
        bus, item->service, item->path, ITEM_IFACE, method,
        g_variant_new("(ii)", 0, 0), G_VARIANT_TYPE("()"), &error
    );
    if (!reply) {
        g_printerr("TRAY_FAIL action=%s id=%s error=%s\n",
            action, id, error ? error->message : "unknown");
        return 1;
    }
    g_print("TRAY_OK action=%s id=%s\n", action, id);
    return 0;
}

typedef struct watch_ctx {
    GDBusConnection *bus;
    GMainLoop *loop;
    guint debounce;
} WatchCtx;

static gboolean debounce_emit(gpointer data) {
    WatchCtx *ctx = data;
    ctx->debounce = 0;
    emit_state(ctx->bus);
    return G_SOURCE_REMOVE;
}

static void schedule_emit(WatchCtx *ctx) {
    if (ctx->debounce) g_source_remove(ctx->debounce);
    ctx->debounce = g_timeout_add(50, debounce_emit, ctx);
}

static void signal_cb(
        GDBusConnection *connection,
        const char *sender_name,
        const char *object_path,
        const char *interface_name,
        const char *signal_name,
        GVariant *parameters,
        gpointer user_data) {
    (void)connection; (void)sender_name; (void)object_path;
    (void)interface_name; (void)signal_name; (void)parameters;
    schedule_emit(user_data);
}

static int watch_cli(GDBusConnection *bus) {
    g_autoptr(GError) error = NULL;
    const char *unique = g_dbus_connection_get_unique_name(bus);
    if (!unique) return 1;

    g_autoptr(GVariant) reg = call_sync(
        bus, WATCHER_SERVICE, WATCHER_PATH, WATCHER_IFACE,
        "RegisterStatusNotifierHost",
        g_variant_new("(s)", unique),
        G_VARIANT_TYPE("()"), &error
    );
    if (!reg) {
        g_printerr("TRAY_FAIL register-host error=%s\n",
            error ? error->message : "unknown");
        return 1;
    }

    WatchCtx ctx = { .bus = bus, .loop = g_main_loop_new(NULL, FALSE), .debounce = 0 };
    guint a = g_dbus_connection_signal_subscribe(
        bus, WATCHER_SERVICE, WATCHER_IFACE, "StatusNotifierItemRegistered",
        WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_cb, &ctx, NULL
    );
    guint b = g_dbus_connection_signal_subscribe(
        bus, WATCHER_SERVICE, WATCHER_IFACE, "StatusNotifierItemUnregistered",
        WATCHER_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_cb, &ctx, NULL
    );
    guint c = g_dbus_connection_signal_subscribe(
        bus, NULL, PROPS_IFACE, "PropertiesChanged",
        NULL, ITEM_IFACE, G_DBUS_SIGNAL_FLAGS_NONE, signal_cb, &ctx, NULL
    );

    if (!emit_state(bus)) {
        g_dbus_connection_signal_unsubscribe(bus, a);
        g_dbus_connection_signal_unsubscribe(bus, b);
        g_dbus_connection_signal_unsubscribe(bus, c);
        g_main_loop_unref(ctx.loop);
        return 1;
    }

    g_main_loop_run(ctx.loop);
    if (ctx.debounce) g_source_remove(ctx.debounce);
    g_dbus_connection_signal_unsubscribe(bus, a);
    g_dbus_connection_signal_unsubscribe(bus, b);
    g_dbus_connection_signal_unsubscribe(bus, c);
    g_main_loop_unref(ctx.loop);
    return 0;
}

int onyrion_tray_cli(int argc, char **argv) {
    if (argc < 1) return 64;
    g_autoptr(GError) error = NULL;
#ifdef ONYRION_TRAY_TEST_SESSION_BUS
    GBusType bus_type = G_BUS_TYPE_SESSION;
#else
    GBusType bus_type = G_BUS_TYPE_SESSION;
#endif
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(bus_type, NULL, &error);
    if (!bus) {
        g_printerr("TRAY_FAIL bus=%s\n", error ? error->message : "unknown");
        return 1;
    }

    if (argc == 1 && g_strcmp0(argv[0], "state") == 0) {
        return emit_state(bus) ? 0 : 1;
    }
    if (argc == 1 && g_strcmp0(argv[0], "watch") == 0) {
        return watch_cli(bus);
    }
    if (argc == 2 &&
            (g_strcmp0(argv[0], "activate") == 0 ||
             g_strcmp0(argv[0], "context") == 0)) {
        return action_cli(bus, argv[0], argv[1]);
    }

    g_printerr("usage: onyrion-shell tray state|watch|activate ID|context ID\n");
    return 64;
}
