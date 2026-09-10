#include "wifi_extended.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define NM_SERVICE "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_IFACE "org.freedesktop.NetworkManager"
#define NM_SETTINGS_PATH "/org/freedesktop/NetworkManager/Settings"
#define NM_SETTINGS_IFACE "org.freedesktop.NetworkManager.Settings"
#define NM_SETTINGS_CONN_IFACE \
    "org.freedesktop.NetworkManager.Settings.Connection"
#define NM_DEVICE_IFACE "org.freedesktop.NetworkManager.Device"
#define NM_WIFI_IFACE "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_ACTIVE_IFACE "org.freedesktop.NetworkManager.Connection.Active"
#define DBUS_PROPS_IFACE "org.freedesktop.DBus.Properties"

#define NM_DEVICE_TYPE_WIFI 2u
#define NM_DEVICE_STATE_ACTIVATED 100u
#define NM_ACTIVE_CONNECTION_STATE_ACTIVATED 2u

#define NM_802_11_AP_FLAGS_PRIVACY 0x1u
#define NM_802_11_AP_SEC_KEY_MGMT_PSK 0x100u
#define NM_802_11_AP_SEC_KEY_MGMT_802_1X 0x200u
#define NM_802_11_AP_SEC_KEY_MGMT_SAE 0x400u
#define NM_802_11_AP_SEC_KEY_MGMT_OWE 0x800u
#define NM_802_11_AP_SEC_KEY_MGMT_OWE_TM 0x1000u
#define NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192 0x2000u

typedef struct wifi_ap {
    char *path;
    char *ssid;
    char *ssid_b64;
    char *bssid;
    guint8 strength;
    guint32 frequency;
    guint32 flags;
    guint32 wpa_flags;
    guint32 rsn_flags;
    bool saved;
    bool active;
} WifiAp;

typedef struct wifi_snapshot {
    bool available;
    bool enabled;
    bool hardware_enabled;
    char *device;
    char *device_path;
    char *active_ap_path;
    char *active_bssid;
    char *active_ssid;
    gint64 last_scan;
    GPtrArray *aps;
} WifiSnapshot;

typedef struct wifi_watch {
    GDBusConnection *bus;
    GMainLoop *loop;
    guint debounce_id;
    guint signal_id;
} WifiWatch;

static void wifi_ap_free(gpointer data) {
    WifiAp *ap = data;
    if (!ap) {
        return;
    }
    g_free(ap->path);
    g_free(ap->ssid);
    g_free(ap->ssid_b64);
    g_free(ap->bssid);
    g_free(ap);
}

static void wifi_snapshot_init(WifiSnapshot *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->last_scan = -1;
    snapshot->aps = g_ptr_array_new_with_free_func(wifi_ap_free);
}

static void wifi_snapshot_clear(WifiSnapshot *snapshot) {
    g_free(snapshot->device);
    g_free(snapshot->device_path);
    g_free(snapshot->active_ap_path);
    g_free(snapshot->active_bssid);
    g_free(snapshot->active_ssid);
    g_clear_pointer(&snapshot->aps, g_ptr_array_unref);
    memset(snapshot, 0, sizeof(*snapshot));
}

static GVariant *nm_call(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *method,
        GVariant *parameters,
        const GVariantType *reply_type,
        GError **error) {
    return g_dbus_connection_call_sync(
        bus,
        NM_SERVICE,
        path,
        iface,
        method,
        parameters,
        reply_type,
        G_DBUS_CALL_FLAGS_NONE,
        8000,
        NULL,
        error
    );
}

static GVariant *nm_get_property(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *property,
        GError **error) {
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        bus,
        NM_SERVICE,
        path,
        DBUS_PROPS_IFACE,
        "Get",
        g_variant_new("(ss)", iface, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        error
    );
    if (!reply) {
        return NULL;
    }

    g_autoptr(GVariant) boxed = NULL;
    g_variant_get(reply, "(@v)", &boxed);
    return g_variant_get_variant(boxed);
}

static bool variant_bool(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *property,
        bool *out,
        GError **error) {
    g_autoptr(GVariant) value =
        nm_get_property(bus, path, iface, property, error);
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        return false;
    }
    *out = g_variant_get_boolean(value);
    return true;
}

static bool variant_u32(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *property,
        guint32 *out,
        GError **error) {
    g_autoptr(GVariant) value =
        nm_get_property(bus, path, iface, property, error);
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        return false;
    }
    *out = g_variant_get_uint32(value);
    return true;
}

static bool variant_i64(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *property,
        gint64 *out,
        GError **error) {
    g_autoptr(GVariant) value =
        nm_get_property(bus, path, iface, property, error);
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) {
        return false;
    }
    *out = g_variant_get_int64(value);
    return true;
}

static char *variant_string_dup(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *property,
        GError **error) {
    g_autoptr(GVariant) value =
        nm_get_property(bus, path, iface, property, error);
    if (!value ||
            !(g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
              g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH))) {
        return NULL;
    }
    return g_strdup(g_variant_get_string(value, NULL));
}

static char *ssid_text_from_variant(GVariant *ssid) {
    gsize size = 0;
    const guint8 *bytes = g_variant_get_fixed_array(
        ssid,
        &size,
        sizeof(guint8)
    );
    if (!bytes || size == 0) {
        return g_strdup("<hidden>");
    }
    if (g_utf8_validate((const char *)bytes, (gssize)size, NULL)) {
        return g_strndup((const char *)bytes, size);
    }
    g_autofree char *b64 = g_base64_encode(bytes, size);
    return g_strdup_printf("base64:%s", b64);
}

static char *ssid_b64_from_variant(GVariant *ssid) {
    gsize size = 0;
    const guint8 *bytes = g_variant_get_fixed_array(
        ssid,
        &size,
        sizeof(guint8)
    );
    return g_base64_encode(bytes, size);
}

static bool find_wifi_device(
        GDBusConnection *bus,
        char **path_out,
        char **iface_out,
        GError **error) {
    g_autoptr(GVariant) reply = nm_call(
        bus,
        NM_PATH,
        NM_IFACE,
        "GetDevices",
        NULL,
        G_VARIANT_TYPE("(ao)"),
        error
    );
    if (!reply) {
        return false;
    }

    g_autoptr(GVariant) paths = NULL;
    g_variant_get(reply, "(@ao)", &paths);

    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, paths);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        guint32 type = 0;
        g_autoptr(GError) local_error = NULL;
        if (!variant_u32(
                    bus,
                    path,
                    NM_DEVICE_IFACE,
                    "DeviceType",
                    &type,
                    &local_error)) {
            continue;
        }
        if (type != NM_DEVICE_TYPE_WIFI) {
            continue;
        }

        g_autofree char *iface = variant_string_dup(
            bus,
            path,
            NM_DEVICE_IFACE,
            "Interface",
            error
        );
        if (!iface) {
            return false;
        }
        *path_out = g_strdup(path);
        *iface_out = g_steal_pointer(&iface);
        return true;
    }

    g_set_error(
        error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "no NetworkManager Wi-Fi device"
    );
    return false;
}

static GHashTable *saved_wifi_profiles(
        GDBusConnection *bus,
        GError **error) {
    g_autoptr(GVariant) reply = nm_call(
        bus,
        NM_SETTINGS_PATH,
        NM_SETTINGS_IFACE,
        "ListConnections",
        NULL,
        G_VARIANT_TYPE("(ao)"),
        error
    );
    if (!reply) {
        return NULL;
    }

    GHashTable *profiles = g_hash_table_new_full(
        g_str_hash,
        g_str_equal,
        g_free,
        g_free
    );

    g_autoptr(GVariant) paths = NULL;
    g_variant_get(reply, "(@ao)", &paths);

    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, paths);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        g_autoptr(GError) local_error = NULL;
        g_autoptr(GVariant) settings_reply = nm_call(
            bus,
            path,
            NM_SETTINGS_CONN_IFACE,
            "GetSettings",
            NULL,
            G_VARIANT_TYPE("(a{sa{sv}})"),
            &local_error
        );
        if (!settings_reply) {
            continue;
        }

        g_autoptr(GVariant) settings = NULL;
        g_variant_get(settings_reply, "(@a{sa{sv}})", &settings);

        g_autoptr(GVariant) connection = g_variant_lookup_value(
            settings,
            "connection",
            G_VARIANT_TYPE("a{sv}")
        );
        g_autoptr(GVariant) wifi = g_variant_lookup_value(
            settings,
            "802-11-wireless",
            G_VARIANT_TYPE("a{sv}")
        );
        if (!connection || !wifi) {
            continue;
        }

        g_autoptr(GVariant) type = g_variant_lookup_value(
            connection,
            "type",
            G_VARIANT_TYPE_STRING
        );
        g_autoptr(GVariant) ssid = g_variant_lookup_value(
            wifi,
            "ssid",
            G_VARIANT_TYPE("ay")
        );
        if (!type || !ssid ||
                g_strcmp0(
                    g_variant_get_string(type, NULL),
                    "802-11-wireless") != 0) {
            continue;
        }

        g_autofree char *key = ssid_b64_from_variant(ssid);
        if (!g_hash_table_contains(profiles, key)) {
            g_hash_table_insert(
                profiles,
                g_strdup(key),
                g_strdup(path)
            );
        }
    }

    return profiles;
}

static const char *security_name(const WifiAp *ap) {
    const guint32 sec = ap->wpa_flags | ap->rsn_flags;
    if ((sec & NM_802_11_AP_SEC_KEY_MGMT_OWE) != 0) {
        return "owe";
    }
    if ((sec & NM_802_11_AP_SEC_KEY_MGMT_SAE) != 0 &&
            (sec & NM_802_11_AP_SEC_KEY_MGMT_PSK) != 0) {
        return "wpa2-wpa3-personal";
    }
    if ((sec & NM_802_11_AP_SEC_KEY_MGMT_SAE) != 0) {
        return "wpa3-personal";
    }
    if ((sec & NM_802_11_AP_SEC_KEY_MGMT_PSK) != 0) {
        return "wpa-personal";
    }
    if ((sec & (NM_802_11_AP_SEC_KEY_MGMT_802_1X |
                NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)) != 0) {
        return "enterprise";
    }
    if ((ap->flags & NM_802_11_AP_FLAGS_PRIVACY) != 0) {
        return "wep";
    }
    return "open";
}

static bool security_needs_password(const WifiAp *ap) {
    const guint32 sec = ap->wpa_flags | ap->rsn_flags;
    return (sec & (NM_802_11_AP_SEC_KEY_MGMT_PSK |
                   NM_802_11_AP_SEC_KEY_MGMT_SAE)) != 0;
}

static bool security_supported_new(const WifiAp *ap) {
    const char *name = security_name(ap);
    return g_strcmp0(name, "open") == 0 ||
        g_strcmp0(name, "owe") == 0 ||
        g_strcmp0(name, "wpa-personal") == 0 ||
        g_strcmp0(name, "wpa2-wpa3-personal") == 0 ||
        g_strcmp0(name, "wpa3-personal") == 0;
}

static bool query_snapshot(
        GDBusConnection *bus,
        WifiSnapshot *snapshot,
        GError **error) {
    wifi_snapshot_init(snapshot);

    if (!variant_bool(
                bus,
                NM_PATH,
                NM_IFACE,
                "WirelessEnabled",
                &snapshot->enabled,
                error) ||
            !variant_bool(
                bus,
                NM_PATH,
                NM_IFACE,
                "WirelessHardwareEnabled",
                &snapshot->hardware_enabled,
                error)) {
        wifi_snapshot_clear(snapshot);
        return false;
    }

    g_autofree char *device_path = NULL;
    g_autofree char *device = NULL;
    g_autoptr(GError) device_error = NULL;
    if (!find_wifi_device(
                bus,
                &device_path,
                &device,
                &device_error)) {
        snapshot->available = false;
        return true;
    }

    snapshot->available = true;
    snapshot->device_path = g_steal_pointer(&device_path);
    snapshot->device = g_steal_pointer(&device);

    snapshot->active_ap_path = variant_string_dup(
        bus,
        snapshot->device_path,
        NM_WIFI_IFACE,
        "ActiveAccessPoint",
        error
    );
    if (!snapshot->active_ap_path) {
        wifi_snapshot_clear(snapshot);
        return false;
    }

    if (!variant_i64(
                bus,
                snapshot->device_path,
                NM_WIFI_IFACE,
                "LastScan",
                &snapshot->last_scan,
                error)) {
        wifi_snapshot_clear(snapshot);
        return false;
    }

    g_autoptr(GHashTable) saved = saved_wifi_profiles(bus, error);
    if (!saved) {
        wifi_snapshot_clear(snapshot);
        return false;
    }

    g_autoptr(GVariant) aps_reply = nm_call(
        bus,
        snapshot->device_path,
        NM_WIFI_IFACE,
        "GetAllAccessPoints",
        NULL,
        G_VARIANT_TYPE("(ao)"),
        error
    );
    if (!aps_reply) {
        wifi_snapshot_clear(snapshot);
        return false;
    }

    g_autoptr(GVariant) paths = NULL;
    g_variant_get(aps_reply, "(@ao)", &paths);

    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, paths);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        g_autoptr(GError) local_error = NULL;
        g_autoptr(GVariant) ssid = nm_get_property(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "Ssid",
            &local_error
        );
        if (!ssid || !g_variant_is_of_type(ssid, G_VARIANT_TYPE("ay"))) {
            continue;
        }

        WifiAp *ap = g_new0(WifiAp, 1);
        ap->path = g_strdup(path);
        ap->ssid = ssid_text_from_variant(ssid);
        ap->ssid_b64 = ssid_b64_from_variant(ssid);
        ap->bssid = variant_string_dup(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "HwAddress",
            NULL
        );
        guint32 strength32 = 0;
        g_autoptr(GVariant) strength = nm_get_property(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "Strength",
            NULL
        );
        if (strength && g_variant_is_of_type(strength, G_VARIANT_TYPE_BYTE)) {
            ap->strength = g_variant_get_byte(strength);
        } else {
            ap->strength = (guint8)strength32;
        }
        (void)variant_u32(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "Frequency",
            &ap->frequency,
            NULL
        );
        (void)variant_u32(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "Flags",
            &ap->flags,
            NULL
        );
        (void)variant_u32(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "WpaFlags",
            &ap->wpa_flags,
            NULL
        );
        (void)variant_u32(
            bus,
            path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "RsnFlags",
            &ap->rsn_flags,
            NULL
        );

        ap->saved = g_hash_table_contains(saved, ap->ssid_b64);
        ap->active = snapshot->active_ap_path &&
            g_strcmp0(snapshot->active_ap_path, ap->path) == 0;
        if (ap->active) {
            snapshot->active_bssid = g_strdup(ap->bssid);
            snapshot->active_ssid = g_strdup(ap->ssid);
        }
        g_ptr_array_add(snapshot->aps, ap);
    }

    return true;
}

static char *snapshot_json(const WifiSnapshot *snapshot) {
    g_autoptr(JsonBuilder) builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(builder, snapshot->available);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, snapshot->enabled);
    json_builder_set_member_name(builder, "hardware_enabled");
    json_builder_add_boolean_value(builder, snapshot->hardware_enabled);

    json_builder_set_member_name(builder, "device");
    if (snapshot->device) {
        json_builder_add_string_value(builder, snapshot->device);
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_set_member_name(builder, "device_path");
    if (snapshot->device_path) {
        json_builder_add_string_value(builder, snapshot->device_path);
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_set_member_name(builder, "active_bssid");
    if (snapshot->active_bssid) {
        json_builder_add_string_value(builder, snapshot->active_bssid);
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_set_member_name(builder, "active_ssid");
    if (snapshot->active_ssid) {
        json_builder_add_string_value(builder, snapshot->active_ssid);
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_set_member_name(builder, "last_scan");
    json_builder_add_int_value(builder, snapshot->last_scan);

    json_builder_set_member_name(builder, "networks");
    json_builder_begin_array(builder);
    for (guint i = 0; snapshot->aps && i < snapshot->aps->len; i++) {
        const WifiAp *ap = g_ptr_array_index(snapshot->aps, i);
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "ssid");
        json_builder_add_string_value(builder, ap->ssid);
        json_builder_set_member_name(builder, "ssid_b64");
        json_builder_add_string_value(builder, ap->ssid_b64);
        json_builder_set_member_name(builder, "bssid");
        json_builder_add_string_value(builder, ap->bssid ? ap->bssid : "");
        json_builder_set_member_name(builder, "ap_path");
        json_builder_add_string_value(builder, ap->path);
        json_builder_set_member_name(builder, "strength");
        json_builder_add_int_value(builder, ap->strength);
        json_builder_set_member_name(builder, "frequency");
        json_builder_add_int_value(builder, ap->frequency);
        json_builder_set_member_name(builder, "security");
        json_builder_add_string_value(builder, security_name(ap));
        json_builder_set_member_name(builder, "secured");
        json_builder_add_boolean_value(
            builder,
            g_strcmp0(security_name(ap), "open") != 0
        );
        json_builder_set_member_name(builder, "saved");
        json_builder_add_boolean_value(builder, ap->saved);
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, ap->active);

        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    json_builder_end_object(builder);

    g_autoptr(JsonNode) root = json_builder_get_root(builder);
    g_autoptr(JsonGenerator) generator = json_generator_new();
    json_generator_set_root(generator, root);
    return json_generator_to_data(generator, NULL);
}

static bool emit_state(GDBusConnection *bus, GError **error) {
    WifiSnapshot snapshot;
    if (!query_snapshot(bus, &snapshot, error)) {
        return false;
    }
    g_autofree char *json = snapshot_json(&snapshot);
    puts(json);
    fflush(stdout);
    wifi_snapshot_clear(&snapshot);
    return true;
}

static gboolean watch_emit_cb(gpointer data) {
    WifiWatch *watch = data;
    watch->debounce_id = 0;
    g_autoptr(GError) error = NULL;
    if (!emit_state(watch->bus, &error)) {
        fprintf(
            stderr,
            "WIFI_FAIL watch state: %s\n",
            error ? error->message : "unknown"
        );
        g_main_loop_quit(watch->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_REMOVE;
}

static void watch_signal_cb(
        GDBusConnection *connection,
        const gchar *sender_name,
        const gchar *object_path,
        const gchar *interface_name,
        const gchar *signal_name,
        GVariant *parameters,
        gpointer user_data) {
    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    (void)parameters;
    WifiWatch *watch = user_data;
    if (watch->debounce_id == 0) {
        watch->debounce_id = g_timeout_add(75, watch_emit_cb, watch);
    }
}

static gboolean watch_quit_cb(gpointer data) {
    WifiWatch *watch = data;
    g_main_loop_quit(watch->loop);
    return G_SOURCE_CONTINUE;
}

static int run_watch(GDBusConnection *bus) {
    WifiWatch watch = {
        .bus = bus,
        .loop = g_main_loop_new(NULL, false),
    };
    if (!watch.loop) {
        fprintf(stderr, "WIFI_FAIL cannot create watch loop\n");
        return 1;
    }

    g_autoptr(GError) error = NULL;
    if (!emit_state(bus, &error)) {
        fprintf(
            stderr,
            "WIFI_FAIL initial watch state: %s\n",
            error ? error->message : "unknown"
        );
        g_main_loop_unref(watch.loop);
        return 1;
    }

    watch.signal_id = g_dbus_connection_signal_subscribe(
        bus,
        NM_SERVICE,
        NULL,
        NULL,
        NULL,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        watch_signal_cb,
        &watch,
        NULL
    );
    if (watch.signal_id == 0) {
        fprintf(stderr, "WIFI_FAIL cannot subscribe to NetworkManager\n");
        g_main_loop_unref(watch.loop);
        return 1;
    }

    guint sigint_id = g_unix_signal_add(SIGINT, watch_quit_cb, &watch);
    guint sigterm_id = g_unix_signal_add(SIGTERM, watch_quit_cb, &watch);

    g_main_loop_run(watch.loop);

    if (watch.debounce_id != 0) {
        g_source_remove(watch.debounce_id);
    }
    if (sigint_id != 0) {
        g_source_remove(sigint_id);
    }
    if (sigterm_id != 0) {
        g_source_remove(sigterm_id);
    }
    g_dbus_connection_signal_unsubscribe(bus, watch.signal_id);
    g_main_loop_unref(watch.loop);
    return 0;
}

static int run_scan(GDBusConnection *bus) {
    g_autofree char *device_path = NULL;
    g_autofree char *device = NULL;
    g_autoptr(GError) error = NULL;
    if (!find_wifi_device(bus, &device_path, &device, &error)) {
        fprintf(
            stderr,
            "WIFI_FAIL scan device: %s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_autoptr(GVariant) reply = nm_call(
        bus,
        device_path,
        NM_WIFI_IFACE,
        "RequestScan",
        g_variant_new("(a{sv})", &options),
        G_VARIANT_TYPE("()"),
        &error
    );
    if (!reply) {
        fprintf(
            stderr,
            "WIFI_FAIL RequestScan: %s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    /*
     * RequestScan is asynchronous.  The long-lived `wifi networks watch`
     * already observes NetworkManager changes and emits the authoritative
     * updated list, so blocking here for LastScan convergence only makes the
     * UI look broken on drivers that update LastScan late or coalesce scans.
     */
    fprintf(stdout, "CONTROL_OK wifi scan requested device=%s\n", device);
    return 0;
}

static WifiAp *find_ap_by_bssid(
        WifiSnapshot *snapshot,
        const char *bssid) {
    for (guint i = 0; snapshot->aps && i < snapshot->aps->len; i++) {
        WifiAp *ap = g_ptr_array_index(snapshot->aps, i);
        if (ap->bssid && g_ascii_strcasecmp(ap->bssid, bssid) == 0) {
            return ap;
        }
    }
    return NULL;
}

static char *read_password_stdin(GError **error) {
    char buffer[4098];
    if (!fgets(buffer, sizeof(buffer), stdin)) {
        g_set_error(
            error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "failed to read Wi-Fi password from stdin"
        );
        return NULL;
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
    if (buffer[0] == '\0') {
        g_set_error(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "empty Wi-Fi password"
        );
        return NULL;
    }
    return g_strdup(buffer);
}

#ifndef ONYRION_WIFI_CHECKPOINT_CALL_TIMEOUT_MS
#define ONYRION_WIFI_CHECKPOINT_CALL_TIMEOUT_MS 20000
#endif

#define ONYRION_WIFI_CHECKPOINT_ROLLBACK_SECONDS 30u
#define ONYRION_WIFI_CHECKPOINT_RECONCILE_ATTEMPTS 50
#define ONYRION_WIFI_CHECKPOINT_RECONCILE_US 100000
#define NM_CHECKPOINT_IFACE "org.freedesktop.NetworkManager.Checkpoint"

static GVariant *checkpoint_paths(
        GDBusConnection *bus,
        GError **error) {
    g_autoptr(GVariant) paths = nm_get_property(
        bus,
        NM_PATH,
        NM_IFACE,
        "Checkpoints",
        error
    );
    if (!paths || !g_variant_is_of_type(paths, G_VARIANT_TYPE("ao"))) {
        return NULL;
    }
    return g_steal_pointer(&paths);
}

static bool checkpoint_contains_device(
        GDBusConnection *bus,
        const char *checkpoint_path,
        const char *device_path,
        GError **error) {
    g_autoptr(GVariant) devices = nm_get_property(
        bus,
        checkpoint_path,
        NM_CHECKPOINT_IFACE,
        "Devices",
        error
    );
    if (!devices || !g_variant_is_of_type(devices, G_VARIANT_TYPE("ao"))) {
        return false;
    }

    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, devices);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        if (g_strcmp0(path, device_path) == 0) {
            return true;
        }
    }
    return false;
}

static bool checkpoint_path_exists(
        GVariant *paths,
        const char *checkpoint_path) {
    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, paths);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        if (g_strcmp0(path, checkpoint_path) == 0) {
            return true;
        }
    }
    return false;
}

static bool checkpoint_device_busy(
        GDBusConnection *bus,
        GVariant *paths,
        const char *device_path,
        GError **error) {
    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, paths);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        g_autoptr(GError) local_error = NULL;
        if (checkpoint_contains_device(
                    bus,
                    path,
                    device_path,
                    &local_error)) {
            return true;
        }
        if (local_error) {
            g_propagate_error(error, g_steal_pointer(&local_error));
            return false;
        }
    }
    return false;
}

static char *checkpoint_find_new_matching(
        GDBusConnection *bus,
        GVariant *before,
        const char *device_path,
        GError **error) {
    g_autoptr(GVariant) after = checkpoint_paths(bus, error);
    if (!after) {
        return NULL;
    }

    char *match = NULL;
    GVariantIter iter;
    const char *path = NULL;
    g_variant_iter_init(&iter, after);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        if (checkpoint_path_exists(before, path)) {
            continue;
        }

        g_autoptr(GError) local_error = NULL;
        if (!checkpoint_contains_device(
                    bus,
                    path,
                    device_path,
                    &local_error)) {
            if (local_error) {
                g_clear_error(&local_error);
            }
            continue;
        }

        guint32 rollback_timeout = 0;
        if (!variant_u32(
                    bus,
                    path,
                    NM_CHECKPOINT_IFACE,
                    "RollbackTimeout",
                    &rollback_timeout,
                    &local_error) ||
                rollback_timeout !=
                    ONYRION_WIFI_CHECKPOINT_ROLLBACK_SECONDS) {
            g_clear_error(&local_error);
            continue;
        }

        if (match) {
            g_free(match);
            g_set_error(
                error,
                G_IO_ERROR,
                G_IO_ERROR_BUSY,
                "multiple new matching NetworkManager checkpoints appeared"
            );
            return NULL;
        }
        match = g_strdup(path);
    }
    return match;
}

static char *checkpoint_create(
        GDBusConnection *bus,
        const char *device_path,
        GError **error) {
    g_autoptr(GVariant) before = checkpoint_paths(bus, error);
    if (!before) {
        return NULL;
    }

    g_autoptr(GError) busy_error = NULL;
    if (checkpoint_device_busy(
                bus,
                before,
                device_path,
                &busy_error)) {
        g_set_error(
            error,
            G_IO_ERROR,
            G_IO_ERROR_BUSY,
            "NetworkManager checkpoint already active for Wi-Fi device"
        );
        return NULL;
    }
    if (busy_error) {
        g_propagate_error(error, g_steal_pointer(&busy_error));
        return NULL;
    }

    GVariantBuilder devices;
    g_variant_builder_init(&devices, G_VARIANT_TYPE("ao"));
    g_variant_builder_add(&devices, "o", device_path);

    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        bus,
        NM_SERVICE,
        NM_PATH,
        NM_IFACE,
        "CheckpointCreate",
        g_variant_new(
            "(aouu)",
            &devices,
            ONYRION_WIFI_CHECKPOINT_ROLLBACK_SECONDS,
            0u
        ),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        ONYRION_WIFI_CHECKPOINT_CALL_TIMEOUT_MS,
        NULL,
        error
    );
    if (reply) {
#ifndef ONYRION_WIFI_CHECKPOINT_TEST_FORCE_RECONCILE
        const char *path = NULL;
        g_variant_get(reply, "(&o)", &path);
        return g_strdup(path);
#else
        g_clear_pointer(&reply, g_variant_unref);
#endif
    } else if (!error || !*error ||
            !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
        return NULL;
    }

    if (error) {
        g_clear_error(error);
    }
    for (int i = 0;
            i < ONYRION_WIFI_CHECKPOINT_RECONCILE_ATTEMPTS;
            i++) {
        g_usleep(ONYRION_WIFI_CHECKPOINT_RECONCILE_US);
        g_autoptr(GError) reconcile_error = NULL;
        g_autofree char *late = checkpoint_find_new_matching(
            bus,
            before,
            device_path,
            &reconcile_error
        );
        if (late) {
            g_set_error(
                error,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                "checkpoint create timed out; a new matching checkpoint "
                "%s appeared; activation was not attempted and the "
                "checkpoint is left to its automatic rollback",
                late
            );
            return NULL;
        }
        if (reconcile_error &&
                !g_error_matches(
                    reconcile_error,
                    G_IO_ERROR,
                    G_IO_ERROR_BUSY)) {
            continue;
        }
        if (reconcile_error) {
            g_propagate_error(error, g_steal_pointer(&reconcile_error));
            return NULL;
        }
    }

    g_set_error(
        error,
        G_IO_ERROR,
        G_IO_ERROR_TIMED_OUT,
        "checkpoint create timed out; no matching late checkpoint was "
        "observed and activation was not attempted"
    );
    return NULL;
}
static bool checkpoint_destroy(
        GDBusConnection *bus,
        const char *checkpoint,
        GError **error) {
    g_autoptr(GVariant) reply = nm_call(
        bus,
        NM_PATH,
        NM_IFACE,
        "CheckpointDestroy",
        g_variant_new("(o)", checkpoint),
        G_VARIANT_TYPE("()"),
        error
    );
    return reply != NULL;
}

static bool checkpoint_rollback(
        GDBusConnection *bus,
        const char *checkpoint) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = nm_call(
        bus,
        NM_PATH,
        NM_IFACE,
        "CheckpointRollback",
        g_variant_new("(o)", checkpoint),
        G_VARIANT_TYPE("(a{su})"),
        &error
    );
    if (!reply) {
        fprintf(
            stderr,
            "WIFI_FAIL checkpoint rollback: %s\n",
            error ? error->message : "unknown"
        );
        return false;
    }
    return true;
}

static bool wait_target_active(
        GDBusConnection *bus,
        const char *device_path,
        const char *active_connection_path,
        const char *target_ssid_b64,
        char **actual_bssid_out,
        GError **error) {
    for (int i = 0; i < 200; i++) {
        g_usleep(100000);

        guint32 active_state = 0;
        g_clear_error(error);
        if (!variant_u32(
                    bus,
                    active_connection_path,
                    NM_ACTIVE_IFACE,
                    "State",
                    &active_state,
                    error)) {
            continue;
        }
        if (active_state != NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
            continue;
        }

        guint32 device_state = 0;
        g_clear_error(error);
        if (!variant_u32(
                    bus,
                    device_path,
                    NM_DEVICE_IFACE,
                    "State",
                    &device_state,
                    error) ||
                device_state != NM_DEVICE_STATE_ACTIVATED) {
            continue;
        }

        g_autofree char *device_active_connection = variant_string_dup(
            bus,
            device_path,
            NM_DEVICE_IFACE,
            "ActiveConnection",
            error
        );
        if (!device_active_connection ||
                g_strcmp0(
                    device_active_connection,
                    active_connection_path) != 0) {
            continue;
        }

        g_autofree char *ap_path = variant_string_dup(
            bus,
            device_path,
            NM_WIFI_IFACE,
            "ActiveAccessPoint",
            error
        );
        if (!ap_path || g_strcmp0(ap_path, "/") == 0) {
            continue;
        }

        g_autoptr(GVariant) ssid = nm_get_property(
            bus,
            ap_path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "Ssid",
            error
        );
        if (!ssid || !g_variant_is_of_type(ssid, G_VARIANT_TYPE("ay"))) {
            continue;
        }
        g_autofree char *active_ssid_b64 = ssid_b64_from_variant(ssid);
        if (g_strcmp0(active_ssid_b64, target_ssid_b64) != 0) {
            continue;
        }

        g_autofree char *bssid = variant_string_dup(
            bus,
            ap_path,
            "org.freedesktop.NetworkManager.AccessPoint",
            "HwAddress",
            error
        );
        if (!bssid) {
            continue;
        }

        if (actual_bssid_out) {
            *actual_bssid_out = g_steal_pointer(&bssid);
        }
        return true;
    }

    g_set_error(
        error,
        G_IO_ERROR,
        G_IO_ERROR_TIMED_OUT,
        "target Wi-Fi connection did not become active"
    );
    return false;
}

static bool persist_memory_profile(
        GDBusConnection *bus,
        const char *profile_path,
        GError **error) {
    GVariantBuilder settings;
    g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));

    g_autoptr(GVariant) reply = nm_call(
        bus,
        profile_path,
        NM_SETTINGS_CONN_IFACE,
        "Update2",
        g_variant_new(
            "(a{sa{sv}}ua{sv})",
            &settings,
            0x1u,
            &args
        ),
        G_VARIANT_TYPE("(a{sv})"),
        error
    );
    return reply != NULL;
}

static bool delete_profile_best_effort(
        GDBusConnection *bus,
        const char *profile_path) {
    if (!profile_path || g_strcmp0(profile_path, "/") == 0) {
        return true;
    }
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = nm_call(
        bus,
        profile_path,
        NM_SETTINGS_CONN_IFACE,
        "Delete",
        NULL,
        G_VARIANT_TYPE("()"),
        &error
    );
    return reply != NULL;
}

static GVariant *build_new_connection(
        const WifiAp *ap,
        const char *password) {
    gsize ssid_size = 0;
    g_autofree guchar *ssid_bytes = g_base64_decode(
        ap->ssid_b64,
        &ssid_size
    );
    g_autofree char *uuid = g_uuid_string_random();
    g_autofree char *id = g_strdup_printf("Onyrion %s", ap->ssid);

    GVariantBuilder settings;
    g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));

    GVariantBuilder connection;
    g_variant_builder_init(&connection, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &connection,
        "{sv}",
        "id",
        g_variant_new_string(id)
    );
    g_variant_builder_add(
        &connection,
        "{sv}",
        "uuid",
        g_variant_new_string(uuid)
    );
    g_variant_builder_add(
        &connection,
        "{sv}",
        "type",
        g_variant_new_string("802-11-wireless")
    );
    g_variant_builder_add(
        &connection,
        "{sv}",
        "autoconnect",
        g_variant_new_boolean(true)
    );
    g_variant_builder_add(
        &settings,
        "{s@a{sv}}",
        "connection",
        g_variant_builder_end(&connection)
    );

    GVariantBuilder wifi;
    g_variant_builder_init(&wifi, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &wifi,
        "{sv}",
        "ssid",
        g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE,
            ssid_bytes,
            ssid_size,
            sizeof(guint8)
        )
    );
    g_variant_builder_add(
        &wifi,
        "{sv}",
        "mode",
        g_variant_new_string("infrastructure")
    );
    g_variant_builder_add(
        &settings,
        "{s@a{sv}}",
        "802-11-wireless",
        g_variant_builder_end(&wifi)
    );

    const guint32 sec = ap->wpa_flags | ap->rsn_flags;
    if (g_strcmp0(security_name(ap), "open") != 0) {
        GVariantBuilder security;
        g_variant_builder_init(&security, G_VARIANT_TYPE("a{sv}"));

        const char *key_mgmt = NULL;
        if ((sec & NM_802_11_AP_SEC_KEY_MGMT_OWE) != 0) {
            key_mgmt = "owe";
        } else if ((sec & NM_802_11_AP_SEC_KEY_MGMT_SAE) != 0 &&
                (sec & NM_802_11_AP_SEC_KEY_MGMT_PSK) == 0) {
            key_mgmt = "sae";
        } else {
            key_mgmt = "wpa-psk";
        }
        g_variant_builder_add(
            &security,
            "{sv}",
            "key-mgmt",
            g_variant_new_string(key_mgmt)
        );
        if (password) {
            g_variant_builder_add(
                &security,
                "{sv}",
                "psk",
                g_variant_new_string(password)
            );
        }
        g_variant_builder_add(
            &settings,
            "{s@a{sv}}",
            "802-11-wireless-security",
            g_variant_builder_end(&security)
        );
    }

    GVariantBuilder ipv4;
    g_variant_builder_init(&ipv4, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &ipv4,
        "{sv}",
        "method",
        g_variant_new_string("auto")
    );
    g_variant_builder_add(
        &settings,
        "{s@a{sv}}",
        "ipv4",
        g_variant_builder_end(&ipv4)
    );

    GVariantBuilder ipv6;
    g_variant_builder_init(&ipv6, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &ipv6,
        "{sv}",
        "method",
        g_variant_new_string("auto")
    );
    g_variant_builder_add(
        &settings,
        "{s@a{sv}}",
        "ipv6",
        g_variant_builder_end(&ipv6)
    );

    return g_variant_builder_end(&settings);
}

static int run_connect(
        GDBusConnection *bus,
        const char *bssid,
        bool password_stdin) {
    WifiSnapshot snapshot;
    g_autoptr(GError) error = NULL;
    if (!query_snapshot(bus, &snapshot, &error)) {
        fprintf(
            stderr,
            "WIFI_FAIL connect state: %s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    WifiAp *ap = find_ap_by_bssid(&snapshot, bssid);
    if (!ap) {
        fprintf(stderr, "WIFI_FAIL unknown BSSID=%s\n", bssid);
        wifi_snapshot_clear(&snapshot);
        return 1;
    }

    if (ap->active) {
        fprintf(
            stdout,
            "CONTROL_OK wifi already-active bssid=%s ssid=%s\n",
            ap->bssid,
            ap->ssid
        );
        wifi_snapshot_clear(&snapshot);
        return 0;
    }

    g_autoptr(GHashTable) profiles = saved_wifi_profiles(bus, &error);
    if (!profiles) {
        fprintf(
            stderr,
            "WIFI_FAIL saved profiles: %s\n",
            error ? error->message : "unknown"
        );
        wifi_snapshot_clear(&snapshot);
        return 1;
    }
    const char *saved_path = g_hash_table_lookup(profiles, ap->ssid_b64);

    g_autofree char *password = NULL;
    if (!saved_path && security_needs_password(ap)) {
        if (!password_stdin) {
            fprintf(
                stderr,
                "WIFI_FAIL password-required bssid=%s security=%s; use --password-stdin\n",
                ap->bssid,
                security_name(ap)
            );
            wifi_snapshot_clear(&snapshot);
            return 2;
        }
        password = read_password_stdin(&error);
        if (!password) {
            fprintf(
                stderr,
                "WIFI_FAIL password input: %s\n",
                error ? error->message : "unknown"
            );
            wifi_snapshot_clear(&snapshot);
            return 1;
        }
    }

    if (!saved_path && !security_supported_new(ap)) {
        fprintf(
            stderr,
            "WIFI_FAIL unsupported-new-network security=%s\n",
            security_name(ap)
        );
        wifi_snapshot_clear(&snapshot);
        return 2;
    }

    g_autofree char *checkpoint = checkpoint_create(
        bus,
        snapshot.device_path,
        &error
    );
    if (!checkpoint) {
        fprintf(
            stderr,
            "WIFI_FAIL checkpoint-create: %s\n",
            error ? error->message : "unknown"
        );
        wifi_snapshot_clear(&snapshot);
        return 1;
    }

    g_autofree char *new_profile = NULL;
    g_autofree char *active_connection = NULL;
    bool activation_started = false;

    if (saved_path) {
        g_clear_error(&error);
        g_autoptr(GVariant) reply = nm_call(
            bus,
            NM_PATH,
            NM_IFACE,
            "ActivateConnection",
            g_variant_new(
                "(ooo)",
                saved_path,
                snapshot.device_path,
                ap->path
            ),
            G_VARIANT_TYPE("(o)"),
            &error
        );
        if (reply) {
            const char *active_path = NULL;
            g_variant_get(reply, "(&o)", &active_path);
            active_connection = g_strdup(active_path);
            activation_started =
                active_connection &&
                g_strcmp0(active_connection, "/") != 0;
        }
    } else {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(
            &options,
            "{sv}",
            "persist",
            g_variant_new_string("memory")
        );

        g_autoptr(GVariant) settings = build_new_connection(ap, password);
        g_clear_error(&error);
        g_autoptr(GVariant) reply = nm_call(
            bus,
            NM_PATH,
            NM_IFACE,
            "AddAndActivateConnection2",
            g_variant_new(
                "(@a{sa{sv}}ooa{sv})",
                g_steal_pointer(&settings),
                snapshot.device_path,
                ap->path,
                &options
            ),
            G_VARIANT_TYPE("(ooa{sv})"),
            &error
        );
        if (reply) {
            const char *profile_path = NULL;
            const char *active_path = NULL;
            g_autoptr(GVariant) result = NULL;
            g_variant_get(
                reply,
                "(&o&o@a{sv})",
                &profile_path,
                &active_path,
                &result
            );
            new_profile = g_strdup(profile_path);
            active_connection = g_strdup(active_path);
            activation_started =
                active_connection &&
                g_strcmp0(active_connection, "/") != 0;
        }
    }

    if (!activation_started) {
        fprintf(
            stderr,
            "WIFI_FAIL activation-start: %s\n",
            error ? error->message : "unknown"
        );
        (void)checkpoint_rollback(bus, checkpoint);
        (void)delete_profile_best_effort(bus, new_profile);
        wifi_snapshot_clear(&snapshot);
        return 1;
    }

    g_autofree char *actual_bssid = NULL;
    g_clear_error(&error);
    if (!wait_target_active(
                bus,
                snapshot.device_path,
                active_connection,
                ap->ssid_b64,
                &actual_bssid,
                &error)) {
        fprintf(
            stderr,
            "WIFI_FAIL activation: %s\n",
            error ? error->message : "unknown"
        );
        (void)checkpoint_rollback(bus, checkpoint);
        (void)delete_profile_best_effort(bus, new_profile);
        wifi_snapshot_clear(&snapshot);
        return 1;
    }

    if (new_profile) {
        g_clear_error(&error);
        if (!persist_memory_profile(bus, new_profile, &error)) {
            fprintf(
                stderr,
                "WIFI_FAIL persist profile: %s\n",
                error ? error->message : "unknown"
            );
            (void)checkpoint_rollback(bus, checkpoint);
            (void)delete_profile_best_effort(bus, new_profile);
            wifi_snapshot_clear(&snapshot);
            return 1;
        }
    }

    g_clear_error(&error);
    if (!checkpoint_destroy(bus, checkpoint, &error)) {
        fprintf(
            stderr,
            "WIFI_FAIL checkpoint-destroy after successful activation: %s\n",
            error ? error->message : "unknown"
        );
        wifi_snapshot_clear(&snapshot);
        return 1;
    }

    fprintf(
        stdout,
        "CONTROL_OK wifi connected bssid=%s requested_bssid=%s ssid=%s saved=%s\n",
        actual_bssid ? actual_bssid : "",
        ap->bssid,
        ap->ssid,
        saved_path ? "existing" : "new"
    );
    wifi_snapshot_clear(&snapshot);
    return 0;
}


static int run_disconnect(GDBusConnection *bus) {
    WifiSnapshot snapshot;
    g_autoptr(GError) error = NULL;

    if (!query_snapshot(bus, &snapshot, &error)) {
        fprintf(stderr, "WIFI_FAIL disconnect state: %s\n", error ? error->message : "unknown");
        return 1;
    }

    g_autofree char *active_connection = variant_string_dup(
        bus,
        snapshot.device_path,
        NM_DEVICE_IFACE,
        "ActiveConnection",
        &error
    );

    if (!active_connection || g_strcmp0(active_connection, "/") == 0) {
        fprintf(stdout, "CONTROL_OK wifi disconnected state=noop\n");
        wifi_snapshot_clear(&snapshot);
        return 0;
    }

    g_clear_error(&error);
    g_autoptr(GVariant) reply = nm_call(
        bus,
        NM_PATH,
        NM_IFACE,
        "DeactivateConnection",
        g_variant_new("(o)", active_connection),
        NULL,
        &error
    );

    if (!reply) {
        fprintf(stderr, "WIFI_FAIL disconnect: %s\n", error ? error->message : "unknown");
        wifi_snapshot_clear(&snapshot);
        return 1;
    }

    fprintf(stdout, "CONTROL_OK wifi disconnected\n");
    wifi_snapshot_clear(&snapshot);
    return 0;
}

bool onyrion_wifi_extended_handles(int argc, char **argv) {
    if (argc < 3 || g_strcmp0(argv[1], "wifi") != 0) {
        return false;
    }
    if (g_strcmp0(argv[2], "networks") == 0) {
        return true;
    }
    if (g_strcmp0(argv[2], "connect") == 0 ||
            g_strcmp0(argv[2], "disconnect") == 0) {
        return true;
    }
    return false;
}

static void usage(const char *argv0) {
    fprintf(
        stderr,
        "wifi extended usage:\n"
        "  %s wifi networks state\n"
        "  %s wifi networks watch\n"
        "  %s wifi networks scan\n"
        "  %s wifi connect BSSID\n"
        "  %s wifi connect BSSID --password-stdin\n"
        "  %s wifi disconnect\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0
    );
}

int onyrion_wifi_extended_cli(int argc, char **argv) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(
        G_BUS_TYPE_SYSTEM,
        NULL,
        &error
    );
    if (!bus) {
        fprintf(
            stderr,
            "WIFI_FAIL system bus: %s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    if (argc == 4 &&
            g_strcmp0(argv[1], "wifi") == 0 &&
            g_strcmp0(argv[2], "networks") == 0 &&
            g_strcmp0(argv[3], "state") == 0) {
        if (!emit_state(bus, &error)) {
            fprintf(
                stderr,
                "WIFI_FAIL state: %s\n",
                error ? error->message : "unknown"
            );
            return 1;
        }
        return 0;
    }

    if (argc == 4 &&
            g_strcmp0(argv[1], "wifi") == 0 &&
            g_strcmp0(argv[2], "networks") == 0 &&
            g_strcmp0(argv[3], "watch") == 0) {
        return run_watch(bus);
    }

    if (argc == 4 &&
            g_strcmp0(argv[1], "wifi") == 0 &&
            g_strcmp0(argv[2], "networks") == 0 &&
            g_strcmp0(argv[3], "scan") == 0) {
        return run_scan(bus);
    }

    if (argc == 3 &&
            g_strcmp0(argv[1], "wifi") == 0 &&
            g_strcmp0(argv[2], "disconnect") == 0) {
        return run_disconnect(bus);
    }

    if ((argc == 4 || argc == 5) &&
            g_strcmp0(argv[1], "wifi") == 0 &&
            g_strcmp0(argv[2], "connect") == 0) {
        const bool password_stdin = argc == 5 &&
            g_strcmp0(argv[4], "--password-stdin") == 0;
        if (argc == 5 && !password_stdin) {
            usage(argv[0]);
            return 2;
        }
        return run_connect(bus, argv[3], password_stdin);
    }

    usage(argv[0]);
    return 2;
}
