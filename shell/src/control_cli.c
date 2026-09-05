#include "audio_control.h"
#include "control_watch.h"
#include "control_plane.h"
#include "wifi_extended.h"
#include "bluetooth_extended.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <json-glib/json-glib.h>

static void print_usage(
        const char *program) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s state\n"
        "  %s watch\n"
        "  %s wifi set on|off\n"
        "  %s wifi toggle\n"
        "  %s bluetooth set on|off\n"
        "  %s bluetooth toggle\n"
        "  %s power-profile set NAME\n",
        program,
        program,
        program,
        program,
        program,
        program,
        program
    );
}

static bool parse_on_off(
        const char *value,
        bool *enabled) {
    if (g_strcmp0(value, "on") == 0) {
        *enabled = true;
        return true;
    }

    if (g_strcmp0(value, "off") == 0) {
        *enabled = false;
        return true;
    }

    return false;
}

static void add_nullable_string(
        JsonBuilder *builder,
        const char *name,
        const char *value) {
    json_builder_set_member_name(
        builder,
        name
    );

    if (value) {
        json_builder_add_string_value(
            builder,
            value
        );
    } else {
        json_builder_add_null_value(
            builder
        );
    }
}

static bool print_state(void) {
    OnyrionControlSnapshot snapshot;
    onyrion_control_snapshot_init(
        &snapshot
    );

    g_autoptr(GError) error = NULL;

    if (!onyrion_control_query(
            &snapshot,
            &error)) {
        fprintf(
            stderr,
            "CONTROL_FAIL state error=%s\n",
            error
                ? error->message
                : "unknown"
        );

        onyrion_control_snapshot_clear(
            &snapshot
        );

        return false;
    }

    g_autoptr(JsonBuilder) builder =
        json_builder_new();

    json_builder_begin_object(
        builder
    );

    json_builder_set_member_name(
        builder,
        "wifi"
    );
    json_builder_begin_object(
        builder
    );
    json_builder_set_member_name(
        builder,
        "available"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.wifi_available
    );
    json_builder_set_member_name(
        builder,
        "enabled"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.wifi_enabled
    );
    json_builder_set_member_name(
        builder,
        "hardware_enabled"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.wifi_hardware_enabled
    );
    add_nullable_string(
        builder,
        "connection",
        snapshot.wifi_connection
    );
    json_builder_end_object(
        builder
    );

    json_builder_set_member_name(
        builder,
        "bluetooth"
    );
    json_builder_begin_object(
        builder
    );
    json_builder_set_member_name(
        builder,
        "available"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.bluetooth_available
    );
    json_builder_set_member_name(
        builder,
        "powered"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.bluetooth_powered
    );
    add_nullable_string(
        builder,
        "alias",
        snapshot.bluetooth_alias
    );
    json_builder_set_member_name(
        builder,
        "devices"
    );
    json_builder_begin_array(
        builder
    );

    for (guint i = 0;
            i < snapshot.bluetooth_devices->len;
            i++) {
        const OnyrionControlBluetoothDevice *device =
            g_ptr_array_index(
                snapshot.bluetooth_devices,
                i
            );

        json_builder_begin_object(
            builder
        );
        add_nullable_string(
            builder,
            "path",
            device->path
        );
        add_nullable_string(
            builder,
            "address",
            device->address
        );
        add_nullable_string(
            builder,
            "name",
            device->name
        );
        json_builder_set_member_name(
            builder,
            "connected"
        );
        json_builder_add_boolean_value(
            builder,
            device->connected
        );
        json_builder_set_member_name(
            builder,
            "paired"
        );
        json_builder_add_boolean_value(
            builder,
            device->paired
        );
        json_builder_set_member_name(
            builder,
            "trusted"
        );
        json_builder_add_boolean_value(
            builder,
            device->trusted
        );
        json_builder_end_object(
            builder
        );
    }

    json_builder_end_array(
        builder
    );
    json_builder_end_object(
        builder
    );

    json_builder_set_member_name(
        builder,
        "power_profile"
    );
    json_builder_begin_object(
        builder
    );
    json_builder_set_member_name(
        builder,
        "available"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.power_profiles_available
    );
    add_nullable_string(
        builder,
        "active",
        snapshot.power_profile_active
    );
    json_builder_set_member_name(
        builder,
        "profiles"
    );
    json_builder_begin_array(
        builder
    );

    for (guint i = 0;
            i < snapshot.power_profiles->len;
            i++) {
        json_builder_add_string_value(
            builder,
            g_ptr_array_index(
                snapshot.power_profiles,
                i
            )
        );
    }

    json_builder_end_array(
        builder
    );
    json_builder_end_object(
        builder
    );

    json_builder_set_member_name(
        builder,
        "battery"
    );
    json_builder_begin_object(
        builder
    );
    json_builder_set_member_name(
        builder,
        "available"
    );
    json_builder_add_boolean_value(
        builder,
        snapshot.battery_available
    );
    json_builder_set_member_name(
        builder,
        "percentage"
    );
    json_builder_add_double_value(
        builder,
        snapshot.battery_percentage
    );
    json_builder_set_member_name(
        builder,
        "state"
    );
    json_builder_add_int_value(
        builder,
        snapshot.battery_state
    );
    json_builder_set_member_name(
        builder,
        "time_to_full"
    );
    json_builder_add_int_value(
        builder,
        snapshot.battery_time_to_full
    );
    json_builder_set_member_name(
        builder,
        "time_to_empty"
    );
    json_builder_add_int_value(
        builder,
        snapshot.battery_time_to_empty
    );
    add_nullable_string(
        builder,
        "icon",
        snapshot.battery_icon
    );
    json_builder_end_object(
        builder
    );

    json_builder_end_object(
        builder
    );

    g_autoptr(JsonNode) root =
        json_builder_get_root(
            builder
        );

    g_autoptr(JsonGenerator) generator =
        json_generator_new();

    json_generator_set_root(
        generator,
        root
    );

    g_autofree char *json =
        json_generator_to_data(
            generator,
            NULL
        );

    puts(json);

    onyrion_control_snapshot_clear(
        &snapshot
    );

    return true;
}

static bool run_set(
        const char *kind,
        const char *value) {
    g_autoptr(GError) error = NULL;

    if (g_strcmp0(kind, "wifi") == 0) {
        bool enabled = false;

        if (!parse_on_off(
                value,
                &enabled)) {
            fprintf(
                stderr,
                "CONTROL_FAIL wifi invalid=%s\n",
                value
            );
            return false;
        }

        if (!onyrion_control_wifi_set(
                enabled,
                &error)) {
            fprintf(
                stderr,
                "CONTROL_FAIL wifi error=%s\n",
                error
                    ? error->message
                    : "unknown"
            );
            return false;
        }

        printf(
            "CONTROL_OK wifi enabled=%s\n",
            enabled
                ? "true"
                : "false"
        );

        return true;
    }

    if (g_strcmp0(
            kind,
            "bluetooth") == 0) {
        bool enabled = false;

        if (!parse_on_off(
                value,
                &enabled)) {
            fprintf(
                stderr,
                "CONTROL_FAIL bluetooth invalid=%s\n",
                value
            );
            return false;
        }

        if (!onyrion_control_bluetooth_set(
                enabled,
                &error)) {
            fprintf(
                stderr,
                "CONTROL_FAIL bluetooth error=%s\n",
                error
                    ? error->message
                    : "unknown"
            );
            return false;
        }

        printf(
            "CONTROL_OK bluetooth powered=%s\n",
            enabled
                ? "true"
                : "false"
        );

        return true;
    }

    if (g_strcmp0(
            kind,
            "power-profile") == 0) {
        if (!onyrion_control_power_profile_set(
                value,
                &error)) {
            fprintf(
                stderr,
                "CONTROL_FAIL power-profile error=%s\n",
                error
                    ? error->message
                    : "unknown"
            );
            return false;
        }

        printf(
            "CONTROL_OK power-profile active=%s\n",
            value
        );

        return true;
    }

    return false;
}

static bool run_toggle(
        const char *domain) {
    OnyrionControlSnapshot snapshot;
    onyrion_control_snapshot_init(&snapshot);

    g_autoptr(GError) error = NULL;

    if (!onyrion_control_query(
            &snapshot,
            &error)) {
        fprintf(
            stderr,
            "CONTROL_FAIL %s toggle-state=%s\n",
            domain,
            error
                ? error->message
                : "unknown"
        );
        onyrion_control_snapshot_clear(&snapshot);
        return false;
    }

    bool next = false;

    if (g_strcmp0(domain, "wifi") == 0) {
        if (!snapshot.wifi_available) {
            fprintf(
                stderr,
                "CONTROL_FAIL wifi unavailable\n"
            );
            onyrion_control_snapshot_clear(&snapshot);
            return false;
        }
        next = !snapshot.wifi_enabled;
    } else if (g_strcmp0(domain, "bluetooth") == 0) {
        if (!snapshot.bluetooth_available) {
            fprintf(
                stderr,
                "CONTROL_FAIL bluetooth unavailable\n"
            );
            onyrion_control_snapshot_clear(&snapshot);
            return false;
        }
        next = !snapshot.bluetooth_powered;
    } else {
        onyrion_control_snapshot_clear(&snapshot);
        return false;
    }

    onyrion_control_snapshot_clear(&snapshot);
    g_clear_error(&error);

    bool ok = false;

    if (g_strcmp0(domain, "wifi") == 0) {
        ok =
            onyrion_control_wifi_set(
                next,
                &error
            );
    } else {
        ok =
            onyrion_control_bluetooth_set(
                next,
                &error
            );
    }

    if (!ok) {
        fprintf(
            stderr,
            "CONTROL_FAIL %s toggle=%s\n",
            domain,
            error
                ? error->message
                : "unknown"
        );
        return false;
    }

    printf(
        "CONTROL_OK %s toggled=%s\n",
        domain,
        next
            ? "on"
            : "off"
    );

    return true;
}

int main(
        int argc,
        char **argv) {
    if (onyrion_bluetooth_extended_handles(argc, argv)) {
        return onyrion_bluetooth_extended_cli(argc, argv);
    }
    if (onyrion_wifi_extended_handles(argc, argv)) {
        return onyrion_wifi_extended_cli(argc, argv);
    }
    if (argc >= 2 &&
            g_strcmp0(
                argv[1],
                "audio") == 0) {
        return onyrion_audio_cli(
            argc - 2,
            &argv[2]
        );
    }

    if (argc == 2 &&
            g_strcmp0(
                argv[1],
                "watch") == 0) {
        g_autoptr(GError) error = NULL;

        if (!onyrion_control_watch_run(&error)) {
            fprintf(
                stderr,
                "CONTROL_FAIL watch error=%s\n",
                error
                    ? error->message
                    : "unknown"
            );
            return 1;
        }

        return 0;
    }

    if (argc == 2 &&
            g_strcmp0(
                argv[1],
                "state") == 0) {
        return print_state()
            ? 0
            : 1;
    }

    if (argc == 3 &&
            g_strcmp0(
                argv[2],
                "toggle") == 0 &&
            (
                g_strcmp0(
                    argv[1],
                    "wifi") == 0 ||
                g_strcmp0(
                    argv[1],
                    "bluetooth") == 0
            )) {
        return run_toggle(argv[1])
            ? 0
            : 1;
    }

    if (argc == 4 &&
            g_strcmp0(
                argv[2],
                "set") == 0 &&
            (
                g_strcmp0(
                    argv[1],
                    "wifi") == 0 ||
                g_strcmp0(
                    argv[1],
                    "bluetooth") == 0 ||
                g_strcmp0(
                    argv[1],
                    "power-profile") == 0
            )) {
        return run_set(
            argv[1],
            argv[3]
        )
            ? 0
            : 1;
    }

    print_usage(argv[0]);
    return 2;
}
