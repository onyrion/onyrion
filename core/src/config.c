#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kdl/parser.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

typedef enum config_read_status {
    CONFIG_READ_OK,
    CONFIG_READ_MISSING,
    CONFIG_READ_ERROR,
} ConfigReadStatus;

typedef enum config_node_kind {
    CONFIG_NODE_ROOT,
    CONFIG_NODE_VERSION,
    CONFIG_NODE_INPUT,
    CONFIG_NODE_KEYBOARD,
    CONFIG_NODE_KEYBOARD_LAYOUT,
    CONFIG_NODE_KEYBOARD_MODEL,
    CONFIG_NODE_KEYBOARD_VARIANT,
    CONFIG_NODE_KEYBOARD_OPTIONS,
    CONFIG_NODE_REPEAT_RATE,
    CONFIG_NODE_REPEAT_DELAY,
    CONFIG_NODE_TOUCHPAD,
    CONFIG_NODE_TOUCHPAD_NATURAL_SCROLL,
    CONFIG_NODE_TOUCHPAD_DISABLE_WHILE_TYPING,
    CONFIG_NODE_TOUCHPAD_CLICKFINGER,
    CONFIG_NODE_TOUCHPAD_SCROLL_FACTOR,
    CONFIG_NODE_POINTER_DEVICE,
    CONFIG_NODE_POINTER_DEVICE_SENSITIVITY,
    CONFIG_NODE_BINDINGS,
    CONFIG_NODE_BINDING,
    CONFIG_NODE_POINTER_BINDING,
    CONFIG_NODE_GESTURE_BINDING,
    CONFIG_NODE_LAYOUT,
    CONFIG_NODE_INITIAL_SPLIT_RATIO,
    CONFIG_NODE_RESIZE_STEP,
    CONFIG_NODE_RESIZE_MIN,
    CONFIG_NODE_RESIZE_MAX,
    CONFIG_NODE_OUTER_GAP,
    CONFIG_NODE_INNER_GAP,
    CONFIG_NODE_WORKSPACES,
    CONFIG_NODE_WORKSPACE,
    CONFIG_NODE_WORKSPACE_NAME,
    CONFIG_NODE_WORKSPACE_ICON,
    CONFIG_NODE_WORKSPACE_PERSISTENT,
    CONFIG_NODE_WORKSPACE_STARTUP,
    CONFIG_NODE_WORKSPACE_OUTPUT_AFFINITY,
    CONFIG_NODE_WINDOW_RULES,
    CONFIG_NODE_WINDOW_RULE,
    CONFIG_NODE_RULE_APP_ID,
    CONFIG_NODE_RULE_TITLE,
    CONFIG_NODE_RULE_WORKSPACE,
    CONFIG_NODE_RULE_OUTPUT,
    CONFIG_NODE_RULE_PLACEMENT,
} ConfigNodeKind;

typedef struct binding_candidate {
    uint32_t keysym;
    uint32_t keycode;
    uint32_t button;
    OnyrionPointerWheelDirection wheel_direction;
    uint32_t gesture_fingers;
    OnyrionInputBindingSelector gesture_selector;
    OnyrionGestureDirection gesture_direction;
    uint64_t workspace_id;
    uint32_t modifiers;
    OnyrionKeyboardBindingTrigger trigger;
    OnyrionActionKind action_kind;
    OnyrionDirection direction;
    OnyrionActionSplitOrientation orientation;
    char capability[ONYRION_ACTION_IDENTIFIER_CAPACITY];
    char shell_action[ONYRION_ACTION_IDENTIFIER_CAPACITY];

    bool has_key;
    bool has_keycode;
    bool has_button;
    bool has_wheel;
    bool has_gesture;
    bool has_fingers;
    bool has_gesture_direction;
    bool has_workspace;
    bool has_modifiers;
    bool has_trigger;
    bool has_action;
    bool has_direction;
    bool has_orientation;
    bool has_capability;
    bool has_shell_action;
} BindingCandidate;

typedef struct config_frame {
    ConfigNodeKind kind;
    size_t argument_count;
    BindingCandidate binding;
} ConfigFrame;

typedef struct parse_context {
    OnyrionCoreConfig *candidate;
    ConfigFrame frames[8];
    size_t depth;

    bool seen_version;
    bool seen_input;
    bool seen_keyboard;
    bool seen_keyboard_layout;
    bool seen_keyboard_model;
    bool seen_keyboard_variant;
    bool seen_keyboard_options;
    bool seen_repeat_rate;
    bool seen_repeat_delay;

    bool seen_touchpad;
    bool seen_touchpad_natural_scroll;
    bool seen_touchpad_disable_while_typing;
    bool seen_touchpad_clickfinger;
    bool seen_touchpad_scroll_factor;

    bool seen_pointer_device;
    bool seen_pointer_device_sensitivity;

    bool seen_bindings;
    bool seen_layout;
    bool seen_initial_split_ratio;
    bool seen_resize_step;
    bool seen_resize_min;
    bool seen_resize_max;
    bool seen_outer_gap;
    bool seen_inner_gap;
    bool seen_workspaces;
    bool seen_window_rules;

    size_t active_workspace_index;
    bool seen_workspace_name;
    bool seen_workspace_icon;
    bool seen_workspace_persistent;
    bool seen_workspace_startup;
    bool seen_workspace_output_affinity;

    size_t active_window_rule_index;
    bool seen_rule_app_id;
    bool seen_rule_title;
    bool seen_rule_workspace;
    bool seen_rule_output;
    bool seen_rule_placement;

    char *error;
    size_t error_size;
} ParseContext;

static bool set_error(
        char *error,
        size_t error_size,
        const char *format,
        ...) {
    if (error &&
            error_size > 0) {
        va_list args;
        va_start(args, format);

        vsnprintf(
            error,
            error_size,
            format,
            args
        );

        va_end(args);
    }

    return false;
}

static bool parse_error(
        ParseContext *context,
        const char *format,
        ...) {
    if (context->error &&
            context->error_size > 0) {
        va_list args;
        va_start(args, format);

        vsnprintf(
            context->error,
            context->error_size,
            format,
            args
        );

        va_end(args);
    }

    return false;
}

static bool str_eq(
        kdl_str value,
        const char *expected) {
    const size_t length =
        strlen(expected);

    return
        value.len == length &&
        memcmp(
            value.data,
            expected,
            length
        ) == 0;
}

static bool has_annotation(
        const kdl_value *value) {
    return
        value &&
        value->type_annotation.data &&
        value->type_annotation.len > 0;
}

static char *kdl_string_copy(
        kdl_str value) {
    if (value.len == SIZE_MAX) {
        return NULL;
    }

    char *copy =
        malloc(value.len + 1);

    if (!copy) {
        return NULL;
    }

    if (value.len > 0) {
        memcpy(
            copy,
            value.data,
            value.len
        );
    }

    copy[value.len] = '\0';

    return copy;
}

static bool policy_reserve_bindings(
        OnyrionInputPolicy *input,
        size_t needed,
        char *error,
        size_t error_size) {
    if (needed <=
            input->binding_capacity) {
        return true;
    }

    size_t capacity =
        input->binding_capacity
            ? input->binding_capacity
            : 16;

    while (capacity < needed) {
        if (capacity >
                SIZE_MAX / 2) {
            return set_error(
                error,
                error_size,
                "too many keyboard bindings"
            );
        }

        capacity *= 2;
    }

    if (capacity >
            SIZE_MAX /
                sizeof(*input->bindings)) {
        return set_error(
            error,
            error_size,
            "too many keyboard bindings"
        );
    }

    OnyrionKeyboardBinding *bindings =
        realloc(
            input->bindings,
            capacity *
                sizeof(*bindings)
        );

    if (!bindings) {
        return set_error(
            error,
            error_size,
            "out of memory"
        );
    }

    input->bindings = bindings;
    input->binding_capacity = capacity;

    return true;
}

static bool policy_add_binding(
        OnyrionInputPolicy *input,
        OnyrionKeyboardBinding binding,
        char *error,
        size_t error_size) {
    for (size_t i = 0;
            i < input->binding_count;
            i++) {
        const OnyrionKeyboardBinding *existing =
            &input->bindings[i];

        bool same_selector =
            existing->selector ==
                binding.selector;

        if (same_selector) {
            switch (binding.selector) {
            case ONYRION_INPUT_BINDING_KEYBOARD:
                same_selector =
                    existing->use_keycode ==
                        binding.use_keycode &&
                    (binding.use_keycode
                        ? existing->keycode ==
                            binding.keycode
                        : existing->keysym ==
                            binding.keysym);
                break;

            case ONYRION_INPUT_BINDING_POINTER_BUTTON:
                same_selector =
                    existing->button ==
                        binding.button;
                break;

            case ONYRION_INPUT_BINDING_POINTER_WHEEL:
                same_selector =
                    existing->wheel_direction ==
                        binding.wheel_direction;
                break;

            case ONYRION_INPUT_BINDING_GESTURE_SWIPE:
            case ONYRION_INPUT_BINDING_GESTURE_PINCH:
                same_selector =
                    existing->gesture_fingers ==
                        binding.gesture_fingers &&
                    existing->gesture_direction ==
                        binding.gesture_direction;
                break;

            case ONYRION_INPUT_BINDING_SELECTOR_COUNT:
                same_selector = false;
                break;
            }
        }

        if (same_selector &&
                existing->modifiers ==
                    binding.modifiers &&
                existing->modifiers_wildcard ==
                    binding.modifiers_wildcard) {
            if (existing->trigger ==
                    binding.trigger) {
                return set_error(
                    error,
                    error_size,
                    "duplicate input binding"
                );
            }

            if (binding.selector ==
                    ONYRION_INPUT_BINDING_POINTER_BUTTON) {
                const bool existing_interactive_press =
                    existing->trigger ==
                        ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS &&
                    (existing->action.kind ==
                        ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE ||
                     existing->action.kind ==
                        ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE);

                const bool binding_interactive_press =
                    binding.trigger ==
                        ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS &&
                    (binding.action.kind ==
                        ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE ||
                     binding.action.kind ==
                        ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE);

                if ((existing_interactive_press &&
                        binding.trigger ==
                            ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE) ||
                    (binding_interactive_press &&
                        existing->trigger ==
                            ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE)) {
                    return set_error(
                        error,
                        error_size,
                        "interactive pointer grab cannot share selector with release binding"
                    );
                }
            }
        }
    }

    if (!policy_reserve_bindings(
            input,
            input->binding_count + 1,
            error,
            error_size)) {
        return false;
    }

    input->bindings[
        input->binding_count
    ] = binding;

    input->binding_count++;

    return true;
}

static void policy_clear_bindings(
        OnyrionInputPolicy *input) {
    free(input->bindings);

    input->bindings = NULL;
    input->binding_count = 0;
    input->binding_capacity = 0;
}

static bool policy_reserve_workspaces(
        OnyrionWorkspacePolicySet *workspaces,
        size_t needed,
        char *error,
        size_t error_size) {
    if (needed <= workspaces->capacity) {
        return true;
    }

    size_t capacity =
        workspaces->capacity
            ? workspaces->capacity
            : 8;

    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            return set_error(
                error,
                error_size,
                "too many workspace declarations"
            );
        }

        capacity *= 2;
    }

    if (capacity >
            SIZE_MAX / sizeof(*workspaces->items)) {
        return set_error(
            error,
            error_size,
            "too many workspace declarations"
        );
    }

    OnyrionWorkspacePolicy *items =
        realloc(
            workspaces->items,
            capacity * sizeof(*items)
        );

    if (!items) {
        return set_error(
            error,
            error_size,
            "out of memory"
        );
    }

    workspaces->items = items;
    workspaces->capacity = capacity;

    return true;
}

static bool policy_add_workspace(
        OnyrionWorkspacePolicySet *workspaces,
        uint64_t id,
        size_t *index,
        char *error,
        size_t error_size) {
    for (size_t i = 0;
            i < workspaces->count;
            i++) {
        if (workspaces->items[i].id == id) {
            return set_error(
                error,
                error_size,
                "duplicate workspace id %" PRIu64,
                id
            );
        }
    }

    if (!policy_reserve_workspaces(
            workspaces,
            workspaces->count + 1,
            error,
            error_size)) {
        return false;
    }

    const size_t target =
        workspaces->count++;

    workspaces->items[target] =
        (OnyrionWorkspacePolicy){
            .id = id,
        };

    if (index) {
        *index = target;
    }

    return true;
}

static void policy_clear_workspaces(
        OnyrionWorkspacePolicySet *workspaces) {
    free(workspaces->items);

    workspaces->items = NULL;
    workspaces->count = 0;
    workspaces->capacity = 0;
}

static bool policy_reserve_window_rules(
        OnyrionWindowRuleSet *rules,
        size_t needed,
        char *error,
        size_t error_size) {
    if (needed <= rules->capacity) {
        return true;
    }

    size_t capacity =
        rules->capacity
            ? rules->capacity
            : 8;

    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            return set_error(
                error,
                error_size,
                "too many window placement rules"
            );
        }

        capacity *= 2;
    }

    if (capacity >
            SIZE_MAX / sizeof(*rules->items)) {
        return set_error(
            error,
            error_size,
            "too many window placement rules"
        );
    }

    OnyrionWindowRule *items =
        realloc(
            rules->items,
            capacity * sizeof(*items)
        );

    if (!items) {
        return set_error(
            error,
            error_size,
            "out of memory"
        );
    }

    rules->items = items;
    rules->capacity = capacity;

    return true;
}

static bool policy_add_window_rule(
        OnyrionWindowRuleSet *rules,
        size_t *index,
        char *error,
        size_t error_size) {
    if (!policy_reserve_window_rules(
            rules,
            rules->count + 1,
            error,
            error_size)) {
        return false;
    }

    const size_t target =
        rules->count++;

    rules->items[target] =
        (OnyrionWindowRule){
            .placement =
                ONYRION_WINDOW_RULE_PLACEMENT_COUNT,
        };

    if (index) {
        *index = target;
    }

    return true;
}

static void policy_clear_window_rules(
        OnyrionWindowRuleSet *rules) {
    free(rules->items);

    rules->items = NULL;
    rules->count = 0;
    rules->capacity = 0;
}

static bool keysym_from_name(
        const char *name,
        uint32_t *keysym,
        char *error,
        size_t error_size) {
    const xkb_keysym_t parsed =
        xkb_keysym_from_name(
            name,
            XKB_KEYSYM_NO_FLAGS
        );

    if (parsed == XKB_KEY_NoSymbol) {
        return set_error(
            error,
            error_size,
            "unknown key '%s'",
            name
        );
    }

    *keysym = (uint32_t)parsed;

    return true;
}

static bool config_set_defaults(
        OnyrionCoreConfig *config,
        char *error,
        size_t error_size) {
    (void)error;
    (void)error_size;

    *config = (OnyrionCoreConfig){
        .version = 1,
        .source = ONYRION_CONFIG_SOURCE_DEFAULTS,
        .policy = {
            .input = {
                .keyboard = {
                    .repeat_rate = 25,
                    .repeat_delay = 600,
                },
                .pointer = {
                    .touchpad = {
                        .scroll_factor = 1.0,
                    },
                },
            },
            .layout = {
                .initial_split_ratio = 0.5,
                .resize_step = 0.05,
                .resize_min = 0.10,
                .resize_max = 0.90,
                .outer_gap = 0,
                .inner_gap = 0,
            },
        },
    };

    return true;
}

void onyrion_core_config_finish(
        OnyrionCoreConfig *config) {
    if (!config) {
        return;
    }

    policy_clear_bindings(
        &config->policy.input
    );

    policy_clear_workspaces(
        &config->policy.workspaces
    );

    policy_clear_window_rules(
        &config->policy.window_rules
    );

    free(config->source_path);
    free(config->source_text);

    *config =
        (OnyrionCoreConfig){0};
}

static ConfigReadStatus read_config_file(
        const char *path,
        char **contents,
        size_t *length,
        char *error,
        size_t error_size) {
    *contents = NULL;
    *length = 0;

    const int fd =
        open(
            path,
            O_RDONLY | O_CLOEXEC
        );

    if (fd < 0) {
        if (errno == ENOENT) {
            return CONFIG_READ_MISSING;
        }

        set_error(
            error,
            error_size,
            "cannot open '%s': %s",
            path,
            strerror(errno)
        );

        return CONFIG_READ_ERROR;
    }

    struct stat stat_buffer;

    if (fstat(
            fd,
            &stat_buffer) != 0) {
        const int saved_errno = errno;
        close(fd);

        set_error(
            error,
            error_size,
            "cannot stat '%s': %s",
            path,
            strerror(saved_errno)
        );

        return CONFIG_READ_ERROR;
    }

    if (stat_buffer.st_size < 0 ||
            (uintmax_t)stat_buffer.st_size >
                SIZE_MAX - 1) {
        close(fd);

        set_error(
            error,
            error_size,
            "config file is too large"
        );

        return CONFIG_READ_ERROR;
    }

    const size_t size =
        (size_t)stat_buffer.st_size;

    char *buffer =
        malloc(size + 1);

    if (!buffer) {
        close(fd);

        set_error(
            error,
            error_size,
            "out of memory"
        );

        return CONFIG_READ_ERROR;
    }

    size_t offset = 0;

    while (offset < size) {
        const ssize_t result =
            read(
                fd,
                buffer + offset,
                size - offset
            );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            const int saved_errno = errno;

            free(buffer);
            close(fd);

            set_error(
                error,
                error_size,
                "cannot read '%s': %s",
                path,
                strerror(saved_errno)
            );

            return CONFIG_READ_ERROR;
        }

        if (result == 0) {
            free(buffer);
            close(fd);

            set_error(
                error,
                error_size,
                "short read from '%s'",
                path
            );

            return CONFIG_READ_ERROR;
        }

        offset +=
            (size_t)result;
    }

    close(fd);

    buffer[size] = '\0';

    *contents = buffer;
    *length = size;

    return CONFIG_READ_OK;
}

static bool mark_once(
        ParseContext *context,
        bool *seen,
        const char *name) {
    if (*seen) {
        return parse_error(
            context,
            "duplicate config node '%s'",
            name
        );
    }

    *seen = true;

    return true;
}

static bool begin_node(
        ParseContext *context,
        ConfigNodeKind parent,
        kdl_str name,
        ConfigNodeKind *kind) {
    if (parent == CONFIG_NODE_ROOT) {
        if (str_eq(name, "version")) {
            if (!mark_once(
                    context,
                    &context->seen_version,
                    "version")) {
                return false;
            }

            *kind = CONFIG_NODE_VERSION;
            return true;
        }

        if (str_eq(name, "input")) {
            if (!mark_once(
                    context,
                    &context->seen_input,
                    "input")) {
                return false;
            }

            *kind = CONFIG_NODE_INPUT;
            return true;
        }

        if (str_eq(name, "layout")) {
            if (!mark_once(
                    context,
                    &context->seen_layout,
                    "layout")) {
                return false;
            }

            *kind = CONFIG_NODE_LAYOUT;
            return true;
        }

        if (str_eq(name, "workspaces")) {
            if (!mark_once(
                    context,
                    &context->seen_workspaces,
                    "workspaces")) {
                return false;
            }

            *kind = CONFIG_NODE_WORKSPACES;
            return true;
        }

        if (str_eq(name, "window-rules")) {
            if (!mark_once(
                    context,
                    &context->seen_window_rules,
                    "window-rules")) {
                return false;
            }

            policy_clear_window_rules(
                &context->candidate->
                    policy.window_rules
            );

            *kind =
                CONFIG_NODE_WINDOW_RULES;
            return true;
        }

        return parse_error(
            context,
            "unknown root config node"
        );
    }

    if (parent == CONFIG_NODE_INPUT) {
        if (str_eq(name, "keyboard")) {
            if (!mark_once(
                    context,
                    &context->seen_keyboard,
                    "input.keyboard")) {
                return false;
            }

            *kind = CONFIG_NODE_KEYBOARD;
            return true;
        }

        if (str_eq(name, "touchpad")) {
            if (!mark_once(
                    context,
                    &context->seen_touchpad,
                    "input.touchpad")) {
                return false;
            }

            *kind = CONFIG_NODE_TOUCHPAD;
            return true;
        }

        if (str_eq(name, "pointer-device")) {
            if (!mark_once(
                    context,
                    &context->seen_pointer_device,
                    "input.pointer-device")) {
                return false;
            }

            *kind = CONFIG_NODE_POINTER_DEVICE;
            return true;
        }

        if (str_eq(name, "bindings")) {
            if (!mark_once(
                    context,
                    &context->seen_bindings,
                    "input.bindings")) {
                return false;
            }

            policy_clear_bindings(
                &context->candidate->
                    policy.input
            );

            *kind = CONFIG_NODE_BINDINGS;
            return true;
        }

        return parse_error(
            context,
            "unknown input config node"
        );
    }

    if (parent == CONFIG_NODE_KEYBOARD) {
        if (str_eq(name, "layout")) {
            if (!mark_once(
                    context,
                    &context->seen_keyboard_layout,
                    "input.keyboard.layout")) {
                return false;
            }

            *kind =
                CONFIG_NODE_KEYBOARD_LAYOUT;
            return true;
        }

        if (str_eq(name, "model")) {
            if (!mark_once(
                    context,
                    &context->seen_keyboard_model,
                    "input.keyboard.model")) {
                return false;
            }

            *kind =
                CONFIG_NODE_KEYBOARD_MODEL;
            return true;
        }

        if (str_eq(name, "variant")) {
            if (!mark_once(
                    context,
                    &context->seen_keyboard_variant,
                    "input.keyboard.variant")) {
                return false;
            }

            *kind =
                CONFIG_NODE_KEYBOARD_VARIANT;
            return true;
        }

        if (str_eq(name, "options")) {
            if (!mark_once(
                    context,
                    &context->seen_keyboard_options,
                    "input.keyboard.options")) {
                return false;
            }

            *kind =
                CONFIG_NODE_KEYBOARD_OPTIONS;
            return true;
        }

        if (str_eq(name, "repeat-rate")) {
            if (!mark_once(
                    context,
                    &context->seen_repeat_rate,
                    "input.keyboard.repeat-rate")) {
                return false;
            }

            *kind =
                CONFIG_NODE_REPEAT_RATE;
            return true;
        }

        if (str_eq(name, "repeat-delay")) {
            if (!mark_once(
                    context,
                    &context->seen_repeat_delay,
                    "input.keyboard.repeat-delay")) {
                return false;
            }

            *kind =
                CONFIG_NODE_REPEAT_DELAY;
            return true;
        }

        return parse_error(
            context,
            "unknown input.keyboard config node"
        );
    }

    if (parent == CONFIG_NODE_TOUCHPAD) {
        if (str_eq(name, "natural-scroll")) {
            if (!mark_once(
                    context,
                    &context->
                        seen_touchpad_natural_scroll,
                    "input.touchpad.natural-scroll")) {
                return false;
            }

            *kind =
                CONFIG_NODE_TOUCHPAD_NATURAL_SCROLL;
            return true;
        }

        if (str_eq(
                name,
                "disable-while-typing")) {
            if (!mark_once(
                    context,
                    &context->
                        seen_touchpad_disable_while_typing,
                    "input.touchpad.disable-while-typing")) {
                return false;
            }

            *kind =
                CONFIG_NODE_TOUCHPAD_DISABLE_WHILE_TYPING;
            return true;
        }

        if (str_eq(name, "clickfinger")) {
            if (!mark_once(
                    context,
                    &context->
                        seen_touchpad_clickfinger,
                    "input.touchpad.clickfinger")) {
                return false;
            }

            *kind =
                CONFIG_NODE_TOUCHPAD_CLICKFINGER;
            return true;
        }

        if (str_eq(name, "scroll-factor")) {
            if (!mark_once(
                    context,
                    &context->
                        seen_touchpad_scroll_factor,
                    "input.touchpad.scroll-factor")) {
                return false;
            }

            *kind =
                CONFIG_NODE_TOUCHPAD_SCROLL_FACTOR;
            return true;
        }

        return parse_error(
            context,
            "unknown input.touchpad config node"
        );
    }

    if (parent ==
            CONFIG_NODE_POINTER_DEVICE) {
        if (str_eq(name, "sensitivity")) {
            if (!mark_once(
                    context,
                    &context->
                        seen_pointer_device_sensitivity,
                    "input.pointer-device.sensitivity")) {
                return false;
            }

            *kind =
                CONFIG_NODE_POINTER_DEVICE_SENSITIVITY;
            return true;
        }

        return parse_error(
            context,
            "unknown input.pointer-device config node"
        );
    }

    if (parent == CONFIG_NODE_BINDINGS) {
        if (str_eq(name, "binding")) {
            *kind =
                CONFIG_NODE_BINDING;
            return true;
        }

        if (str_eq(name, "pointer-binding")) {
            *kind =
                CONFIG_NODE_POINTER_BINDING;
            return true;
        }

        if (str_eq(name, "gesture-binding")) {
            *kind =
                CONFIG_NODE_GESTURE_BINDING;
            return true;
        }

        return parse_error(
            context,
            "unknown input.bindings config node"
        );
    }

    if (parent == CONFIG_NODE_LAYOUT) {
        if (str_eq(
                name,
                "initial-split-ratio")) {
            if (!mark_once(
                    context,
                    &context->
                        seen_initial_split_ratio,
                    "layout.initial-split-ratio")) {
                return false;
            }

            *kind =
                CONFIG_NODE_INITIAL_SPLIT_RATIO;
            return true;
        }

        if (str_eq(name, "resize-step")) {
            if (!mark_once(
                    context,
                    &context->seen_resize_step,
                    "layout.resize-step")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RESIZE_STEP;
            return true;
        }

        if (str_eq(name, "resize-min")) {
            if (!mark_once(
                    context,
                    &context->seen_resize_min,
                    "layout.resize-min")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RESIZE_MIN;
            return true;
        }

        if (str_eq(name, "resize-max")) {
            if (!mark_once(
                    context,
                    &context->seen_resize_max,
                    "layout.resize-max")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RESIZE_MAX;
            return true;
        }

        if (str_eq(name, "outer-gap")) {
            if (!mark_once(
                    context,
                    &context->seen_outer_gap,
                    "layout.outer-gap")) {
                return false;
            }

            *kind =
                CONFIG_NODE_OUTER_GAP;
            return true;
        }

        if (str_eq(name, "inner-gap")) {
            if (!mark_once(
                    context,
                    &context->seen_inner_gap,
                    "layout.inner-gap")) {
                return false;
            }

            *kind =
                CONFIG_NODE_INNER_GAP;
            return true;
        }

        return parse_error(
            context,
            "unknown layout config node"
        );
    }

    if (parent == CONFIG_NODE_WORKSPACES) {
        if (str_eq(name, "workspace")) {
            context->active_workspace_index = SIZE_MAX;
            context->seen_workspace_name = false;
            context->seen_workspace_icon = false;
            context->seen_workspace_persistent = false;
            context->seen_workspace_startup = false;
            context->seen_workspace_output_affinity = false;

            *kind = CONFIG_NODE_WORKSPACE;
            return true;
        }

        return parse_error(
            context,
            "unknown workspaces config node"
        );
    }

    if (parent == CONFIG_NODE_WORKSPACE) {
        if (context->active_workspace_index == SIZE_MAX) {
            return parse_error(
                context,
                "workspace id must precede child nodes"
            );
        }

        if (str_eq(name, "name")) {
            if (!mark_once(
                    context,
                    &context->seen_workspace_name,
                    "workspace.name")) {
                return false;
            }

            *kind = CONFIG_NODE_WORKSPACE_NAME;
            return true;
        }

        if (str_eq(name, "icon")) {
            if (!mark_once(
                    context,
                    &context->seen_workspace_icon,
                    "workspace.icon")) {
                return false;
            }

            *kind = CONFIG_NODE_WORKSPACE_ICON;
            return true;
        }

        if (str_eq(name, "persistent")) {
            if (!mark_once(
                    context,
                    &context->seen_workspace_persistent,
                    "workspace.persistent")) {
                return false;
            }

            *kind = CONFIG_NODE_WORKSPACE_PERSISTENT;
            return true;
        }

        if (str_eq(name, "startup")) {
            if (!mark_once(
                    context,
                    &context->seen_workspace_startup,
                    "workspace.startup")) {
                return false;
            }

            *kind = CONFIG_NODE_WORKSPACE_STARTUP;
            return true;
        }

        if (str_eq(name, "output-affinity")) {
            if (!mark_once(
                    context,
                    &context->seen_workspace_output_affinity,
                    "workspace.output-affinity")) {
                return false;
            }

            *kind = CONFIG_NODE_WORKSPACE_OUTPUT_AFFINITY;
            return true;
        }

        return parse_error(
            context,
            "unknown workspace config node"
        );
    }

    if (parent == CONFIG_NODE_WINDOW_RULES) {
        if (!str_eq(name, "rule")) {
            return parse_error(
                context,
                "unknown window-rules config node"
            );
        }

        context->active_window_rule_index =
            SIZE_MAX;
        context->seen_rule_app_id = false;
        context->seen_rule_title = false;
        context->seen_rule_workspace = false;
        context->seen_rule_output = false;
        context->seen_rule_placement = false;

        if (!policy_add_window_rule(
                &context->candidate->
                    policy.window_rules,
                &context->
                    active_window_rule_index,
                context->error,
                context->error_size)) {
            return false;
        }

        *kind =
            CONFIG_NODE_WINDOW_RULE;
        return true;
    }

    if (parent == CONFIG_NODE_WINDOW_RULE) {
        if (context->active_window_rule_index ==
                SIZE_MAX) {
            return parse_error(
                context,
                "window rule is not active"
            );
        }

        if (str_eq(name, "app-id")) {
            if (!mark_once(
                    context,
                    &context->seen_rule_app_id,
                    "window-rules.rule.app-id")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RULE_APP_ID;
            return true;
        }

        if (str_eq(name, "title")) {
            if (!mark_once(
                    context,
                    &context->seen_rule_title,
                    "window-rules.rule.title")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RULE_TITLE;
            return true;
        }

        if (str_eq(name, "workspace")) {
            if (!mark_once(
                    context,
                    &context->seen_rule_workspace,
                    "window-rules.rule.workspace")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RULE_WORKSPACE;
            return true;
        }

        if (str_eq(name, "output")) {
            if (!mark_once(
                    context,
                    &context->seen_rule_output,
                    "window-rules.rule.output")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RULE_OUTPUT;
            return true;
        }

        if (str_eq(name, "placement")) {
            if (!mark_once(
                    context,
                    &context->seen_rule_placement,
                    "window-rules.rule.placement")) {
                return false;
            }

            *kind =
                CONFIG_NODE_RULE_PLACEMENT;
            return true;
        }

        return parse_error(
            context,
            "unknown window rule config node"
        );
    }

    return parse_error(
        context,
        "config leaf node cannot have children"
    );
}

static bool string_to_buffer(
        ParseContext *context,
        const kdl_value *value,
        char *result,
        size_t result_size) {
    if (has_annotation(value) ||
            value->type !=
                KDL_TYPE_STRING) {
        return parse_error(
            context,
            "config value must be a string"
        );
    }

    if (value->string.len >=
            result_size) {
        return parse_error(
            context,
            "config string value is too long"
        );
    }

    if (value->string.len > 0) {
        memcpy(
            result,
            value->string.data,
            value->string.len
        );
    }

    result[value->string.len] = '\0';

    return true;
}

static bool number_to_integer(
        ParseContext *context,
        const kdl_value *value,
        long long *result) {
    if (has_annotation(value) ||
            value->type !=
                KDL_TYPE_NUMBER ||
            value->number.type !=
                KDL_NUMBER_TYPE_INTEGER) {
        return parse_error(
            context,
            "config value must be an integer"
        );
    }

    *result =
        value->number.integer;

    return true;
}

static bool number_to_double(
        ParseContext *context,
        const kdl_value *value,
        double *result) {
    if (has_annotation(value) ||
            value->type !=
                KDL_TYPE_NUMBER) {
        return parse_error(
            context,
            "config value must be a number"
        );
    }

    switch (value->number.type) {
    case KDL_NUMBER_TYPE_INTEGER:
        *result =
            (double)value->number.integer;
        break;

    case KDL_NUMBER_TYPE_FLOATING_POINT:
        *result =
            value->number.floating_point;
        break;

    case KDL_NUMBER_TYPE_STRING_ENCODED:
        return parse_error(
            context,
            "config number is outside supported range"
        );
    }

    if (!isfinite(*result)) {
        return parse_error(
            context,
            "config number must be finite"
        );
    }

    return true;
}

static bool boolean_to_bool(
        ParseContext *context,
        const kdl_value *value,
        bool *result) {
    if (has_annotation(value) ||
            value->type != KDL_TYPE_BOOLEAN) {
        return parse_error(
            context,
            "config value must be a boolean"
        );
    }

    *result = value->boolean;
    return true;
}

static bool string_property(
        ParseContext *context,
        const kdl_value *value,
        char **result) {
    if (has_annotation(value) ||
            value->type !=
                KDL_TYPE_STRING) {
        return parse_error(
            context,
            "binding property must be a string"
        );
    }

    *result =
        kdl_string_copy(
            value->string
        );

    if (!*result) {
        return parse_error(
            context,
            "out of memory"
        );
    }

    return true;
}

static bool action_kind_from_name(
        const char *name,
        OnyrionActionKind *kind) {
    static const struct {
        const char *name;
        OnyrionActionKind kind;
    } actions[] = {
        {
            "workspace-next",
            ONYRION_ACTION_WORKSPACE_NEXT,
        },
        {
            "workspace-previous",
            ONYRION_ACTION_WORKSPACE_PREVIOUS,
        },
        {
            "interactive-move-active",
            ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE,
        },
        {
            "interactive-resize-active",
            ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE,
        },
        {
            "workspace-activate",
            ONYRION_ACTION_WORKSPACE_ACTIVATE,
        },
        {
            "split-active",
            ONYRION_ACTION_SPLIT_ACTIVE,
        },
        {
            "focus-direction",
            ONYRION_ACTION_FOCUS_DIRECTION,
        },
        {
            "move-direction",
            ONYRION_ACTION_MOVE_DIRECTION,
        },
        {
            "resize-direction",
            ONYRION_ACTION_RESIZE_DIRECTION,
        },
        {
            "flip-active-split",
            ONYRION_ACTION_FLIP_ACTIVE_SPLIT,
        },
        {
            "toggle-fullscreen-active",
            ONYRION_ACTION_TOGGLE_FULLSCREEN_ACTIVE,
        },
        {
            "close-active",
            ONYRION_ACTION_CLOSE_ACTIVE,
        },
        {
            "window-next",
            ONYRION_ACTION_WINDOW_NEXT,
        },
        {
            "window-previous",
            ONYRION_ACTION_WINDOW_PREVIOUS,
        },
        {
            "float-active",
            ONYRION_ACTION_FLOAT_ACTIVE,
        },
        {
            "tile-active",
            ONYRION_ACTION_TILE_ACTIVE,
        },
        {
            "shell.invoke",
            ONYRION_ACTION_SHELL_INVOKE,
        },
        {
            "session.exit",
            ONYRION_ACTION_SESSION_EXIT,
        },
    };

    for (size_t i = 0;
            i < sizeof(actions) /
                sizeof(actions[0]);
            i++) {
        if (strcmp(
                name,
                actions[i].name) == 0) {
            *kind =
                actions[i].kind;
            return true;
        }
    }

    return false;
}

static bool gesture_selector_from_name(
        const char *name,
        OnyrionInputBindingSelector *selector) {
    if (strcmp(name, "swipe") == 0) {
        *selector =
            ONYRION_INPUT_BINDING_GESTURE_SWIPE;
        return true;
    }

    if (strcmp(name, "pinch") == 0) {
        *selector =
            ONYRION_INPUT_BINDING_GESTURE_PINCH;
        return true;
    }

    return false;
}

static bool gesture_direction_from_name(
        const char *name,
        OnyrionGestureDirection *direction) {
    static const struct {
        const char *name;
        OnyrionGestureDirection direction;
    } values[] = {
        {"left", ONYRION_GESTURE_LEFT},
        {"right", ONYRION_GESTURE_RIGHT},
        {"up", ONYRION_GESTURE_UP},
        {"down", ONYRION_GESTURE_DOWN},
        {"in", ONYRION_GESTURE_IN},
        {"out", ONYRION_GESTURE_OUT},
    };

    for (size_t i = 0;
            i < sizeof(values) / sizeof(values[0]);
            i++) {
        if (strcmp(name, values[i].name) == 0) {
            *direction = values[i].direction;
            return true;
        }
    }

    return false;
}

static bool direction_from_name(
        const char *name,
        OnyrionDirection *direction) {
    if (strcmp(name, "left") == 0) {
        *direction =
            ONYRION_DIRECTION_LEFT;
        return true;
    }

    if (strcmp(name, "right") == 0) {
        *direction =
            ONYRION_DIRECTION_RIGHT;
        return true;
    }

    if (strcmp(name, "up") == 0) {
        *direction =
            ONYRION_DIRECTION_UP;
        return true;
    }

    if (strcmp(name, "down") == 0) {
        *direction =
            ONYRION_DIRECTION_DOWN;
        return true;
    }

    return false;
}

static bool orientation_from_name(
        const char *name,
        OnyrionActionSplitOrientation *orientation) {
    if (strcmp(
            name,
            "horizontal") == 0) {
        *orientation =
            ONYRION_ACTION_SPLIT_HORIZONTAL;
        return true;
    }

    if (strcmp(
            name,
            "vertical") == 0) {
        *orientation =
            ONYRION_ACTION_SPLIT_VERTICAL;
        return true;
    }

    return false;
}

static bool modifiers_from_name(
        const char *name,
        uint32_t *modifiers) {
    if (!name ||
            !modifiers ||
            name[0] == '\0') {
        return false;
    }

    if (strcmp(name, "none") == 0) {
        *modifiers = 0;
        return true;
    }

    char *copy = strdup(name);

    if (!copy) {
        return false;
    }

    uint32_t result = 0;
    char *save = NULL;

    for (char *token =
            strtok_r(copy, "+", &save);
            token;
            token = strtok_r(NULL, "+", &save)) {
        uint32_t bit = 0;

        if (strcmp(token, "shift") == 0) {
            bit =
                ONYRION_KEYBOARD_MODIFIER_SHIFT;
        } else if (strcmp(token, "control") == 0 ||
                strcmp(token, "ctrl") == 0) {
            bit =
                ONYRION_KEYBOARD_MODIFIER_CONTROL;
        } else if (strcmp(token, "alt") == 0) {
            bit =
                ONYRION_KEYBOARD_MODIFIER_ALT;
        } else if (strcmp(token, "super") == 0) {
            bit =
                ONYRION_KEYBOARD_MODIFIER_SUPER;
        } else {
            free(copy);
            return false;
        }

        if (result & bit) {
            free(copy);
            return false;
        }

        result |= bit;
    }

    free(copy);

    if (result == 0) {
        return false;
    }

    *modifiers = result;
    return true;
}

static bool binding_trigger_from_name(
        const char *name,
        OnyrionKeyboardBindingTrigger *trigger) {
    if (strcmp(name, "press") == 0) {
        *trigger =
            ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS;
        return true;
    }

    if (strcmp(name, "release") == 0) {
        *trigger =
            ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE;
        return true;
    }

    if (strcmp(
            name,
            "standalone-release") == 0) {
        *trigger =
            ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE;
        return true;
    }

    return false;
}

static bool pointer_button_from_name(
        const char *name,
        uint32_t *button) {
    static const struct {
        const char *name;
        uint32_t button;
    } buttons[] = {
        { "BTN_LEFT", BTN_LEFT },
        { "BTN_RIGHT", BTN_RIGHT },
        { "BTN_MIDDLE", BTN_MIDDLE },
        { "BTN_SIDE", BTN_SIDE },
        { "BTN_EXTRA", BTN_EXTRA },
        { "BTN_FORWARD", BTN_FORWARD },
        { "BTN_BACK", BTN_BACK },
        { "BTN_TASK", BTN_TASK },
    };

    for (size_t i = 0;
            i < sizeof(buttons) /
                sizeof(buttons[0]);
            i++) {
        if (strcmp(name, buttons[i].name) == 0) {
            *button = buttons[i].button;
            return true;
        }
    }

    return false;
}

static bool pointer_wheel_from_name(
        const char *name,
        OnyrionPointerWheelDirection *direction) {
    if (strcmp(name, "up") == 0) {
        *direction = ONYRION_POINTER_WHEEL_UP;
        return true;
    }

    if (strcmp(name, "down") == 0) {
        *direction = ONYRION_POINTER_WHEEL_DOWN;
        return true;
    }

    return false;
}

static bool copy_action_identifier(
        ParseContext *context,
        const kdl_value *value,
        char destination[
            static ONYRION_ACTION_IDENTIFIER_CAPACITY
        ],
        const char *property) {
    char *text = NULL;

    if (!string_property(
            context,
            value,
            &text)) {
        return false;
    }

    const size_t length =
        strlen(text);

    if (length == 0 ||
            length >=
                ONYRION_ACTION_IDENTIFIER_CAPACITY) {
        free(text);

        return parse_error(
            context,
            "binding property '%s' must be 1..%u bytes",
            property,
            (unsigned)
                (ONYRION_ACTION_IDENTIFIER_CAPACITY - 1)
        );
    }

    memcpy(
        destination,
        text,
        length + 1
    );

    free(text);

    return true;
}


static bool parse_binding_property(
        ParseContext *context,
        ConfigFrame *frame,
        kdl_str name,
        const kdl_value *value) {
    BindingCandidate *binding =
        &frame->binding;

    char *text = NULL;

    if (str_eq(name, "key")) {
        if (binding->has_key) {
            return parse_error(
                context,
                "duplicate binding property 'key'"
            );
        }

        if (binding->has_keycode) {
            return parse_error(
                context,
                "binding cannot contain both 'key' and 'keycode'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            keysym_from_name(
                text,
                &binding->keysym,
                context->error,
                context->error_size
            );

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_key = true;
        return true;
    }

    if (str_eq(name, "keycode")) {
        if (binding->has_keycode) {
            return parse_error(
                context,
                "duplicate binding property 'keycode'"
            );
        }

        if (binding->has_key) {
            return parse_error(
                context,
                "binding cannot contain both 'key' and 'keycode'"
            );
        }

        long long keycode = 0;

        if (!number_to_integer(
                context,
                value,
                &keycode)) {
            return false;
        }

        if (keycode < 0 ||
                keycode > KEY_MAX) {
            return parse_error(
                context,
                "binding keycode must be 0..%u",
                (unsigned)KEY_MAX
            );
        }

        binding->keycode =
            (uint32_t)keycode;

        binding->has_keycode = true;
        return true;
    }

    if (str_eq(name, "button")) {
        if (binding->has_button) {
            return parse_error(
                context,
                "duplicate binding property 'button'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            pointer_button_from_name(
                text,
                &binding->button
            );

        if (!ok) {
            parse_error(
                context,
                "unknown pointer button '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_button = true;
        return true;
    }

    if (str_eq(name, "wheel")) {
        if (binding->has_wheel) {
            return parse_error(
                context,
                "duplicate binding property 'wheel'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            pointer_wheel_from_name(
                text,
                &binding->wheel_direction
            );

        if (!ok) {
            parse_error(
                context,
                "unknown pointer wheel direction '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_wheel = true;
        return true;
    }

    if (str_eq(name, "gesture")) {
        if (binding->has_gesture) {
            return parse_error(
                context,
                "duplicate binding property 'gesture'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            gesture_selector_from_name(
                text,
                &binding->gesture_selector
            );

        if (!ok) {
            parse_error(
                context,
                "gesture must be 'swipe' or 'pinch'"
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_gesture = true;
        return true;
    }

    if (str_eq(name, "fingers")) {
        if (binding->has_fingers) {
            return parse_error(
                context,
                "duplicate binding property 'fingers'"
            );
        }

        long long fingers = 0;

        if (!number_to_integer(
                context,
                value,
                &fingers)) {
            return false;
        }

        if (fingers < 2 || fingers > 10) {
            return parse_error(
                context,
                "gesture fingers must be 2..10"
            );
        }

        binding->gesture_fingers =
            (uint32_t)fingers;
        binding->has_fingers = true;
        return true;
    }

    if (str_eq(name, "gesture-direction")) {
        if (binding->has_gesture_direction) {
            return parse_error(
                context,
                "duplicate binding property 'gesture-direction'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            gesture_direction_from_name(
                text,
                &binding->gesture_direction
            );

        if (!ok) {
            parse_error(
                context,
                "unknown gesture direction '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_gesture_direction = true;
        return true;
    }

    if (str_eq(name, "modifiers")) {
        if (binding->has_modifiers) {
            return parse_error(
                context,
                "duplicate binding property 'modifiers'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            modifiers_from_name(
                text,
                &binding->modifiers
            );

        if (!ok) {
            parse_error(
                context,
                "unknown modifiers '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_modifiers = true;
        return true;
    }

    if (str_eq(name, "trigger")) {
        if (binding->has_trigger) {
            return parse_error(
                context,
                "duplicate binding property 'trigger'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            binding_trigger_from_name(
                text,
                &binding->trigger
            );

        if (!ok) {
            parse_error(
                context,
                "unknown binding trigger '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_trigger = true;
        return true;
    }

    if (str_eq(name, "action")) {
        if (binding->has_action) {
            return parse_error(
                context,
                "duplicate binding property 'action'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            action_kind_from_name(
                text,
                &binding->action_kind
            );

        if (!ok) {
            parse_error(
                context,
                "unknown action '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_action = true;
        return true;
    }

    if (str_eq(name, "workspace")) {
        if (binding->has_workspace) {
            return parse_error(
                context,
                "duplicate binding property 'workspace'"
            );
        }

        long long workspace = 0;

        if (!number_to_integer(
                context,
                value,
                &workspace)) {
            return false;
        }

        if (workspace <= 0) {
            return parse_error(
                context,
                "binding workspace must be a positive integer"
            );
        }

        binding->workspace_id =
            (uint64_t)workspace;
        binding->has_workspace = true;
        return true;
    }

    if (str_eq(name, "direction")) {
        if (binding->has_direction) {
            return parse_error(
                context,
                "duplicate binding property 'direction'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            direction_from_name(
                text,
                &binding->direction
            );

        if (!ok) {
            parse_error(
                context,
                "unknown direction '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_direction = true;
        return true;
    }

    if (str_eq(name, "orientation")) {
        if (binding->has_orientation) {
            return parse_error(
                context,
                "duplicate binding property 'orientation'"
            );
        }

        if (!string_property(
                context,
                value,
                &text)) {
            return false;
        }

        const bool ok =
            orientation_from_name(
                text,
                &binding->orientation
            );

        if (!ok) {
            parse_error(
                context,
                "unknown orientation '%s'",
                text
            );
        }

        free(text);

        if (!ok) {
            return false;
        }

        binding->has_orientation = true;
        return true;
    }

    if (str_eq(name, "capability")) {
        if (binding->has_capability) {
            return parse_error(
                context,
                "duplicate binding property 'capability'"
            );
        }

        if (!copy_action_identifier(
                context,
                value,
                binding->capability,
                "capability")) {
            return false;
        }

        binding->has_capability = true;
        return true;
    }

    if (str_eq(name, "shell-action")) {
        if (binding->has_shell_action) {
            return parse_error(
                context,
                "duplicate binding property 'shell-action'"
            );
        }

        if (!copy_action_identifier(
                context,
                value,
                binding->shell_action,
                "shell-action")) {
            return false;
        }

        binding->has_shell_action = true;
        return true;
    }

    return parse_error(
        context,
        "unknown binding property"
    );
}

static bool parse_argument(
        ParseContext *context,
        ConfigFrame *frame,
        const kdl_value *value) {
    if (frame->argument_count != 0) {
        return parse_error(
            context,
            "config node has too many arguments"
        );
    }

    frame->argument_count++;

    long long integer = 0;
    double number = 0.0;

    switch (frame->kind) {
    case CONFIG_NODE_VERSION:
        if (!number_to_integer(
                context,
                value,
                &integer)) {
            return false;
        }

        if (integer != 1) {
            return parse_error(
                context,
                "unsupported config version"
            );
        }

        context->candidate->version = 1;
        return true;

    case CONFIG_NODE_KEYBOARD_LAYOUT:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.input.keyboard.layout,
            sizeof(context->candidate->
                policy.input.keyboard.layout)
        );

    case CONFIG_NODE_KEYBOARD_MODEL:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.input.keyboard.model,
            sizeof(context->candidate->
                policy.input.keyboard.model)
        );

    case CONFIG_NODE_KEYBOARD_VARIANT:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.input.keyboard.variant,
            sizeof(context->candidate->
                policy.input.keyboard.variant)
        );

    case CONFIG_NODE_KEYBOARD_OPTIONS:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.input.keyboard.options,
            sizeof(context->candidate->
                policy.input.keyboard.options)
        );

    case CONFIG_NODE_REPEAT_RATE:
    case CONFIG_NODE_REPEAT_DELAY:
        if (!number_to_integer(
                context,
                value,
                &integer)) {
            return false;
        }

        if (integer < 0 ||
                integer > INT_MAX) {
            return parse_error(
                context,
                "keyboard repeat value is outside supported range"
            );
        }

        if (frame->kind ==
                CONFIG_NODE_REPEAT_RATE) {
            context->candidate->
                policy.input.keyboard.repeat_rate =
                    (int)integer;
        } else {
            context->candidate->
                policy.input.keyboard.repeat_delay =
                    (int)integer;
        }

        return true;

    case CONFIG_NODE_TOUCHPAD_NATURAL_SCROLL:
        context->candidate->
            policy.input.pointer.touchpad.
                has_natural_scroll = true;

        return boolean_to_bool(
            context,
            value,
            &context->candidate->
                policy.input.pointer.touchpad.
                    natural_scroll
        );

    case CONFIG_NODE_TOUCHPAD_DISABLE_WHILE_TYPING:
        context->candidate->
            policy.input.pointer.touchpad.
                has_disable_while_typing = true;

        return boolean_to_bool(
            context,
            value,
            &context->candidate->
                policy.input.pointer.touchpad.
                    disable_while_typing
        );

    case CONFIG_NODE_TOUCHPAD_CLICKFINGER:
        context->candidate->
            policy.input.pointer.touchpad.
                has_clickfinger = true;

        return boolean_to_bool(
            context,
            value,
            &context->candidate->
                policy.input.pointer.touchpad.
                    clickfinger
        );

    case CONFIG_NODE_TOUCHPAD_SCROLL_FACTOR:
        if (!number_to_double(
                context,
                value,
                &number)) {
            return false;
        }

        context->candidate->
            policy.input.pointer.touchpad.
                has_scroll_factor = true;
        context->candidate->
            policy.input.pointer.touchpad.
                scroll_factor = number;

        return true;

    case CONFIG_NODE_POINTER_DEVICE:
        context->candidate->
            policy.input.pointer.device.
                configured = true;

        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.input.pointer.device.name,
            sizeof(context->candidate->
                policy.input.pointer.device.name)
        );

    case CONFIG_NODE_POINTER_DEVICE_SENSITIVITY:
        if (!number_to_double(
                context,
                value,
                &number)) {
            return false;
        }

        context->candidate->
            policy.input.pointer.device.
                has_sensitivity = true;
        context->candidate->
            policy.input.pointer.device.
                sensitivity = number;

        return true;

    case CONFIG_NODE_OUTER_GAP:
    case CONFIG_NODE_INNER_GAP:
        if (!number_to_integer(
                context,
                value,
                &integer)) {
            return false;
        }

        if (integer < 0 ||
                integer > INT_MAX) {
            return parse_error(
                context,
                "layout gap must be a non-negative supported integer"
            );
        }

        if (frame->kind ==
                CONFIG_NODE_OUTER_GAP) {
            context->candidate->
                policy.layout.outer_gap =
                    (int)integer;
        } else {
            context->candidate->
                policy.layout.inner_gap =
                    (int)integer;
        }

        return true;

    case CONFIG_NODE_INITIAL_SPLIT_RATIO:
    case CONFIG_NODE_RESIZE_STEP:
    case CONFIG_NODE_RESIZE_MIN:
    case CONFIG_NODE_RESIZE_MAX:
        if (!number_to_double(
                context,
                value,
                &number)) {
            return false;
        }

        switch (frame->kind) {
        case CONFIG_NODE_INITIAL_SPLIT_RATIO:
            context->candidate->
                policy.layout.initial_split_ratio =
                    number;
            break;

        case CONFIG_NODE_RESIZE_STEP:
            context->candidate->
                policy.layout.resize_step =
                    number;
            break;

        case CONFIG_NODE_RESIZE_MIN:
            context->candidate->
                policy.layout.resize_min =
                    number;
            break;

        case CONFIG_NODE_RESIZE_MAX:
            context->candidate->
                policy.layout.resize_max =
                    number;
            break;

        default:
            return false;
        }

        return true;

    case CONFIG_NODE_WORKSPACE:
        if (!number_to_integer(
                context,
                value,
                &integer)) {
            return false;
        }

        if (integer <= 0) {
            return parse_error(
                context,
                "workspace id must be a positive integer"
            );
        }

        if (!policy_add_workspace(
                &context->candidate->
                    policy.workspaces,
                (uint64_t)integer,
                &context->active_workspace_index,
                context->error,
                context->error_size)) {
            return false;
        }

        return true;

    case CONFIG_NODE_WORKSPACE_NAME:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].name,
            sizeof(context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].name)
        );

    case CONFIG_NODE_WORKSPACE_ICON:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].icon,
            sizeof(context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].icon)
        );

    case CONFIG_NODE_WORKSPACE_OUTPUT_AFFINITY:
        return string_to_buffer(
            context,
            value,
            context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].output_affinity,
            sizeof(context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].output_affinity)
        );

    case CONFIG_NODE_WORKSPACE_PERSISTENT:
        return boolean_to_bool(
            context,
            value,
            &context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].persistent
        );

    case CONFIG_NODE_WORKSPACE_STARTUP:
        return boolean_to_bool(
            context,
            value,
            &context->candidate->
                policy.workspaces.items[
                    context->active_workspace_index
                ].startup
        );

    case CONFIG_NODE_RULE_APP_ID: {
        OnyrionWindowRule *rule =
            &context->candidate->
                policy.window_rules.items[
                    context->active_window_rule_index
                ];

        if (!string_to_buffer(
                context,
                value,
                rule->app_id,
                sizeof(rule->app_id))) {
            return false;
        }

        rule->match_app_id = true;
        return true;
    }

    case CONFIG_NODE_RULE_TITLE: {
        OnyrionWindowRule *rule =
            &context->candidate->
                policy.window_rules.items[
                    context->active_window_rule_index
                ];

        if (!string_to_buffer(
                context,
                value,
                rule->title,
                sizeof(rule->title))) {
            return false;
        }

        rule->match_title = true;
        return true;
    }

    case CONFIG_NODE_RULE_OUTPUT: {
        OnyrionWindowRule *rule =
            &context->candidate->
                policy.window_rules.items[
                    context->active_window_rule_index
                ];

        if (!string_to_buffer(
                context,
                value,
                rule->output,
                sizeof(rule->output))) {
            return false;
        }

        rule->match_output = true;
        return true;
    }

    case CONFIG_NODE_RULE_WORKSPACE: {
        if (!number_to_integer(
                context,
                value,
                &integer)) {
            return false;
        }

        if (integer <= 0) {
            return parse_error(
                context,
                "window rule workspace must be a positive integer"
            );
        }

        OnyrionWindowRule *rule =
            &context->candidate->
                policy.window_rules.items[
                    context->active_window_rule_index
                ];

        rule->workspace_id =
            (uint64_t)integer;
        rule->match_workspace = true;
        return true;
    }

    case CONFIG_NODE_RULE_PLACEMENT: {
        char placement[32] = {0};

        if (!string_to_buffer(
                context,
                value,
                placement,
                sizeof(placement))) {
            return false;
        }

        OnyrionWindowRule *rule =
            &context->candidate->
                policy.window_rules.items[
                    context->active_window_rule_index
                ];

        if (strcmp(
                placement,
                "tiled") == 0) {
            rule->placement =
                ONYRION_WINDOW_RULE_PLACEMENT_TILED;
            return true;
        }

        if (strcmp(
                placement,
                "floating") == 0) {
            rule->placement =
                ONYRION_WINDOW_RULE_PLACEMENT_FLOATING;
            return true;
        }

        return parse_error(
            context,
            "window rule placement must be 'tiled' or 'floating'"
        );
    }

    case CONFIG_NODE_ROOT:
    case CONFIG_NODE_INPUT:
    case CONFIG_NODE_KEYBOARD:
    case CONFIG_NODE_TOUCHPAD:
    case CONFIG_NODE_BINDINGS:
    case CONFIG_NODE_BINDING:
    case CONFIG_NODE_POINTER_BINDING:
    case CONFIG_NODE_GESTURE_BINDING:
    case CONFIG_NODE_LAYOUT:
    case CONFIG_NODE_WORKSPACES:
    case CONFIG_NODE_WINDOW_RULES:
    case CONFIG_NODE_WINDOW_RULE:
        return parse_error(
            context,
            "config container does not accept arguments"
        );
    }

    return false;
}

static bool finalize_binding(
        ParseContext *context,
        ConfigFrame *frame) {
    BindingCandidate *binding =
        &frame->binding;

    const bool pointer_binding =
        frame->kind == CONFIG_NODE_POINTER_BINDING;

    const bool gesture_binding =
        frame->kind == CONFIG_NODE_GESTURE_BINDING;

    if (gesture_binding) {
        if (binding->has_key ||
                binding->has_keycode ||
                binding->has_button ||
                binding->has_wheel ||
                binding->has_modifiers ||
                binding->has_trigger) {
            return parse_error(
                context,
                "gesture-binding accepts only gesture selector and action properties"
            );
        }

        if (!binding->has_gesture ||
                !binding->has_fingers ||
                !binding->has_gesture_direction) {
            return parse_error(
                context,
                "gesture-binding requires gesture, fingers and gesture-direction"
            );
        }

        if (binding->gesture_selector ==
                    ONYRION_INPUT_BINDING_GESTURE_SWIPE &&
                (binding->gesture_direction == ONYRION_GESTURE_IN ||
                 binding->gesture_direction == ONYRION_GESTURE_OUT)) {
            return parse_error(
                context,
                "swipe gesture direction must be left/right/up/down"
            );
        }

        if (binding->gesture_selector ==
                    ONYRION_INPUT_BINDING_GESTURE_PINCH &&
                binding->gesture_direction != ONYRION_GESTURE_IN &&
                binding->gesture_direction != ONYRION_GESTURE_OUT) {
            return parse_error(
                context,
                "pinch gesture direction must be in/out"
            );
        }
    } else if (pointer_binding) {
        if (binding->has_key ||
                binding->has_keycode ||
                binding->has_gesture ||
                binding->has_fingers ||
                binding->has_gesture_direction) {
            return parse_error(
                context,
                "pointer-binding does not accept key or gesture selectors"
            );
        }

        if (binding->has_button ==
                binding->has_wheel) {
            return parse_error(
                context,
                "pointer-binding requires exactly one of 'button' or 'wheel'"
            );
        }
    } else {
        if (binding->has_button ||
                binding->has_wheel ||
                binding->has_gesture ||
                binding->has_fingers ||
                binding->has_gesture_direction) {
            return parse_error(
                context,
                "keyboard binding does not accept pointer or gesture selectors"
            );
        }

        if (!binding->has_key &&
                !binding->has_keycode) {
            return parse_error(
                context,
                "binding requires property 'key' or 'keycode'"
            );
        }
    }

    if (!binding->has_action) {
        return parse_error(
            context,
            "binding requires property 'action'"
        );
    }

    const bool directional =
        binding->action_kind ==
            ONYRION_ACTION_FOCUS_DIRECTION ||
        binding->action_kind ==
            ONYRION_ACTION_MOVE_DIRECTION ||
        binding->action_kind ==
            ONYRION_ACTION_RESIZE_DIRECTION;

    const bool split =
        binding->action_kind ==
            ONYRION_ACTION_SPLIT_ACTIVE;

    const bool shell_invoke =
        binding->action_kind ==
            ONYRION_ACTION_SHELL_INVOKE;

    const bool workspace_activate =
        binding->action_kind ==
            ONYRION_ACTION_WORKSPACE_ACTIVATE;

    const bool interactive_pointer =
        binding->action_kind ==
            ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE ||
        binding->action_kind ==
            ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE;

    const OnyrionKeyboardBindingTrigger trigger =
        binding->has_trigger
            ? binding->trigger
            : ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS;

    const bool standalone_release =
        trigger ==
            ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE;

    if (!pointer_binding &&
            trigger ==
                ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE) {
        return parse_error(
            context,
            "keyboard binding does not support trigger 'release'"
        );
    }

    if (pointer_binding &&
            binding->has_wheel &&
            binding->has_trigger) {
        return parse_error(
            context,
            "wheel pointer-binding does not accept property 'trigger'"
        );
    }

    if (pointer_binding &&
            trigger ==
                ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE) {
        return parse_error(
            context,
            "pointer-binding does not support standalone-release"
        );
    }

    if (interactive_pointer &&
            (!pointer_binding ||
             !binding->has_button ||
             trigger !=
                ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS)) {
        return parse_error(
            context,
            "interactive pointer action requires pointer button trigger 'press'"
        );
    }

    if (standalone_release &&
            binding->has_keycode) {
        return parse_error(
            context,
            "standalone-release requires property 'key'"
        );
    }

    if (standalone_release &&
            binding->has_key &&
            binding->keysym != XKB_KEY_Super_L &&
            binding->keysym != XKB_KEY_Super_R) {
        return parse_error(
            context,
            "standalone-release key must be Super_L or Super_R"
        );
    }

    if (standalone_release &&
            binding->has_modifiers) {
        return parse_error(
            context,
            "standalone-release does not accept property 'modifiers'"
        );
    }

    if (directional &&
            !binding->has_direction) {
        return parse_error(
            context,
            "directional action requires property 'direction'"
        );
    }

    if (!directional &&
            binding->has_direction) {
        return parse_error(
            context,
            "property 'direction' is invalid for this action"
        );
    }

    if (split &&
            !binding->has_orientation) {
        return parse_error(
            context,
            "split action requires property 'orientation'"
        );
    }

    if (!split &&
            binding->has_orientation) {
        return parse_error(
            context,
            "property 'orientation' is invalid for this action"
        );
    }

    if (shell_invoke &&
            !binding->has_capability) {
        return parse_error(
            context,
            "shell.invoke requires property 'capability'"
        );
    }

    if (shell_invoke &&
            !binding->has_shell_action) {
        return parse_error(
            context,
            "shell.invoke requires property 'shell-action'"
        );
    }

    if (!shell_invoke &&
            binding->has_capability) {
        return parse_error(
            context,
            "property 'capability' is only valid for shell.invoke"
        );
    }

    if (!shell_invoke &&
            binding->has_shell_action) {
        return parse_error(
            context,
            "property 'shell-action' is only valid for shell.invoke"
        );
    }

    if (workspace_activate &&
            !binding->has_workspace) {
        return parse_error(
            context,
            "workspace-activate requires property 'workspace'"
        );
    }

    if (!workspace_activate &&
            binding->has_workspace) {
        return parse_error(
            context,
            "property 'workspace' is only valid for workspace-activate"
        );
    }

    OnyrionActionRequest request = {
        .kind =
            binding->action_kind,
    };

    if (workspace_activate) {
        request.object_id =
            binding->workspace_id;
    }

    if (directional) {
        request.direction =
            binding->direction;
    }

    if (split) {
        request.split_orientation =
            binding->orientation;
    }

    if (shell_invoke) {
        memcpy(
            request.shell_invoke.capability,
            binding->capability,
            sizeof(
                request.shell_invoke.capability
            )
        );

        memcpy(
            request.shell_invoke.action,
            binding->shell_action,
            sizeof(
                request.shell_invoke.action
            )
        );
    }

    if (!policy_add_binding(
            &context->candidate->
                policy.input,
            (OnyrionKeyboardBinding){
                .selector =
                    gesture_binding
                        ? binding->gesture_selector
                        : (pointer_binding
                            ? (binding->has_button
                                ? ONYRION_INPUT_BINDING_POINTER_BUTTON
                                : ONYRION_INPUT_BINDING_POINTER_WHEEL)
                            : ONYRION_INPUT_BINDING_KEYBOARD),
                .keysym = binding->keysym,
                .keycode = binding->keycode,
                .button = binding->button,
                .wheel_direction =
                    binding->wheel_direction,
                .gesture_fingers =
                    binding->gesture_fingers,
                .gesture_direction =
                    binding->gesture_direction,
                .use_keycode =
                    binding->has_keycode,
                .modifiers =
                    binding->modifiers,
                .trigger = trigger,
                .modifiers_wildcard =
                    !binding->has_modifiers,
                .action = request,
            },
            context->error,
            context->error_size)) {
        return false;
    }

    return true;
}

static bool finalize_frame(
        ParseContext *context,
        ConfigFrame *frame) {
    switch (frame->kind) {
    case CONFIG_NODE_VERSION:
    case CONFIG_NODE_KEYBOARD_LAYOUT:
    case CONFIG_NODE_KEYBOARD_MODEL:
    case CONFIG_NODE_KEYBOARD_VARIANT:
    case CONFIG_NODE_KEYBOARD_OPTIONS:
    case CONFIG_NODE_REPEAT_RATE:
    case CONFIG_NODE_REPEAT_DELAY:
    case CONFIG_NODE_TOUCHPAD_NATURAL_SCROLL:
    case CONFIG_NODE_TOUCHPAD_DISABLE_WHILE_TYPING:
    case CONFIG_NODE_TOUCHPAD_CLICKFINGER:
    case CONFIG_NODE_TOUCHPAD_SCROLL_FACTOR:
    case CONFIG_NODE_POINTER_DEVICE_SENSITIVITY:
    case CONFIG_NODE_INITIAL_SPLIT_RATIO:
    case CONFIG_NODE_RESIZE_STEP:
    case CONFIG_NODE_RESIZE_MIN:
    case CONFIG_NODE_RESIZE_MAX:
    case CONFIG_NODE_OUTER_GAP:
    case CONFIG_NODE_INNER_GAP:
    case CONFIG_NODE_WORKSPACE_NAME:
    case CONFIG_NODE_WORKSPACE_ICON:
    case CONFIG_NODE_WORKSPACE_PERSISTENT:
    case CONFIG_NODE_WORKSPACE_STARTUP:
    case CONFIG_NODE_WORKSPACE_OUTPUT_AFFINITY:
    case CONFIG_NODE_RULE_APP_ID:
    case CONFIG_NODE_RULE_TITLE:
    case CONFIG_NODE_RULE_WORKSPACE:
    case CONFIG_NODE_RULE_OUTPUT:
    case CONFIG_NODE_RULE_PLACEMENT:
        if (frame->argument_count != 1) {
            return parse_error(
                context,
                "config value node requires exactly one argument"
            );
        }

        return true;

    case CONFIG_NODE_POINTER_DEVICE:
        if (frame->argument_count != 1) {
            return parse_error(
                context,
                "pointer-device requires exactly one name argument"
            );
        }

        if (!context->
                seen_pointer_device_sensitivity) {
            return parse_error(
                context,
                "pointer-device requires sensitivity"
            );
        }

        return true;

    case CONFIG_NODE_WORKSPACE:
        if (frame->argument_count != 1) {
            return parse_error(
                context,
                "workspace requires exactly one id argument"
            );
        }

        context->active_workspace_index = SIZE_MAX;
        return true;

    case CONFIG_NODE_BINDING:
    case CONFIG_NODE_POINTER_BINDING:
    case CONFIG_NODE_GESTURE_BINDING:
        if (frame->argument_count != 0) {
            return parse_error(
                context,
                "binding does not accept arguments"
            );
        }

        return finalize_binding(
            context,
            frame
        );

    case CONFIG_NODE_WINDOW_RULE:
        if (frame->argument_count != 0) {
            return parse_error(
                context,
                "window rule does not accept arguments"
            );
        }

        if (!context->seen_rule_placement) {
            return parse_error(
                context,
                "window rule requires placement"
            );
        }

        context->active_window_rule_index =
            SIZE_MAX;
        return true;

    case CONFIG_NODE_ROOT:
    case CONFIG_NODE_INPUT:
    case CONFIG_NODE_KEYBOARD:
    case CONFIG_NODE_TOUCHPAD:
    case CONFIG_NODE_BINDINGS:
    case CONFIG_NODE_LAYOUT:
    case CONFIG_NODE_WORKSPACES:
    case CONFIG_NODE_WINDOW_RULES:
        if (frame->argument_count != 0) {
            return parse_error(
                context,
                "config container does not accept arguments"
            );
        }

        return true;
    }

    return false;
}

static bool validate_action(
        const OnyrionActionRequest *request,
        char *error,
        size_t error_size) {
    if ((unsigned)request->kind >=
            (unsigned)ONYRION_ACTION_KIND_COUNT) {
        return set_error(
            error,
            error_size,
            "invalid action kind"
        );
    }

    switch (request->kind) {
    case ONYRION_ACTION_SPLIT_ACTIVE:
        if ((unsigned)request->
                    split_orientation >=
                (unsigned)
                    ONYRION_ACTION_SPLIT_ORIENTATION_COUNT) {
            return set_error(
                error,
                error_size,
                "invalid split orientation"
            );
        }
        break;

    case ONYRION_ACTION_FOCUS_DIRECTION:
    case ONYRION_ACTION_MOVE_DIRECTION:
    case ONYRION_ACTION_RESIZE_DIRECTION:
        if ((unsigned)request->direction >=
                (unsigned)ONYRION_DIRECTION_COUNT) {
            return set_error(
                error,
                error_size,
                "invalid action direction"
            );
        }
        break;

    case ONYRION_ACTION_WORKSPACE_NEXT:
    case ONYRION_ACTION_WORKSPACE_PREVIOUS:
    case ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE:
    case ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE:
    case ONYRION_ACTION_FLIP_ACTIVE_SPLIT:
    case ONYRION_ACTION_TOGGLE_FULLSCREEN_ACTIVE:
    case ONYRION_ACTION_CLOSE_ACTIVE:
    case ONYRION_ACTION_WINDOW_NEXT:
    case ONYRION_ACTION_WINDOW_PREVIOUS:
    case ONYRION_ACTION_FLOAT_ACTIVE:
    case ONYRION_ACTION_TILE_ACTIVE:
    case ONYRION_ACTION_SESSION_EXIT:
        break;

    case ONYRION_ACTION_WORKSPACE_ACTIVATE:
    case ONYRION_ACTION_GROUP_FOCUS:
    case ONYRION_ACTION_WINDOW_FOCUS:
        if (request->object_id == 0) {
            return set_error(
                error,
                error_size,
                "object action requires nonzero id"
            );
        }
        break;

    case ONYRION_ACTION_GROUP_MERGE:
    case ONYRION_ACTION_WINDOW_MOVE_TO_GROUP:
    case ONYRION_ACTION_WINDOW_SPLIT:
    case ONYRION_ACTION_WINDOW_RANGE_MOVE:
    case ONYRION_ACTION_GROUP_MOVE_TO_WORKSPACE:
    case ONYRION_ACTION_WINDOW_MOVE_TO_WORKSPACE:
    case ONYRION_ACTION_GROUP_SPLIT_AT:
    case ONYRION_ACTION_WINDOW_FLOAT:
    case ONYRION_ACTION_WINDOW_TILE:
    case ONYRION_ACTION_GROUP_FLOAT:
    case ONYRION_ACTION_GROUP_TILE:
    case ONYRION_ACTION_GROUP_SET_PINNED:
        return set_error(
            error,
            error_size,
            "object-id action is runtime-only"
        );

    case ONYRION_ACTION_SHELL_INVOKE:
        if (request->shell_invoke.capability[0] == '\0' ||
                request->shell_invoke.action[0] == '\0') {
            return set_error(
                error,
                error_size,
                "shell.invoke requires capability and shell-action"
            );
        }
        break;

    case ONYRION_ACTION_KIND_COUNT:
        return set_error(
            error,
            error_size,
            "invalid action kind"
        );
    }

    return true;
}

static bool validate_policy(
        const OnyrionCorePolicy *policy,
        char *error,
        size_t error_size) {
    const OnyrionKeyboardPolicy *keyboard =
        &policy->input.keyboard;

    struct xkb_context *xkb_context =
        xkb_context_new(
            XKB_CONTEXT_NO_FLAGS
        );

    if (!xkb_context) {
        return set_error(
            error,
            error_size,
            "failed to create XKB validation context"
        );
    }

    const struct xkb_rule_names names = {
        .model =
            keyboard->model[0]
                ? keyboard->model
                : NULL,
        .layout =
            keyboard->layout[0]
                ? keyboard->layout
                : NULL,
        .variant =
            keyboard->variant[0]
                ? keyboard->variant
                : NULL,
        .options =
            keyboard->options[0]
                ? keyboard->options
                : NULL,
    };

    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(
            xkb_context,
            &names,
            XKB_KEYMAP_COMPILE_NO_FLAGS
        );

    if (!keymap) {
        xkb_context_unref(
            xkb_context
        );

        return set_error(
            error,
            error_size,
            "invalid keyboard XKB layout/model/variant/options"
        );
    }

    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb_context);

    if (policy->input.keyboard.repeat_rate < 0 ||
            policy->input.keyboard.repeat_delay < 0) {
        return set_error(
            error,
            error_size,
            "keyboard repeat values must be non-negative"
        );
    }

    const OnyrionTouchpadPolicy *touchpad =
        &policy->input.pointer.touchpad;

    if (!isfinite(touchpad->scroll_factor) ||
            touchpad->scroll_factor <= 0.0) {
        return set_error(
            error,
            error_size,
            "input.touchpad.scroll-factor"
            " must be finite and greater than 0"
        );
    }

    const OnyrionPointerDevicePolicy *pointer_device =
        &policy->input.pointer.device;

    if (pointer_device->configured) {
        if (pointer_device->name[0] == '\0') {
            return set_error(
                error,
                error_size,
                "input.pointer-device name must not be empty"
            );
        }

        if (!pointer_device->has_sensitivity) {
            return set_error(
                error,
                error_size,
                "input.pointer-device requires sensitivity"
            );
        }

        if (!isfinite(
                pointer_device->sensitivity) ||
                pointer_device->sensitivity < -1.0 ||
                pointer_device->sensitivity > 1.0) {
            return set_error(
                error,
                error_size,
                "input.pointer-device sensitivity"
                " must be between -1 and 1"
            );
        }
    }

    const double initial =
        policy->layout.initial_split_ratio;

    const double step =
        policy->layout.resize_step;

    const double minimum =
        policy->layout.resize_min;

    const double maximum =
        policy->layout.resize_max;

    if (!isfinite(initial) ||
            initial <= 0.0 ||
            initial >= 1.0) {
        return set_error(
            error,
            error_size,
            "layout.initial-split-ratio must be between 0 and 1"
        );
    }

    if (!isfinite(step) ||
            step <= 0.0 ||
            step >= 1.0) {
        return set_error(
            error,
            error_size,
            "layout.resize-step must be between 0 and 1"
        );
    }

    if (!isfinite(minimum) ||
            minimum <= 0.0 ||
            minimum >= 1.0) {
        return set_error(
            error,
            error_size,
            "layout.resize-min must be between 0 and 1"
        );
    }

    if (!isfinite(maximum) ||
            maximum <= 0.0 ||
            maximum >= 1.0) {
        return set_error(
            error,
            error_size,
            "layout.resize-max must be between 0 and 1"
        );
    }

    if (minimum >= maximum) {
        return set_error(
            error,
            error_size,
            "layout.resize-min must be less than layout.resize-max"
        );
    }

    if (initial < minimum ||
            initial > maximum) {
        return set_error(
            error,
            error_size,
            "layout.initial-split-ratio must be within resize bounds"
        );
    }

    if (policy->layout.outer_gap < 0 ||
            policy->layout.inner_gap < 0) {
        return set_error(
            error,
            error_size,
            "layout gaps must be non-negative"
        );
    }

    for (size_t i = 0;
            i < policy->input.binding_count;
            i++) {
        const OnyrionKeyboardBinding *binding =
            &policy->input.bindings[i];

        if ((unsigned)binding->selector >=
                (unsigned)
                    ONYRION_INPUT_BINDING_SELECTOR_COUNT) {
            return set_error(
                error,
                error_size,
                "binding has invalid selector kind"
            );
        }

        switch (binding->selector) {
        case ONYRION_INPUT_BINDING_KEYBOARD:
            if ((!binding->use_keycode &&
                        binding->keysym == 0) ||
                    (binding->use_keycode &&
                        binding->keycode > KEY_MAX) ||
                    binding->button != 0 ||
                    binding->trigger ==
                        ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE) {
                return set_error(
                    error,
                    error_size,
                    "binding has invalid keyboard selector"
                );
            }
            break;

        case ONYRION_INPUT_BINDING_POINTER_BUTTON:
            if (binding->button == 0 ||
                    binding->button > KEY_MAX ||
                    binding->keysym != 0 ||
                    binding->use_keycode ||
                    binding->trigger ==
                        ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE) {
                return set_error(
                    error,
                    error_size,
                    "binding has invalid pointer button selector"
                );
            }
            break;

        case ONYRION_INPUT_BINDING_POINTER_WHEEL:
            if ((unsigned)binding->wheel_direction >=
                        (unsigned)
                            ONYRION_POINTER_WHEEL_DIRECTION_COUNT ||
                    binding->keysym != 0 ||
                    binding->use_keycode ||
                    binding->button != 0 ||
                    binding->trigger !=
                        ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS) {
                return set_error(
                    error,
                    error_size,
                    "binding has invalid pointer wheel selector"
                );
            }
            break;

        case ONYRION_INPUT_BINDING_GESTURE_SWIPE:
        case ONYRION_INPUT_BINDING_GESTURE_PINCH:
            if (binding->gesture_fingers < 2 ||
                    binding->gesture_fingers > 10 ||
                    (unsigned)binding->gesture_direction >=
                        (unsigned)ONYRION_GESTURE_DIRECTION_COUNT ||
                    binding->keysym != 0 ||
                    binding->use_keycode ||
                    binding->button != 0 ||
                    binding->modifiers != 0 ||
                    !binding->modifiers_wildcard ||
                    binding->trigger !=
                        ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS) {
                return set_error(
                    error,
                    error_size,
                    "binding has invalid gesture selector"
                );
            }

            if (binding->selector ==
                        ONYRION_INPUT_BINDING_GESTURE_SWIPE &&
                    (binding->gesture_direction == ONYRION_GESTURE_IN ||
                     binding->gesture_direction == ONYRION_GESTURE_OUT)) {
                return set_error(
                    error,
                    error_size,
                    "swipe binding has invalid gesture direction"
                );
            }

            if (binding->selector ==
                        ONYRION_INPUT_BINDING_GESTURE_PINCH &&
                    binding->gesture_direction != ONYRION_GESTURE_IN &&
                    binding->gesture_direction != ONYRION_GESTURE_OUT) {
                return set_error(
                    error,
                    error_size,
                    "pinch binding has invalid gesture direction"
                );
            }
            break;

        case ONYRION_INPUT_BINDING_SELECTOR_COUNT:
            return set_error(
                error,
                error_size,
                "binding has invalid selector kind"
            );
        }

        if ((binding->action.kind ==
                    ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE ||
                binding->action.kind ==
                    ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE) &&
                (binding->selector !=
                    ONYRION_INPUT_BINDING_POINTER_BUTTON ||
                 binding->trigger !=
                    ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS)) {
            return set_error(
                error,
                error_size,
                "interactive pointer action has invalid selector"
            );
        }

        if (binding->modifiers &
                ~(uint32_t)(
                    ONYRION_KEYBOARD_MODIFIER_SHIFT |
                    ONYRION_KEYBOARD_MODIFIER_CONTROL |
                    ONYRION_KEYBOARD_MODIFIER_ALT |
                    ONYRION_KEYBOARD_MODIFIER_SUPER)) {
            return set_error(
                error,
                error_size,
                "binding has invalid modifier mask"
            );
        }

        if (binding->modifiers_wildcard &&
                binding->modifiers != 0) {
            return set_error(
                error,
                error_size,
                "wildcard binding cannot carry explicit modifiers"
            );
        }

        if ((unsigned)binding->trigger >=
                (unsigned)
                    ONYRION_KEYBOARD_BINDING_TRIGGER_COUNT) {
            return set_error(
                error,
                error_size,
                "invalid keyboard binding trigger"
            );
        }

        if (binding->trigger ==
                ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE &&
                (binding->selector !=
                    ONYRION_INPUT_BINDING_KEYBOARD ||
                 binding->use_keycode ||
                 (binding->keysym != XKB_KEY_Super_L &&
                  binding->keysym != XKB_KEY_Super_R) ||
                 binding->modifiers != 0 ||
                 !binding->modifiers_wildcard)) {
            return set_error(
                error,
                error_size,
                "invalid standalone-release keyboard binding"
            );
        }

        if (!validate_action(
                &binding->action,
                error,
                error_size)) {
            return false;
        }
    }

    for (size_t i = 0;
            i < policy->window_rules.count;
            i++) {
        const OnyrionWindowRule *rule =
            &policy->window_rules.items[i];

        if ((unsigned)rule->placement >=
                (unsigned)ONYRION_WINDOW_RULE_PLACEMENT_COUNT) {
            return set_error(
                error,
                error_size,
                "window rule %zu has invalid placement",
                i
            );
        }

        if (rule->match_app_id &&
                rule->app_id[0] == '\0') {
            return set_error(
                error,
                error_size,
                "window rule %zu app-id cannot be empty",
                i
            );
        }

        if (rule->match_title &&
                rule->title[0] == '\0') {
            return set_error(
                error,
                error_size,
                "window rule %zu title cannot be empty",
                i
            );
        }

        if (rule->match_output &&
                rule->output[0] == '\0') {
            return set_error(
                error,
                error_size,
                "window rule %zu output cannot be empty",
                i
            );
        }

        if (rule->match_workspace &&
                rule->workspace_id == 0) {
            return set_error(
                error,
                error_size,
                "window rule %zu workspace must be nonzero",
                i
            );
        }
    }

    for (size_t i = 0;
            i < policy->workspaces.count;
            i++) {
        const OnyrionWorkspacePolicy *workspace =
            &policy->workspaces.items[i];

        if (workspace->id == 0) {
            return set_error(
                error,
                error_size,
                "workspace id must be nonzero"
            );
        }

        if (workspace->startup &&
                workspace->output_affinity[0] == '\0') {
            return set_error(
                error,
                error_size,
                "workspace %" PRIu64
                " startup=true requires output-affinity",
                workspace->id
            );
        }

        if (!workspace->startup) {
            continue;
        }

        for (size_t j = i + 1;
                j < policy->workspaces.count;
                j++) {
            const OnyrionWorkspacePolicy *other =
                &policy->workspaces.items[j];

            if (other->startup &&
                    strcmp(
                        workspace->output_affinity,
                        other->output_affinity) == 0) {
                return set_error(
                    error,
                    error_size,
                    "output-affinity '%s'"
                    " has multiple startup workspaces",
                    workspace->output_affinity
                );
            }
        }
    }

    return true;
}

static bool parse_document(
        OnyrionCoreConfig *candidate,
        const char *contents,
        size_t length,
        char *error,
        size_t error_size) {
    kdl_parser *parser =
        kdl_create_string_parser(
            (kdl_str){
                .data = contents,
                .len = length,
            },
            KDL_READ_VERSION_2
        );

    if (!parser) {
        return set_error(
            error,
            error_size,
            "failed to create KDL2 parser"
        );
    }

    ParseContext context = {
        .candidate = candidate,
        .active_workspace_index = SIZE_MAX,
        .active_window_rule_index = SIZE_MAX,
        .error = error,
        .error_size = error_size,
    };

    bool success = true;

    for (;;) {
        kdl_event_data *event =
            kdl_parser_next_event(
                parser
            );

        if (!event) {
            success =
                parse_error(
                    &context,
                    "KDL2 parser returned no event"
                );
            break;
        }

        if (event->event ==
                KDL_EVENT_PARSE_ERROR) {
            success =
                parse_error(
                    &context,
                    "KDL2 parse error"
                );
            break;
        }

        if (event->event ==
                KDL_EVENT_EOF) {
            break;
        }

        if (event->event ==
                KDL_EVENT_START_NODE) {
            if (has_annotation(
                    &event->value)) {
                success =
                    parse_error(
                        &context,
                        "type annotations are not supported"
                    );
                break;
            }

            if (context.depth >=
                    sizeof(context.frames) /
                        sizeof(context.frames[0])) {
                success =
                    parse_error(
                        &context,
                        "config nesting is too deep"
                    );
                break;
            }

            const ConfigNodeKind parent =
                context.depth == 0
                    ? CONFIG_NODE_ROOT
                    : context.frames[
                        context.depth - 1
                    ].kind;

            ConfigNodeKind kind;

            if (!begin_node(
                    &context,
                    parent,
                    event->name,
                    &kind)) {
                success = false;
                break;
            }

            ConfigFrame frame = {
                .kind = kind,
                .binding = {
                    .action_kind =
                        ONYRION_ACTION_KIND_COUNT,
                    .direction =
                        ONYRION_DIRECTION_COUNT,
                    .orientation =
                        ONYRION_ACTION_SPLIT_ORIENTATION_COUNT,
                },
            };

            context.frames[
                context.depth
            ] = frame;

            context.depth++;

            continue;
        }

        if (context.depth == 0) {
            success =
                parse_error(
                    &context,
                    "KDL event outside config node"
                );
            break;
        }

        ConfigFrame *frame =
            &context.frames[
                context.depth - 1
            ];

        if (event->event ==
                KDL_EVENT_ARGUMENT) {
            if (!parse_argument(
                    &context,
                    frame,
                    &event->value)) {
                success = false;
                break;
            }

            continue;
        }

        if (event->event ==
                KDL_EVENT_PROPERTY) {
            if (frame->kind !=
                    CONFIG_NODE_BINDING &&
                frame->kind !=
                    CONFIG_NODE_POINTER_BINDING &&
                frame->kind !=
                    CONFIG_NODE_GESTURE_BINDING) {
                success =
                    parse_error(
                        &context,
                        "properties are only valid on binding nodes"
                    );
                break;
            }

            if (!parse_binding_property(
                    &context,
                    frame,
                    event->name,
                    &event->value)) {
                success = false;
                break;
            }

            continue;
        }

        if (event->event ==
                KDL_EVENT_END_NODE) {
            if (!finalize_frame(
                    &context,
                    frame)) {
                success = false;
                break;
            }

            context.depth--;
            continue;
        }

        success =
            parse_error(
                &context,
                "unsupported KDL parser event"
            );
        break;
    }

    kdl_destroy_parser(parser);

    if (!success) {
        return false;
    }

    if (context.depth != 0) {
        return set_error(
            error,
            error_size,
            "unterminated config node"
        );
    }

    if (!context.seen_version) {
        return set_error(
            error,
            error_size,
            "config requires 'version 1'"
        );
    }

    return validate_policy(
        &candidate->policy,
        error,
        error_size
    );
}

bool onyrion_core_config_load(
        OnyrionCoreConfig *config,
        const char *path,
        bool allow_missing,
        char *error,
        size_t error_size) {
    if (!config || !path) {
        return set_error(
            error,
            error_size,
            "invalid config load arguments"
        );
    }

    OnyrionCoreConfig candidate = {0};

    if (!config_set_defaults(
            &candidate,
            error,
            error_size)) {
        onyrion_core_config_finish(
            &candidate
        );

        return false;
    }

    char *contents = NULL;
    size_t length = 0;

    const ConfigReadStatus status =
        read_config_file(
            path,
            &contents,
            &length,
            error,
            error_size
        );

    if (status ==
            CONFIG_READ_MISSING) {
        if (!allow_missing) {
            onyrion_core_config_finish(
                &candidate
            );

            return set_error(
                error,
                error_size,
                "config file not found: %s",
                path
            );
        }

        onyrion_core_config_finish(
            config
        );

        *config = candidate;

        return true;
    }

    if (status ==
            CONFIG_READ_ERROR) {
        onyrion_core_config_finish(
            &candidate
        );

        return false;
    }

    const bool parsed =
        parse_document(
            &candidate,
            contents,
            length,
            error,
            error_size
        );

    if (!parsed) {
        free(contents);
        onyrion_core_config_finish(
            &candidate
        );

        return false;
    }

    candidate.source_path =
        strdup(path);

    if (!candidate.source_path) {
        free(contents);
        onyrion_core_config_finish(&candidate);

        return set_error(
            error,
            error_size,
            "out of memory"
        );
    }

    candidate.loaded_from_file = true;
    candidate.source =
        ONYRION_CONFIG_SOURCE_CURRENT;
    candidate.source_text = contents;
    candidate.source_length = length;

    onyrion_core_config_finish(
        config
    );

    *config = candidate;

    return true;
}

bool onyrion_core_config_load_defaults(
        OnyrionCoreConfig *config,
        char *error,
        size_t error_size) {
    if (!config) {
        return set_error(
            error,
            error_size,
            "invalid defaults target"
        );
    }

    OnyrionCoreConfig candidate = {0};

    if (!config_set_defaults(
            &candidate,
            error,
            error_size)) {
        return false;
    }

    onyrion_core_config_finish(config);
    *config = candidate;

    return true;
}

char *onyrion_core_config_default_path(
        char *error,
        size_t error_size) {
    const char *base =
        getenv(
            "XDG_CONFIG_HOME"
        );

    const char *suffix =
        "/onyrion/compositor.kdl";

    char *owned_base = NULL;

    if (!base || !*base) {
        const char *home =
            getenv("HOME");

        if (!home || !*home) {
            set_error(
                error,
                error_size,
                "HOME is unset and XDG_CONFIG_HOME is unavailable"
            );

            return NULL;
        }

        const size_t home_length =
            strlen(home);

        const char *config_suffix =
            "/.config";

        const size_t config_length =
            strlen(config_suffix);

        if (home_length >
                SIZE_MAX -
                    config_length - 1) {
            set_error(
                error,
                error_size,
                "config path is too long"
            );

            return NULL;
        }

        owned_base =
            malloc(
                home_length +
                config_length +
                1
            );

        if (!owned_base) {
            set_error(
                error,
                error_size,
                "out of memory"
            );

            return NULL;
        }

        memcpy(
            owned_base,
            home,
            home_length
        );

        memcpy(
            owned_base + home_length,
            config_suffix,
            config_length + 1
        );

        base = owned_base;
    }

    const size_t base_length =
        strlen(base);

    const size_t suffix_length =
        strlen(suffix);

    if (base_length >
            SIZE_MAX -
                suffix_length - 1) {
        free(owned_base);

        set_error(
            error,
            error_size,
            "config path is too long"
        );

        return NULL;
    }

    char *path =
        malloc(
            base_length +
            suffix_length +
            1
        );

    if (!path) {
        free(owned_base);

        set_error(
            error,
            error_size,
            "out of memory"
        );

        return NULL;
    }

    memcpy(
        path,
        base,
        base_length
    );

    memcpy(
        path + base_length,
        suffix,
        suffix_length + 1
    );

    free(owned_base);

    return path;
}

OnyrionKeyboardBindingMatch
onyrion_core_policy_match_binding(
        const OnyrionCorePolicy *policy,
        uint32_t keysym,
        uint32_t keycode,
        uint32_t modifiers,
        const OnyrionKeyboardBinding **binding) {
    if (binding) {
        *binding = NULL;
    }

    if (!policy ||
            !binding) {
        return
            ONYRION_KEYBOARD_BINDING_MATCH_NONE;
    }

    const OnyrionKeyboardBinding *wildcard =
        NULL;

    size_t active_specific_bindings = 0;

    for (size_t i = 0;
            i < policy->input.binding_count;
            i++) {
        const OnyrionKeyboardBinding *candidate =
            &policy->input.bindings[i];

        if (candidate->selector !=
                    ONYRION_INPUT_BINDING_KEYBOARD ||
                candidate->trigger !=
                    ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS) {
            continue;
        }

        if (candidate->use_keycode
                ? candidate->keycode != keycode
                : candidate->keysym != keysym) {
            continue;
        }

        if (candidate->modifiers_wildcard) {
            wildcard = candidate;
            continue;
        }

        if (candidate->modifiers ==
                modifiers) {
            *binding = candidate;

            return
                ONYRION_KEYBOARD_BINDING_MATCH_ACTION;
        }

        if (candidate->modifiers != 0 &&
                (candidate->modifiers &
                    modifiers) ==
                    candidate->modifiers) {
            active_specific_bindings++;
        }
    }

    if (wildcard) {
        *binding = wildcard;

        return
            ONYRION_KEYBOARD_BINDING_MATCH_ACTION;
    }

    if (active_specific_bindings >= 2) {
        return
            ONYRION_KEYBOARD_BINDING_MATCH_CONSUMED;
    }

    return
        ONYRION_KEYBOARD_BINDING_MATCH_NONE;
}


const OnyrionKeyboardBinding *
onyrion_core_policy_match_standalone_modifier(
        const OnyrionCorePolicy *policy,
        uint32_t keysym) {
    if (!policy ||
            (keysym != XKB_KEY_Super_L &&
             keysym != XKB_KEY_Super_R)) {
        return NULL;
    }

    for (size_t i = 0;
            i < policy->input.binding_count;
            i++) {
        const OnyrionKeyboardBinding *candidate =
            &policy->input.bindings[i];

        if (candidate->selector !=
                    ONYRION_INPUT_BINDING_KEYBOARD ||
                candidate->trigger !=
                    ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE ||
                candidate->use_keycode ||
                candidate->keysym != keysym) {
            continue;
        }

        return candidate;
    }

    return NULL;
}


static bool binding_modifiers_match(
        const OnyrionKeyboardBinding *binding,
        uint32_t modifiers) {
    return
        binding->modifiers_wildcard ||
        binding->modifiers == modifiers;
}

const OnyrionKeyboardBinding *
onyrion_core_policy_match_pointer_button(
        const OnyrionCorePolicy *policy,
        uint32_t button,
        OnyrionKeyboardBindingTrigger trigger,
        uint32_t modifiers) {
    if (!policy ||
            (trigger !=
                    ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS &&
             trigger !=
                    ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE)) {
        return NULL;
    }

    const OnyrionKeyboardBinding *wildcard = NULL;

    for (size_t i = 0;
            i < policy->input.binding_count;
            i++) {
        const OnyrionKeyboardBinding *candidate =
            &policy->input.bindings[i];

        if (candidate->selector !=
                    ONYRION_INPUT_BINDING_POINTER_BUTTON ||
                candidate->button != button ||
                candidate->trigger != trigger) {
            continue;
        }

        if (candidate->modifiers_wildcard) {
            wildcard = candidate;
            continue;
        }

        if (binding_modifiers_match(
                candidate,
                modifiers)) {
            return candidate;
        }
    }

    return wildcard;
}

const OnyrionKeyboardBinding *
onyrion_core_policy_match_pointer_wheel(
        const OnyrionCorePolicy *policy,
        OnyrionPointerWheelDirection direction,
        uint32_t modifiers) {
    if (!policy ||
            (unsigned)direction >=
                (unsigned)
                    ONYRION_POINTER_WHEEL_DIRECTION_COUNT) {
        return NULL;
    }

    const OnyrionKeyboardBinding *wildcard = NULL;

    for (size_t i = 0;
            i < policy->input.binding_count;
            i++) {
        const OnyrionKeyboardBinding *candidate =
            &policy->input.bindings[i];

        if (candidate->selector !=
                    ONYRION_INPUT_BINDING_POINTER_WHEEL ||
                candidate->wheel_direction != direction) {
            continue;
        }

        if (candidate->modifiers_wildcard) {
            wildcard = candidate;
            continue;
        }

        if (binding_modifiers_match(
                candidate,
                modifiers)) {
            return candidate;
        }
    }

    return wildcard;
}

const OnyrionKeyboardBinding *
onyrion_core_policy_match_gesture(
        const OnyrionCorePolicy *policy,
        OnyrionInputBindingSelector selector,
        uint32_t fingers,
        OnyrionGestureDirection direction) {
    if (!policy ||
            (selector != ONYRION_INPUT_BINDING_GESTURE_SWIPE &&
             selector != ONYRION_INPUT_BINDING_GESTURE_PINCH) ||
            fingers < 2 ||
            fingers > 10 ||
            (unsigned)direction >=
                (unsigned)ONYRION_GESTURE_DIRECTION_COUNT) {
        return NULL;
    }

    for (size_t i = 0;
            i < policy->input.binding_count;
            i++) {
        const OnyrionKeyboardBinding *candidate =
            &policy->input.bindings[i];

        if (candidate->selector == selector &&
                candidate->gesture_fingers == fingers &&
                candidate->gesture_direction == direction) {
            return candidate;
        }
    }

    return NULL;
}

bool onyrion_core_policy_match_window_rule(
        const OnyrionCorePolicy *policy,
        const char *app_id,
        const char *title,
        uint64_t workspace_id,
        const char *output,
        OnyrionWindowRulePlacement *placement,
        size_t *rule_index) {
    if (!policy) {
        return false;
    }

    for (size_t i = 0;
            i < policy->window_rules.count;
            i++) {
        const OnyrionWindowRule *rule =
            &policy->window_rules.items[i];

        if (rule->match_app_id &&
                (!app_id ||
                 strcmp(
                    rule->app_id,
                    app_id) != 0)) {
            continue;
        }

        if (rule->match_title &&
                (!title ||
                 strcmp(
                    rule->title,
                    title) != 0)) {
            continue;
        }

        if (rule->match_workspace &&
                rule->workspace_id !=
                    workspace_id) {
            continue;
        }

        if (rule->match_output &&
                (!output ||
                 strcmp(
                    rule->output,
                    output) != 0)) {
            continue;
        }

        if (placement) {
            *placement =
                rule->placement;
        }

        if (rule_index) {
            *rule_index = i;
        }

        return true;
    }

    return false;
}
