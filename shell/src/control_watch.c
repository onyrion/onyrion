#include "control_watch.h"

#include "control_plane.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#define CONTROL_WATCH_DEBOUNCE_MS 75

#define NM_SERVICE "org.freedesktop.NetworkManager"
#define BLUEZ_SERVICE "org.bluez"
#define PPD_SERVICE "org.freedesktop.UPower.PowerProfiles"
#define UPOWER_SERVICE "org.freedesktop.UPower"

typedef struct control_watch {
    GDBusConnection *bus;
    GMainLoop *loop;
    guint debounce_source;
    char *last_json;
} ControlWatch;

static void json_add_nullable_string(
        JsonBuilder *builder,
        const char *name,
        const char *value) {
    json_builder_set_member_name(builder, name);

    if (value) {
        json_builder_add_string_value(builder, value);
    } else {
        json_builder_add_null_value(builder);
    }
}

static char *snapshot_to_json(
        const OnyrionControlSnapshot *snapshot) {
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = NULL;
    g_autoptr(JsonNode) root = NULL;

    if (!builder) {
        return NULL;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "wifi");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(builder, snapshot->wifi_available);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, snapshot->wifi_enabled);
    json_builder_set_member_name(builder, "hardware_enabled");
    json_builder_add_boolean_value(builder, snapshot->wifi_hardware_enabled);
    json_add_nullable_string(
        builder,
        "connection",
        snapshot->wifi_connection
    );
    json_builder_set_member_name(builder, "display");
    if (!snapshot->wifi_available) {
        json_builder_add_string_value(builder, "Wi-Fi · unavailable");
    } else if (!snapshot->wifi_enabled) {
        json_builder_add_string_value(builder, "Wi-Fi · off");
    } else if (snapshot->wifi_connection &&
            snapshot->wifi_connection[0] != '\0') {
        g_autofree char *label =
            g_strdup_printf(
                "Wi-Fi · %s",
                snapshot->wifi_connection
            );
        json_builder_add_string_value(builder, label);
    } else {
        json_builder_add_string_value(builder, "Wi-Fi · on");
    }
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "bluetooth");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(builder, snapshot->bluetooth_available);
    json_builder_set_member_name(builder, "powered");
    json_builder_add_boolean_value(builder, snapshot->bluetooth_powered);
    json_add_nullable_string(
        builder,
        "alias",
        snapshot->bluetooth_alias
    );
    json_builder_set_member_name(builder, "display");
    if (!snapshot->bluetooth_available) {
        json_builder_add_string_value(builder, "Bluetooth · unavailable");
    } else if (!snapshot->bluetooth_powered) {
        json_builder_add_string_value(builder, "Bluetooth · off");
    } else {
        json_builder_add_string_value(builder, "Bluetooth · on");
    }
    json_builder_set_member_name(builder, "devices");
    json_builder_begin_array(builder);

    if (snapshot->bluetooth_devices) {
        for (guint i = 0; i < snapshot->bluetooth_devices->len; i++) {
            const OnyrionControlBluetoothDevice *device =
                g_ptr_array_index(
                    snapshot->bluetooth_devices,
                    i
                );

            if (!device) {
                continue;
            }

            json_builder_begin_object(builder);
            json_add_nullable_string(builder, "path", device->path);
            json_add_nullable_string(builder, "address", device->address);
            json_add_nullable_string(builder, "name", device->name);
            json_builder_set_member_name(builder, "connected");
            json_builder_add_boolean_value(builder, device->connected);
            json_builder_set_member_name(builder, "paired");
            json_builder_add_boolean_value(builder, device->paired);
            json_builder_set_member_name(builder, "trusted");
            json_builder_add_boolean_value(builder, device->trusted);
            json_builder_end_object(builder);
        }
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "power_profile");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(
        builder,
        snapshot->power_profiles_available
    );
    json_add_nullable_string(
        builder,
        "active",
        snapshot->power_profile_active
    );
    json_builder_set_member_name(builder, "display");
    if (!snapshot->power_profiles_available) {
        json_builder_add_string_value(builder, "Power profile · unavailable");
    } else if (snapshot->power_profile_active) {
        g_autofree char *label =
            g_strdup_printf(
                "Power profile · %s",
                snapshot->power_profile_active
            );
        json_builder_add_string_value(builder, label);
    } else {
        json_builder_add_string_value(builder, "Power profile · unknown");
    }
    json_builder_set_member_name(builder, "profiles");
    json_builder_begin_array(builder);

    if (snapshot->power_profiles) {
        for (guint i = 0; i < snapshot->power_profiles->len; i++) {
            const char *profile =
                g_ptr_array_index(
                    snapshot->power_profiles,
                    i
                );

            if (profile) {
                json_builder_add_string_value(
                    builder,
                    profile
                );
            }
        }
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    json_builder_set_member_name(builder, "battery");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "available");
    json_builder_add_boolean_value(builder, snapshot->battery_available);
    json_builder_set_member_name(builder, "percentage");
    json_builder_add_double_value(builder, snapshot->battery_percentage);
    json_builder_set_member_name(builder, "state");
    json_builder_add_int_value(builder, snapshot->battery_state);
    json_builder_set_member_name(builder, "time_to_full");
    json_builder_add_int_value(builder, snapshot->battery_time_to_full);
    json_builder_set_member_name(builder, "time_to_empty");
    json_builder_add_int_value(builder, snapshot->battery_time_to_empty);
    json_add_nullable_string(
        builder,
        "icon",
        snapshot->battery_icon
    );
    json_builder_set_member_name(builder, "display");

    if (snapshot->battery_available) {
        g_autofree char *label =
            g_strdup_printf(
                "Battery · %.0f%%",
                snapshot->battery_percentage
            );
        json_builder_add_string_value(builder, label);
    } else {
        json_builder_add_string_value(builder, "Battery · unavailable");
    }

    json_builder_end_object(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    if (!root) {
        return NULL;
    }

    generator = json_generator_new();
    if (!generator) {
        return NULL;
    }

    json_generator_set_root(generator, root);

    return json_generator_to_data(generator, NULL);
}

static bool emit_fresh_state(
        ControlWatch *watch,
        GError **error) {
    OnyrionControlSnapshot snapshot;
    onyrion_control_snapshot_init(&snapshot);

    const bool queried =
        onyrion_control_query(
            &snapshot,
            error
        );

    if (!queried) {
        onyrion_control_snapshot_clear(&snapshot);
        return false;
    }

    g_autofree char *json =
        snapshot_to_json(&snapshot);

    onyrion_control_snapshot_clear(&snapshot);

    if (!json) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "failed to serialize control snapshot"
        );
        return false;
    }

    if (g_strcmp0(json, watch->last_json) == 0) {
        return true;
    }

    if (printf("%s\n", json) < 0 ||
            fflush(stdout) != 0) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_BROKEN_PIPE,
            "failed to write control watch state"
        );
        return false;
    }

    g_free(watch->last_json);
    watch->last_json = g_strdup(json);

    if (!watch->last_json) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "failed to retain control watch state"
        );
        return false;
    }

    return true;
}

static gboolean emit_debounced(
        gpointer data) {
    ControlWatch *watch = data;
    watch->debounce_source = 0;

    g_autoptr(GError) error = NULL;

    if (!emit_fresh_state(
            watch,
            &error)) {
        g_printerr(
            "CONTROL_WATCH_FAIL refresh=%s\n",
            error
                ? error->message
                : "unknown"
        );

        if (watch->loop) {
            g_main_loop_quit(watch->loop);
        }
    }

    return G_SOURCE_REMOVE;
}

static void schedule_refresh(
        ControlWatch *watch) {
    if (!watch ||
            watch->debounce_source != 0) {
        return;
    }

    watch->debounce_source =
        g_timeout_add(
            CONTROL_WATCH_DEBOUNCE_MS,
            emit_debounced,
            watch
        );
}

static void properties_changed(
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

    schedule_refresh(user_data);
}

static void object_manager_changed(
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

    schedule_refresh(user_data);
}

static guint subscribe_properties(
        GDBusConnection *bus,
        const char *sender,
        ControlWatch *watch) {
    return g_dbus_connection_signal_subscribe(
        bus,
        sender,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        NULL,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        properties_changed,
        watch,
        NULL
    );
}

static gboolean stop_watch(
        gpointer data) {
    ControlWatch *watch = data;

    if (watch && watch->loop) {
        g_main_loop_quit(watch->loop);
    }

    return G_SOURCE_CONTINUE;
}

bool onyrion_control_watch_run(
        GError **error) {
    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync(
            G_BUS_TYPE_SYSTEM,
            NULL,
            error
        );

    if (!bus) {
        return false;
    }

    g_autoptr(GMainLoop) loop =
        g_main_loop_new(
            NULL,
            false
        );

    if (!loop) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NO_SPACE,
            "failed to create control watch main loop"
        );
        return false;
    }

    ControlWatch watch = {
        .bus = bus,
        .loop = loop,
    };

    guint subscriptions[6] = {0};

    subscriptions[0] =
        subscribe_properties(
            bus,
            NM_SERVICE,
            &watch
        );

    subscriptions[1] =
        subscribe_properties(
            bus,
            BLUEZ_SERVICE,
            &watch
        );

    subscriptions[2] =
        g_dbus_connection_signal_subscribe(
            bus,
            BLUEZ_SERVICE,
            "org.freedesktop.DBus.ObjectManager",
            "InterfacesAdded",
            NULL,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            object_manager_changed,
            &watch,
            NULL
        );

    subscriptions[3] =
        g_dbus_connection_signal_subscribe(
            bus,
            BLUEZ_SERVICE,
            "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved",
            NULL,
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            object_manager_changed,
            &watch,
            NULL
        );

    subscriptions[4] =
        subscribe_properties(
            bus,
            PPD_SERVICE,
            &watch
        );

    subscriptions[5] =
        subscribe_properties(
            bus,
            UPOWER_SERVICE,
            &watch
        );

    for (size_t i = 0; i < G_N_ELEMENTS(subscriptions); i++) {
        if (subscriptions[i] == 0) {
            for (size_t j = 0; j < i; j++) {
                if (subscriptions[j] != 0) {
                    g_dbus_connection_signal_unsubscribe(
                        bus,
                        subscriptions[j]
                    );
                }
            }

            g_set_error_literal(
                error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "failed to subscribe to control authority signals"
            );
            return false;
        }
    }

    const guint sigint_source =
        g_unix_signal_add(
            SIGINT,
            stop_watch,
            &watch
        );

    const guint sigterm_source =
        g_unix_signal_add(
            SIGTERM,
            stop_watch,
            &watch
        );

    if (!emit_fresh_state(
            &watch,
            error)) {
        for (size_t i = 0; i < G_N_ELEMENTS(subscriptions); i++) {
            g_dbus_connection_signal_unsubscribe(
                bus,
                subscriptions[i]
            );
        }

        if (sigint_source != 0) {
            g_source_remove(sigint_source);
        }

        if (sigterm_source != 0) {
            g_source_remove(sigterm_source);
        }

        g_free(watch.last_json);
        return false;
    }

    g_main_loop_run(loop);

    if (watch.debounce_source != 0) {
        g_source_remove(watch.debounce_source);
        watch.debounce_source = 0;
    }

    for (size_t i = 0; i < G_N_ELEMENTS(subscriptions); i++) {
        g_dbus_connection_signal_unsubscribe(
            bus,
            subscriptions[i]
        );
    }

    if (sigint_source != 0) {
        g_source_remove(sigint_source);
    }

    if (sigterm_source != 0) {
        g_source_remove(sigterm_source);
    }

    g_free(watch.last_json);

    return true;
}
