#pragma once

#include "action.h"
#include "compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum onyrion_config_source {
    ONYRION_CONFIG_SOURCE_DEFAULTS,
    ONYRION_CONFIG_SOURCE_CURRENT,
    ONYRION_CONFIG_SOURCE_LEGACY,
    ONYRION_CONFIG_SOURCE_LKG,
    ONYRION_CONFIG_SOURCE_INSTALLED,
    ONYRION_CONFIG_SOURCE_EXPLICIT,
    ONYRION_CONFIG_SOURCE_COUNT,
} OnyrionConfigSource;

typedef enum onyrion_keyboard_modifier {
    ONYRION_KEYBOARD_MODIFIER_SHIFT = 1u << 0,
    ONYRION_KEYBOARD_MODIFIER_CONTROL = 1u << 1,
    ONYRION_KEYBOARD_MODIFIER_ALT = 1u << 2,
    ONYRION_KEYBOARD_MODIFIER_SUPER = 1u << 3,
} OnyrionKeyboardModifier;

typedef enum onyrion_keyboard_binding_match {
    ONYRION_KEYBOARD_BINDING_MATCH_NONE,
    ONYRION_KEYBOARD_BINDING_MATCH_ACTION,
    ONYRION_KEYBOARD_BINDING_MATCH_CONSUMED,
} OnyrionKeyboardBindingMatch;

typedef enum onyrion_input_binding_selector {
    ONYRION_INPUT_BINDING_KEYBOARD,
    ONYRION_INPUT_BINDING_POINTER_BUTTON,
    ONYRION_INPUT_BINDING_POINTER_WHEEL,
    ONYRION_INPUT_BINDING_GESTURE_SWIPE,
    ONYRION_INPUT_BINDING_GESTURE_PINCH,
    ONYRION_INPUT_BINDING_SELECTOR_COUNT,
} OnyrionInputBindingSelector;

typedef enum onyrion_pointer_wheel_direction {
    ONYRION_POINTER_WHEEL_UP,
    ONYRION_POINTER_WHEEL_DOWN,
    ONYRION_POINTER_WHEEL_DIRECTION_COUNT,
} OnyrionPointerWheelDirection;

typedef enum onyrion_gesture_direction {
    ONYRION_GESTURE_LEFT,
    ONYRION_GESTURE_RIGHT,
    ONYRION_GESTURE_UP,
    ONYRION_GESTURE_DOWN,
    ONYRION_GESTURE_IN,
    ONYRION_GESTURE_OUT,
    ONYRION_GESTURE_DIRECTION_COUNT,
} OnyrionGestureDirection;

typedef enum onyrion_keyboard_binding_trigger {
    ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS,
    ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE,
    ONYRION_KEYBOARD_BINDING_TRIGGER_STANDALONE_RELEASE,
    ONYRION_KEYBOARD_BINDING_TRIGGER_COUNT,
} OnyrionKeyboardBindingTrigger;

typedef struct onyrion_keyboard_binding {
    OnyrionInputBindingSelector selector;
    uint32_t keysym;
    uint32_t keycode;
    uint32_t button;
    OnyrionPointerWheelDirection wheel_direction;
    uint32_t gesture_fingers;
    OnyrionGestureDirection gesture_direction;
    uint32_t modifiers;
    OnyrionKeyboardBindingTrigger trigger;
    bool use_keycode;
    bool modifiers_wildcard;
    OnyrionActionRequest action;
} OnyrionKeyboardBinding;

typedef struct onyrion_keyboard_policy {
    char layout[64];
    char model[64];
    char variant[64];
    char options[256];

    int repeat_rate;
    int repeat_delay;
} OnyrionKeyboardPolicy;

typedef struct onyrion_touchpad_policy {
    bool has_natural_scroll;
    bool natural_scroll;

    bool has_disable_while_typing;
    bool disable_while_typing;

    bool has_clickfinger;
    bool clickfinger;

    bool has_scroll_factor;
    double scroll_factor;
} OnyrionTouchpadPolicy;

typedef struct onyrion_pointer_device_policy {
    bool configured;
    char name[128];

    bool has_sensitivity;
    double sensitivity;
} OnyrionPointerDevicePolicy;

typedef struct onyrion_pointer_policy {
    OnyrionTouchpadPolicy touchpad;
    OnyrionPointerDevicePolicy device;
} OnyrionPointerPolicy;

typedef struct onyrion_input_policy {
    OnyrionKeyboardPolicy keyboard;
    OnyrionPointerPolicy pointer;

    OnyrionKeyboardBinding *bindings;
    size_t binding_count;
    size_t binding_capacity;
} OnyrionInputPolicy;

typedef struct onyrion_layout_policy {
    double initial_split_ratio;
    double resize_step;
    double resize_min;
    double resize_max;
    int outer_gap;
    int inner_gap;
} OnyrionLayoutPolicy;

typedef struct onyrion_workspace_policy {
    uint64_t id;
    char name[64];
    char icon[64];
    bool persistent;
    bool startup;
    char output_affinity[128];
} OnyrionWorkspacePolicy;

typedef struct onyrion_workspace_policy_set {
    OnyrionWorkspacePolicy *items;
    size_t count;
    size_t capacity;
} OnyrionWorkspacePolicySet;

typedef enum onyrion_window_rule_placement {
    ONYRION_WINDOW_RULE_PLACEMENT_TILED,
    ONYRION_WINDOW_RULE_PLACEMENT_FLOATING,
    ONYRION_WINDOW_RULE_PLACEMENT_COUNT,
} OnyrionWindowRulePlacement;

typedef struct onyrion_window_rule {
    char app_id[256];
    char title[256];
    char output[128];
    uint64_t workspace_id;

    bool match_app_id;
    bool match_title;
    bool match_output;
    bool match_workspace;

    OnyrionWindowRulePlacement placement;
} OnyrionWindowRule;

typedef struct onyrion_window_rule_set {
    OnyrionWindowRule *items;
    size_t count;
    size_t capacity;
} OnyrionWindowRuleSet;

typedef struct onyrion_core_policy {
    OnyrionInputPolicy input;
    OnyrionLayoutPolicy layout;
    OnyrionWorkspacePolicySet workspaces;
    OnyrionWindowRuleSet window_rules;
} OnyrionCorePolicy;

typedef struct onyrion_core_config {
    unsigned version;
    bool loaded_from_file;
    bool explicit_config;
    OnyrionConfigSource source;

    char *source_path;
    char *source_text;
    size_t source_length;

    OnyrionCorePolicy policy;
} OnyrionCoreConfig;

void onyrion_core_config_finish(
    OnyrionCoreConfig *config
);

[[nodiscard]]
bool onyrion_core_config_load(
    OnyrionCoreConfig *config,
    const char *path,
    bool allow_missing,
    char *error,
    size_t error_size
);

[[nodiscard]]
bool onyrion_core_config_load_defaults(
    OnyrionCoreConfig *config,
    char *error,
    size_t error_size
);

[[nodiscard]]
bool onyrion_core_config_load_startup(
    OnyrionCoreConfig *config,
    const char *explicit_path,
    bool explicit_config,
    bool migrate_legacy,
    char **watch_path,
    char *error,
    size_t error_size
);

[[nodiscard]]
bool onyrion_core_config_persist_lkg(
    const OnyrionCoreConfig *config,
    char *error,
    size_t error_size
);

[[nodiscard]]
char *onyrion_core_config_default_path(
    char *error,
    size_t error_size
);

[[nodiscard]]
char *onyrion_core_config_legacy_path(
    char *error,
    size_t error_size
);

[[nodiscard]]
char *onyrion_core_config_state_dir(
    char *error,
    size_t error_size
);

[[nodiscard]]
const char *onyrion_core_config_source_name(
    OnyrionConfigSource source
);

[[nodiscard]]
OnyrionKeyboardBindingMatch onyrion_core_policy_match_binding(
    const OnyrionCorePolicy *policy,
    uint32_t keysym,
    uint32_t keycode,
    uint32_t modifiers,
    const OnyrionKeyboardBinding **binding
);

[[nodiscard]]
const OnyrionKeyboardBinding *
onyrion_core_policy_match_standalone_modifier(
    const OnyrionCorePolicy *policy,
    uint32_t keysym
);

[[nodiscard]]
const OnyrionKeyboardBinding *
onyrion_core_policy_match_pointer_button(
    const OnyrionCorePolicy *policy,
    uint32_t button,
    OnyrionKeyboardBindingTrigger trigger,
    uint32_t modifiers
);

[[nodiscard]]
const OnyrionKeyboardBinding *
onyrion_core_policy_match_pointer_wheel(
    const OnyrionCorePolicy *policy,
    OnyrionPointerWheelDirection direction,
    uint32_t modifiers
);

[[nodiscard]]
const OnyrionKeyboardBinding *
onyrion_core_policy_match_gesture(
    const OnyrionCorePolicy *policy,
    OnyrionInputBindingSelector selector,
    uint32_t fingers,
    OnyrionGestureDirection direction
);

[[nodiscard]]
bool onyrion_core_policy_match_window_rule(
    const OnyrionCorePolicy *policy,
    const char *app_id,
    const char *title,
    uint64_t workspace_id,
    const char *output,
    OnyrionWindowRulePlacement *placement,
    size_t *rule_index
);
