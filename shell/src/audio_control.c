#include "audio_control.h"
#include "audio_extended.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#include <pipewire/device.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>

#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

#define AUDIO_MAX_NODES 256
#define AUDIO_MAX_CHANNELS 64

typedef enum audio_kind {
    AUDIO_KIND_SINK,
    AUDIO_KIND_SOURCE,
} AudioKind;

typedef enum audio_mode {
    AUDIO_MODE_STATE,
    AUDIO_MODE_WATCH,
    AUDIO_MODE_ACTION,
} AudioMode;

typedef enum audio_action {
    AUDIO_ACTION_NONE,
    AUDIO_ACTION_VOLUME_SET,
    AUDIO_ACTION_VOLUME_CHANGE,
    AUDIO_ACTION_MUTE_SET,
    AUDIO_ACTION_MUTE_TOGGLE,
} AudioAction;

typedef struct audio_node_record {
    bool used;
    uint32_t id;
    char *name;
    char *description;
    char *media_class;
    uint32_t device_id;
    int32_t route_device;
} AudioNodeRecord;

struct audio_app;

typedef struct audio_target {
    struct audio_app *app;
    AudioKind kind;

    char *default_name;

    struct pw_node *node;
    struct pw_device *device;
    struct spa_hook node_listener;
    struct spa_hook device_listener;
    bool node_listener_live;
    bool device_listener_live;

    uint32_t node_id;
    uint32_t device_id;
    int32_t route_device;
    int32_t route_index;

    char *name;
    char *description;

    bool prefers_route;
    bool route_control;
    bool have_volume;
    bool have_mute;

    uint32_t channels;
    double volume;
    bool muted;
} AudioTarget;

typedef struct audio_app {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;

    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct spa_hook metadata_listener;
    bool core_listener_live;
    bool registry_listener_live;
    bool metadata_listener_live;

    uint32_t metadata_id;

    AudioNodeRecord nodes[AUDIO_MAX_NODES];

    AudioTarget sink;
    AudioTarget source;

    AudioMode mode;
    AudioAction action;
    AudioKind action_kind;
    double action_value;
    bool action_bool;

    bool registry_done;
    bool metadata_bound;
    bool metadata_ready;
    bool targets_started;
    bool ready;
    bool failed;
    bool broken_pipe;

    int initial_seq;
    int metadata_seq;
    int targets_seq;
    int refresh_seq;
    int action_seq;
    bool refresh_again;

    double action_result_volume;
    bool action_result_muted;

    char *last_json;
} AudioApp;

static const char *kind_name(
        AudioKind kind) {
    return kind == AUDIO_KIND_SINK
        ? "sink"
        : "source";
}

static const char *kind_media_class(
        AudioKind kind) {
    return kind == AUDIO_KIND_SINK
        ? "Audio/Sink"
        : "Audio/Source";
}

static const char *kind_display_prefix(
        AudioKind kind) {
    return kind == AUDIO_KIND_SINK
        ? "VOL"
        : "MIC";
}

static double volume_from_raw(
        float raw) {
    if (raw <= 0.0f) {
        return 0.0;
    }

    double low = 0.0;
    double high = 1.0;

    while (high * high * high <
            (double)raw &&
            high < 16.0) {
        high *= 2.0;
    }

    for (unsigned int i = 0; i < 48; i++) {
        const double mid =
            (low + high) / 2.0;

        if (mid * mid * mid <
                (double)raw) {
            low = mid;
        } else {
            high = mid;
        }
    }

    return (low + high) / 2.0;
}

static float raw_from_volume(
        double volume) {
    return (float)(
        volume *
        volume *
        volume
    );
}

static double clamp_volume(
        double volume) {
    if (volume < 0.0) {
        return 0.0;
    }

    if (volume > 1.0) {
        return 1.0;
    }

    return volume;
}

static bool parse_u32_text(
        const char *text,
        uint32_t *value_out) {
    if (!text ||
            !text[0] ||
            !value_out) {
        return false;
    }

    char *end = NULL;
    errno = 0;

    const unsigned long value =
        strtoul(
            text,
            &end,
            10
        );

    if (errno != 0 ||
            !end ||
            *end != '\0' ||
            value > UINT32_MAX) {
        return false;
    }

    *value_out =
        (uint32_t)value;

    return true;
}

static bool parse_i32_text(
        const char *text,
        int32_t *value_out) {
    if (!text ||
            !text[0] ||
            !value_out) {
        return false;
    }

    char *end = NULL;
    errno = 0;

    const long value =
        strtol(
            text,
            &end,
            10
        );

    if (errno != 0 ||
            !end ||
            *end != '\0' ||
            value < INT32_MIN ||
            value > INT32_MAX) {
        return false;
    }

    *value_out =
        (int32_t)value;

    return true;
}

static bool parse_double_text(
        const char *text,
        double *value_out) {
    if (!text ||
            !text[0] ||
            !value_out) {
        return false;
    }

    char *end = NULL;
    errno = 0;

    const double value =
        g_ascii_strtod(
            text,
            &end
        );

    if (errno != 0 ||
            !end ||
            *end != '\0') {
        return false;
    }

    *value_out = value;

    return true;
}

static void node_record_clear(
        AudioNodeRecord *record) {
    if (!record) {
        return;
    }

    g_free(record->name);
    g_free(record->description);
    g_free(record->media_class);

    memset(
        record,
        0,
        sizeof(*record)
    );
}

static AudioNodeRecord *node_record_by_id(
        AudioApp *app,
        uint32_t id) {
    for (size_t i = 0;
            i < AUDIO_MAX_NODES;
            i++) {
        AudioNodeRecord *record =
            &app->nodes[i];

        if (record->used &&
                record->id == id) {
            return record;
        }
    }

    return NULL;
}

static AudioNodeRecord *node_record_by_name(
        AudioApp *app,
        const char *name,
        AudioKind kind) {
    if (!name) {
        return NULL;
    }

    const char *media_class =
        kind_media_class(kind);

    for (size_t i = 0;
            i < AUDIO_MAX_NODES;
            i++) {
        AudioNodeRecord *record =
            &app->nodes[i];

        if (!record->used) {
            continue;
        }

        if (g_strcmp0(
                record->name,
                name) == 0 &&
            g_strcmp0(
                record->media_class,
                media_class) == 0) {
            return record;
        }
    }

    return NULL;
}

static AudioNodeRecord *node_record_allocate(
        AudioApp *app) {
    for (size_t i = 0;
            i < AUDIO_MAX_NODES;
            i++) {
        if (!app->nodes[i].used) {
            return &app->nodes[i];
        }
    }

    return NULL;
}

static void target_runtime_clear(
        AudioTarget *target) {
    if (!target) {
        return;
    }

    if (target->device_listener_live) {
        spa_hook_remove(
            &target->device_listener
        );
        target->device_listener_live = false;
    }

    if (target->device) {
        pw_proxy_destroy(
            (struct pw_proxy *)target->device
        );
        target->device = NULL;
    }

    if (target->node_listener_live) {
        spa_hook_remove(
            &target->node_listener
        );
        target->node_listener_live = false;
    }

    if (target->node) {
        pw_proxy_destroy(
            (struct pw_proxy *)target->node
        );
        target->node = NULL;
    }

    g_free(target->name);
    g_free(target->description);

    target->name = NULL;
    target->description = NULL;
    target->node_id = UINT32_MAX;
    target->device_id = UINT32_MAX;
    target->route_device = -1;
    target->route_index = -1;
    target->prefers_route = false;
    target->route_control = false;
    target->have_volume = false;
    target->have_mute = false;
    target->channels = 0;
    target->volume = 0.0;
    target->muted = false;
}

static bool target_available(
        const AudioTarget *target) {
    return target &&
        target->node &&
        target->have_volume;
}

static const char *target_backend(
        const AudioTarget *target) {
    if (!target_available(target)) {
        return "unavailable";
    }

    if (target->route_control) {
        return "device-route";
    }

    if (target->prefers_route) {
        return "device-route-pending";
    }

    return "node-props";
}

static void app_queue_refresh(
        AudioApp *app);

static void target_update_props(
        AudioTarget *target,
        const struct spa_pod *props) {
    if (!target ||
            !props) {
        return;
    }

    const struct spa_pod_prop *volume_prop =
        spa_pod_find_prop(
            props,
            NULL,
            SPA_PROP_channelVolumes
        );

    if (volume_prop) {
        float volumes[AUDIO_MAX_CHANNELS] = {0};

        const uint32_t count =
            spa_pod_copy_array(
                &volume_prop->value,
                SPA_TYPE_Float,
                volumes,
                AUDIO_MAX_CHANNELS
            );

        if (count > 0 &&
                count <= AUDIO_MAX_CHANNELS) {
            target->channels = count;
            target->volume =
                volume_from_raw(
                    volumes[0]
                );
            target->have_volume = true;
        }
    }

    const struct spa_pod_prop *mute_prop =
        spa_pod_find_prop(
            props,
            NULL,
            SPA_PROP_mute
        );

    if (mute_prop) {
        bool muted = false;

        if (spa_pod_get_bool(
                &mute_prop->value,
                &muted) == 0) {
            target->muted = muted;
            target->have_mute = true;
        }
    }
}

static void node_param(
        void *data,
        int seq,
        uint32_t id,
        uint32_t index,
        uint32_t next,
        const struct spa_pod *param) {
    (void)seq;
    (void)index;
    (void)next;

    AudioTarget *target = data;

    if (id != SPA_PARAM_Props ||
            !param) {
        return;
    }

    target_update_props(
        target,
        param
    );

    app_queue_refresh(
        target->app
    );
}

static bool target_start_route_device(
        AudioTarget *target);

static void node_info(
        void *data,
        const struct pw_node_info *info) {
    AudioTarget *target = data;

    if (!target ||
            !target->node ||
            !info) {
        return;
    }

    if (info->props) {
        uint32_t device_id = UINT32_MAX;
        int32_t route_device = -1;

        const bool have_device =
            parse_u32_text(
                spa_dict_lookup(
                    info->props,
                    "device.id"
                ),
                &device_id
            );

        const bool have_route =
            parse_i32_text(
                spa_dict_lookup(
                    info->props,
                    "card.profile.device"
                ),
                &route_device
            );

        if (have_device &&
                have_route) {
            const bool changed =
                !target->prefers_route ||
                target->device_id != device_id ||
                target->route_device != route_device;

            if (changed) {
                if (target->device_listener_live) {
                    spa_hook_remove(
                        &target->device_listener
                    );
                    target->device_listener_live = false;
                }

                if (target->device) {
                    pw_proxy_destroy(
                        (struct pw_proxy *)target->device
                    );
                    target->device = NULL;
                }

                target->device_id = device_id;
                target->route_device = route_device;
                target->route_index = -1;
                target->prefers_route = true;
                target->route_control = false;

                if (!target_start_route_device(
                        target)) {
                    target->app->failed = true;
                    pw_main_loop_quit(
                        target->app->loop
                    );
                    return;
                }
            }
        }
    }

    const int rc =
        pw_node_enum_params(
            target->node,
            300,
            SPA_PARAM_Props,
            0,
            UINT32_MAX,
            NULL
        );

    if (rc >= 0) {
        app_queue_refresh(
            target->app
        );
    }
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_info,
    .param = node_param,
};

static void device_param(
        void *data,
        int seq,
        uint32_t id,
        uint32_t index,
        uint32_t next,
        const struct spa_pod *param) {
    (void)seq;
    (void)index;
    (void)next;

    AudioTarget *target = data;

    if (!target ||
            id != SPA_PARAM_Route ||
            !param) {
        return;
    }

    int32_t route_device = -1;
    int32_t route_index = -1;

    const struct spa_pod_prop *prop =
        spa_pod_find_prop(
            param,
            NULL,
            SPA_PARAM_ROUTE_device
        );

    if (prop) {
        (void)spa_pod_get_int(
            &prop->value,
            &route_device
        );
    }

    if (route_device !=
            target->route_device) {
        return;
    }

    prop =
        spa_pod_find_prop(
            param,
            NULL,
            SPA_PARAM_ROUTE_index
        );

    if (prop) {
        (void)spa_pod_get_int(
            &prop->value,
            &route_index
        );
    }

    prop =
        spa_pod_find_prop(
            param,
            NULL,
            SPA_PARAM_ROUTE_props
        );

    if (!prop) {
        return;
    }

    target->route_index =
        route_index;
    target->route_control =
        route_index >= 0;

    target_update_props(
        target,
        &prop->value
    );

    app_queue_refresh(
        target->app
    );
}

static void device_info(
        void *data,
        const struct pw_device_info *info) {
    (void)info;

    AudioTarget *target = data;

    if (!target ||
            !target->device) {
        return;
    }

    const int rc =
        pw_device_enum_params(
            target->device,
            400,
            SPA_PARAM_Route,
            0,
            UINT32_MAX,
            NULL
        );

    if (rc >= 0) {
        app_queue_refresh(
            target->app
        );
    }
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .info = device_info,
    .param = device_param,
};

static bool target_start_route_device(
        AudioTarget *target) {
    if (!target ||
            !target->app ||
            !target->prefers_route ||
            target->device_id == UINT32_MAX ||
            target->route_device < 0) {
        return false;
    }

    if (target->device) {
        return true;
    }

    AudioApp *app =
        target->app;

    target->device =
        pw_registry_bind(
            app->registry,
            target->device_id,
            PW_TYPE_INTERFACE_Device,
            PW_VERSION_DEVICE,
            0
        );

    if (!target->device) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s device-bind id=%u\n",
            kind_name(target->kind),
            target->device_id
        );
        return false;
    }

    pw_device_add_listener(
        target->device,
        &target->device_listener,
        &device_events,
        target
    );
    target->device_listener_live = true;

    const int route_enum_rc =
        pw_device_enum_params(
            target->device,
            401,
            SPA_PARAM_Route,
            0,
            UINT32_MAX,
            NULL
        );

    if (route_enum_rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s route-enum=%s\n",
            kind_name(target->kind),
            spa_strerror(route_enum_rc)
        );
        spa_hook_remove(
            &target->device_listener
        );
        target->device_listener_live = false;
        pw_proxy_destroy(
            (struct pw_proxy *)target->device
        );
        target->device = NULL;
        return false;
    }

    if (!app->ready) {
        const int route_sync =
            pw_core_sync(
                app->core,
                PW_ID_CORE,
                0
            );

        if (route_sync < 0) {
            fprintf(
                stderr,
                "AUDIO_FAIL %s route-sync=%s\n",
                kind_name(target->kind),
                spa_strerror(route_sync)
            );
            return false;
        }

        app->targets_seq =
            route_sync;
    }

    return true;
}

static bool target_bind(
        AudioTarget *target) {
    if (!target ||
            !target->app) {
        return false;
    }

    AudioApp *app =
        target->app;

    target_runtime_clear(target);

    AudioNodeRecord *record =
        node_record_by_name(
            app,
            target->default_name,
            target->kind
        );

    if (!record) {
        return true;
    }

    target->node_id =
        record->id;
    target->device_id =
        record->device_id;
    target->route_device =
        record->route_device;
    target->prefers_route =
        record->device_id != UINT32_MAX &&
        record->route_device >= 0;

    target->name =
        g_strdup(record->name);
    target->description =
        g_strdup(
            record->description
                ? record->description
                : record->name
        );

    if (!target->name ||
            !target->description) {
        return false;
    }

    target->node =
        pw_registry_bind(
            app->registry,
            record->id,
            PW_TYPE_INTERFACE_Node,
            PW_VERSION_NODE,
            0
        );

    if (!target->node) {
        return false;
    }

    pw_node_add_listener(
        target->node,
        &target->node_listener,
        &node_events,
        target
    );
    target->node_listener_live = true;

    const int node_enum_rc =
        pw_node_enum_params(
            target->node,
            301,
            SPA_PARAM_Props,
            0,
            UINT32_MAX,
            NULL
        );

    if (node_enum_rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s node-enum=%s\n",
            kind_name(target->kind),
            spa_strerror(node_enum_rc)
        );
        return false;
    }

    if (!target->prefers_route) {
        return true;
    }

    target->device =
        pw_registry_bind(
            app->registry,
            target->device_id,
            PW_TYPE_INTERFACE_Device,
            PW_VERSION_DEVICE,
            0
        );

    if (!target->device) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s device-bind id=%u\n",
            kind_name(target->kind),
            target->device_id
        );
        return false;
    }

    pw_device_add_listener(
        target->device,
        &target->device_listener,
        &device_events,
        target
    );
    target->device_listener_live = true;

    const int route_enum_rc =
        pw_device_enum_params(
            target->device,
            401,
            SPA_PARAM_Route,
            0,
            UINT32_MAX,
            NULL
        );

    if (route_enum_rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s route-enum=%s\n",
            kind_name(target->kind),
            spa_strerror(route_enum_rc)
        );
        return false;
    }

    return true;
}

static char *metadata_default_name(
        const char *value) {
    if (!value) {
        return NULL;
    }

    JsonParser *parser =
        json_parser_new();

    if (!parser) {
        return NULL;
    }

    if (!json_parser_load_from_data(
            parser,
            value,
            -1,
            NULL)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root =
        json_parser_get_root(parser);

    if (!root ||
            !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *object =
        json_node_get_object(root);

    const char *name = NULL;

    if (json_object_has_member(
            object,
            "name")) {
        name =
            json_object_get_string_member(
                object,
                "name"
            );
    }

    char *result =
        name
            ? g_strdup(name)
            : NULL;

    g_object_unref(parser);

    return result;
}

static void update_default_name(
        AudioApp *app,
        AudioTarget *target,
        const char *value) {
    g_autofree char *name =
        metadata_default_name(value);

    if (g_strcmp0(
            target->default_name,
            name) == 0) {
        return;
    }

    g_free(target->default_name);
    target->default_name =
        g_strdup(name);

    if (!app->ready) {
        return;
    }

    if (!target_bind(target)) {
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    app_queue_refresh(app);
}

static int metadata_property(
        void *data,
        uint32_t subject,
        const char *key,
        const char *type,
        const char *value) {
    (void)type;

    AudioApp *app = data;

    if (subject != PW_ID_CORE ||
            !key) {
        return 0;
    }

    if (strcmp(
            key,
            "default.audio.sink") == 0) {
        update_default_name(
            app,
            &app->sink,
            value
        );
    } else if (strcmp(
            key,
            "default.audio.source") == 0) {
        update_default_name(
            app,
            &app->source,
            value
        );
    }

    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

static void json_add_target(
        JsonBuilder *builder,
        const AudioTarget *target) {
    json_builder_begin_object(builder);

    json_builder_set_member_name(
        builder,
        "available"
    );
    json_builder_add_boolean_value(
        builder,
        target_available(target)
    );

    json_builder_set_member_name(
        builder,
        "name"
    );

    if (target->name) {
        json_builder_add_string_value(
            builder,
            target->name
        );
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_set_member_name(
        builder,
        "description"
    );

    if (target->description) {
        json_builder_add_string_value(
            builder,
            target->description
        );
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_set_member_name(
        builder,
        "volume"
    );
    json_builder_add_double_value(
        builder,
        target->have_volume
            ? target->volume
            : 0.0
    );

    json_builder_set_member_name(
        builder,
        "muted"
    );
    json_builder_add_boolean_value(
        builder,
        target->have_mute &&
            target->muted
    );

    json_builder_set_member_name(
        builder,
        "backend"
    );
    json_builder_add_string_value(
        builder,
        target_backend(target)
    );

    g_autofree char *display = NULL;

    if (!target_available(target)) {
        display =
            g_strdup_printf(
                "%s --",
                kind_display_prefix(
                    target->kind
                )
            );
    } else if (target->muted) {
        display =
            g_strdup_printf(
                "%s MUTE",
                kind_display_prefix(
                    target->kind
                )
            );
    } else {
        const int percent =
            (int)(
                target->volume *
                100.0 +
                0.5
            );

        display =
            g_strdup_printf(
                "%s %d%%",
                kind_display_prefix(
                    target->kind
                ),
                percent
            );
    }

    json_builder_set_member_name(
        builder,
        "display"
    );
    json_builder_add_string_value(
        builder,
        display
            ? display
            : "AUDIO --"
    );

    json_builder_end_object(builder);
}

static char *audio_json(
        const AudioApp *app) {
    JsonBuilder *builder =
        json_builder_new();

    if (!builder) {
        return NULL;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(
        builder,
        "sink"
    );
    json_add_target(
        builder,
        &app->sink
    );

    json_builder_set_member_name(
        builder,
        "source"
    );
    json_add_target(
        builder,
        &app->source
    );

    json_builder_end_object(builder);

    JsonNode *root =
        json_builder_get_root(builder);
    JsonGenerator *generator =
        json_generator_new();

    if (!root ||
            !generator) {
        if (root) {
            json_node_free(root);
        }

        if (generator) {
            g_object_unref(generator);
        }

        g_object_unref(builder);
        return NULL;
    }

    json_generator_set_root(
        generator,
        root
    );

    char *json =
        json_generator_to_data(
            generator,
            NULL
        );

    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);

    return json;
}

static bool app_emit_json(
        AudioApp *app) {
    g_autofree char *json =
        audio_json(app);

    if (!json) {
        fprintf(
            stderr,
            "AUDIO_FAIL serialize\n"
        );
        return false;
    }

    if (g_strcmp0(
            json,
            app->last_json) == 0) {
        return true;
    }

    if (fputs(
            json,
            stdout) == EOF ||
        fputc(
            '\n',
            stdout) == EOF ||
        fflush(stdout) == EOF) {
        if (errno == EPIPE) {
            app->broken_pipe = true;
            pw_main_loop_quit(app->loop);
            return true;
        }

        fprintf(
            stderr,
            "AUDIO_FAIL stdout=%s\n",
            strerror(errno)
        );
        return false;
    }

    g_free(app->last_json);
    app->last_json =
        g_strdup(json);

    return app->last_json != NULL;
}

static void app_queue_refresh(
        AudioApp *app) {
    if (!app ||
            !app->ready ||
            app->mode != AUDIO_MODE_WATCH) {
        return;
    }

    if (app->refresh_seq >= 0) {
        app->refresh_again = true;
        return;
    }

    app->refresh_seq =
        pw_core_sync(
            app->core,
            PW_ID_CORE,
            0
        );

    if (app->refresh_seq < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL refresh-sync=%s\n",
            spa_strerror(
                app->refresh_seq
            )
        );
        app->failed = true;
        pw_main_loop_quit(app->loop);
    }
}

static bool target_write(
        AudioApp *app,
        AudioTarget *target,
        double volume,
        bool muted) {
    if (!target_available(target) ||
            target->channels == 0 ||
            target->channels >
                AUDIO_MAX_CHANNELS) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s unavailable-for-write\n",
            kind_name(target->kind)
        );
        return false;
    }

    volume =
        clamp_volume(volume);

    float volumes[AUDIO_MAX_CHANNELS] = {0};
    const float raw =
        raw_from_volume(volume);

    for (uint32_t i = 0;
            i < target->channels;
            i++) {
        volumes[i] = raw;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(
            buffer,
            sizeof(buffer)
        );

    struct spa_pod *props =
        spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_Props,
            SPA_PARAM_Props,
            SPA_PROP_channelVolumes,
            SPA_POD_Array(
                sizeof(float),
                SPA_TYPE_Float,
                target->channels,
                volumes
            ),
            SPA_PROP_mute,
            SPA_POD_Bool(muted)
        );

    if (!props) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s build-props\n",
            kind_name(target->kind)
        );
        return false;
    }

    int rc = -EINVAL;

    if (target->prefers_route) {
        if (!target->route_control ||
                !target->device ||
                target->route_index < 0) {
            fprintf(
                stderr,
                "AUDIO_FAIL %s route-not-ready\n",
                kind_name(target->kind)
            );
            return false;
        }

        struct spa_pod *route =
            spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamRoute,
                SPA_PARAM_Route,
                SPA_PARAM_ROUTE_device,
                SPA_POD_Int(
                    target->route_device
                ),
                SPA_PARAM_ROUTE_index,
                SPA_POD_Int(
                    target->route_index
                ),
                SPA_PARAM_ROUTE_props,
                SPA_POD_PodObject(props),
                SPA_PARAM_ROUTE_save,
                SPA_POD_Bool(true)
            );

        if (!route) {
            fprintf(
                stderr,
                "AUDIO_FAIL %s build-route\n",
                kind_name(target->kind)
            );
            return false;
        }

        rc =
            pw_device_set_param(
                target->device,
                SPA_PARAM_Route,
                0,
                route
            );
    } else {
        rc =
            pw_node_set_param(
                target->node,
                SPA_PARAM_Props,
                0,
                props
            );
    }

    if (rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s write=%s\n",
            kind_name(target->kind),
            spa_strerror(rc)
        );
        return false;
    }

    app->action_result_volume =
        volume;
    app->action_result_muted =
        muted;

    app->action_seq =
        pw_core_sync(
            app->core,
            PW_ID_CORE,
            0
        );

    if (app->action_seq < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL action-sync=%s\n",
            spa_strerror(
                app->action_seq
            )
        );
        return false;
    }

    return true;
}

static bool app_execute_action(
        AudioApp *app) {
    AudioTarget *target =
        app->action_kind ==
            AUDIO_KIND_SINK
            ? &app->sink
            : &app->source;

    if (!target_available(target)) {
        fprintf(
            stderr,
            "AUDIO_FAIL %s unavailable\n",
            kind_name(
                app->action_kind
            )
        );
        return false;
    }

    double volume =
        target->volume;
    bool muted =
        target->muted;

    switch (app->action) {
    case AUDIO_ACTION_VOLUME_SET:
        volume =
            clamp_volume(
                app->action_value
            );
        break;

    case AUDIO_ACTION_VOLUME_CHANGE:
        volume =
            clamp_volume(
                target->volume +
                app->action_value
            );
        break;

    case AUDIO_ACTION_MUTE_SET:
        muted =
            app->action_bool;
        break;

    case AUDIO_ACTION_MUTE_TOGGLE:
        muted =
            !target->muted;
        break;

    case AUDIO_ACTION_NONE:
        return false;
    }

    return target_write(
        app,
        target,
        volume,
        muted
    );
}

static bool app_bind_targets(
        AudioApp *app) {
    if (!target_bind(
            &app->sink)) {
        return false;
    }

    if (!target_bind(
            &app->source)) {
        return false;
    }

    return true;
}

static void app_start_targets(
        AudioApp *app) {
    if (app->targets_started ||
            !app->registry_done ||
            (
                app->metadata_bound &&
                !app->metadata_ready
            )) {
        return;
    }

    app->targets_started = true;

    if (!app_bind_targets(app)) {
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    app->targets_seq =
        pw_core_sync(
            app->core,
            PW_ID_CORE,
            0
        );

    if (app->targets_seq < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL targets-sync=%s\n",
            spa_strerror(
                app->targets_seq
            )
        );
        app->failed = true;
        pw_main_loop_quit(app->loop);
    }
}

static void core_done(
        void *data,
        uint32_t id,
        int seq) {
    AudioApp *app = data;

    if (id != PW_ID_CORE) {
        return;
    }

    if (seq == app->initial_seq) {
        app->registry_done = true;
        app_start_targets(app);
        return;
    }

    if (app->metadata_bound &&
            seq == app->metadata_seq) {
        app->metadata_ready = true;
        app_start_targets(app);
        return;
    }

    if (app->targets_started &&
            seq == app->targets_seq) {
        app->ready = true;

        if (app->mode ==
                AUDIO_MODE_ACTION) {
            if (!app_execute_action(app)) {
                app->failed = true;
                pw_main_loop_quit(
                    app->loop
                );
            }
            return;
        }

        if (!app_emit_json(app)) {
            app->failed = true;
            pw_main_loop_quit(app->loop);
            return;
        }

        if (app->mode ==
                AUDIO_MODE_STATE) {
            pw_main_loop_quit(app->loop);
        }

        return;
    }

    if (app->action_seq >= 0 &&
            seq == app->action_seq) {
        printf(
            "CONTROL_OK audio %s volume=%.3f muted=%s backend=%s\n",
            kind_name(
                app->action_kind
            ),
            app->action_result_volume,
            app->action_result_muted
                ? "on"
                : "off",
            target_backend(
                app->action_kind ==
                    AUDIO_KIND_SINK
                    ? &app->sink
                    : &app->source
            )
        );
        fflush(stdout);
        pw_main_loop_quit(app->loop);
        return;
    }

    if (app->refresh_seq >= 0 &&
            seq == app->refresh_seq) {
        app->refresh_seq = -1;

        if (!app_emit_json(app)) {
            app->failed = true;
            pw_main_loop_quit(app->loop);
            return;
        }

        if (app->refresh_again) {
            app->refresh_again = false;
            app_queue_refresh(app);
        }
    }
}

static void core_error(
        void *data,
        uint32_t id,
        int seq,
        int res,
        const char *message) {
    (void)id;
    (void)seq;

    AudioApp *app = data;

    fprintf(
        stderr,
        "AUDIO_FAIL core=%s (%d)\n",
        message
            ? message
            : spa_strerror(res),
        res
    );

    app->failed = true;

    if (app->loop) {
        pw_main_loop_quit(app->loop);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done,
    .error = core_error,
};

static void app_rebind_target_if_needed(
        AudioApp *app,
        AudioTarget *target,
        const AudioNodeRecord *record) {
    if (!app->ready ||
            !target->default_name ||
            !record ||
            g_strcmp0(
                target->default_name,
                record->name) != 0 ||
            g_strcmp0(
                kind_media_class(
                    target->kind
                ),
                record->media_class) != 0) {
        return;
    }

    if (!target_bind(target)) {
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    app_queue_refresh(app);
}

static void registry_global(
        void *data,
        uint32_t id,
        uint32_t permissions,
        const char *type,
        uint32_t version,
        const struct spa_dict *props) {
    (void)permissions;
    (void)version;

    AudioApp *app = data;

    if (!type) {
        return;
    }

    if (strcmp(
            type,
            PW_TYPE_INTERFACE_Metadata) == 0 &&
        !app->metadata) {
        const char *name =
            props
                ? spa_dict_lookup(
                    props,
                    PW_KEY_METADATA_NAME
                )
                : NULL;

        if (g_strcmp0(
                name,
                "default") == 0) {
            app->metadata =
                pw_registry_bind(
                    app->registry,
                    id,
                    type,
                    PW_VERSION_METADATA,
                    0
                );

            if (app->metadata) {
                app->metadata_id = id;
                app->metadata_bound = true;

                pw_metadata_add_listener(
                    app->metadata,
                    &app->metadata_listener,
                    &metadata_events,
                    app
                );
                app->metadata_listener_live =
                    true;

                app->metadata_seq =
                    pw_core_sync(
                        app->core,
                        PW_ID_CORE,
                        0
                    );

                if (app->metadata_seq < 0) {
                    app->failed = true;
                    pw_main_loop_quit(
                        app->loop
                    );
                }
            }
        }

        return;
    }

    if (strcmp(
            type,
            PW_TYPE_INTERFACE_Node) != 0 ||
        !props) {
        return;
    }

    const char *media_class =
        spa_dict_lookup(
            props,
            PW_KEY_MEDIA_CLASS
        );

    if (g_strcmp0(
            media_class,
            "Audio/Sink") != 0 &&
        g_strcmp0(
            media_class,
            "Audio/Source") != 0) {
        return;
    }

    AudioNodeRecord *record =
        node_record_by_id(
            app,
            id
        );

    if (!record) {
        record =
            node_record_allocate(app);
    }

    if (!record) {
        fprintf(
            stderr,
            "AUDIO_FAIL node-record-capacity\n"
        );
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    node_record_clear(record);

    record->used = true;
    record->id = id;
    record->device_id = UINT32_MAX;
    record->route_device = -1;

    record->name =
        g_strdup(
            spa_dict_lookup(
                props,
                PW_KEY_NODE_NAME
            )
        );
    record->description =
        g_strdup(
            spa_dict_lookup(
                props,
                PW_KEY_NODE_DESCRIPTION
            )
        );
    record->media_class =
        g_strdup(media_class);

    const char *device_text =
        spa_dict_lookup(
            props,
            "device.id"
        );

    uint32_t device_id = UINT32_MAX;

    if (parse_u32_text(
            device_text,
            &device_id)) {
        record->device_id =
            device_id;
    }

    const char *route_text =
        spa_dict_lookup(
            props,
            "card.profile.device"
        );

    int32_t route_device = -1;

    if (parse_i32_text(
            route_text,
            &route_device)) {
        record->route_device =
            route_device;
    }

    app_rebind_target_if_needed(
        app,
        &app->sink,
        record
    );
    app_rebind_target_if_needed(
        app,
        &app->source,
        record
    );
}

static void registry_global_remove(
        void *data,
        uint32_t id) {
    AudioApp *app = data;

    if (app->metadata &&
            id == app->metadata_id) {
        if (app->metadata_listener_live) {
            spa_hook_remove(
                &app->metadata_listener
            );
            app->metadata_listener_live =
                false;
        }

        pw_proxy_destroy(
            (struct pw_proxy *)app->metadata
        );
        app->metadata = NULL;
        app->metadata_bound = false;
        app->metadata_ready = false;

        g_clear_pointer(
            &app->sink.default_name,
            g_free
        );
        g_clear_pointer(
            &app->source.default_name,
            g_free
        );

        target_runtime_clear(
            &app->sink
        );
        target_runtime_clear(
            &app->source
        );

        app_queue_refresh(app);
    }

    if (app->sink.node_id == id ||
            app->sink.device_id == id) {
        target_runtime_clear(
            &app->sink
        );
        app_queue_refresh(app);
    }

    if (app->source.node_id == id ||
            app->source.device_id == id) {
        target_runtime_clear(
            &app->source
        );
        app_queue_refresh(app);
    }

    AudioNodeRecord *record =
        node_record_by_id(
            app,
            id
        );

    if (record) {
        node_record_clear(record);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void app_cleanup(
        AudioApp *app) {
    if (!app) {
        return;
    }

    target_runtime_clear(
        &app->sink
    );
    target_runtime_clear(
        &app->source
    );

    g_clear_pointer(
        &app->sink.default_name,
        g_free
    );
    g_clear_pointer(
        &app->source.default_name,
        g_free
    );

    if (app->metadata_listener_live) {
        spa_hook_remove(
            &app->metadata_listener
        );
        app->metadata_listener_live =
            false;
    }

    if (app->metadata) {
        pw_proxy_destroy(
            (struct pw_proxy *)app->metadata
        );
        app->metadata = NULL;
    }

    if (app->registry_listener_live) {
        spa_hook_remove(
            &app->registry_listener
        );
        app->registry_listener_live =
            false;
    }

    if (app->registry) {
        pw_proxy_destroy(
            (struct pw_proxy *)app->registry
        );
        app->registry = NULL;
    }

    if (app->core_listener_live) {
        spa_hook_remove(
            &app->core_listener
        );
        app->core_listener_live = false;
    }

    if (app->core) {
        pw_core_disconnect(app->core);
        app->core = NULL;
    }

    if (app->context) {
        pw_context_destroy(app->context);
        app->context = NULL;
    }

    if (app->loop) {
        pw_main_loop_destroy(app->loop);
        app->loop = NULL;
    }

    for (size_t i = 0;
            i < AUDIO_MAX_NODES;
            i++) {
        node_record_clear(
            &app->nodes[i]
        );
    }

    g_clear_pointer(
        &app->last_json,
        g_free
    );
}

static bool app_start_pipewire(
        AudioApp *app) {
    app->loop =
        pw_main_loop_new(NULL);

    if (!app->loop) {
        fprintf(
            stderr,
            "AUDIO_FAIL main-loop\n"
        );
        return false;
    }

    app->context =
        pw_context_new(
            pw_main_loop_get_loop(
                app->loop
            ),
            NULL,
            0
        );

    if (!app->context) {
        fprintf(
            stderr,
            "AUDIO_FAIL context\n"
        );
        return false;
    }

    app->core =
        pw_context_connect(
            app->context,
            NULL,
            0
        );

    if (!app->core) {
        fprintf(
            stderr,
            "AUDIO_FAIL connect\n"
        );
        return false;
    }

    pw_core_add_listener(
        app->core,
        &app->core_listener,
        &core_events,
        app
    );
    app->core_listener_live = true;

    app->registry =
        pw_core_get_registry(
            app->core,
            PW_VERSION_REGISTRY,
            0
        );

    if (!app->registry) {
        fprintf(
            stderr,
            "AUDIO_FAIL registry\n"
        );
        return false;
    }

    pw_registry_add_listener(
        app->registry,
        &app->registry_listener,
        &registry_events,
        app
    );
    app->registry_listener_live = true;

    app->initial_seq =
        pw_core_sync(
            app->core,
            PW_ID_CORE,
            0
        );

    if (app->initial_seq < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL initial-sync=%s\n",
            spa_strerror(
                app->initial_seq
            )
        );
        return false;
    }

    return true;
}

static void print_audio_usage(void) {
    fprintf(
        stderr,
        "audio usage:\n"
        "  audio state\n"
        "  audio watch\n"
        "  audio sink|source volume set VALUE\n"
        "  audio sink|source volume change DELTA\n"
        "  audio sink|source mute set on|off\n"
        "  audio sink|source mute toggle\n"
    );
}

static bool parse_kind(
        const char *text,
        AudioKind *kind_out) {
    if (g_strcmp0(
            text,
            "sink") == 0) {
        *kind_out =
            AUDIO_KIND_SINK;
        return true;
    }

    if (g_strcmp0(
            text,
            "source") == 0) {
        *kind_out =
            AUDIO_KIND_SOURCE;
        return true;
    }

    return false;
}

static bool parse_bool_word(
        const char *text,
        bool *value_out) {
    if (g_strcmp0(
            text,
            "on") == 0) {
        *value_out = true;
        return true;
    }

    if (g_strcmp0(
            text,
            "off") == 0) {
        *value_out = false;
        return true;
    }

    return false;
}

static bool parse_audio_cli(
        int argc,
        char **argv,
        AudioApp *app) {
    if (argc == 1 &&
            g_strcmp0(
                argv[0],
                "state") == 0) {
        app->mode =
            AUDIO_MODE_STATE;
        return true;
    }

    if (argc == 1 &&
            g_strcmp0(
                argv[0],
                "watch") == 0) {
        app->mode =
            AUDIO_MODE_WATCH;
        return true;
    }

    if (argc < 3 ||
            !parse_kind(
                argv[0],
                &app->action_kind)) {
        return false;
    }

    app->mode =
        AUDIO_MODE_ACTION;

    if (g_strcmp0(
            argv[1],
            "volume") == 0 &&
        argc == 4) {
        if (!parse_double_text(
                argv[3],
                &app->action_value)) {
            return false;
        }

        if (g_strcmp0(
                argv[2],
                "set") == 0) {
            app->action =
                AUDIO_ACTION_VOLUME_SET;
            return true;
        }

        if (g_strcmp0(
                argv[2],
                "change") == 0) {
            app->action =
                AUDIO_ACTION_VOLUME_CHANGE;
            return true;
        }

        return false;
    }

    if (g_strcmp0(
            argv[1],
            "mute") == 0) {
        if (argc == 3 &&
                g_strcmp0(
                    argv[2],
                    "toggle") == 0) {
            app->action =
                AUDIO_ACTION_MUTE_TOGGLE;
            return true;
        }

        if (argc == 4 &&
                g_strcmp0(
                    argv[2],
                    "set") == 0 &&
                parse_bool_word(
                    argv[3],
                    &app->action_bool)) {
            app->action =
                AUDIO_ACTION_MUTE_SET;
            return true;
        }
    }

    return false;
}

int onyrion_audio_cli(
        int argc,
        char **argv) {
    if (onyrion_audio_extended_handles(argc, argv)) {
        return onyrion_audio_extended_cli(argc, argv);
    }

    AudioApp app = {
        .metadata_id = UINT32_MAX,
        .initial_seq = -1,
        .metadata_seq = -1,
        .targets_seq = -1,
        .refresh_seq = -1,
        .action_seq = -1,
    };

    app.sink.app = &app;
    app.sink.kind =
        AUDIO_KIND_SINK;
    app.source.app = &app;
    app.source.kind =
        AUDIO_KIND_SOURCE;

    target_runtime_clear(
        &app.sink
    );
    target_runtime_clear(
        &app.source
    );

    if (!parse_audio_cli(
            argc,
            argv,
            &app)) {
        print_audio_usage();
        app_cleanup(&app);
        return 2;
    }

    int pw_argc = 1;
    char program[] =
        "onyrion-control";
    char *pw_argv_storage[] = {
        program,
        NULL,
    };
    char **pw_argv =
        pw_argv_storage;

    pw_init(
        &pw_argc,
        &pw_argv
    );

    if (!app_start_pipewire(
            &app)) {
        app_cleanup(&app);
        pw_deinit();
        return 1;
    }

    pw_main_loop_run(app.loop);

    const bool ok =
        !app.failed;

    app_cleanup(&app);
    pw_deinit();

    return ok
        ? 0
        : 1;
}
