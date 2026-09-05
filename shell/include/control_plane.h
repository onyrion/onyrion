#ifndef ONYRION_CONTROL_PLANE_H
#define ONYRION_CONTROL_PLANE_H

#include <stdbool.h>

#include <gio/gio.h>
#include <glib.h>

typedef struct onyrion_control_bluetooth_device {
    char *path;
    char *address;
    char *name;
    bool connected;
    bool paired;
    bool trusted;
} OnyrionControlBluetoothDevice;

typedef struct onyrion_control_snapshot {
    bool wifi_available;
    bool wifi_enabled;
    bool wifi_hardware_enabled;
    char *wifi_connection;

    bool bluetooth_available;
    bool bluetooth_powered;
    char *bluetooth_alias;
    GPtrArray *bluetooth_devices;

    bool power_profiles_available;
    char *power_profile_active;
    GPtrArray *power_profiles;

    bool battery_available;
    double battery_percentage;
    guint battery_state;
    gint64 battery_time_to_full;
    gint64 battery_time_to_empty;
    char *battery_icon;
} OnyrionControlSnapshot;

void onyrion_control_snapshot_init(
    OnyrionControlSnapshot *snapshot
);

void onyrion_control_snapshot_clear(
    OnyrionControlSnapshot *snapshot
);

bool onyrion_control_query(
    OnyrionControlSnapshot *snapshot,
    GError **error
);

bool onyrion_control_wifi_set(
    bool enabled,
    GError **error
);

bool onyrion_control_bluetooth_set(
    bool enabled,
    GError **error
);

bool onyrion_control_power_profile_set(
    const char *profile,
    GError **error
);

#endif
