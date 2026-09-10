#include "bluetooth_extended.h"

#include <gio/gio.h>
#include <glib.h>

#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ROOT "/"
#define BLUEZ_ADAPTER_IFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE "org.bluez.Device1"
#define DBUS_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define DBUS_PROPERTIES "org.freedesktop.DBus.Properties"

static GVariant *call_sync(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *method,
        GVariant *params,
        const GVariantType *reply_type,
        int timeout_ms,
        GError **error) {
    return g_dbus_connection_call_sync(
        bus,
        BLUEZ_SERVICE,
        path,
        iface,
        method,
        params,
        reply_type,
        G_DBUS_CALL_FLAGS_NONE,
        timeout_ms,
        NULL,
        error
    );
}

static GVariant *managed_objects(
        GDBusConnection *bus,
        GError **error) {
    g_autoptr(GVariant) reply = call_sync(
        bus,
        BLUEZ_ROOT,
        DBUS_OBJECT_MANAGER,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        8000,
        error
    );
    if (!reply) return NULL;

    GVariant *objects = NULL;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    return objects;
}

static char *find_adapter(
        GDBusConnection *bus,
        GError **error) {
    g_autoptr(GVariant) objects = managed_objects(bus, error);
    if (!objects) return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(
                &iter,
                "{&o@a{sa{sv}}}",
                &path,
                &ifaces)) {
        g_autoptr(GVariant) owned_ifaces = ifaces;
        g_autoptr(GVariant) props = g_variant_lookup_value(
            owned_ifaces,
            BLUEZ_ADAPTER_IFACE,
            G_VARIANT_TYPE("a{sv}")
        );
        if (props) return g_strdup(path);
    }

    g_set_error(
        error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "no BlueZ Adapter1 object available"
    );
    return NULL;
}

static char *find_device(
        GDBusConnection *bus,
        const char *address,
        GError **error) {
    g_autoptr(GVariant) objects = managed_objects(bus, error);
    if (!objects) return NULL;

    GVariantIter iter;
    const char *path = NULL;
    GVariant *ifaces = NULL;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(
                &iter,
                "{&o@a{sa{sv}}}",
                &path,
                &ifaces)) {
        g_autoptr(GVariant) owned_ifaces = ifaces;
        g_autoptr(GVariant) props = g_variant_lookup_value(
            owned_ifaces,
            BLUEZ_DEVICE_IFACE,
            G_VARIANT_TYPE("a{sv}")
        );
        if (!props) continue;

        const char *candidate = NULL;
        if (g_variant_lookup(props, "Address", "&s", &candidate) &&
                g_ascii_strcasecmp(candidate, address) == 0) {
            return g_strdup(path);
        }
    }

    g_set_error(
        error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "Bluetooth device %s is not present in BlueZ ObjectManager",
        address
    );
    return NULL;
}

static bool valid_address(const char *value) {
    if (!value || strlen(value) != 17) return false;
    for (size_t i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':') return false;
        } else if (!g_ascii_isxdigit(value[i])) {
            return false;
        }
    }
    return true;
}

static bool bool_property(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *name,
        bool *out,
        GError **error) {
    g_autoptr(GVariant) reply = call_sync(
        bus,
        path,
        DBUS_PROPERTIES,
        "Get",
        g_variant_new("(ss)", iface, name),
        G_VARIANT_TYPE("(v)"),
        8000,
        error
    );
    if (!reply) return false;

    g_autoptr(GVariant) value = NULL;
    g_variant_get(reply, "(@v)", &value);
    g_autoptr(GVariant) inner = g_variant_get_variant(value);
    if (!g_variant_is_of_type(inner, G_VARIANT_TYPE_BOOLEAN)) {
        g_set_error(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "%s.%s is not boolean",
            iface,
            name
        );
        return false;
    }
    *out = g_variant_get_boolean(inner);
    return true;
}

static bool wait_bool(
        GDBusConnection *bus,
        const char *path,
        const char *iface,
        const char *name,
        bool expected,
        int attempts,
        GError **error) {
    for (int i = 0; i < attempts; i++) {
        bool value = false;
        g_autoptr(GError) local_error = NULL;
        if (bool_property(
                    bus,
                    path,
                    iface,
                    name,
                    &value,
                    &local_error) &&
                value == expected) {
            return true;
        }
        g_usleep(100000);
    }

    g_set_error(
        error,
        G_IO_ERROR,
        G_IO_ERROR_TIMED_OUT,
        "%s.%s did not become %s",
        iface,
        name,
        expected ? "true" : "false"
    );
    return false;
}

static int scan_cli(
        GDBusConnection *bus,
        int seconds) {
    g_autoptr(GError) error = NULL;
    g_autofree char *adapter = find_adapter(bus, &error);
    if (!adapter) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan adapter=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    bool powered = false;
    if (!bool_property(
                bus,
                adapter,
                BLUEZ_ADAPTER_IFACE,
                "Powered",
                &powered,
                &error) ||
            !powered) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan adapter-not-powered%s%s\n",
            error ? " error=" : "",
            error ? error->message : ""
        );
        return 1;
    }

    g_autoptr(GVariant) started = call_sync(
        bus,
        adapter,
        BLUEZ_ADAPTER_IFACE,
        "StartDiscovery",
        NULL,
        G_VARIANT_TYPE("()"),
        10000,
        &error
    );
    if (!started) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan start=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_print(
        "CONTROL_OK bluetooth scan state=started seconds=%d\n",
        seconds
    );
    fflush(stdout);

    for (int i = 0; i < seconds * 10; i++) {
        g_usleep(100000);
    }

    g_clear_error(&error);
    g_autoptr(GVariant) stopped = call_sync(
        bus,
        adapter,
        BLUEZ_ADAPTER_IFACE,
        "StopDiscovery",
        NULL,
        G_VARIANT_TYPE("()"),
        10000,
        &error
    );
    if (!stopped) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan stop=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_print("CONTROL_OK bluetooth scan state=stopped\n");
    return 0;
}

static int scan_hold_cli(GDBusConnection *bus) {
    g_autoptr(GError) error = NULL;
    g_autofree char *adapter = find_adapter(bus, &error);
    if (!adapter) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=hold adapter=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    bool powered = false;
    if (!bool_property(
                bus, adapter, BLUEZ_ADAPTER_IFACE, "Powered",
                &powered, &error) || !powered) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=hold adapter-not-powered%s%s\n",
            error ? " error=" : "",
            error ? error->message : ""
        );
        return 1;
    }

    g_autoptr(GVariant) started = call_sync(
        bus, adapter, BLUEZ_ADAPTER_IFACE, "StartDiscovery",
        NULL, G_VARIANT_TYPE("()"), 10000, &error
    );
    if (!started) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=hold start=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_clear_error(&error);
    if (!wait_bool(
                bus, adapter, BLUEZ_ADAPTER_IFACE, "Discovering",
                true, 50, &error)) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=hold verify-start=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_print("CONTROL_OK bluetooth scan state=started mode=hold\n");
    fflush(stdout);

    char buffer[64];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n > 0) continue;
        if (n == 0) break;
        if (errno == EINTR) continue;
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=hold stdin=%s\n",
            g_strerror(errno)
        );
        break;
    }

    g_clear_error(&error);
    g_autoptr(GVariant) stopped = call_sync(
        bus, adapter, BLUEZ_ADAPTER_IFACE, "StopDiscovery",
        NULL, G_VARIANT_TYPE("()"), 10000, &error
    );
    if (!stopped) {
        g_clear_error(&error);
        bool discovering = true;
        if (!bool_property(
                    bus, adapter, BLUEZ_ADAPTER_IFACE, "Discovering",
                    &discovering, &error) || discovering) {
            g_printerr(
                "CONTROL_FAIL bluetooth scan action=hold stop=%s\n",
                error ? error->message : "unknown"
            );
            return 1;
        }
    }

    g_print("CONTROL_OK bluetooth scan state=stopped mode=hold\n");
    fflush(stdout);
    return 0;
}

static int scan_set_cli(
        GDBusConnection *bus,
        bool start) {
    g_autoptr(GError) error = NULL;
    g_autofree char *adapter = find_adapter(bus, &error);
    if (!adapter) {
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=%s adapter=%s\n",
            start ? "start" : "stop",
            error ? error->message : "unknown"
        );
        return 1;
    }

    if (start) {
        bool powered = false;
        if (!bool_property(
                    bus, adapter, BLUEZ_ADAPTER_IFACE, "Powered",
                    &powered, &error) || !powered) {
            g_printerr(
                "CONTROL_FAIL bluetooth scan action=start adapter-not-powered%s%s\n",
                error ? " error=" : "",
                error ? error->message : ""
            );
            return 1;
        }
    }

    const char *method = start ? "StartDiscovery" : "StopDiscovery";
    g_autoptr(GVariant) result = call_sync(
        bus, adapter, BLUEZ_ADAPTER_IFACE, method,
        NULL, G_VARIANT_TYPE("()"), 10000, &error
    );
    if (!result) {
        /* BlueZ may report NotReady/Failed for redundant StopDiscovery.
         * Treat an already-stopped discovery as idempotent success only when
         * the Discovering property confirms false. */
        if (!start) {
            g_clear_error(&error);
            bool discovering = true;
            if (bool_property(
                        bus, adapter, BLUEZ_ADAPTER_IFACE, "Discovering",
                        &discovering, &error) && !discovering) {
                g_print("CONTROL_OK bluetooth scan state=stopped already=1\n");
                return 0;
            }
        }
        g_printerr(
            "CONTROL_FAIL bluetooth scan action=%s error=%s\n",
            start ? "start" : "stop",
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_print(
        "CONTROL_OK bluetooth scan state=%s\n",
        start ? "started" : "stopped"
    );
    return 0;
}

static int device_cli(
        GDBusConnection *bus,
        const char *action,
        const char *address) {
    g_autoptr(GError) error = NULL;
    g_autofree char *path = find_device(bus, address, &error);
    if (!path) {
        g_printerr(
            "CONTROL_FAIL bluetooth device action=%s address=%s error=%s\n",
            action,
            address,
            error ? error->message : "unknown"
        );
        return 1;
    }

    const char *method = NULL;
    const char *verify_prop = NULL;
    bool expected = false;
    int timeout_ms = 30000;

    if (g_strcmp0(action, "connect") == 0) {
        method = "Connect";
        verify_prop = "Connected";
        expected = true;
    } else if (g_strcmp0(action, "disconnect") == 0) {
        method = "Disconnect";
        verify_prop = "Connected";
        expected = false;
    } else if (g_strcmp0(action, "pair") == 0) {
        method = "Pair";
        verify_prop = "Paired";
        expected = true;
        timeout_ms = 120000;
    } else {
        g_printerr(
            "CONTROL_FAIL bluetooth device invalid-action=%s\n",
            action
        );
        return 2;
    }

    bool current = false;
    if (bool_property(
                bus,
                path,
                BLUEZ_DEVICE_IFACE,
                verify_prop,
                &current,
                &error) &&
            current == expected) {
        g_print(
            "CONTROL_OK bluetooth device action=%s address=%s state=noop\n",
            action,
            address
        );
        return 0;
    }
    g_clear_error(&error);

    g_autoptr(GVariant) reply = call_sync(
        bus,
        path,
        BLUEZ_DEVICE_IFACE,
        method,
        NULL,
        G_VARIANT_TYPE("()"),
        timeout_ms,
        &error
    );
    if (!reply) {
        g_printerr(
            "CONTROL_FAIL bluetooth device action=%s address=%s error=%s\n",
            action,
            address,
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_clear_error(&error);
    if (!wait_bool(
                bus,
                path,
                BLUEZ_DEVICE_IFACE,
                verify_prop,
                expected,
                action[0] == 'p' ? 300 : 120,
                &error)) {
        g_printerr(
            "CONTROL_FAIL bluetooth device action=%s address=%s verify=%s\n",
            action,
            address,
            error ? error->message : "unknown"
        );
        return 1;
    }

    g_print(
        "CONTROL_OK bluetooth device action=%s address=%s state=%s\n",
        action,
        address,
        expected ? "true" : "false"
    );
    return 0;
}

bool onyrion_bluetooth_extended_handles(
        int argc,
        char **argv) {
    if (argc < 3 || g_strcmp0(argv[1], "bluetooth") != 0) return false;
    return g_strcmp0(argv[2], "scan") == 0 ||
        g_strcmp0(argv[2], "device") == 0;
}

int onyrion_bluetooth_extended_cli(
        int argc,
        char **argv) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(
        G_BUS_TYPE_SYSTEM,
        NULL,
        &error
    );
    if (!bus) {
        g_printerr(
            "CONTROL_FAIL bluetooth dbus=%s\n",
            error ? error->message : "unknown"
        );
        return 1;
    }

    if (g_strcmp0(argv[2], "scan") == 0) {
        if (argc == 4 && g_strcmp0(argv[3], "hold") == 0)
            return scan_hold_cli(bus);
        if (argc == 4 && g_strcmp0(argv[3], "start") == 0)
            return scan_set_cli(bus, true);
        if (argc == 4 && g_strcmp0(argv[3], "stop") == 0)
            return scan_set_cli(bus, false);

        int seconds = 6;
        if (argc == 4) {
            char *end = NULL;
            long parsed = strtol(argv[3], &end, 10);
            if (!end || *end != '\0' || parsed < 1 || parsed > 30) {
                g_printerr(
                    "CONTROL_FAIL bluetooth scan invalid-argument=%s\n",
                    argv[3]
                );
                return 2;
            }
            seconds = (int)parsed;
        } else if (argc != 3) {
            g_printerr(
                "usage: onyrion-control bluetooth scan [SECONDS|hold|start|stop]\n"
            );
            return 2;
        }
        return scan_cli(bus, seconds);
    }

    if (g_strcmp0(argv[2], "device") == 0) {
        if (argc != 5 || !valid_address(argv[4])) {
            g_printerr(
                "usage: onyrion-control bluetooth device connect|disconnect|pair ADDRESS\n"
            );
            return 2;
        }
        return device_cli(bus, argv[3], argv[4]);
    }

    return 2;
}
