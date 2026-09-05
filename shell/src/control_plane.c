#include "control_plane.h"

#include <string.h>

#define NM_SERVICE "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_IFACE "org.freedesktop.NetworkManager"
#define NM_ACTIVE_IFACE \
    "org.freedesktop.NetworkManager.Connection.Active"

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ROOT "/"
#define BLUEZ_ADAPTER_IFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE "org.bluez.Device1"

#define PPD_SERVICE "org.freedesktop.UPower.PowerProfiles"
#define PPD_PATH "/org/freedesktop/UPower/PowerProfiles"
#define PPD_IFACE "org.freedesktop.UPower.PowerProfiles"

#define UPOWER_SERVICE "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_IFACE "org.freedesktop.UPower"
#define UPOWER_DEVICE_IFACE "org.freedesktop.UPower.Device"

static void bluetooth_device_free(
        gpointer data) {
    OnyrionControlBluetoothDevice *device =
        data;

    if (!device) {
        return;
    }

    g_free(device->path);
    g_free(device->address);
    g_free(device->name);
    g_free(device);
}

void onyrion_control_snapshot_init(
        OnyrionControlSnapshot *snapshot) {
    g_return_if_fail(snapshot != NULL);

    *snapshot =
        (OnyrionControlSnapshot){0};

    snapshot->bluetooth_devices =
        g_ptr_array_new_with_free_func(
            bluetooth_device_free
        );

    snapshot->power_profiles =
        g_ptr_array_new_with_free_func(
            g_free
        );
}

void onyrion_control_snapshot_clear(
        OnyrionControlSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    g_free(snapshot->wifi_connection);
    g_free(snapshot->bluetooth_alias);
    g_clear_pointer(
        &snapshot->bluetooth_devices,
        g_ptr_array_unref
    );
    g_free(snapshot->power_profile_active);
    g_clear_pointer(
        &snapshot->power_profiles,
        g_ptr_array_unref
    );
    g_free(snapshot->battery_icon);

    *snapshot =
        (OnyrionControlSnapshot){0};
}

static GVariant *call_sync(
        GDBusConnection *bus,
        const char *service,
        const char *path,
        const char *interface,
        const char *method,
        GVariant *parameters,
        const GVariantType *reply_type,
        GError **error) {
    return g_dbus_connection_call_sync(
        bus,
        service,
        path,
        interface,
        method,
        parameters,
        reply_type,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        error
    );
}

static GVariant *get_property(
        GDBusConnection *bus,
        const char *service,
        const char *path,
        const char *interface,
        const char *property,
        GError **error) {
    g_autoptr(GVariant) reply =
        call_sync(
            bus,
            service,
            path,
            "org.freedesktop.DBus.Properties",
            "Get",
            g_variant_new(
                "(ss)",
                interface,
                property
            ),
            G_VARIANT_TYPE("(v)"),
            error
        );

    if (!reply) {
        return NULL;
    }

    GVariant *value = NULL;

    g_variant_get(
        reply,
        "(v)",
        &value
    );

    return value;
}

static bool set_property(
        GDBusConnection *bus,
        const char *service,
        const char *path,
        const char *interface,
        const char *property,
        GVariant *value,
        GError **error) {
    g_autoptr(GVariant) reply =
        call_sync(
            bus,
            service,
            path,
            "org.freedesktop.DBus.Properties",
            "Set",
            g_variant_new(
                "(ssv)",
                interface,
                property,
                value
            ),
            G_VARIANT_TYPE_UNIT,
            error
        );

    return reply != NULL;
}

static bool name_has_owner(
        GDBusConnection *bus,
        const char *service,
        GError **error) {
    g_autoptr(GVariant) reply =
        call_sync(
            bus,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "NameHasOwner",
            g_variant_new(
                "(s)",
                service
            ),
            G_VARIANT_TYPE("(b)"),
            error
        );

    if (!reply) {
        return false;
    }

    gboolean owned = FALSE;

    g_variant_get(
        reply,
        "(b)",
        &owned
    );

    return owned;
}

static bool query_network_manager(
        GDBusConnection *bus,
        OnyrionControlSnapshot *snapshot,
        GError **error) {
    if (!name_has_owner(
            bus,
            NM_SERVICE,
            error)) {
        if (error && *error) {
            return false;
        }

        return true;
    }

    snapshot->wifi_available = true;

    g_autoptr(GVariant) wireless =
        get_property(
            bus,
            NM_SERVICE,
            NM_PATH,
            NM_IFACE,
            "WirelessEnabled",
            error
        );

    if (!wireless) {
        return false;
    }

    snapshot->wifi_enabled =
        g_variant_get_boolean(
            wireless
        );

    g_autoptr(GVariant) hardware =
        get_property(
            bus,
            NM_SERVICE,
            NM_PATH,
            NM_IFACE,
            "WirelessHardwareEnabled",
            error
        );

    if (!hardware) {
        return false;
    }

    snapshot->wifi_hardware_enabled =
        g_variant_get_boolean(
            hardware
        );

    g_autoptr(GVariant) primary =
        get_property(
            bus,
            NM_SERVICE,
            NM_PATH,
            NM_IFACE,
            "PrimaryConnection",
            error
        );

    if (!primary) {
        return false;
    }

    const char *primary_path =
        g_variant_get_string(
            primary,
            NULL
        );

    if (g_strcmp0(
            primary_path,
            "/") == 0) {
        return true;
    }

    g_autoptr(GVariant) id =
        get_property(
            bus,
            NM_SERVICE,
            primary_path,
            NM_ACTIVE_IFACE,
            "Id",
            error
        );

    if (!id) {
        return false;
    }

    snapshot->wifi_connection =
        g_variant_dup_string(
            id,
            NULL
        );

    return true;
}

static bool query_bluez(
        GDBusConnection *bus,
        OnyrionControlSnapshot *snapshot,
        GError **error) {
    if (!name_has_owner(
            bus,
            BLUEZ_SERVICE,
            error)) {
        if (error && *error) {
            return false;
        }

        return true;
    }

    g_autoptr(GVariant) managed =
        call_sync(
            bus,
            BLUEZ_SERVICE,
            BLUEZ_ROOT,
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE(
                "(a{oa{sa{sv}}})"
            ),
            error
        );

    if (!managed) {
        return false;
    }

    GVariantIter *objects = NULL;
    char *object_path = NULL;
    GVariant *interfaces = NULL;

    g_variant_get(
        managed,
        "(a{oa{sa{sv}}})",
        &objects
    );

    while (g_variant_iter_next(
            objects,
            "{o@a{sa{sv}}}",
            &object_path,
            &interfaces)) {
        g_autoptr(GVariant) adapter_props =
            g_variant_lookup_value(
                interfaces,
                BLUEZ_ADAPTER_IFACE,
                G_VARIANT_TYPE(
                    "a{sv}"
                )
            );

        if (adapter_props &&
                !snapshot->bluetooth_available) {
            snapshot->bluetooth_available =
                true;

            g_autoptr(GVariant) powered =
                g_variant_lookup_value(
                    adapter_props,
                    "Powered",
                    G_VARIANT_TYPE_BOOLEAN
                );

            g_autoptr(GVariant) alias =
                g_variant_lookup_value(
                    adapter_props,
                    "Alias",
                    G_VARIANT_TYPE_STRING
                );

            snapshot->bluetooth_powered =
                powered &&
                g_variant_get_boolean(
                    powered
                );

            if (alias) {
                snapshot->bluetooth_alias =
                    g_variant_dup_string(
                        alias,
                        NULL
                    );
            }
        }

        g_autoptr(GVariant) device_props =
            g_variant_lookup_value(
                interfaces,
                BLUEZ_DEVICE_IFACE,
                G_VARIANT_TYPE(
                    "a{sv}"
                )
            );

        if (device_props) {
            OnyrionControlBluetoothDevice *device =
                g_new0(
                    OnyrionControlBluetoothDevice,
                    1
                );

            device->path =
                g_strdup(
                    object_path
                );

            g_autoptr(GVariant) address =
                g_variant_lookup_value(
                    device_props,
                    "Address",
                    G_VARIANT_TYPE_STRING
                );

            g_autoptr(GVariant) name =
                g_variant_lookup_value(
                    device_props,
                    "Name",
                    G_VARIANT_TYPE_STRING
                );

            g_autoptr(GVariant) connected =
                g_variant_lookup_value(
                    device_props,
                    "Connected",
                    G_VARIANT_TYPE_BOOLEAN
                );

            g_autoptr(GVariant) paired =
                g_variant_lookup_value(
                    device_props,
                    "Paired",
                    G_VARIANT_TYPE_BOOLEAN
                );

            g_autoptr(GVariant) trusted =
                g_variant_lookup_value(
                    device_props,
                    "Trusted",
                    G_VARIANT_TYPE_BOOLEAN
                );

            device->address =
                address
                    ? g_variant_dup_string(
                        address,
                        NULL
                    )
                    : g_strdup("");

            device->name =
                name
                    ? g_variant_dup_string(
                        name,
                        NULL
                    )
                    : g_strdup("");

            device->connected =
                connected &&
                g_variant_get_boolean(
                    connected
                );

            device->paired =
                paired &&
                g_variant_get_boolean(
                    paired
                );

            device->trusted =
                trusted &&
                g_variant_get_boolean(
                    trusted
                );

            g_ptr_array_add(
                snapshot->bluetooth_devices,
                device
            );
        }

        g_variant_unref(interfaces);
        interfaces = NULL;

        g_free(object_path);
        object_path = NULL;
    }

    g_variant_iter_free(objects);
    return true;
}

static bool query_power_profiles(
        GDBusConnection *bus,
        OnyrionControlSnapshot *snapshot,
        GError **error) {
    if (!name_has_owner(
            bus,
            PPD_SERVICE,
            error)) {
        if (error && *error) {
            return false;
        }

        return true;
    }

    snapshot->power_profiles_available =
        true;

    g_autoptr(GVariant) active =
        get_property(
            bus,
            PPD_SERVICE,
            PPD_PATH,
            PPD_IFACE,
            "ActiveProfile",
            error
        );

    if (!active) {
        return false;
    }

    snapshot->power_profile_active =
        g_variant_dup_string(
            active,
            NULL
        );

    g_autoptr(GVariant) profiles =
        get_property(
            bus,
            PPD_SERVICE,
            PPD_PATH,
            PPD_IFACE,
            "Profiles",
            error
        );

    if (!profiles) {
        return false;
    }

    GVariantIter iter;
    GVariant *entry = NULL;

    g_variant_iter_init(
        &iter,
        profiles
    );

    while ((entry =
            g_variant_iter_next_value(
                &iter
            ))) {
        g_autoptr(GVariant) profile =
            g_variant_lookup_value(
                entry,
                "Profile",
                G_VARIANT_TYPE_STRING
            );

        if (profile) {
            g_ptr_array_add(
                snapshot->power_profiles,
                g_variant_dup_string(
                    profile,
                    NULL
                )
            );
        }

        g_variant_unref(entry);
    }

    return true;
}

static bool query_upower(
        GDBusConnection *bus,
        OnyrionControlSnapshot *snapshot,
        GError **error) {
    if (!name_has_owner(
            bus,
            UPOWER_SERVICE,
            error)) {
        if (error && *error) {
            return false;
        }

        return true;
    }

    g_autoptr(GVariant) display =
        call_sync(
            bus,
            UPOWER_SERVICE,
            UPOWER_PATH,
            UPOWER_IFACE,
            "GetDisplayDevice",
            NULL,
            G_VARIANT_TYPE("(o)"),
            error
        );

    if (!display) {
        return false;
    }

    const char *path = NULL;

    g_variant_get(
        display,
        "(&o)",
        &path
    );

    if (!path ||
            g_strcmp0(path, "/") == 0) {
        return true;
    }

    snapshot->battery_available = true;

    g_autoptr(GVariant) percentage =
        get_property(
            bus,
            UPOWER_SERVICE,
            path,
            UPOWER_DEVICE_IFACE,
            "Percentage",
            error
        );

    if (!percentage) {
        return false;
    }

    snapshot->battery_percentage =
        g_variant_get_double(
            percentage
        );

    g_autoptr(GVariant) state =
        get_property(
            bus,
            UPOWER_SERVICE,
            path,
            UPOWER_DEVICE_IFACE,
            "State",
            error
        );

    if (!state) {
        return false;
    }

    snapshot->battery_state =
        g_variant_get_uint32(
            state
        );

    g_autoptr(GVariant) time_to_full =
        get_property(
            bus,
            UPOWER_SERVICE,
            path,
            UPOWER_DEVICE_IFACE,
            "TimeToFull",
            error
        );

    if (!time_to_full) {
        return false;
    }

    snapshot->battery_time_to_full =
        g_variant_get_int64(
            time_to_full
        );

    g_autoptr(GVariant) time_to_empty =
        get_property(
            bus,
            UPOWER_SERVICE,
            path,
            UPOWER_DEVICE_IFACE,
            "TimeToEmpty",
            error
        );

    if (!time_to_empty) {
        return false;
    }

    snapshot->battery_time_to_empty =
        g_variant_get_int64(
            time_to_empty
        );

    g_autoptr(GVariant) icon =
        get_property(
            bus,
            UPOWER_SERVICE,
            path,
            UPOWER_DEVICE_IFACE,
            "IconName",
            error
        );

    if (!icon) {
        return false;
    }

    snapshot->battery_icon =
        g_variant_dup_string(
            icon,
            NULL
        );

    return true;
}

bool onyrion_control_query(
        OnyrionControlSnapshot *snapshot,
        GError **error) {
    g_return_val_if_fail(
        snapshot != NULL,
        false
    );

    onyrion_control_snapshot_clear(
        snapshot
    );

    onyrion_control_snapshot_init(
        snapshot
    );

    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync(
            G_BUS_TYPE_SYSTEM,
            NULL,
            error
        );

    if (!bus) {
        return false;
    }

    return
        query_network_manager(
            bus,
            snapshot,
            error
        ) &&
        query_bluez(
            bus,
            snapshot,
            error
        ) &&
        query_power_profiles(
            bus,
            snapshot,
            error
        ) &&
        query_upower(
            bus,
            snapshot,
            error
        );
}

bool onyrion_control_wifi_set(
        bool enabled,
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

    return set_property(
        bus,
        NM_SERVICE,
        NM_PATH,
        NM_IFACE,
        "WirelessEnabled",
        g_variant_new_boolean(
            enabled
        ),
        error
    );
}

static char *first_bluez_adapter_path(
        GDBusConnection *bus,
        GError **error) {
    g_autoptr(GVariant) managed =
        call_sync(
            bus,
            BLUEZ_SERVICE,
            BLUEZ_ROOT,
            "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE(
                "(a{oa{sa{sv}}})"
            ),
            error
        );

    if (!managed) {
        return NULL;
    }

    GVariantIter *objects = NULL;
    char *object_path = NULL;
    GVariant *interfaces = NULL;
    char *result = NULL;

    g_variant_get(
        managed,
        "(a{oa{sa{sv}}})",
        &objects
    );

    while (g_variant_iter_next(
            objects,
            "{o@a{sa{sv}}}",
            &object_path,
            &interfaces)) {
        g_autoptr(GVariant) adapter =
            g_variant_lookup_value(
                interfaces,
                BLUEZ_ADAPTER_IFACE,
                G_VARIANT_TYPE(
                    "a{sv}"
                )
            );

        if (adapter) {
            result =
                g_strdup(
                    object_path
                );
        }

        g_variant_unref(interfaces);
        interfaces = NULL;

        g_free(object_path);
        object_path = NULL;

        if (result) {
            break;
        }
    }

    g_variant_iter_free(objects);

    if (!result) {
        g_set_error_literal(
            error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "no BlueZ Adapter1 object available"
        );
    }

    return result;
}

bool onyrion_control_bluetooth_set(
        bool enabled,
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

    g_autofree char *adapter_path =
        first_bluez_adapter_path(
            bus,
            error
        );

    if (!adapter_path) {
        return false;
    }

    return set_property(
        bus,
        BLUEZ_SERVICE,
        adapter_path,
        BLUEZ_ADAPTER_IFACE,
        "Powered",
        g_variant_new_boolean(
            enabled
        ),
        error
    );
}

bool onyrion_control_power_profile_set(
        const char *profile,
        GError **error) {
    g_return_val_if_fail(
        profile != NULL,
        false
    );

    OnyrionControlSnapshot snapshot;
    onyrion_control_snapshot_init(
        &snapshot
    );

    bool ok =
        onyrion_control_query(
            &snapshot,
            error
        );

    if (!ok) {
        onyrion_control_snapshot_clear(
            &snapshot
        );

        return false;
    }

    bool found = false;

    for (guint i = 0;
            i < snapshot.power_profiles->len;
            i++) {
        const char *candidate =
            g_ptr_array_index(
                snapshot.power_profiles,
                i
            );

        if (g_strcmp0(
                candidate,
                profile) == 0) {
            found = true;
            break;
        }
    }

    onyrion_control_snapshot_clear(
        &snapshot
    );

    if (!found) {
        g_set_error(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "unsupported power profile: %s",
            profile
        );

        return false;
    }

    g_autoptr(GDBusConnection) bus =
        g_bus_get_sync(
            G_BUS_TYPE_SYSTEM,
            NULL,
            error
        );

    if (!bus) {
        return false;
    }

    return set_property(
        bus,
        PPD_SERVICE,
        PPD_PATH,
        PPD_IFACE,
        "ActiveProfile",
        g_variant_new_string(
            profile
        ),
        error
    );
}
