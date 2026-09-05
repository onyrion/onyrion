#include "shell_config.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>
#include <kdl/parser.h>

typedef enum config_value_type {
    CONFIG_VALUE_STRING,
    CONFIG_VALUE_BOOL,
    CONFIG_VALUE_INTEGER,
} ConfigValueType;

typedef struct config_value {
    ConfigValueType type;
    union {
        char *string;
        bool boolean;
        long long integer;
    };
} ConfigValue;

typedef struct config_property {
    char *name;
    ConfigValue value;
} ConfigProperty;

typedef struct config_node {
    char *name;
    ConfigValue *arguments;
    size_t argument_count;
    ConfigProperty *properties;
    size_t property_count;
    struct config_node **children;
    size_t child_count;
    struct config_node *parent;
} ConfigNode;

static void config_value_finish(ConfigValue *value) {
    if (value->type == CONFIG_VALUE_STRING) {
        g_free(value->string);
    }
    *value = (ConfigValue){0};
}

static void config_node_finish(ConfigNode *node) {
    if (!node) {
        return;
    }

    g_free(node->name);
    for (size_t i = 0; i < node->argument_count; i++) {
        config_value_finish(&node->arguments[i]);
    }
    g_free(node->arguments);

    for (size_t i = 0; i < node->property_count; i++) {
        g_free(node->properties[i].name);
        config_value_finish(&node->properties[i].value);
    }
    g_free(node->properties);

    for (size_t i = 0; i < node->child_count; i++) {
        config_node_finish(node->children[i]);
        g_free(node->children[i]);
    }
    g_free(node->children);
    *node = (ConfigNode){0};
}

static char *copy_kdl_str(kdl_str value) {
    return g_strndup(value.data ? value.data : "", value.len);
}

static bool config_value_from_kdl(
        const kdl_value *source,
        ConfigValue *target,
        GError **error) {
    if (source->type_annotation.len != 0) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "KDL type annotations are not supported in shell.kdl"
        );
        return false;
    }

    switch (source->type) {
    case KDL_TYPE_STRING:
        *target = (ConfigValue){
            .type = CONFIG_VALUE_STRING,
            .string = copy_kdl_str(source->string),
        };
        return target->string != NULL;

    case KDL_TYPE_BOOLEAN:
        *target = (ConfigValue){
            .type = CONFIG_VALUE_BOOL,
            .boolean = source->boolean,
        };
        return true;

    case KDL_TYPE_NUMBER:
        if (source->number.type != KDL_NUMBER_TYPE_INTEGER) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "shell.kdl numeric values must be integers"
            );
            return false;
        }
        *target = (ConfigValue){
            .type = CONFIG_VALUE_INTEGER,
            .integer = source->number.integer,
        };
        return true;

    case KDL_TYPE_NULL:
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "null is not a supported shell.kdl value"
        );
        return false;
    }

    g_set_error_literal(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_INVAL,
        "unsupported shell.kdl scalar"
    );
    return false;
}

static const ConfigProperty *node_property(
        const ConfigNode *node,
        const char *name) {
    for (size_t i = 0; i < node->property_count; i++) {
        if (strcmp(node->properties[i].name, name) == 0) {
            return &node->properties[i];
        }
    }
    return NULL;
}

static bool append_argument(
        ConfigNode *node,
        const kdl_value *value,
        GError **error) {
    ConfigValue parsed = {0};
    if (!config_value_from_kdl(value, &parsed, error)) {
        return false;
    }

    node->arguments = g_realloc_n(
        node->arguments,
        node->argument_count + 1,
        sizeof(*node->arguments)
    );
    node->arguments[node->argument_count++] = parsed;
    return true;
}

static bool append_property(
        ConfigNode *node,
        kdl_str name,
        const kdl_value *value,
        GError **error) {
    g_autofree char *property_name = copy_kdl_str(name);
    if (!property_name || property_name[0] == '\0') {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "KDL property name must not be empty"
        );
        return false;
    }

    if (node_property(node, property_name)) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "duplicate property '%s' on node '%s'",
            property_name,
            node->name
        );
        return false;
    }

    ConfigValue parsed = {0};
    if (!config_value_from_kdl(value, &parsed, error)) {
        return false;
    }

    node->properties = g_realloc_n(
        node->properties,
        node->property_count + 1,
        sizeof(*node->properties)
    );
    node->properties[node->property_count] = (ConfigProperty){
        .name = g_steal_pointer(&property_name),
        .value = parsed,
    };
    node->property_count++;
    return true;
}

static void append_child(ConfigNode *parent, ConfigNode *child) {
    parent->children = g_realloc_n(
        parent->children,
        parent->child_count + 1,
        sizeof(*parent->children)
    );
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

static bool parse_kdl_tree(
        const char *contents,
        size_t length,
        ConfigNode *root,
        GError **error) {
    const kdl_str document = {
        .data = contents,
        .len = length,
    };
    kdl_parser *parser = kdl_create_string_parser(
        document,
        KDL_READ_VERSION_2
    );
    if (!parser) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_FAILED,
            "cannot create KDL 2 parser"
        );
        return false;
    }

    ConfigNode *current = root;
    bool ok = true;

    for (;;) {
        kdl_event_data *event = kdl_parser_next_event(parser);
        if (!event) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "KDL parser returned no event"
            );
            ok = false;
            break;
        }

        if (event->event == KDL_EVENT_EOF) {
            if (current != root) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "KDL document ended inside a node"
                );
                ok = false;
            }
            break;
        }

        if (event->event == KDL_EVENT_PARSE_ERROR) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "malformed KDL 2 document"
            );
            ok = false;
            break;
        }

        if (event->event == KDL_EVENT_START_NODE) {
            if (event->value.type_annotation.len != 0) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "KDL node type annotations are not supported"
                );
                ok = false;
                break;
            }

            ConfigNode *node = g_new0(ConfigNode, 1);
            node->name = copy_kdl_str(event->name);
            if (!node->name || node->name[0] == '\0') {
                config_node_finish(node);
                g_free(node);
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "KDL node name must not be empty"
                );
                ok = false;
                break;
            }

            append_child(current, node);
            current = node;
            continue;
        }

        if (event->event == KDL_EVENT_END_NODE) {
            if (current == root || !current->parent) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "unexpected KDL node end"
                );
                ok = false;
                break;
            }
            current = current->parent;
            continue;
        }

        if (current == root) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "KDL value appears outside a node"
            );
            ok = false;
            break;
        }

        if (event->event == KDL_EVENT_ARGUMENT) {
            ok = append_argument(current, &event->value, error);
        } else if (event->event == KDL_EVENT_PROPERTY) {
            ok = append_property(current, event->name, &event->value, error);
        } else {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "unexpected KDL parser event"
            );
            ok = false;
        }

        if (!ok) {
            break;
        }
    }

    kdl_destroy_parser(parser);
    return ok;
}

static bool allowed_name(
        const char *name,
        const char *const *allowed,
        size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool reject_unknown_properties(
        const ConfigNode *node,
        const char *const *allowed,
        size_t count,
        GError **error) {
    for (size_t i = 0; i < node->property_count; i++) {
        if (!allowed_name(node->properties[i].name, allowed, count)) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "unknown property '%s' on node '%s'",
                node->properties[i].name,
                node->name
            );
            return false;
        }
    }
    return true;
}

static bool require_argument_count(
        const ConfigNode *node,
        size_t expected,
        GError **error) {
    if (node->argument_count == expected) {
        return true;
    }
    g_set_error(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_INVAL,
        "node '%s' requires %zu argument(s), got %zu",
        node->name,
        expected,
        node->argument_count
    );
    return false;
}

static bool require_no_properties(
        const ConfigNode *node,
        GError **error) {
    if (node->property_count == 0) {
        return true;
    }
    g_set_error(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_INVAL,
        "node '%s' must not have properties",
        node->name
    );
    return false;
}

static bool require_no_children(
        const ConfigNode *node,
        GError **error) {
    if (node->child_count == 0) {
        return true;
    }
    g_set_error(
        error,
        G_FILE_ERROR,
        G_FILE_ERROR_INVAL,
        "node '%s' must not have child nodes",
        node->name
    );
    return false;
}

static bool value_string(
        const ConfigValue *value,
        const char *context,
        char **target,
        GError **error) {
    if (value->type != CONFIG_VALUE_STRING) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "%s must be a string",
            context
        );
        return false;
    }
    if (!value->string || value->string[0] == '\0') {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "%s must not be empty",
            context
        );
        return false;
    }
    *target = g_strdup(value->string);
    return *target != NULL;
}

static bool value_bool(
        const ConfigValue *value,
        const char *context,
        bool *target,
        GError **error) {
    if (value->type != CONFIG_VALUE_BOOL) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "%s must be a boolean",
            context
        );
        return false;
    }
    *target = value->boolean;
    return true;
}

static bool value_int(
        const ConfigValue *value,
        const char *context,
        int *target,
        GError **error) {
    if (value->type != CONFIG_VALUE_INTEGER ||
            value->integer < INT_MIN ||
            value->integer > INT_MAX) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "%s must be an integer in int range",
            context
        );
        return false;
    }
    *target = (int)value->integer;
    return true;
}

static bool value_unsigned(
        const ConfigValue *value,
        const char *context,
        unsigned *target,
        GError **error) {
    if (value->type != CONFIG_VALUE_INTEGER ||
            value->integer < 0 ||
            (unsigned long long)value->integer > UINT_MAX) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "%s must be an unsigned integer",
            context
        );
        return false;
    }
    *target = (unsigned)value->integer;
    return true;
}

static bool property_string(
        const ConfigNode *node,
        const char *name,
        bool required,
        char **target,
        GError **error) {
    const ConfigProperty *property = node_property(node, name);
    if (!property) {
        if (!required) {
            return true;
        }
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "node '%s' requires property '%s'",
            node->name,
            name
        );
        return false;
    }

    g_autofree char *context = g_strdup_printf("%s.%s", node->name, name);
    return value_string(&property->value, context, target, error);
}

static bool property_bool(
        const ConfigNode *node,
        const char *name,
        bool default_value,
        bool *target,
        GError **error) {
    const ConfigProperty *property = node_property(node, name);
    if (!property) {
        *target = default_value;
        return true;
    }

    g_autofree char *context = g_strdup_printf("%s.%s", node->name, name);
    return value_bool(&property->value, context, target, error);
}

static bool property_unsigned(
        const ConfigNode *node,
        const char *name,
        unsigned default_value,
        unsigned *target,
        GError **error) {
    const ConfigProperty *property = node_property(node, name);
    if (!property) {
        *target = default_value;
        return true;
    }

    g_autofree char *context = g_strdup_printf("%s.%s", node->name, name);
    return value_unsigned(&property->value, context, target, error);
}

static bool valid_token(const char *value) {
    if (!value || value[0] == '\0') {
        return false;
    }

    for (const unsigned char *cursor = (const unsigned char *)value;
            *cursor;
            cursor++) {
        const unsigned char c = *cursor;
        if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == ':' || c == '_' || c == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static bool parse_exec_node(
        const ConfigNode *node,
        char ***argv,
        size_t *argc,
        GError **error) {
    if (!require_no_properties(node, error) ||
            !require_no_children(node, error)) {
        return false;
    }
    if (node->argument_count == 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "node '%s' argv must not be empty",
            node->name
        );
        return false;
    }

    char **result = g_new0(char *, node->argument_count + 1);
    for (size_t i = 0; i < node->argument_count; i++) {
        g_autofree char *context = g_strdup_printf("%s argv[%zu]", node->name, i);
        if (!value_string(&node->arguments[i], context, &result[i], error)) {
            g_strfreev(result);
            return false;
        }
    }

    *argv = result;
    *argc = node->argument_count;
    return true;
}

static bool parse_single_int_node(
        const ConfigNode *node,
        int *target,
        GError **error) {
    return require_argument_count(node, 1, error) &&
        require_no_properties(node, error) &&
        require_no_children(node, error) &&
        value_int(&node->arguments[0], node->name, target, error);
}

static bool parse_single_bool_node(
        const ConfigNode *node,
        bool *target,
        GError **error) {
    return require_argument_count(node, 1, error) &&
        require_no_properties(node, error) &&
        require_no_children(node, error) &&
        value_bool(&node->arguments[0], node->name, target, error);
}

static bool parse_restart_node(
        const ConfigNode *node,
        ProviderConfig *provider,
        GError **error) {
    static const char *const allowed[] = {
        "policy",
        "delay-ms",
        "max-delay-ms",
    };

    if (!require_argument_count(node, 0, error) ||
            !require_no_children(node, error) ||
            !reject_unknown_properties(node, allowed, G_N_ELEMENTS(allowed), error)) {
        return false;
    }

    g_autofree char *policy = NULL;
    if (!property_string(node, "policy", true, &policy, error) ||
            !property_unsigned(node, "delay-ms", 1000, &provider->restart_delay_ms, error) ||
            !property_unsigned(node, "max-delay-ms", 30000, &provider->restart_max_delay_ms, error)) {
        return false;
    }

    if (strcmp(policy, "never") == 0) {
        provider->restart_policy = PROVIDER_RESTART_NEVER;
    } else if (strcmp(policy, "on-failure") == 0) {
        provider->restart_policy = PROVIDER_RESTART_ON_FAILURE;
    } else if (strcmp(policy, "always") == 0) {
        provider->restart_policy = PROVIDER_RESTART_ALWAYS;
    } else {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unsupported restart policy '%s'",
            policy
        );
        return false;
    }

    return true;
}

static bool parse_health_node(
        const ConfigNode *node,
        ProviderConfig *provider,
        GError **error) {
    static const char *const allowed[] = {
        "type",
        "interval-ms",
        "timeout-ms",
        "startup-timeout-ms",
    };

    if (!require_argument_count(node, 0, error) ||
            !reject_unknown_properties(node, allowed, G_N_ELEMENTS(allowed), error)) {
        return false;
    }

    g_autofree char *mode = NULL;
    if (!property_string(node, "type", true, &mode, error)) {
        return false;
    }

    if (strcmp(mode, "process") == 0) {
        provider->health_mode = PROVIDER_HEALTH_PROCESS;
        if (node_property(node, "interval-ms") ||
                node_property(node, "timeout-ms") ||
                node_property(node, "startup-timeout-ms") ||
                node->child_count != 0) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "process health must not define command timings or child nodes"
            );
            return false;
        }
        return true;
    }

    if (strcmp(mode, "command") != 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unsupported health type '%s'",
            mode
        );
        return false;
    }

    provider->health_mode = PROVIDER_HEALTH_COMMAND;
    if (!property_unsigned(node, "interval-ms", 500, &provider->health_interval_ms, error) ||
            !property_unsigned(node, "timeout-ms", 1000, &provider->health_timeout_ms, error) ||
            !property_unsigned(node, "startup-timeout-ms", 5000, &provider->health_startup_timeout_ms, error)) {
        return false;
    }

    bool saw_exec = false;
    for (size_t i = 0; i < node->child_count; i++) {
        const ConfigNode *child = node->children[i];
        if (strcmp(child->name, "exec") != 0) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "unknown node '%s' inside health",
                child->name
            );
            return false;
        }
        if (saw_exec) {
            g_set_error_literal(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "duplicate health exec node"
            );
            return false;
        }
        saw_exec = true;
        if (!parse_exec_node(child, &provider->health_argv, &provider->health_argc, error)) {
            return false;
        }
    }

    if (!saw_exec) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "command health requires exec node"
        );
        return false;
    }

    return true;
}

static bool parse_action_node(
        const ConfigNode *node,
        ProviderActionConfig *action,
        GError **error) {
    if (!require_argument_count(node, 1, error) ||
            !require_no_properties(node, error) ||
            !value_string(&node->arguments[0], "action id", &action->name, error)) {
        return false;
    }

    if (!valid_token(action->name)) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "invalid action id '%s'",
            action->name
        );
        return false;
    }

    bool saw_exec = false;
    for (size_t i = 0; i < node->child_count; i++) {
        const ConfigNode *child = node->children[i];
        if (strcmp(child->name, "exec") != 0) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "unknown node '%s' inside action '%s'",
                child->name,
                action->name
            );
            return false;
        }
        if (saw_exec) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "duplicate exec inside action '%s'",
                action->name
            );
            return false;
        }
        saw_exec = true;
        if (!parse_exec_node(child, &action->argv, &action->argc, error)) {
            return false;
        }
    }

    if (!saw_exec) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "action '%s' requires exec node",
            action->name
        );
        return false;
    }

    return true;
}

static bool parse_capability_node(
        const ConfigNode *node,
        ProviderCapabilityConfig *capability,
        GError **error) {
    if (!require_argument_count(node, 1, error) ||
            !require_no_properties(node, error) ||
            !value_string(&node->arguments[0], "capability id", &capability->id, error)) {
        return false;
    }

    if (!valid_token(capability->id)) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "invalid capability id '%s'",
            capability->id
        );
        return false;
    }

    for (size_t i = 0; i < node->child_count; i++) {
        const ConfigNode *child = node->children[i];
        if (strcmp(child->name, "action") != 0) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "unknown node '%s' inside capability '%s'",
                child->name,
                capability->id
            );
            return false;
        }

        capability->actions = g_realloc_n(
            capability->actions,
            capability->action_count + 1,
            sizeof(*capability->actions)
        );
        ProviderActionConfig *action = &capability->actions[capability->action_count++];
        *action = (ProviderActionConfig){0};

        if (!parse_action_node(child, action, error)) {
            return false;
        }

        for (size_t j = 0; j + 1 < capability->action_count; j++) {
            if (strcmp(capability->actions[j].name, action->name) == 0) {
                g_set_error(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate action '%s' in capability '%s'",
                    action->name,
                    capability->id
                );
                return false;
            }
        }
    }

    if (capability->action_count == 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "capability '%s' requires at least one action",
            capability->id
        );
        return false;
    }

    return true;
}

static bool parse_provider_node(
        const ConfigNode *node,
        ProviderConfig *provider,
        GError **error) {
    static const char *const allowed[] = {"type"};

    *provider = (ProviderConfig){
        .restart_policy = PROVIDER_RESTART_NEVER,
        .restart_delay_ms = 1000,
        .restart_max_delay_ms = 30000,
        .health_mode = PROVIDER_HEALTH_PROCESS,
        .health_interval_ms = 500,
        .health_timeout_ms = 1000,
        .health_startup_timeout_ms = 5000,
    };

    if (!require_argument_count(node, 1, error) ||
            !reject_unknown_properties(node, allowed, G_N_ELEMENTS(allowed), error) ||
            !value_string(&node->arguments[0], "provider id", &provider->id, error) ||
            !property_string(node, "type", true, &provider->type, error)) {
        return false;
    }

    if (!valid_token(provider->id)) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "invalid provider id '%s'",
            provider->id
        );
        return false;
    }

    bool saw_priority = false;
    bool saw_autostart = false;
    bool saw_required = false;
    bool saw_exec = false;
    bool saw_restart = false;
    bool saw_health = false;

    for (size_t i = 0; i < node->child_count; i++) {
        const ConfigNode *child = node->children[i];

        if (strcmp(child->name, "priority") == 0) {
            if (saw_priority) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "duplicate priority in provider '%s'", provider->id);
                return false;
            }
            saw_priority = true;
            if (!parse_single_int_node(child, &provider->priority, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "autostart") == 0) {
            if (saw_autostart) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "duplicate autostart in provider '%s'", provider->id);
                return false;
            }
            saw_autostart = true;
            if (!parse_single_bool_node(child, &provider->autostart, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "required") == 0) {
            if (saw_required) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "duplicate required in provider '%s'", provider->id);
                return false;
            }
            saw_required = true;
            if (!parse_single_bool_node(child, &provider->required, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "exec") == 0) {
            if (saw_exec) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "duplicate exec in provider '%s'", provider->id);
                return false;
            }
            saw_exec = true;
            if (!parse_exec_node(child, &provider->exec_argv, &provider->exec_argc, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "restart") == 0) {
            if (saw_restart) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "duplicate restart in provider '%s'", provider->id);
                return false;
            }
            saw_restart = true;
            if (!parse_restart_node(child, provider, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "health") == 0) {
            if (saw_health) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "duplicate health in provider '%s'", provider->id);
                return false;
            }
            saw_health = true;
            if (!parse_health_node(child, provider, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "capability") == 0) {
            provider->capabilities = g_realloc_n(
                provider->capabilities,
                provider->capability_count + 1,
                sizeof(*provider->capabilities)
            );
            ProviderCapabilityConfig *capability =
                &provider->capabilities[provider->capability_count++];
            *capability = (ProviderCapabilityConfig){0};

            if (!parse_capability_node(child, capability, error)) {
                return false;
            }

            for (size_t j = 0; j + 1 < provider->capability_count; j++) {
                if (strcmp(provider->capabilities[j].id, capability->id) == 0) {
                    g_set_error(
                        error,
                        G_FILE_ERROR,
                        G_FILE_ERROR_INVAL,
                        "duplicate capability '%s' in provider '%s'",
                        capability->id,
                        provider->id
                    );
                    return false;
                }
            }
            continue;
        }

        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unknown node '%s' inside provider '%s'",
            child->name,
            provider->id
        );
        return false;
    }

    if (!saw_exec) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
            "provider '%s' requires exec node", provider->id);
        return false;
    }
    if (provider->capability_count == 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
            "provider '%s' requires at least one capability", provider->id);
        return false;
    }

    return true;
}

static void fallback_action_finish(FallbackActionConfig *action) {
    g_free(action->label);
    g_free(action->shortcut);
    g_free(action->desktop_id);
    *action = (FallbackActionConfig){0};
}

static void fallback_finish(FallbackConfig *fallback) {
    g_free(fallback->title);
    g_strfreev(fallback->show_when_capabilities_missing);
    for (size_t i = 0; i < fallback->action_count; i++) {
        fallback_action_finish(&fallback->actions[i]);
    }
    g_free(fallback->actions);
    *fallback = (FallbackConfig){0};
}

static void appearance_finish(
        AppearanceConfig *appearance) {
    g_free(appearance->wallpaper);
    *appearance = (AppearanceConfig){0};
}

void shell_config_init(ShellConfig *config) {
    *config = (ShellConfig){
        .version = 1,
        .appearance = {
            .wallpaper = g_strdup("auto"),
        },
        .fallback = {
            .enabled = true,
            .title = g_strdup("Desktop UI unavailable"),
        },
    };
}

void shell_config_finish(ShellConfig *config) {
    for (size_t i = 0; i < config->provider_count; i++) {
        provider_config_finish(&config->providers[i]);
    }
    g_free(config->providers);
    appearance_finish(&config->appearance);
    fallback_finish(&config->fallback);
    *config = (ShellConfig){0};
}

static bool parse_appearance_node(
        const ConfigNode *node,
        AppearanceConfig *appearance,
        GError **error) {
    if (!require_argument_count(node, 0, error) ||
            !require_no_properties(node, error)) {
        return false;
    }

    bool saw_wallpaper = false;

    for (size_t i = 0; i < node->child_count; i++) {
        const ConfigNode *child = node->children[i];

        if (strcmp(child->name, "wallpaper") == 0) {
            if (saw_wallpaper) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate appearance wallpaper"
                );
                return false;
            }

            saw_wallpaper = true;

            if (!require_argument_count(child, 1, error) ||
                    !require_no_properties(child, error) ||
                    !require_no_children(child, error)) {
                return false;
            }

            g_free(appearance->wallpaper);
            appearance->wallpaper = NULL;

            if (!value_string(
                    &child->arguments[0],
                    "appearance wallpaper",
                    &appearance->wallpaper,
                    error)) {
                return false;
            }

            continue;
        }

        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unknown node '%s' inside appearance",
            child->name
        );
        return false;
    }

    return true;
}

static bool parse_fallback_action_node(
        const ConfigNode *node,
        FallbackActionConfig *action,
        GError **error) {
    static const char *const allowed[] = {"shortcut", "desktop-id"};

    return require_argument_count(node, 1, error) &&
        require_no_children(node, error) &&
        reject_unknown_properties(node, allowed, G_N_ELEMENTS(allowed), error) &&
        value_string(&node->arguments[0], "fallback action label", &action->label, error) &&
        property_string(node, "shortcut", true, &action->shortcut, error) &&
        property_string(node, "desktop-id", true, &action->desktop_id, error);
}

static bool parse_fallback_node(
        const ConfigNode *node,
        FallbackConfig *fallback,
        GError **error) {
    static const char *const allowed[] = {"enabled"};

    if (!require_argument_count(node, 0, error) ||
            !reject_unknown_properties(node, allowed, G_N_ELEMENTS(allowed), error) ||
            !property_bool(node, "enabled", true, &fallback->enabled, error)) {
        return false;
    }

    bool saw_title = false;
    bool saw_show_when_missing = false;

    for (size_t i = 0; i < node->child_count; i++) {
        const ConfigNode *child = node->children[i];

        if (strcmp(child->name, "title") == 0) {
            if (saw_title) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate fallback title"
                );
                return false;
            }
            saw_title = true;
            if (!require_argument_count(child, 1, error) ||
                    !require_no_properties(child, error) ||
                    !require_no_children(child, error)) {
                return false;
            }
            g_free(fallback->title);
            fallback->title = NULL;
            if (!value_string(&child->arguments[0], "fallback title", &fallback->title, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(child->name, "show-when-missing") == 0) {
            if (saw_show_when_missing) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate fallback show-when-missing"
                );
                return false;
            }
            saw_show_when_missing = true;
            if (!require_no_properties(child, error) ||
                    !require_no_children(child, error)) {
                return false;
            }

            fallback->show_when_capabilities_missing = g_new0(
                char *,
                child->argument_count + 1
            );
            for (size_t j = 0; j < child->argument_count; j++) {
                g_autofree char *context = g_strdup_printf(
                    "fallback show-when-missing[%zu]",
                    j
                );
                if (!value_string(
                        &child->arguments[j],
                        context,
                        &fallback->show_when_capabilities_missing[j],
                        error)) {
                    return false;
                }
                if (!valid_token(fallback->show_when_capabilities_missing[j])) {
                    g_set_error(
                        error,
                        G_FILE_ERROR,
                        G_FILE_ERROR_INVAL,
                        "invalid fallback capability id '%s'",
                        fallback->show_when_capabilities_missing[j]
                    );
                    return false;
                }
            }
            fallback->show_when_capabilities_missing_count = child->argument_count;
            continue;
        }

        if (strcmp(child->name, "action") == 0) {
            fallback->actions = g_realloc_n(
                fallback->actions,
                fallback->action_count + 1,
                sizeof(*fallback->actions)
            );
            FallbackActionConfig *action = &fallback->actions[fallback->action_count++];
            *action = (FallbackActionConfig){0};
            if (!parse_fallback_action_node(child, action, error)) {
                return false;
            }
            continue;
        }

        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unknown node '%s' inside fallback",
            child->name
        );
        return false;
    }

    return true;
}

static bool validate_provider(
        const ProviderConfig *provider,
        GError **error) {
    if (!provider->exec_argv || provider->exec_argc == 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "provider '%s' has empty exec argv",
            provider->id
        );
        return false;
    }

    if (provider->restart_delay_ms == 0 ||
            provider->restart_max_delay_ms == 0 ||
            provider->restart_delay_ms > provider->restart_max_delay_ms) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "provider '%s' restart bounds require 0 < delay-ms <= max-delay-ms",
            provider->id
        );
        return false;
    }

    if (provider->health_mode == PROVIDER_HEALTH_COMMAND) {
        if (!provider->health_argv || provider->health_argc == 0 ||
                provider->health_interval_ms == 0 ||
                provider->health_timeout_ms == 0 ||
                provider->health_startup_timeout_ms == 0) {
            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "provider '%s' command health requires non-empty argv and positive timings",
                provider->id
            );
            return false;
        }
    } else if (provider->health_argv || provider->health_argc != 0) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "provider '%s' process health must not have command argv",
            provider->id
        );
        return false;
    }

    return true;
}

static bool pair_overlap(
        const ProviderConfig *left,
        const ProviderConfig *right,
        GError **error) {
    if (left->priority != right->priority) {
        return false;
    }

    for (size_t i = 0; i < left->capability_count; i++) {
        const ProviderCapabilityConfig *left_cap = &left->capabilities[i];
        const ProviderCapabilityConfig *right_cap =
            provider_config_find_capability(right, left_cap->id);
        if (!right_cap) {
            continue;
        }

        for (size_t j = 0; j < left_cap->action_count; j++) {
            const char *action = left_cap->actions[j].name;
            if (!provider_capability_find_action(right_cap, action)) {
                continue;
            }

            g_set_error(
                error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "ambiguous provider resolution for capability '%s' action '%s': providers '%s' and '%s' have equal priority %d",
                left_cap->id,
                action,
                left->id,
                right->id,
                left->priority
            );
            return true;
        }
    }

    return false;
}

static bool validate_config(
        const ShellConfig *config,
        GError **error) {
    if (!config->appearance.wallpaper ||
            config->appearance.wallpaper[0] == '\0') {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "appearance wallpaper must not be empty"
        );
        return false;
    }

    if (strcmp(
            config->appearance.wallpaper,
            "auto") != 0 &&
            !g_path_is_absolute(
                config->appearance.wallpaper) &&
            !g_str_has_prefix(
                config->appearance.wallpaper,
                "~/")) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "appearance wallpaper must be 'auto', an absolute path, or a ~/ path"
        );
        return false;
    }

    if (config->version != 1) {
        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unsupported config version %u",
            config->version
        );
        return false;
    }

    for (size_t i = 0; i < config->provider_count; i++) {
        const ProviderConfig *provider = &config->providers[i];
        if (!validate_provider(provider, error)) {
            return false;
        }

        for (size_t j = 0; j < i; j++) {
            const ProviderConfig *other = &config->providers[j];
            if (strcmp(provider->id, other->id) == 0) {
                g_set_error(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate provider id '%s'",
                    provider->id
                );
                return false;
            }
            if (pair_overlap(provider, other, error)) {
                return false;
            }
        }
    }

    return true;
}

static bool parse_semantic_config(
        const ConfigNode *root,
        ShellConfig *config,
        GError **error) {
    bool saw_version = false;
    bool saw_appearance = false;
    bool saw_fallback = false;

    for (size_t i = 0; i < root->child_count; i++) {
        const ConfigNode *node = root->children[i];

        if (strcmp(node->name, "version") == 0) {
            if (saw_version) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate version node"
                );
                return false;
            }
            saw_version = true;
            int version = 0;
            if (!require_argument_count(node, 1, error) ||
                    !require_no_properties(node, error) ||
                    !require_no_children(node, error) ||
                    !value_int(&node->arguments[0], "version", &version, error)) {
                return false;
            }
            if (version != 1) {
                g_set_error(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "unsupported config version %d",
                    version
                );
                return false;
            }
            config->version = 1;
            continue;
        }

        if (strcmp(node->name, "appearance") == 0) {
            if (saw_appearance) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate appearance node"
                );
                return false;
            }

            saw_appearance = true;

            if (!parse_appearance_node(
                    node,
                    &config->appearance,
                    error)) {
                return false;
            }

            continue;
        }

        if (strcmp(node->name, "provider") == 0) {
            config->providers = g_realloc_n(
                config->providers,
                config->provider_count + 1,
                sizeof(*config->providers)
            );
            ProviderConfig *provider = &config->providers[config->provider_count++];
            *provider = (ProviderConfig){0};
            if (!parse_provider_node(node, provider, error)) {
                return false;
            }
            continue;
        }

        if (strcmp(node->name, "fallback") == 0) {
            if (saw_fallback) {
                g_set_error_literal(
                    error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "duplicate fallback node"
                );
                return false;
            }
            saw_fallback = true;
            if (!parse_fallback_node(node, &config->fallback, error)) {
                return false;
            }
            continue;
        }

        g_set_error(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "unknown top-level node '%s'",
            node->name
        );
        return false;
    }

    if (!saw_version) {
        g_set_error_literal(
            error,
            G_FILE_ERROR,
            G_FILE_ERROR_INVAL,
            "missing required version node"
        );
        return false;
    }

    return validate_config(config, error);
}

bool shell_config_load(
        ShellConfig *config,
        const char *path,
        bool allow_missing,
        GError **error) {
    g_autofree char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;

    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        if (allow_missing &&
                read_error &&
                read_error->domain == G_FILE_ERROR &&
                read_error->code == G_FILE_ERROR_NOENT) {
            g_clear_error(&read_error);
            return true;
        }
        g_propagate_error(error, read_error);
        return false;
    }

    ConfigNode root = {0};
    if (!parse_kdl_tree(contents, length, &root, error)) {
        config_node_finish(&root);
        return false;
    }

    ShellConfig candidate;
    shell_config_init(&candidate);

    if (!parse_semantic_config(&root, &candidate, error)) {
        config_node_finish(&root);
        shell_config_finish(&candidate);
        return false;
    }

    config_node_finish(&root);
    candidate.loaded_from_file = true;

    shell_config_finish(config);
    *config = candidate;
    return true;
}

const ProviderConfig *shell_config_provider_for_capability_action(
        const ShellConfig *config,
        const char *capability,
        const char *action) {
    const ProviderConfig *best = NULL;
    if (!config || !capability || !action) {
        return NULL;
    }

    for (size_t i = 0; i < config->provider_count; i++) {
        const ProviderConfig *provider = &config->providers[i];
        if (!provider_config_find_action(provider, capability, action)) {
            continue;
        }
        if (!best || provider->priority > best->priority) {
            best = provider;
        }
    }
    return best;
}
