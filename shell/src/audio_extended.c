#include "audio_extended.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#include <pipewire/extensions/metadata.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>

#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

#define EXT_MAX_NODES 256
#define EXT_MAX_CHANNELS 64

typedef enum ext_mode {
    EXT_MODE_DEVICES_STATE,
    EXT_MODE_DEVICES_WATCH,
    EXT_MODE_DEVICE_SELECT,
    EXT_MODE_STREAMS_STATE,
    EXT_MODE_STREAMS_WATCH,
    EXT_MODE_STREAM_ACTION,
} ExtMode;

typedef enum ext_kind {
    EXT_KIND_SINK,
    EXT_KIND_SOURCE,
    EXT_KIND_PLAYBACK,
    EXT_KIND_CAPTURE,
} ExtKind;

typedef enum ext_action {
    EXT_ACTION_NONE,
    EXT_ACTION_VOLUME_SET,
    EXT_ACTION_VOLUME_CHANGE,
    EXT_ACTION_MUTE_SET,
    EXT_ACTION_MUTE_TOGGLE,
} ExtAction;

struct ext_app;

typedef struct ext_node {
    struct ext_app *app;
    bool used;
    uint32_t id;
    uint64_t serial;
    char *media_class;
    char *name;
    char *description;
    char *application;

    struct pw_node *node;
    struct spa_hook listener;
    bool listener_live;

    bool have_volume;
    bool have_mute;
    uint32_t channels;
    double volume;
    bool muted;
} ExtNode;

typedef struct ext_app {
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

    ExtNode nodes[EXT_MAX_NODES];

    char *default_sink;
    char *default_source;
    char *configured_sink;
    char *configured_source;

    ExtMode mode;
    ExtKind kind;
    ExtAction action;
    char *select_name;
    uint64_t stream_serial;
    double action_value;
    bool action_bool;

    int initial_seq;
    int collect_seq;
    int action_seq;
    bool failed;
    bool ready;
    char *last_json;

    double action_result_volume;
    bool action_result_muted;
} ExtApp;

static void emit_watch_state(ExtApp *app);

static const char *kind_media_class(
        ExtKind kind) {
    switch (kind) {
    case EXT_KIND_SINK:
        return "Audio/Sink";
    case EXT_KIND_SOURCE:
        return "Audio/Source";
    case EXT_KIND_PLAYBACK:
        return "Stream/Output/Audio";
    case EXT_KIND_CAPTURE:
        return "Stream/Input/Audio";
    }

    return "";
}

static const char *kind_word(
        ExtKind kind) {
    switch (kind) {
    case EXT_KIND_SINK:
        return "sink";
    case EXT_KIND_SOURCE:
        return "source";
    case EXT_KIND_PLAYBACK:
        return "playback";
    case EXT_KIND_CAPTURE:
        return "capture";
    }

    return "unknown";
}

static bool is_stream_kind(
        ExtKind kind) {
    return kind == EXT_KIND_PLAYBACK ||
        kind == EXT_KIND_CAPTURE;
}

static bool mode_is_devices(
        ExtMode mode) {
    return mode == EXT_MODE_DEVICES_STATE ||
        mode == EXT_MODE_DEVICES_WATCH;
}

static bool mode_is_watch(
        ExtMode mode) {
    return mode == EXT_MODE_DEVICES_WATCH ||
        mode == EXT_MODE_STREAMS_WATCH;
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
    return (float)(volume * volume * volume);
}

static double clamp_volume(
        double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static bool parse_double_text(
        const char *text,
        double *out) {
    if (!text || !text[0] || !out) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const double value =
        g_ascii_strtod(text, &end);

    if (errno != 0 ||
            !end ||
            *end != '\0') {
        return false;
    }

    *out = value;
    return true;
}

static bool parse_u64_text(
        const char *text,
        uint64_t *out) {
    if (!text || !text[0] || !out) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long long value =
        strtoull(text, &end, 10);

    if (errno != 0 ||
            !end ||
            *end != '\0') {
        return false;
    }

    *out = (uint64_t)value;
    return true;
}

static bool parse_bool_word(
        const char *text,
        bool *out) {
    if (g_strcmp0(text, "on") == 0) {
        *out = true;
        return true;
    }
    if (g_strcmp0(text, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_device_kind(
        const char *text,
        ExtKind *out) {
    if (g_strcmp0(text, "sink") == 0) {
        *out = EXT_KIND_SINK;
        return true;
    }
    if (g_strcmp0(text, "source") == 0) {
        *out = EXT_KIND_SOURCE;
        return true;
    }
    return false;
}

static bool parse_stream_kind(
        const char *text,
        ExtKind *out) {
    if (g_strcmp0(text, "playback") == 0) {
        *out = EXT_KIND_PLAYBACK;
        return true;
    }
    if (g_strcmp0(text, "capture") == 0) {
        *out = EXT_KIND_CAPTURE;
        return true;
    }
    return false;
}

static void node_clear(
        ExtNode *node) {
    if (!node) {
        return;
    }

    if (node->listener_live) {
        spa_hook_remove(&node->listener);
        node->listener_live = false;
    }

    if (node->node) {
        pw_proxy_destroy(
            (struct pw_proxy *)node->node
        );
        node->node = NULL;
    }

    g_free(node->media_class);
    g_free(node->name);
    g_free(node->description);
    g_free(node->application);
    memset(node, 0, sizeof(*node));
}

static ExtNode *node_by_id(
        ExtApp *app,
        uint32_t id) {
    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        if (app->nodes[i].used &&
                app->nodes[i].id == id) {
            return &app->nodes[i];
        }
    }
    return NULL;
}

static ExtNode *node_alloc(
        ExtApp *app) {
    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        if (!app->nodes[i].used) {
            return &app->nodes[i];
        }
    }
    return NULL;
}

static ExtNode *find_device(
        ExtApp *app,
        ExtKind kind,
        const char *name) {
    const char *media_class =
        kind_media_class(kind);

    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        ExtNode *node = &app->nodes[i];
        if (node->used &&
                g_strcmp0(node->media_class, media_class) == 0 &&
                g_strcmp0(node->name, name) == 0) {
            return node;
        }
    }
    return NULL;
}

static ExtNode *find_stream(
        ExtApp *app,
        ExtKind kind,
        uint64_t serial) {
    const char *media_class =
        kind_media_class(kind);

    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        ExtNode *node = &app->nodes[i];
        if (node->used &&
                node->serial == serial &&
                g_strcmp0(node->media_class, media_class) == 0) {
            return node;
        }
    }
    return NULL;
}

static void update_stream_props(
        ExtNode *node,
        const struct spa_pod *props) {
    if (!node || !props) {
        return;
    }

    const struct spa_pod_prop *volume_prop =
        spa_pod_find_prop(
            props,
            NULL,
            SPA_PROP_channelVolumes
        );

    if (volume_prop) {
        float volumes[EXT_MAX_CHANNELS] = {0};
        const uint32_t count =
            spa_pod_copy_array(
                &volume_prop->value,
                SPA_TYPE_Float,
                volumes,
                EXT_MAX_CHANNELS
            );

        if (count > 0 && count <= EXT_MAX_CHANNELS) {
            node->channels = count;
            node->volume = volume_from_raw(volumes[0]);
            node->have_volume = true;
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
            node->muted = muted;
            node->have_mute = true;
        }
    }
}

static void stream_param(
        void *data,
        int seq,
        uint32_t id,
        uint32_t index,
        uint32_t next,
        const struct spa_pod *param) {
    (void)seq;
    (void)index;
    (void)next;

    ExtNode *node = data;
    if (id == SPA_PARAM_Props && param) {
        update_stream_props(node, param);
        if (node->app && node->app->ready &&
                node->app->mode == EXT_MODE_STREAMS_WATCH) {
            emit_watch_state(node->app);
        }
    }
}

static const struct pw_node_events stream_node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = stream_param,
};

static char *metadata_name(
        const char *value) {
    if (!value) {
        return NULL;
    }

    JsonParser *parser = json_parser_new();
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

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *object = json_node_get_object(root);
    const char *name =
        json_object_has_member(object, "name")
            ? json_object_get_string_member(object, "name")
            : NULL;
    char *copy = name ? g_strdup(name) : NULL;
    g_object_unref(parser);
    return copy;
}

static void replace_metadata_name(
        char **slot,
        const char *value) {
    g_autofree char *name = metadata_name(value);
    g_free(*slot);
    *slot = name ? g_strdup(name) : NULL;
}

static int metadata_property(
        void *data,
        uint32_t subject,
        const char *key,
        const char *type,
        const char *value) {
    (void)type;
    ExtApp *app = data;
    bool relevant = false;

    if (subject != PW_ID_CORE || !key) {
        return 0;
    }

    if (strcmp(key, "default.audio.sink") == 0) {
        replace_metadata_name(&app->default_sink, value);
        relevant = true;
    } else if (strcmp(key, "default.audio.source") == 0) {
        replace_metadata_name(&app->default_source, value);
        relevant = true;
    } else if (strcmp(key, "default.configured.audio.sink") == 0) {
        replace_metadata_name(&app->configured_sink, value);
        relevant = true;
    } else if (strcmp(key, "default.configured.audio.source") == 0) {
        replace_metadata_name(&app->configured_source, value);
        relevant = true;
    }

    if (relevant && app->ready &&
            app->mode == EXT_MODE_DEVICES_WATCH) {
        emit_watch_state(app);
    }

    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_property,
};

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

static void json_add_device_list(
        JsonBuilder *builder,
        const ExtApp *app,
        ExtKind kind,
        const char *current,
        const char *configured) {
    const char *media_class = kind_media_class(kind);
    json_builder_begin_array(builder);

    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        const ExtNode *node = &app->nodes[i];
        if (!node->used ||
                g_strcmp0(node->media_class, media_class) != 0) {
            continue;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_int_value(builder, node->id);
        json_builder_set_member_name(builder, "serial");
        json_builder_add_int_value(builder, (gint64)node->serial);
        json_add_nullable_string(builder, "name", node->name);
        json_add_nullable_string(builder, "description", node->description);
        json_builder_set_member_name(builder, "default");
        json_builder_add_boolean_value(
            builder,
            node->name && g_strcmp0(node->name, current) == 0
        );
        json_builder_set_member_name(builder, "configured");
        json_builder_add_boolean_value(
            builder,
            node->name && g_strcmp0(node->name, configured) == 0
        );
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
}

static void json_add_stream_list(
        JsonBuilder *builder,
        const ExtApp *app,
        ExtKind kind) {
    const char *media_class = kind_media_class(kind);
    json_builder_begin_array(builder);

    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        const ExtNode *node = &app->nodes[i];
        if (!node->used ||
                !node->node ||
                !node->have_volume ||
                !node->have_mute ||
                g_strcmp0(node->media_class, media_class) != 0) {
            continue;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_int_value(builder, node->id);
        json_builder_set_member_name(builder, "serial");
        json_builder_add_int_value(builder, (gint64)node->serial);
        json_add_nullable_string(builder, "application", node->application);
        json_add_nullable_string(builder, "name", node->name);
        json_add_nullable_string(builder, "description", node->description);
        json_builder_set_member_name(builder, "available");
        json_builder_add_boolean_value(
            builder,
            node->node && node->have_volume
        );
        json_builder_set_member_name(builder, "volume");
        json_builder_add_double_value(
            builder,
            node->have_volume ? node->volume : 0.0
        );
        json_builder_set_member_name(builder, "muted");
        json_builder_add_boolean_value(
            builder,
            node->have_mute && node->muted
        );
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
}

static char *build_state_json(
        const ExtApp *app) {
    JsonBuilder *builder = json_builder_new();
    JsonGenerator *generator = NULL;
    JsonNode *root = NULL;
    char *json = NULL;

    if (!builder) {
        return NULL;
    }

    json_builder_begin_object(builder);

    if (mode_is_devices(app->mode)) {
        json_add_nullable_string(builder, "default_sink", app->default_sink);
        json_add_nullable_string(builder, "default_source", app->default_source);
        json_add_nullable_string(builder, "configured_sink", app->configured_sink);
        json_add_nullable_string(builder, "configured_source", app->configured_source);

        json_builder_set_member_name(builder, "outputs");
        json_add_device_list(
            builder,
            app,
            EXT_KIND_SINK,
            app->default_sink,
            app->configured_sink
        );

        json_builder_set_member_name(builder, "inputs");
        json_add_device_list(
            builder,
            app,
            EXT_KIND_SOURCE,
            app->default_source,
            app->configured_source
        );
    } else {
        json_builder_set_member_name(builder, "playback");
        json_add_stream_list(builder, app, EXT_KIND_PLAYBACK);
        json_builder_set_member_name(builder, "capture");
        json_add_stream_list(builder, app, EXT_KIND_CAPTURE);
    }

    json_builder_end_object(builder);
    root = json_builder_get_root(builder);
    generator = json_generator_new();

    if (root && generator) {
        json_generator_set_root(generator, root);
        json = json_generator_to_data(generator, NULL);
    }

    if (generator) {
        g_object_unref(generator);
    }
    if (root) {
        json_node_free(root);
    }
    g_object_unref(builder);
    return json;
}

static void emit_watch_state(
        ExtApp *app) {
    if (!app || !app->ready || !mode_is_watch(app->mode)) {
        return;
    }

    g_autofree char *json = build_state_json(app);
    if (!json) {
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    if (g_strcmp0(json, app->last_json) == 0) {
        return;
    }

    g_free(app->last_json);
    app->last_json = g_strdup(json);
    if (!app->last_json) {
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    puts(json);
    fflush(stdout);
}

static bool write_stream_props(
        ExtApp *app,
        ExtNode *node,
        double volume,
        bool muted) {
    if (!node || !node->node ||
            !node->have_volume ||
            node->channels == 0 ||
            node->channels > EXT_MAX_CHANNELS) {
        fprintf(stderr, "AUDIO_FAIL stream unavailable-for-write\n");
        return false;
    }

    volume = clamp_volume(volume);
    float values[EXT_MAX_CHANNELS] = {0};
    const float raw = raw_from_volume(volume);
    for (uint32_t i = 0; i < node->channels; i++) {
        values[i] = raw;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod *props =
        spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_Props,
            SPA_PARAM_Props,
            SPA_PROP_channelVolumes,
            SPA_POD_Array(
                sizeof(float),
                SPA_TYPE_Float,
                node->channels,
                values
            ),
            SPA_PROP_mute,
            SPA_POD_Bool(muted)
        );

    if (!props) {
        fprintf(stderr, "AUDIO_FAIL stream build-props\n");
        return false;
    }

    const int rc =
        pw_node_set_param(
            node->node,
            SPA_PARAM_Props,
            0,
            props
        );

    if (rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL stream write=%s\n",
            spa_strerror(rc)
        );
        return false;
    }

    app->action_result_volume = volume;
    app->action_result_muted = muted;
    app->action_seq = pw_core_sync(app->core, PW_ID_CORE, 0);
    return app->action_seq >= 0;
}

static bool execute_action(
        ExtApp *app) {
    if (app->mode == EXT_MODE_DEVICE_SELECT) {
        ExtNode *node =
            find_device(app, app->kind, app->select_name);
        if (!node || !app->metadata) {
            fprintf(stderr, "AUDIO_FAIL device target unavailable\n");
            return false;
        }

        const char *key =
            app->kind == EXT_KIND_SINK
                ? "default.configured.audio.sink"
                : "default.configured.audio.source";
        g_autofree char *value =
            g_strdup_printf(
                "{\"name\":\"%s\"}",
                node->name
            );

        const int rc =
            pw_metadata_set_property(
                app->metadata,
                PW_ID_CORE,
                key,
                "Spa:String:JSON",
                value
            );

        if (rc < 0) {
            fprintf(
                stderr,
                "AUDIO_FAIL device select=%s\n",
                spa_strerror(rc)
            );
            return false;
        }

        app->action_seq =
            pw_core_sync(app->core, PW_ID_CORE, 0);
        return app->action_seq >= 0;
    }

    ExtNode *node =
        find_stream(app, app->kind, app->stream_serial);
    if (!node || !node->have_volume) {
        fprintf(stderr, "AUDIO_FAIL stream target unavailable\n");
        return false;
    }

    double volume = node->volume;
    bool muted = node->muted;

    switch (app->action) {
    case EXT_ACTION_VOLUME_SET:
        volume = app->action_value;
        break;
    case EXT_ACTION_VOLUME_CHANGE:
        volume = node->volume + app->action_value;
        break;
    case EXT_ACTION_MUTE_SET:
        muted = app->action_bool;
        break;
    case EXT_ACTION_MUTE_TOGGLE:
        muted = !node->muted;
        break;
    case EXT_ACTION_NONE:
        return false;
    }

    return write_stream_props(app, node, volume, muted);
}

static void core_done(
        void *data,
        uint32_t id,
        int seq) {
    ExtApp *app = data;
    if (id != PW_ID_CORE) {
        return;
    }

    if (seq == app->initial_seq) {
        app->collect_seq =
            pw_core_sync(app->core, PW_ID_CORE, 0);
        if (app->collect_seq < 0) {
            app->failed = true;
            pw_main_loop_quit(app->loop);
        }
        return;
    }

    if (seq == app->collect_seq) {
        if (app->mode == EXT_MODE_DEVICES_STATE ||
                app->mode == EXT_MODE_STREAMS_STATE) {
            g_autofree char *json = build_state_json(app);
            if (!json) {
                app->failed = true;
            } else {
                puts(json);
            }
            pw_main_loop_quit(app->loop);
            return;
        }

        if (mode_is_watch(app->mode)) {
            app->ready = true;
            emit_watch_state(app);
            return;
        }

        if (!execute_action(app)) {
            app->failed = true;
            pw_main_loop_quit(app->loop);
        }
        return;
    }

    if (app->action_seq >= 0 && seq == app->action_seq) {
        if (app->mode == EXT_MODE_DEVICE_SELECT) {
            printf(
                "CONTROL_OK audio device %s selected=%s backend=wireplumber-default-metadata\n",
                kind_word(app->kind),
                app->select_name
            );
        } else {
            printf(
                "CONTROL_OK audio stream %s serial=%llu volume=%.3f muted=%s backend=node-props\n",
                kind_word(app->kind),
                (unsigned long long)app->stream_serial,
                app->action_result_volume,
                app->action_result_muted ? "on" : "off"
            );
        }
        fflush(stdout);
        pw_main_loop_quit(app->loop);
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
    ExtApp *app = data;
    fprintf(
        stderr,
        "AUDIO_FAIL core=%s (%d)\n",
        message ? message : spa_strerror(res),
        res
    );
    app->failed = true;
    pw_main_loop_quit(app->loop);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = core_done,
    .error = core_error,
};

static void registry_global(
        void *data,
        uint32_t id,
        uint32_t permissions,
        const char *type,
        uint32_t version,
        const struct spa_dict *props) {
    (void)permissions;
    (void)version;
    ExtApp *app = data;

    if (!type) {
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
            !app->metadata) {
        const char *name = props
            ? spa_dict_lookup(props, PW_KEY_METADATA_NAME)
            : NULL;
        if (g_strcmp0(name, "default") == 0) {
            app->metadata =
                pw_registry_bind(
                    app->registry,
                    id,
                    type,
                    PW_VERSION_METADATA,
                    0
                );
            if (app->metadata) {
                pw_metadata_add_listener(
                    app->metadata,
                    &app->metadata_listener,
                    &metadata_events,
                    app
                );
                app->metadata_listener_live = true;
            }
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) {
        return;
    }

    const char *media_class =
        spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    bool interesting = false;
    ExtKind kind = EXT_KIND_SINK;

    if (g_strcmp0(media_class, "Audio/Sink") == 0) {
        interesting = true;
        kind = EXT_KIND_SINK;
    } else if (g_strcmp0(media_class, "Audio/Source") == 0) {
        interesting = true;
        kind = EXT_KIND_SOURCE;
    } else if (g_strcmp0(media_class, "Stream/Output/Audio") == 0) {
        interesting = true;
        kind = EXT_KIND_PLAYBACK;
    } else if (g_strcmp0(media_class, "Stream/Input/Audio") == 0) {
        interesting = true;
        kind = EXT_KIND_CAPTURE;
    }

    if (!interesting) {
        return;
    }

    ExtNode *node = node_by_id(app, id);
    if (!node) {
        node = node_alloc(app);
    }
    if (!node) {
        fprintf(stderr, "AUDIO_FAIL extended node capacity\n");
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    node_clear(node);
    node->app = app;
    node->used = true;
    node->id = id;
    node->media_class = g_strdup(media_class);
    node->name = g_strdup(spa_dict_lookup(props, PW_KEY_NODE_NAME));
    node->description = g_strdup(spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION));
    node->application = g_strdup(spa_dict_lookup(props, PW_KEY_APP_NAME));

    uint64_t serial = 0;
    if (parse_u64_text(
            spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL),
            &serial)) {
        node->serial = serial;
    }

    if (!is_stream_kind(kind)) {
        if (app->ready && app->mode == EXT_MODE_DEVICES_WATCH) {
            emit_watch_state(app);
        }
        return;
    }

    node->node =
        pw_registry_bind(
            app->registry,
            id,
            PW_TYPE_INTERFACE_Node,
            PW_VERSION_NODE,
            0
        );
    if (!node->node) {
        fprintf(stderr, "AUDIO_FAIL stream bind id=%u\n", id);
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    pw_node_add_listener(
        node->node,
        &node->listener,
        &stream_node_events,
        node
    );
    node->listener_live = true;

    uint32_t subscribed_params[] = { SPA_PARAM_Props };
    const int subscribe_rc = pw_node_subscribe_params(
        node->node,
        subscribed_params,
        1
    );
    if (subscribe_rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL stream subscribe id=%u error=%s\n",
            id,
            spa_strerror(subscribe_rc)
        );
        app->failed = true;
        pw_main_loop_quit(app->loop);
        return;
    }

    const int rc = pw_node_enum_params(
        node->node,
        500,
        SPA_PARAM_Props,
        0,
        UINT32_MAX,
        NULL
    );
    if (rc < 0) {
        fprintf(
            stderr,
            "AUDIO_FAIL stream enum id=%u error=%s\n",
            id,
            spa_strerror(rc)
        );
        app->failed = true;
        pw_main_loop_quit(app->loop);
    }

}

static void registry_global_remove(
        void *data,
        uint32_t id) {
    ExtApp *app = data;
    ExtNode *node = node_by_id(app, id);
    if (!node) {
        return;
    }

    const bool was_device =
        g_strcmp0(node->media_class, "Audio/Sink") == 0 ||
        g_strcmp0(node->media_class, "Audio/Source") == 0;
    const bool was_stream =
        g_strcmp0(node->media_class, "Stream/Output/Audio") == 0 ||
        g_strcmp0(node->media_class, "Stream/Input/Audio") == 0;

    node_clear(node);

    if (app->ready && was_device &&
            app->mode == EXT_MODE_DEVICES_WATCH) {
        emit_watch_state(app);
    } else if (app->ready && was_stream &&
            app->mode == EXT_MODE_STREAMS_WATCH) {
        emit_watch_state(app);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool start_pipewire(
        ExtApp *app) {
    app->loop = pw_main_loop_new(NULL);
    if (!app->loop) {
        return false;
    }

    app->context =
        pw_context_new(
            pw_main_loop_get_loop(app->loop),
            NULL,
            0
        );
    if (!app->context) {
        return false;
    }

    app->core = pw_context_connect(app->context, NULL, 0);
    if (!app->core) {
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
        pw_core_sync(app->core, PW_ID_CORE, 0);
    return app->initial_seq >= 0;
}

static void cleanup(
        ExtApp *app) {
    for (size_t i = 0; i < EXT_MAX_NODES; i++) {
        node_clear(&app->nodes[i]);
    }

    g_clear_pointer(&app->default_sink, g_free);
    g_clear_pointer(&app->default_source, g_free);
    g_clear_pointer(&app->configured_sink, g_free);
    g_clear_pointer(&app->configured_source, g_free);
    g_clear_pointer(&app->select_name, g_free);
    g_clear_pointer(&app->last_json, g_free);

    if (app->metadata_listener_live) {
        spa_hook_remove(&app->metadata_listener);
        app->metadata_listener_live = false;
    }
    if (app->metadata) {
        pw_proxy_destroy((struct pw_proxy *)app->metadata);
        app->metadata = NULL;
    }
    if (app->registry_listener_live) {
        spa_hook_remove(&app->registry_listener);
        app->registry_listener_live = false;
    }
    if (app->registry) {
        pw_proxy_destroy((struct pw_proxy *)app->registry);
        app->registry = NULL;
    }
    if (app->core_listener_live) {
        spa_hook_remove(&app->core_listener);
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
}

static void usage(void) {
    fprintf(
        stderr,
        "audio extended usage:\n"
        "  audio devices state\n"
        "  audio devices watch\n"
        "  audio device sink|source select NODE_NAME\n"
        "  audio streams state\n"
        "  audio streams watch\n"
        "  audio stream playback|capture SERIAL volume set VALUE\n"
        "  audio stream playback|capture SERIAL volume change DELTA\n"
        "  audio stream playback|capture SERIAL mute set on|off\n"
        "  audio stream playback|capture SERIAL mute toggle\n"
    );
}

bool onyrion_audio_extended_handles(
        int argc,
        char **argv) {
    if (argc < 1 || !argv || !argv[0]) {
        return false;
    }

    return g_strcmp0(argv[0], "devices") == 0 ||
        g_strcmp0(argv[0], "device") == 0 ||
        g_strcmp0(argv[0], "streams") == 0 ||
        g_strcmp0(argv[0], "stream") == 0;
}

static bool parse_cli(
        int argc,
        char **argv,
        ExtApp *app) {
    if (argc == 2 &&
            g_strcmp0(argv[0], "devices") == 0 &&
            g_strcmp0(argv[1], "state") == 0) {
        app->mode = EXT_MODE_DEVICES_STATE;
        return true;
    }

    if (argc == 2 &&
            g_strcmp0(argv[0], "devices") == 0 &&
            g_strcmp0(argv[1], "watch") == 0) {
        app->mode = EXT_MODE_DEVICES_WATCH;
        return true;
    }

    if (argc == 4 &&
            g_strcmp0(argv[0], "device") == 0 &&
            parse_device_kind(argv[1], &app->kind) &&
            g_strcmp0(argv[2], "select") == 0) {
        app->mode = EXT_MODE_DEVICE_SELECT;
        app->select_name = g_strdup(argv[3]);
        return app->select_name != NULL;
    }

    if (argc == 2 &&
            g_strcmp0(argv[0], "streams") == 0 &&
            g_strcmp0(argv[1], "state") == 0) {
        app->mode = EXT_MODE_STREAMS_STATE;
        return true;
    }

    if (argc == 2 &&
            g_strcmp0(argv[0], "streams") == 0 &&
            g_strcmp0(argv[1], "watch") == 0) {
        app->mode = EXT_MODE_STREAMS_WATCH;
        return true;
    }

    if (argc < 5 ||
            g_strcmp0(argv[0], "stream") != 0 ||
            !parse_stream_kind(argv[1], &app->kind) ||
            !parse_u64_text(argv[2], &app->stream_serial)) {
        return false;
    }

    app->mode = EXT_MODE_STREAM_ACTION;

    if (g_strcmp0(argv[3], "volume") == 0 && argc == 6) {
        if (!parse_double_text(argv[5], &app->action_value)) {
            return false;
        }
        if (g_strcmp0(argv[4], "set") == 0) {
            app->action = EXT_ACTION_VOLUME_SET;
            return true;
        }
        if (g_strcmp0(argv[4], "change") == 0) {
            app->action = EXT_ACTION_VOLUME_CHANGE;
            return true;
        }
        return false;
    }

    if (g_strcmp0(argv[3], "mute") == 0) {
        if (argc == 5 && g_strcmp0(argv[4], "toggle") == 0) {
            app->action = EXT_ACTION_MUTE_TOGGLE;
            return true;
        }
        if (argc == 6 &&
                g_strcmp0(argv[4], "set") == 0 &&
                parse_bool_word(argv[5], &app->action_bool)) {
            app->action = EXT_ACTION_MUTE_SET;
            return true;
        }
    }

    return false;
}

int onyrion_audio_extended_cli(
        int argc,
        char **argv) {
    ExtApp app = {
        .initial_seq = -1,
        .collect_seq = -1,
        .action_seq = -1,
    };

    if (!parse_cli(argc, argv, &app)) {
        usage();
        cleanup(&app);
        return 2;
    }

    int pw_argc = 1;
    char program[] = "onyrion-control";
    char *pw_argv_storage[] = { program, NULL };
    char **pw_argv = pw_argv_storage;
    pw_init(&pw_argc, &pw_argv);

    if (!start_pipewire(&app)) {
        fprintf(stderr, "AUDIO_FAIL extended PipeWire start\n");
        cleanup(&app);
        pw_deinit();
        return 1;
    }

    pw_main_loop_run(app.loop);
    const bool ok = !app.failed;
    cleanup(&app);
    pw_deinit();
    return ok ? 0 : 1;
}
