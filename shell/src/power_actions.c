#include "power_actions.h"

#include <gio/gio.h>
#include <glib.h>

#include <stdbool.h>
#include <string.h>

#define LOGIN1_SERVICE "org.freedesktop.login1"
#define LOGIN1_PATH "/org/freedesktop/login1"
#define LOGIN1_MANAGER "org.freedesktop.login1.Manager"

#ifdef ONYRION_POWER_ACTION_TEST_SESSION_BUS
#define ONYRION_POWER_BUS_TYPE G_BUS_TYPE_SESSION
#else
#define ONYRION_POWER_BUS_TYPE G_BUS_TYPE_SYSTEM
#endif

static const char *power_method(
        const char *action) {
    if (g_strcmp0(action, "suspend") == 0) {
        return "Suspend";
    }
    if (g_strcmp0(action, "reboot") == 0) {
        return "Reboot";
    }
    if (g_strcmp0(action, "poweroff") == 0) {
        return "PowerOff";
    }
    return NULL;
}

bool onyrion_power_action(
        const char *action,
        bool interactive,
        GError **error) {
    const char *method = power_method(action);
    if (!method) {
        g_set_error(
            error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "unsupported power action: %s",
            action ? action : "<null>"
        );
        return false;
    }

    g_autoptr(GDBusConnection) bus = g_bus_get_sync(
        ONYRION_POWER_BUS_TYPE,
        NULL,
        error
    );
    if (!bus) return false;

    g_autoptr(GVariant) reply =
        g_dbus_connection_call_sync(
            bus,
            LOGIN1_SERVICE,
            LOGIN1_PATH,
            LOGIN1_MANAGER,
            method,
            g_variant_new("(b)", interactive),
            G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE,
            30000,
            NULL,
            error
        );

    return reply != NULL;
}
