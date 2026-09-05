#include "action.h"
#include "config.h"
#include "fallback.h"
#include "input.h"
#include "group.h"
#include "layout.h"
#include "output.h"
#include "layer_shell.h"
#include "recovery.h"
#include "server.h"
#include "session_lock.h"
#include "window.h"
#include "workspace.h"

#include <ctype.h>
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <libinput.h>

#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

typedef struct onyrion_standalone_modifier_state {
    bool down;
    bool armed;
    uint32_t keycode;
    OnyrionActionRequest action;
} OnyrionStandaloneModifierState;

enum {
    ONYRION_STANDALONE_RELEASE_DEVICE_GRACE_MS = 50,
};

typedef struct onyrion_keyboard {
    OnyrionServer *server;
    struct wlr_keyboard *wlr_keyboard;

    OnyrionStandaloneModifierState standalone_super_left;
    OnyrionStandaloneModifierState standalone_super_right;

    struct wl_event_source *standalone_release_timer;
    bool standalone_release_pending;
    OnyrionActionRequest standalone_release_action;
    xkb_keysym_t standalone_release_keysym;
    uint32_t standalone_release_keycode;

    struct wl_list link;

    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;
} OnyrionKeyboard;

enum {
    ONYRION_POINTER_BUTTON_CAPTURE_CAPACITY = 8,
};

typedef struct onyrion_pointer_button_capture {
    bool active;
    uint32_t button;
    bool has_release_action;
    OnyrionActionRequest release_action;
} OnyrionPointerButtonCapture;

typedef enum onyrion_gesture_kind {
    ONYRION_GESTURE_NONE,
    ONYRION_GESTURE_SWIPE,
    ONYRION_GESTURE_PINCH,
} OnyrionGestureKind;

typedef enum onyrion_gesture_axis_lock {
    ONYRION_GESTURE_LOCK_NONE,
    ONYRION_GESTURE_LOCK_HORIZONTAL,
    ONYRION_GESTURE_LOCK_VERTICAL,
} OnyrionGestureAxisLock;

typedef struct onyrion_gesture_recognizer {
    OnyrionServer *server;
    OnyrionGestureKind kind;
    OnyrionGestureAxisLock axis_lock;
    uint32_t fingers;
    double dx;
    double dy;
    double scale;
    bool active;
    bool cancelled;

    struct wl_listener swipe_begin;
    struct wl_listener swipe_update;
    struct wl_listener swipe_end;
    struct wl_listener pinch_begin;
    struct wl_listener pinch_update;
    struct wl_listener pinch_end;
} OnyrionGestureRecognizer;

enum {
    ONYRION_GESTURE_DIRECTION_LOCK_THRESHOLD = 24,
    ONYRION_GESTURE_AMBIGUOUS_CANCEL_THRESHOLD = 72,
    ONYRION_GESTURE_SWIPE_COMMIT_THRESHOLD = 120,
};

static const double ONYRION_GESTURE_DIRECTION_LOCK_RATIO = 1.5;
static const double ONYRION_GESTURE_PINCH_COMMIT_THRESHOLD = 0.18;

typedef struct onyrion_pointer {
    OnyrionServer *server;
    struct wlr_input_device *device;

    bool touchpad;
    bool sensitivity_overridden;
    double scroll_factor;

    OnyrionPointerButtonCapture button_captures[
        ONYRION_POINTER_BUTTON_CAPTURE_CAPACITY
    ];

    struct wl_list link;
    struct wl_listener destroy;
} OnyrionPointer;

static uint32_t keyboard_modifier_mask(
    OnyrionKeyboard *keyboard
);

static void normalize_pointer_device_name(
        const char *name,
        char *normalized,
        size_t capacity) {
    if (!normalized || capacity == 0) {
        return;
    }

    normalized[0] = '\0';

    if (!name) {
        return;
    }

    size_t out = 0;
    bool separator = false;

    for (const unsigned char *cursor =
            (const unsigned char *)name;
            *cursor != '\0' &&
            out + 1 < capacity;
            cursor++) {
        const unsigned char ch = *cursor;

        if (isalnum(ch) ||
                ch == '/' ||
                ch == '.') {
            if (separator &&
                    out > 0 &&
                    normalized[out - 1] != '-' &&
                    out + 1 < capacity) {
                normalized[out++] = '-';
            }

            normalized[out++] =
                (char)tolower(ch);
            separator = false;
            continue;
        }

        if (ch == '-') {
            if (out > 0 &&
                    normalized[out - 1] != '-') {
                normalized[out++] = '-';
            }

            separator = false;
            continue;
        }

        separator = out > 0;
    }

    while (out > 0 &&
            normalized[out - 1] == '-') {
        out--;
    }

    normalized[out] = '\0';
}

static bool pointer_name_matches(
        struct wlr_input_device *device,
        const char *policy_name) {
    if (!device ||
            !device->name ||
            !policy_name ||
            policy_name[0] == '\0') {
        return false;
    }

    char actual[128] = {0};
    char expected[128] = {0};

    normalize_pointer_device_name(
        device->name,
        actual,
        sizeof(actual)
    );

    normalize_pointer_device_name(
        policy_name,
        expected,
        sizeof(expected)
    );

    return
        actual[0] != '\0' &&
        expected[0] != '\0' &&
        strcmp(actual, expected) == 0;
}

static bool libinput_device_is_touchpad_class(
        struct libinput_device *device) {
    if (!device) {
        return false;
    }

    const uint32_t click_methods =
        libinput_device_config_click_get_methods(
            device
        );

    return
        libinput_device_config_dwt_is_available(
            device
        ) != 0 ||
        (click_methods &
            LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER) != 0 ||
        libinput_device_config_tap_get_finger_count(
            device
        ) > 0;
}

static void log_libinput_skip(
        struct wlr_input_device *device,
        const char *property) {
    wlr_log(
        WLR_DEBUG,
        "Pointer policy skipped unsupported property:"
        " device=%s property=%s",
        device && device->name
            ? device->name
            : "<unnamed>",
        property ? property : "<unknown>"
    );
}

static bool libinput_status_accepted(
        struct wlr_input_device *device,
        const char *property,
        enum libinput_config_status status) {
    if (status ==
            LIBINPUT_CONFIG_STATUS_SUCCESS) {
        return true;
    }

    if (status ==
            LIBINPUT_CONFIG_STATUS_UNSUPPORTED) {
        log_libinput_skip(
            device,
            property
        );

        return true;
    }

    wlr_log(
        WLR_ERROR,
        "Pointer policy apply failed:"
        " device=%s property=%s status=%s",
        device && device->name
            ? device->name
            : "<unnamed>",
        property ? property : "<unknown>",
        libinput_config_status_to_str(status)
    );

    return false;
}

static bool apply_pointer_policy_one(
        OnyrionPointer *pointer,
        const OnyrionInputPolicy *policy) {
    if (!pointer ||
            !pointer->device ||
            !policy) {
        return false;
    }

    pointer->scroll_factor = 1.0;

    if (!wlr_input_device_is_libinput(
            pointer->device)) {
        pointer->touchpad = false;
        pointer->sensitivity_overridden = false;

        wlr_log(
            WLR_DEBUG,
            "Pointer policy skipped non-libinput device: %s",
            pointer->device->name
                ? pointer->device->name
                : "<unnamed>"
        );

        return true;
    }

    struct libinput_device *libinput_device =
        wlr_libinput_get_device_handle(
            pointer->device
        );

    if (!libinput_device) {
        wlr_log(
            WLR_ERROR,
            "Pointer policy libinput handle unavailable: %s",
            pointer->device->name
                ? pointer->device->name
                : "<unnamed>"
        );

        return false;
    }

    pointer->touchpad =
        libinput_device_is_touchpad_class(
            libinput_device
        );

    const OnyrionTouchpadPolicy *touchpad =
        &policy->pointer.touchpad;

    if (pointer->touchpad) {
        if (libinput_device_config_scroll_has_natural_scroll(
                libinput_device)) {
            const int enabled =
                touchpad->has_natural_scroll
                    ? touchpad->natural_scroll
                    : libinput_device_config_scroll_get_default_natural_scroll_enabled(
                        libinput_device
                    );

            if (!libinput_status_accepted(
                    pointer->device,
                    "natural-scroll",
                    libinput_device_config_scroll_set_natural_scroll_enabled(
                        libinput_device,
                        enabled))) {
                return false;
            }
        } else if (touchpad->has_natural_scroll) {
            log_libinput_skip(
                pointer->device,
                "natural-scroll"
            );
        }

        if (libinput_device_config_dwt_is_available(
                libinput_device)) {
            const enum libinput_config_dwt_state state =
                touchpad->has_disable_while_typing
                    ? (touchpad->disable_while_typing
                        ? LIBINPUT_CONFIG_DWT_ENABLED
                        : LIBINPUT_CONFIG_DWT_DISABLED)
                    : libinput_device_config_dwt_get_default_enabled(
                        libinput_device
                    );

            if (!libinput_status_accepted(
                    pointer->device,
                    "disable-while-typing",
                    libinput_device_config_dwt_set_enabled(
                        libinput_device,
                        state))) {
                return false;
            }
        } else if (touchpad->
                has_disable_while_typing) {
            log_libinput_skip(
                pointer->device,
                "disable-while-typing"
            );
        }

        const uint32_t click_methods =
            libinput_device_config_click_get_methods(
                libinput_device
            );

        enum libinput_config_click_method click_method =
            libinput_device_config_click_get_default_method(
                libinput_device
            );

        if (touchpad->has_clickfinger) {
            click_method =
                touchpad->clickfinger
                    ? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
                    : LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
        }

        if (!touchpad->has_clickfinger ||
                (click_methods & click_method) != 0 ||
                click_method ==
                    LIBINPUT_CONFIG_CLICK_METHOD_NONE) {
            if (!libinput_status_accepted(
                    pointer->device,
                    "clickfinger",
                    libinput_device_config_click_set_method(
                        libinput_device,
                        click_method))) {
                return false;
            }
        } else {
            log_libinput_skip(
                pointer->device,
                "clickfinger"
            );
        }

        pointer->scroll_factor =
            touchpad->has_scroll_factor
                ? touchpad->scroll_factor
                : 1.0;
    }

    const OnyrionPointerDevicePolicy *device_policy =
        &policy->pointer.device;

    const bool override_matches =
        device_policy->configured &&
        pointer_name_matches(
            pointer->device,
            device_policy->name
        );

    if (override_matches &&
            device_policy->has_sensitivity) {
        if (!libinput_device_config_accel_is_available(
                libinput_device)) {
            log_libinput_skip(
                pointer->device,
                "sensitivity"
            );
        } else {
            if (!libinput_status_accepted(
                    pointer->device,
                    "sensitivity",
                    libinput_device_config_accel_set_speed(
                        libinput_device,
                        device_policy->sensitivity))) {
                return false;
            }

            pointer->sensitivity_overridden = true;
        }
    } else if (pointer->sensitivity_overridden) {
        if (libinput_device_config_accel_is_available(
                libinput_device)) {
            if (!libinput_status_accepted(
                    pointer->device,
                    "sensitivity-reset",
                    libinput_device_config_accel_set_speed(
                        libinput_device,
                        libinput_device_config_accel_get_default_speed(
                            libinput_device)))) {
                return false;
            }
        }

        pointer->sensitivity_overridden = false;
    }

    wlr_log(
        WLR_INFO,
        "Pointer policy applied:"
        " device=%s touchpad=%s"
        " scroll_factor=%.6f"
        " sensitivity_override=%s",
        pointer->device->name
            ? pointer->device->name
            : "<unnamed>",
        pointer->touchpad ? "yes" : "no",
        pointer->scroll_factor,
        pointer->sensitivity_overridden
            ? "yes"
            : "no"
    );

    return true;
}

bool onyrion_input_apply_pointer_policy(
        struct onyrion_server *server,
        const struct onyrion_input_policy *policy) {
    if (!server ||
            !server->policy ||
            !policy) {
        return false;
    }

    OnyrionPointer *pointer;

    wl_list_for_each(
            pointer,
            &server->pointers,
            link) {
        if (!apply_pointer_policy_one(
                pointer,
                policy)) {
            wlr_log(
                WLR_ERROR,
                "Pointer policy candidate apply failed;"
                " restoring last-known-good pointer policy"
            );

            OnyrionPointer *rollback_pointer;

            wl_list_for_each(
                    rollback_pointer,
                    &server->pointers,
                    link) {
                if (!apply_pointer_policy_one(
                        rollback_pointer,
                        &server->policy->input)) {
                    wlr_log(
                        WLR_ERROR,
                        "Pointer policy rollback failed:"
                        " device=%s",
                        rollback_pointer->device &&
                        rollback_pointer->device->name
                            ? rollback_pointer->device->name
                            : "<unnamed>"
                    );
                }
            }

            return false;
        }
    }

    return true;
}

static double pointer_axis_scroll_factor(
        OnyrionServer *server,
        struct wlr_pointer *wlr_pointer) {
    if (!server || !wlr_pointer) {
        return 1.0;
    }

    OnyrionPointer *pointer;

    wl_list_for_each(
            pointer,
            &server->pointers,
            link) {
        if (wlr_pointer_from_input_device(
                pointer->device) ==
                wlr_pointer) {
            return pointer->scroll_factor > 0.0
                ? pointer->scroll_factor
                : 1.0;
        }
    }

    return 1.0;
}

static OnyrionWindow *active_window(
        OnyrionServer *server) {
    if (!server->active_group) {
        return NULL;
    }

    return server->active_group->active;
}

static void update_seat_capabilities(
        OnyrionServer *server) {
    uint32_t capabilities = 0;

    if (!wl_list_empty(&server->keyboards)) {
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    }

    if (!wl_list_empty(&server->pointers)) {
        capabilities |= WL_SEAT_CAPABILITY_POINTER;
    }

    wlr_seat_set_capabilities(
        server->seat,
        capabilities
    );
}

static OnyrionStandaloneModifierState *
standalone_modifier_state(
        OnyrionKeyboard *keyboard,
        xkb_keysym_t keysym) {
    if (!keyboard) {
        return NULL;
    }

    if (keysym == XKB_KEY_Super_L) {
        return &keyboard->standalone_super_left;
    }

    if (keysym == XKB_KEY_Super_R) {
        return &keyboard->standalone_super_right;
    }

    return NULL;
}

static void reset_standalone_modifier_state(
        OnyrionStandaloneModifierState *state) {
    if (!state) {
        return;
    }

    *state =
        (OnyrionStandaloneModifierState){0};
}

static void cancel_standalone_modifier_arms(
        OnyrionServer *server,
        const char *reason) {
    if (!server) {
        return;
    }

    OnyrionKeyboard *keyboard;

    wl_list_for_each(
            keyboard,
            &server->keyboards,
            link) {
        OnyrionStandaloneModifierState *states[] = {
            &keyboard->standalone_super_left,
            &keyboard->standalone_super_right,
        };

        for (size_t i = 0;
                i < sizeof(states) /
                    sizeof(states[0]);
                i++) {
            OnyrionStandaloneModifierState *state =
                states[i];

            if (!state->armed) {
                continue;
            }

            state->armed = false;
            state->action =
                (OnyrionActionRequest){0};

            wlr_log(
                WLR_DEBUG,
                "Standalone modifier interrupted:"
                " keycode=%u reason=%s",
                state->keycode,
                reason ? reason : "<unknown>"
            );
        }
    }
}

static bool other_standalone_modifier_down(
        OnyrionServer *server,
        const OnyrionStandaloneModifierState *current) {
    if (!server) {
        return false;
    }

    OnyrionKeyboard *keyboard;

    wl_list_for_each(
            keyboard,
            &server->keyboards,
            link) {
        OnyrionStandaloneModifierState *states[] = {
            &keyboard->standalone_super_left,
            &keyboard->standalone_super_right,
        };

        for (size_t i = 0;
                i < sizeof(states) /
                    sizeof(states[0]);
                i++) {
            if (states[i] != current &&
                    states[i]->down) {
                return true;
            }
        }
    }

    return false;
}

static bool standalone_desktop_action_allowed(
        OnyrionServer *server) {
    return
        server &&
        !onyrion_session_lock_active(server) &&
        !onyrion_layer_shell_has_keyboard_focus(server) &&
        !server->fallback_active;
}

static void clear_pending_standalone_release(
        OnyrionKeyboard *keyboard) {
    if (!keyboard) {
        return;
    }

    keyboard->standalone_release_pending = false;
    keyboard->standalone_release_action =
        (OnyrionActionRequest){0};
    keyboard->standalone_release_keysym =
        XKB_KEY_NoSymbol;
    keyboard->standalone_release_keycode = 0;
}

static void cancel_pending_standalone_release(
        OnyrionKeyboard *keyboard,
        const char *reason) {
    if (!keyboard ||
            !keyboard->standalone_release_pending) {
        return;
    }

    if (keyboard->standalone_release_timer) {
        if (wl_event_source_timer_update(
                keyboard->standalone_release_timer,
                0) != 0) {
            wlr_log(
                WLR_ERROR,
                "Standalone modifier release timer disarm failed"
            );
        }
    }

    wlr_log(
        WLR_DEBUG,
        "Standalone modifier pending release canceled:"
        " keysym=%u keycode=%u reason=%s",
        (uint32_t)
            keyboard->standalone_release_keysym,
        keyboard->standalone_release_keycode,
        reason ? reason : "<unknown>"
    );

    clear_pending_standalone_release(
        keyboard
    );
}

static void destroy_standalone_release_timer(
        OnyrionKeyboard *keyboard,
        const char *reason) {
    if (!keyboard) {
        return;
    }

    cancel_pending_standalone_release(
        keyboard,
        reason
    );

    if (keyboard->standalone_release_timer) {
        wl_event_source_remove(
            keyboard->standalone_release_timer
        );
        keyboard->standalone_release_timer = NULL;
    }
}

static int dispatch_standalone_release_timer(
        void *data) {
    OnyrionKeyboard *keyboard = data;

    if (!keyboard ||
            !keyboard->standalone_release_pending) {
        return 0;
    }

    const OnyrionActionRequest action =
        keyboard->standalone_release_action;

    const xkb_keysym_t keysym =
        keyboard->standalone_release_keysym;

    const uint32_t keycode =
        keyboard->standalone_release_keycode;

    clear_pending_standalone_release(
        keyboard
    );

    if (!standalone_desktop_action_allowed(
            keyboard->server)) {
        wlr_log(
            WLR_DEBUG,
            "Standalone modifier pending release blocked:"
            " keysym=%u keycode=%u",
            (uint32_t)keysym,
            keycode
        );

        return 0;
    }

    const bool applied =
        onyrion_action_execute(
            keyboard->server,
            action
        );

    wlr_log(
        applied ? WLR_INFO : WLR_ERROR,
        "Standalone modifier release action:"
        " keysym=%u keycode=%u kind=%d applied=%s",
        (uint32_t)keysym,
        keycode,
        action.kind,
        applied ? "yes" : "no"
    );

    return 0;
}

static bool queue_standalone_release(
        OnyrionKeyboard *keyboard,
        OnyrionActionRequest action,
        xkb_keysym_t keysym,
        uint32_t keycode) {
    if (!keyboard ||
            !keyboard->server ||
            !keyboard->server->display) {
        return false;
    }

    if (keyboard->standalone_release_pending) {
        cancel_pending_standalone_release(
            keyboard,
            "replace-pending"
        );
    }

    if (!keyboard->standalone_release_timer) {
        struct wl_event_loop *loop =
            wl_display_get_event_loop(
                keyboard->server->display
            );

        if (!loop) {
            wlr_log(
                WLR_ERROR,
                "Standalone modifier release queue failed:"
                " event loop unavailable"
            );

            return false;
        }

        keyboard->standalone_release_timer =
            wl_event_loop_add_timer(
                loop,
                dispatch_standalone_release_timer,
                keyboard
            );

        if (!keyboard->standalone_release_timer) {
            wlr_log(
                WLR_ERROR,
                "Standalone modifier release queue failed:"
                " timer source allocation"
            );

            return false;
        }
    }

    keyboard->standalone_release_action =
        action;
    keyboard->standalone_release_keysym =
        keysym;
    keyboard->standalone_release_keycode =
        keycode;
    keyboard->standalone_release_pending = true;

    if (wl_event_source_timer_update(
            keyboard->standalone_release_timer,
            ONYRION_STANDALONE_RELEASE_DEVICE_GRACE_MS) != 0) {
        clear_pending_standalone_release(
            keyboard
        );

        wlr_log(
            WLR_ERROR,
            "Standalone modifier release queue failed:"
            " timer arm"
        );

        return false;
    }

    wlr_log(
        WLR_DEBUG,
        "Standalone modifier release queued:"
        " keysym=%u keycode=%u kind=%d grace_ms=%d",
        (uint32_t)keysym,
        keycode,
        action.kind,
        ONYRION_STANDALONE_RELEASE_DEVICE_GRACE_MS
    );

    return true;
}

static void handle_standalone_modifier_key(
        OnyrionKeyboard *keyboard,
        struct wlr_keyboard_key_event *event,
        xkb_keysym_t keysym) {
    OnyrionStandaloneModifierState *state =
        standalone_modifier_state(
            keyboard,
            keysym
        );

    if (!state) {
        return;
    }

    if (event->state ==
            WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (state->down &&
                state->keycode ==
                    event->keycode) {
            return;
        }

        const bool chorded =
            other_standalone_modifier_down(
                keyboard->server,
                state
            );

        cancel_standalone_modifier_arms(
            keyboard->server,
            "modifier-chord"
        );

        state->down = true;
        state->armed = false;
        state->keycode = event->keycode;
        state->action =
            (OnyrionActionRequest){0};

        if (chorded ||
                !standalone_desktop_action_allowed(
                    keyboard->server)) {
            return;
        }

        const OnyrionKeyboardBinding *binding =
            onyrion_core_policy_match_standalone_modifier(
                keyboard->server->policy,
                (uint32_t)keysym
            );

        if (!binding) {
            return;
        }

        state->action = binding->action;
        state->armed = true;

        wlr_log(
            WLR_DEBUG,
            "Standalone modifier armed:"
            " keysym=%u keycode=%u kind=%d",
            (uint32_t)keysym,
            event->keycode,
            binding->action.kind
        );

        return;
    }

    if (event->state !=
            WL_KEYBOARD_KEY_STATE_RELEASED ||
            !state->down ||
            state->keycode != event->keycode) {
        return;
    }

    const bool dispatch =
        state->armed &&
        standalone_desktop_action_allowed(
            keyboard->server
        );

    const OnyrionActionRequest action =
        state->action;

    reset_standalone_modifier_state(
        state
    );

    if (!dispatch) {
        return;
    }

    if (!queue_standalone_release(
            keyboard,
            action,
            keysym,
            event->keycode)) {
        wlr_log(
            WLR_ERROR,
            "Standalone modifier release dropped:"
            " keysym=%u keycode=%u kind=%d",
            (uint32_t)keysym,
            event->keycode,
            action.kind
        );
    }
}

static void reset_all_standalone_modifier_states(
        OnyrionServer *server) {
    if (!server) {
        return;
    }

    OnyrionKeyboard *keyboard;

    wl_list_for_each(
            keyboard,
            &server->keyboards,
            link) {
        cancel_pending_standalone_release(
            keyboard,
            "keyboard-policy-apply"
        );

        reset_standalone_modifier_state(
            &keyboard->standalone_super_left
        );

        reset_standalone_modifier_state(
            &keyboard->standalone_super_right
        );
    }
}

void onyrion_input_focus_surface(
        struct onyrion_server *server,
        struct wlr_surface *surface) {
    if (!server->seat) {
        return;
    }

    if (!surface) {
        wlr_seat_keyboard_notify_clear_focus(
            server->seat
        );
        return;
    }

    if (!onyrion_session_lock_surface_allowed(server, surface)) {
        return;
    }

    struct wlr_keyboard *keyboard =
        wlr_seat_get_keyboard(server->seat);

    if (!keyboard) {
        return;
    }

    wlr_seat_keyboard_notify_enter(
        server->seat,
        surface,
        keyboard->keycodes,
        keyboard->num_keycodes,
        &keyboard->modifiers
    );
}

void onyrion_input_focus_window(
        struct onyrion_server *server,
        struct onyrion_window *window) {
    if (onyrion_session_lock_active(server) ||
            onyrion_layer_shell_has_exclusive_keyboard(
                server)) {
        return;
    }

    onyrion_input_focus_surface(
        server,
        window && window->mapped
            ? window->toplevel->base->surface
            : NULL
    );
}

static bool set_default_cursor(
        OnyrionServer *server) {
    if (!server ||
            !server->cursor ||
            !server->xcursor_manager) {
        return false;
    }

    struct wlr_xcursor *cursor =
        wlr_xcursor_manager_get_xcursor(
            server->xcursor_manager,
            "left_ptr",
            1.0f
        );

    const char *name =
        "left_ptr";

    if (!cursor) {
        cursor =
            wlr_xcursor_manager_get_xcursor(
                server->xcursor_manager,
                "default",
                1.0f
            );

        name =
            "default";
    }

    if (!cursor) {
        wlr_log(
            WLR_ERROR,
            "Default cursor image not found"
        );

        return false;
    }

    wlr_cursor_set_xcursor(
        server->cursor,
        server->xcursor_manager,
        name
    );

    server->layout_resize_cursor_owned = false;

    wlr_log(
        WLR_DEBUG,
        "Default cursor applied: %s",
        name
    );

    return true;
}

void onyrion_input_clear_pointer_focus(
        struct onyrion_server *server) {
    if (!server->seat) {
        return;
    }

    wlr_seat_pointer_notify_clear_focus(
        server->seat
    );

    (void)set_default_cursor(
        server
    );
}

static void clear_pointer_focus_preserve_cursor(
        OnyrionServer *server) {
    if (!server || !server->seat) {
        return;
    }

    wlr_seat_pointer_notify_clear_focus(
        server->seat
    );
}

static OnyrionWindow *window_at_cursor(
        OnyrionServer *server) {
    double sx;
    double sy;

    struct wlr_scene_node *node =
        wlr_scene_node_at(
            &server->scene->tree.node,
            server->cursor->x,
            server->cursor->y,
            &sx,
            &sy
        );

    if (!node) {
        return NULL;
    }

    OnyrionWindow *window;

    wl_list_for_each(
            window,
            &server->windows,
            link) {
        struct wlr_scene_node *ancestor =
            node;

        while (ancestor) {
            if (ancestor ==
                    &window->scene_tree->node) {
                return window;
            }

            if (!ancestor->parent) {
                break;
            }

            ancestor =
                &ancestor->parent->node;
        }
    }

    return NULL;
}

enum {
    ONYRION_CHROME_DRAG_THRESHOLD = 8,
    ONYRION_RESIZE_BORDER_SLOP = 6,
};

static OnyrionPointer *pointer_from_wlr(
        OnyrionServer *server,
        struct wlr_pointer *wlr_pointer) {
    if (!server || !wlr_pointer) {
        return NULL;
    }

    OnyrionPointer *pointer;

    wl_list_for_each(
            pointer,
            &server->pointers,
            link) {
        if (wlr_pointer_from_input_device(
                pointer->device) ==
                wlr_pointer) {
            return pointer;
        }
    }

    return NULL;
}

static uint32_t server_modifier_mask(
        OnyrionServer *server) {
    uint32_t modifiers = 0;
    OnyrionKeyboard *keyboard;

    wl_list_for_each(
            keyboard,
            &server->keyboards,
            link) {
        modifiers |=
            keyboard_modifier_mask(
                keyboard
            );
    }

    return modifiers;
}

static OnyrionPointerButtonCapture *
pointer_capture_find(
        OnyrionPointer *pointer,
        uint32_t button) {
    if (!pointer) {
        return NULL;
    }

    for (size_t i = 0;
            i < ONYRION_POINTER_BUTTON_CAPTURE_CAPACITY;
            i++) {
        OnyrionPointerButtonCapture *capture =
            &pointer->button_captures[i];

        if (capture->active &&
                capture->button == button) {
            return capture;
        }
    }

    return NULL;
}

static OnyrionPointerButtonCapture *
pointer_capture_begin(
        OnyrionPointer *pointer,
        uint32_t button,
        const OnyrionKeyboardBinding *release_binding) {
    if (!pointer ||
            pointer_capture_find(
                pointer,
                button)) {
        return NULL;
    }

    for (size_t i = 0;
            i < ONYRION_POINTER_BUTTON_CAPTURE_CAPACITY;
            i++) {
        OnyrionPointerButtonCapture *capture =
            &pointer->button_captures[i];

        if (capture->active) {
            continue;
        }

        *capture =
            (OnyrionPointerButtonCapture){
                .active = true,
                .button = button,
                .has_release_action =
                    release_binding != NULL,
            };

        if (release_binding) {
            capture->release_action =
                release_binding->action;
        }

        return capture;
    }

    return NULL;
}

static void pointer_capture_finish(
        OnyrionPointerButtonCapture *capture) {
    if (capture) {
        *capture =
            (OnyrionPointerButtonCapture){0};
    }
}

static void reset_chrome_drag(
        OnyrionServer *server) {
    server->chrome_pointer_grab = false;
    server->chrome_pointer_button = 0;
    server->chrome_drag_subject =
        ONYRION_CHROME_DRAG_NONE;
    server->chrome_drag_window_id = 0;
    server->chrome_drag_group_id = 0;
    server->chrome_drag_origin_x = 0.0;
    server->chrome_drag_origin_y = 0.0;
    server->chrome_drag_active = false;
}

static void arm_chrome_drag(
        OnyrionServer *server,
        OnyrionChromeDragSubject subject,
        uint64_t window_id,
        uint64_t group_id,
        uint32_t button) {
    server->chrome_pointer_grab = true;
    server->chrome_pointer_button = button;
    server->chrome_drag_subject = subject;
    server->chrome_drag_window_id =
        window_id;
    server->chrome_drag_group_id =
        group_id;
    server->chrome_drag_origin_x =
        server->cursor->x;
    server->chrome_drag_origin_y =
        server->cursor->y;
    server->chrome_drag_active = false;
}

static bool arm_interactive_move_active(
        OnyrionServer *server,
        uint32_t button) {
    if (!server ||
            !server->active_group ||
            !server->active_group->active ||
            !server->active_group->active->mapped ||
            server->active_group->active->fullscreen ||
            server->chrome_pointer_grab ||
            server->layout_resize_pointer_grab ||
            !server->active_group->tile) {
        return false;
    }

    arm_chrome_drag(
        server,
        ONYRION_CHROME_DRAG_GROUP,
        0,
        server->active_group->id,
        button
    );

    onyrion_input_clear_pointer_focus(
        server
    );

    wlr_log(
        WLR_INFO,
        "Interactive move active armed: group=%" PRIu64,
        server->active_group->id
    );

    return true;
}

static void reset_layout_resize(
        OnyrionServer *server) {
    server->layout_resize_pointer_grab = false;
    server->layout_resize_pointer_button = 0;
    server->layout_resize_first_group_id = 0;
    server->layout_resize_second_group_id = 0;
    server->layout_resize_orientation = -1;
}

static bool set_resize_cursor(
        OnyrionServer *server,
        OnyrionSplitOrientation orientation) {
    if (!server ||
            !server->cursor ||
            !server->xcursor_manager) {
        return false;
    }

    static const char *const horizontal_names[] = {
        "col-resize",
        "ew-resize",
        "sb_h_double_arrow",
        "left_side",
        "right_side",
    };

    static const char *const vertical_names[] = {
        "row-resize",
        "ns-resize",
        "sb_v_double_arrow",
        "top_side",
        "bottom_side",
    };

    const char *const *names =
        orientation ==
            ONYRION_SPLIT_HORIZONTAL
            ? horizontal_names
            : vertical_names;

    const size_t count =
        orientation ==
            ONYRION_SPLIT_HORIZONTAL
            ? sizeof(horizontal_names) /
                sizeof(horizontal_names[0])
            : sizeof(vertical_names) /
                sizeof(vertical_names[0]);

    for (size_t i = 0; i < count; i++) {
        struct wlr_xcursor *cursor =
            wlr_xcursor_manager_get_xcursor(
                server->xcursor_manager,
                names[i],
                1.0f
            );

        if (!cursor) {
            continue;
        }

        wlr_cursor_set_xcursor(
            server->cursor,
            server->xcursor_manager,
            names[i]
        );

        server->layout_resize_cursor_owned = true;

        wlr_log(
            WLR_DEBUG,
            "Resize cursor applied:"
            " orientation=%s"
            " name=%s",
            orientation ==
                ONYRION_SPLIT_HORIZONTAL
                ? "horizontal"
                : "vertical",
            names[i]
        );

        return true;
    }

    wlr_log(
        WLR_ERROR,
        "No resize cursor available:"
        " orientation=%s",
        orientation ==
            ONYRION_SPLIT_HORIZONTAL
            ? "horizontal"
            : "vertical"
    );

    return set_default_cursor(server);
}

static bool resize_target_at_cursor(
        OnyrionServer *server,
        OnyrionLayoutResizeTarget *target) {
    if (onyrion_group_chrome_at(
            server,
            server->cursor->x,
            server->cursor->y)) {
        return false;
    }

    OnyrionWindow *window =
        window_at_cursor(server);

    if (window &&
            (window->placement !=
                    ONYRION_WINDOW_PLACEMENT_TILED ||
                window->fullscreen)) {
        return false;
    }

    return
        onyrion_layout_resize_border_at(
            server,
            server->cursor->x,
            server->cursor->y,
            (double)
                ONYRION_RESIZE_BORDER_SLOP,
            target
        );
}

static bool arm_interactive_resize_active(
        OnyrionServer *server,
        uint32_t button) {
    OnyrionLayoutResizeTarget target = {0};

    if (!server ||
            !server->active_group ||
            !server->active_group->active ||
            !server->active_group->active->mapped ||
            server->active_group->active->fullscreen ||
            server->chrome_pointer_grab ||
            server->layout_resize_pointer_grab ||
            !onyrion_layout_resize_active_target_at(
                server,
                server->cursor->x,
                server->cursor->y,
                &target)) {
        return false;
    }

    server->layout_resize_pointer_grab = true;
    server->layout_resize_pointer_button = button;
    server->layout_resize_first_group_id =
        target.first_group_id;
    server->layout_resize_second_group_id =
        target.second_group_id;
    server->layout_resize_orientation =
        (int)target.orientation;

    clear_pointer_focus_preserve_cursor(
        server
    );

    (void)set_resize_cursor(
        server,
        target.orientation
    );

    wlr_log(
        WLR_INFO,
        "Interactive resize active armed:"
        " first_group=%" PRIu64
        " second_group=%" PRIu64
        " orientation=%s",
        target.first_group_id,
        target.second_group_id,
        target.orientation ==
            ONYRION_SPLIT_HORIZONTAL
            ? "horizontal"
            : "vertical"
    );

    return true;
}

static bool execute_pointer_action(
        OnyrionServer *server,
        OnyrionActionRequest action,
        uint32_t button) {
    switch (action.kind) {
    case ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE:
        return arm_interactive_move_active(
            server,
            button
        );

    case ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE:
        return arm_interactive_resize_active(
            server,
            button
        );

    default:
        return onyrion_action_execute(
            server,
            action
        );
    }
}

static OnyrionGroup *group_drop_target_at_cursor(
        OnyrionServer *server) {
    OnyrionGroup *group =
        onyrion_group_chrome_at(
            server,
            server->cursor->x,
            server->cursor->y
        );

    if (group) {
        return group;
    }

    OnyrionWindow *window =
        window_at_cursor(server);

    return
        window &&
        window->group &&
        window->placement ==
            ONYRION_WINDOW_PLACEMENT_TILED
            ? window->group
            : NULL;
}

typedef struct layout_drop_target {
    OnyrionGroup *group;
    OnyrionFocusDirection direction;
} LayoutDropTarget;

static OnyrionGroup *group_by_id(
        OnyrionServer *server,
        uint64_t id) {
    OnyrionGroup *group;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (group->id == id) {
            return group;
        }
    }

    return NULL;
}

static bool layout_drop_target_at_cursor(
        OnyrionServer *server,
        LayoutDropTarget *target) {
    if (!server || !target) {
        return false;
    }

    const double x =
        server->cursor->x;
    const double y =
        server->cursor->y;

    OnyrionGroup *group;

    wl_list_for_each(
            group,
            &server->groups,
            link) {
        if (!group->tile ||
                group->tile->width <= 0 ||
                group->tile->height <= 0) {
            continue;
        }

        const OnyrionTile *tile =
            group->tile;

        if (x < tile->x ||
                y < tile->y ||
                x >= tile->x + tile->width ||
                y >= tile->y + tile->height) {
            continue;
        }

        const double left =
            (x - tile->x) /
            (double)tile->width;

        const double right =
            (tile->x + tile->width - x) /
            (double)tile->width;

        const double top =
            (y - tile->y) /
            (double)tile->height;

        const double bottom =
            (tile->y + tile->height - y) /
            (double)tile->height;

        double best = 0.25;
        OnyrionFocusDirection direction =
            ONYRION_FOCUS_DIRECTION_COUNT;

        if (left < best) {
            best = left;
            direction = ONYRION_FOCUS_LEFT;
        }

        if (right < best) {
            best = right;
            direction = ONYRION_FOCUS_RIGHT;
        }

        if (top < best) {
            best = top;
            direction = ONYRION_FOCUS_UP;
        }

        if (bottom < best) {
            direction = ONYRION_FOCUS_DOWN;
        }

        if (direction ==
                ONYRION_FOCUS_DIRECTION_COUNT) {
            return false;
        }

        target->group = group;
        target->direction = direction;
        return true;
    }

    return false;
}

static void update_drag_icon_positions(
        OnyrionServer *server) {
    if (!server->drag_icons) {
        return;
    }

    struct wlr_scene_node *node;

    wl_list_for_each(
            node,
            &server->drag_icons->children,
            link) {
        wlr_scene_node_set_position(
            node,
            (int)server->cursor->x,
            (int)server->cursor->y
        );
    }
}

static void process_cursor_motion(
        OnyrionServer *server,
        uint32_t time_msec) {
    OnyrionOutput *output =
        onyrion_output_at(
            server,
            server->cursor->x,
            server->cursor->y
        );

    if (output) {
        onyrion_output_focus(
            server,
            output
        );
    }

    if (server->layout_resize_pointer_grab) {
        if (onyrion_session_lock_active(
                server)) {
            reset_layout_resize(server);
        } else {
            const OnyrionLayoutResizeTarget target = {
                .first_group_id =
                    server->
                        layout_resize_first_group_id,
                .second_group_id =
                    server->
                        layout_resize_second_group_id,
                .orientation =
                    (OnyrionSplitOrientation)
                        server->
                            layout_resize_orientation,
            };

            double ratio = 0.0;

            if (onyrion_layout_resize_target_to_position(
                    server,
                    &target,
                    server->cursor->x,
                    server->cursor->y,
                    &ratio)) {
                clear_pointer_focus_preserve_cursor(
                    server
                );

                (void)set_resize_cursor(
                    server,
                    target.orientation
                );

                return;
            }

            wlr_log(
                WLR_DEBUG,
                "Border resize cancelled:"
                " target no longer resolves"
            );

            reset_layout_resize(server);
        }
    }

    OnyrionLayoutResizeTarget hover_target = {0};

    if (!server->chrome_pointer_grab &&
            !onyrion_session_lock_active(server) &&
            resize_target_at_cursor(
                server,
                &hover_target)) {
        clear_pointer_focus_preserve_cursor(
            server
        );

        (void)set_resize_cursor(
            server,
            hover_target.orientation
        );

        return;
    }

    if (server->layout_resize_cursor_owned &&
            !server->chrome_pointer_grab) {
        (void)set_default_cursor(server);
    }

    if (server->chrome_pointer_grab &&
            server->chrome_drag_subject !=
                ONYRION_CHROME_DRAG_NONE) {
        if (onyrion_session_lock_active(
                server)) {
            reset_chrome_drag(server);
        } else {
            if (!server->chrome_drag_active) {
                const double dx =
                    server->cursor->x -
                    server->chrome_drag_origin_x;

                const double dy =
                    server->cursor->y -
                    server->chrome_drag_origin_y;

                const double threshold =
                    (double)
                        ONYRION_CHROME_DRAG_THRESHOLD;

                if (dx * dx + dy * dy >=
                        threshold * threshold) {
                    server->chrome_drag_active =
                        true;

                    wlr_log(
                        WLR_INFO,
                        "Chrome drag started:"
                        " subject=%s"
                        " window=%" PRIu64
                        " group=%" PRIu64,
                        server->chrome_drag_subject ==
                            ONYRION_CHROME_DRAG_WINDOW
                            ? "window"
                            : "group",
                        server->chrome_drag_window_id,
                        server->chrome_drag_group_id
                    );
                }
            }

            onyrion_input_clear_pointer_focus(
                server
            );
            return;
        }
    }

    double sx;
    double sy;

    struct wlr_scene_node *node =
        wlr_scene_node_at(
            &server->scene->tree.node,
            server->cursor->x,
            server->cursor->y,
            &sx,
            &sy
        );

    if (!node ||
            node->type != WLR_SCENE_NODE_BUFFER) {
        wlr_seat_pointer_notify_clear_focus(
            server->seat
        );
        return;
    }

    struct wlr_scene_buffer *scene_buffer =
        wlr_scene_buffer_from_node(node);

    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(
            scene_buffer
        );

    if (!scene_surface) {
        wlr_seat_pointer_notify_clear_focus(
            server->seat
        );
        return;
    }

    wlr_seat_pointer_notify_enter(
        server->seat,
        scene_surface->surface,
        sx,
        sy
    );

    wlr_seat_pointer_notify_motion(
        server->seat,
        time_msec,
        sx,
        sy
    );
}

static void handle_cursor_motion(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            cursor_motion
        );

    struct wlr_pointer_motion_event *event =
        data;

    wlr_cursor_move(
        server->cursor,
        &event->pointer->base,
        event->delta_x,
        event->delta_y
    );

    update_drag_icon_positions(
        server
    );

    process_cursor_motion(
        server,
        event->time_msec
    );
}

static void handle_cursor_motion_absolute(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            cursor_motion_absolute
        );

    struct wlr_pointer_motion_absolute_event *event =
        data;

    wlr_cursor_warp_absolute(
        server->cursor,
        &event->pointer->base,
        event->x,
        event->y
    );

    update_drag_icon_positions(
        server
    );

    process_cursor_motion(
        server,
        event->time_msec
    );
}

static void handle_cursor_button(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            cursor_button
        );

    struct wlr_pointer_button_event *event =
        data;

    cancel_standalone_modifier_arms(
        server,
        "pointer-button"
    );

    OnyrionPointer *source_pointer =
        pointer_from_wlr(
            server,
            event->pointer
        );

    if (event->state ==
            WL_POINTER_BUTTON_STATE_RELEASED) {
        OnyrionPointerButtonCapture *capture =
            pointer_capture_find(
                source_pointer,
                event->button
            );

        if (capture) {
            const bool has_release_action =
                capture->has_release_action;

            const OnyrionActionRequest release_action =
                capture->release_action;

            pointer_capture_finish(capture);

            if (has_release_action &&
                    !onyrion_session_lock_active(server) &&
                    !onyrion_layer_shell_has_keyboard_focus(server) &&
                    !server->fallback_active) {
                (void)execute_pointer_action(
                    server,
                    release_action,
                    event->button
                );
            }

            return;
        }
    }

    if (event->state ==
            WL_POINTER_BUTTON_STATE_RELEASED &&
            server->layout_resize_pointer_grab &&
            event->button ==
                server->layout_resize_pointer_button) {
        const OnyrionLayoutResizeTarget target = {
            .first_group_id =
                server->
                    layout_resize_first_group_id,
            .second_group_id =
                server->
                    layout_resize_second_group_id,
            .orientation =
                (OnyrionSplitOrientation)
                    server->
                        layout_resize_orientation,
        };

        reset_layout_resize(server);

        onyrion_input_clear_pointer_focus(
            server
        );

        OnyrionLayoutResizeTarget hover_target = {0};

        if (resize_target_at_cursor(
                server,
                &hover_target)) {
            (void)set_resize_cursor(
                server,
                hover_target.orientation
            );
        }

        wlr_log(
            WLR_INFO,
            "Border resize ended:"
            " first_group=%" PRIu64
            " second_group=%" PRIu64
            " orientation=%s"
            " cursor=%.1f,%.1f",
            target.first_group_id,
            target.second_group_id,
            target.orientation ==
                ONYRION_SPLIT_HORIZONTAL
                ? "horizontal"
                : "vertical",
            server->cursor->x,
            server->cursor->y
        );

        return;
    }

    if (event->state ==
            WL_POINTER_BUTTON_STATE_RELEASED &&
            server->chrome_pointer_grab &&
            event->button ==
                server->chrome_pointer_button) {
        const OnyrionChromeDragSubject subject =
            server->chrome_drag_subject;

        const uint64_t window_id =
            server->chrome_drag_window_id;

        const uint64_t source_group_id =
            server->chrome_drag_group_id;

        const bool was_drag =
            server->chrome_drag_active;

        if (onyrion_session_lock_active(server)) {
            reset_chrome_drag(server);
            return;
        }

        if (onyrion_layer_shell_focus_at_cursor(
                server)) {
            reset_chrome_drag(server);
            return;
        }

        LayoutDropTarget layout_target = {0};

        const bool has_layout_target =
            was_drag &&
            layout_drop_target_at_cursor(
                server,
                &layout_target
            );

        OnyrionGroup *target =
            was_drag && !has_layout_target
                ? group_drop_target_at_cursor(
                    server
                )
                : NULL;

        const uint64_t target_group_id =
            target
                ? target->id
                : 0;

        const uint64_t layout_target_group_id =
            has_layout_target
                ? layout_target.group->id
                : 0;

        reset_chrome_drag(server);

        bool changed = false;

        if (was_drag &&
                has_layout_target) {
            if (subject ==
                    ONYRION_CHROME_DRAG_WINDOW) {
                changed =
                    onyrion_group_move_window_to_layout_zone_id(
                        server,
                        window_id,
                        layout_target_group_id,
                        layout_target.direction
                    );
            } else if (subject ==
                    ONYRION_CHROME_DRAG_GROUP &&
                    layout_target_group_id !=
                        source_group_id) {
                OnyrionGroup *source =
                    group_by_id(
                        server,
                        source_group_id
                    );

                changed =
                    source &&
                    onyrion_layout_move_group_relative(
                        source,
                        layout_target.group,
                        layout_target.direction
                    );
            }
        } else if (was_drag &&
                target_group_id != 0 &&
                target_group_id !=
                    source_group_id) {
            if (subject ==
                    ONYRION_CHROME_DRAG_WINDOW) {
                changed =
                    onyrion_group_move_window_id(
                        server,
                        window_id,
                        target_group_id
                    );
            } else if (subject ==
                    ONYRION_CHROME_DRAG_GROUP) {
                changed =
                    onyrion_group_merge_id(
                        server,
                        source_group_id,
                        target_group_id
                    );
            }
        }

        if (was_drag &&
                has_layout_target) {
            wlr_log(
                WLR_INFO,
                "Chrome drag layout-drop:"
                " subject=%s"
                " window=%" PRIu64
                " source_group=%" PRIu64
                " target_group=%" PRIu64
                " direction=%d"
                " changed=%s",
                subject ==
                    ONYRION_CHROME_DRAG_WINDOW
                    ? "window"
                    : "group",
                window_id,
                source_group_id,
                layout_target_group_id,
                (int)layout_target.direction,
                changed ? "yes" : "no"
            );
        }

        if (was_drag) {
            wlr_log(
                WLR_INFO,
                "Chrome drag dropped:"
                " subject=%s"
                " window=%" PRIu64
                " source_group=%" PRIu64
                " target_group=%" PRIu64
                " changed=%s",
                subject ==
                    ONYRION_CHROME_DRAG_WINDOW
                    ? "window"
                    : "group",
                window_id,
                source_group_id,
                target_group_id,
                changed ? "yes" : "no"
            );
        }

        return;
    }

    if (event->state ==
            WL_POINTER_BUTTON_STATE_PRESSED) {
        if (onyrion_session_lock_active(server)) {
            (void)onyrion_session_lock_focus_at_cursor(server);
        } else if (!onyrion_layer_shell_focus_at_cursor(
                server)) {
            const uint32_t modifiers =
                server_modifier_mask(
                    server
                );

            const OnyrionKeyboardBinding *press_binding =
                onyrion_core_policy_match_pointer_button(
                    server->policy,
                    event->button,
                    ONYRION_KEYBOARD_BINDING_TRIGGER_PRESS,
                    modifiers
                );

            const OnyrionKeyboardBinding *release_binding =
                onyrion_core_policy_match_pointer_button(
                    server->policy,
                    event->button,
                    ONYRION_KEYBOARD_BINDING_TRIGGER_RELEASE,
                    modifiers
                );

            if (!server->fallback_active &&
                    press_binding &&
                    (press_binding->action.kind ==
                        ONYRION_ACTION_INTERACTIVE_MOVE_ACTIVE ||
                     press_binding->action.kind ==
                        ONYRION_ACTION_INTERACTIVE_RESIZE_ACTIVE)) {
                const bool started =
                    execute_pointer_action(
                        server,
                        press_binding->action,
                        event->button
                    );

                if (!started) {
                    (void)pointer_capture_begin(
                        source_pointer,
                        event->button,
                        NULL
                    );
                }

                return;
            }

            if (!server->fallback_active &&
                    (press_binding || release_binding)) {
                OnyrionPointerButtonCapture *capture =
                    pointer_capture_begin(
                        source_pointer,
                        event->button,
                        release_binding
                    );

                if (!capture) {
                    wlr_log(
                        WLR_ERROR,
                        "Pointer binding capture unavailable: button=%u",
                        event->button
                    );

                    return;
                }

                if (press_binding) {
                    (void)execute_pointer_action(
                        server,
                        press_binding->action,
                        event->button
                    );
                }

                return;
            }

            if (event->button == BTN_LEFT) {
                OnyrionGroup *handle =
                    onyrion_group_handle_at(
                        server,
                        server->cursor->x,
                        server->cursor->y
                    );

                if (handle) {
                    arm_chrome_drag(
                        server,
                        ONYRION_CHROME_DRAG_GROUP,
                        0,
                        handle->id,
                        event->button
                    );

                    if (handle->active) {
                        (void)onyrion_window_focus(
                            handle->active
                        );
                    }

                    onyrion_input_clear_pointer_focus(
                        server
                    );

                    wlr_log(
                        WLR_INFO,
                        "Group drag armed:"
                        " group=%" PRIu64,
                        handle->id
                    );

                    return;
                }

                OnyrionWindow *tab =
                    onyrion_group_tab_at(
                        server,
                        server->cursor->x,
                        server->cursor->y
                    );

                if (tab) {
                    const uint64_t group_id =
                        tab->group->id;

                    arm_chrome_drag(
                        server,
                        ONYRION_CHROME_DRAG_WINDOW,
                        tab->id,
                        group_id,
                        event->button
                    );

                    (void)onyrion_window_focus(tab);
                    onyrion_input_clear_pointer_focus(
                        server
                    );

                    wlr_log(
                        WLR_INFO,
                        "Group tab clicked:"
                        " group=%" PRIu64
                        " window=%" PRIu64,
                        group_id,
                        tab->id
                    );

                    wlr_log(
                        WLR_INFO,
                        "Tab drag armed:"
                        " group=%" PRIu64
                        " window=%" PRIu64,
                        group_id,
                        tab->id
                    );

                    return;
                }

                OnyrionLayoutResizeTarget
                    resize_target = {0};

                if (resize_target_at_cursor(
                        server,
                        &resize_target)) {
                    server->
                        layout_resize_pointer_grab =
                            true;

                    server->
                        layout_resize_pointer_button =
                            event->button;

                    server->
                        layout_resize_first_group_id =
                            resize_target.
                                first_group_id;

                    server->
                        layout_resize_second_group_id =
                            resize_target.
                                second_group_id;

                    server->
                        layout_resize_orientation =
                            (int)
                                resize_target.
                                    orientation;

                    clear_pointer_focus_preserve_cursor(
                        server
                    );

                    (void)set_resize_cursor(
                        server,
                        resize_target.orientation
                    );

                    wlr_log(
                        WLR_INFO,
                        "Border resize armed:"
                        " first_group=%" PRIu64
                        " second_group=%" PRIu64
                        " orientation=%s",
                        resize_target.
                            first_group_id,
                        resize_target.
                            second_group_id,
                        resize_target.orientation ==
                            ONYRION_SPLIT_HORIZONTAL
                            ? "horizontal"
                            : "vertical"
                    );

                    return;
                }
            }

            OnyrionWindow *window =
                window_at_cursor(server);

            if (window) {
                (void)onyrion_window_focus(
                    window
                );
            }
        }
    }

    wlr_seat_pointer_notify_button(
        server->seat,
        event->time_msec,
        event->button,
        event->state
    );
}

static void handle_cursor_axis(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            cursor_axis
        );

    struct wlr_pointer_axis_event *event =
        data;

    if (event->delta != 0.0 ||
            event->delta_discrete != 0) {
        cancel_standalone_modifier_arms(
            server,
            "pointer-axis"
        );
    }

    if (event->orientation ==
                WL_POINTER_AXIS_VERTICAL_SCROLL &&
            event->source ==
                WL_POINTER_AXIS_SOURCE_WHEEL &&
            !onyrion_session_lock_active(server) &&
            !onyrion_layer_shell_has_keyboard_focus(server) &&
            !server->fallback_active) {
        const double direction_value =
            event->delta_discrete != 0
                ? (double)event->delta_discrete
                : event->delta;

        if (direction_value != 0.0) {
            const OnyrionPointerWheelDirection direction =
                direction_value < 0.0
                    ? ONYRION_POINTER_WHEEL_UP
                    : ONYRION_POINTER_WHEEL_DOWN;

            const OnyrionKeyboardBinding *binding =
                onyrion_core_policy_match_pointer_wheel(
                    server->policy,
                    direction,
                    server_modifier_mask(server)
                );

            if (binding) {
                (void)onyrion_action_execute(
                    server,
                    binding->action
                );

                return;
            }
        }
    }

    const double scroll_factor =
        pointer_axis_scroll_factor(
            server,
            event->pointer
        );

    const double scaled_delta =
        event->delta * scroll_factor;

    wlr_seat_pointer_notify_axis(
        server->seat,
        event->time_msec,
        event->orientation,
        scaled_delta,
        event->delta_discrete,
        event->source,
        event->relative_direction
    );
}

static void gesture_reset(
        OnyrionGestureRecognizer *gesture) {
    if (!gesture) {
        return;
    }

    gesture->kind = ONYRION_GESTURE_NONE;
    gesture->axis_lock = ONYRION_GESTURE_LOCK_NONE;
    gesture->fingers = 0;
    gesture->dx = 0.0;
    gesture->dy = 0.0;
    gesture->scale = 1.0;
    gesture->active = false;
    gesture->cancelled = false;
}

static bool gestures_allowed(
        OnyrionServer *server) {
    return
        server &&
        !onyrion_session_lock_active(server) &&
        !onyrion_layer_shell_has_keyboard_focus(server) &&
        !server->fallback_active;
}

static double absolute_double(double value) {
    return value < 0.0 ? -value : value;
}

static void gesture_update_axis_lock(
        OnyrionGestureRecognizer *gesture) {
    if (!gesture ||
            gesture->axis_lock != ONYRION_GESTURE_LOCK_NONE ||
            gesture->cancelled) {
        return;
    }

    const double ax = absolute_double(gesture->dx);
    const double ay = absolute_double(gesture->dy);
    const double major = ax > ay ? ax : ay;

    if (major <
            (double)ONYRION_GESTURE_DIRECTION_LOCK_THRESHOLD) {
        return;
    }

    if (ax >= ay * ONYRION_GESTURE_DIRECTION_LOCK_RATIO) {
        gesture->axis_lock = ONYRION_GESTURE_LOCK_HORIZONTAL;
    } else if (ay >= ax * ONYRION_GESTURE_DIRECTION_LOCK_RATIO) {
        gesture->axis_lock = ONYRION_GESTURE_LOCK_VERTICAL;
    } else if (major >=
            (double)ONYRION_GESTURE_AMBIGUOUS_CANCEL_THRESHOLD) {
        gesture->cancelled = true;

        wlr_log(
            WLR_DEBUG,
            "Gesture cancelled: ambiguous direction dx=%.3f dy=%.3f",
            gesture->dx,
            gesture->dy
        );
    }

    if (gesture->axis_lock != ONYRION_GESTURE_LOCK_NONE) {
        wlr_log(
            WLR_DEBUG,
            "Gesture direction locked: axis=%s dx=%.3f dy=%.3f",
            gesture->axis_lock == ONYRION_GESTURE_LOCK_HORIZONTAL
                ? "horizontal"
                : "vertical",
            gesture->dx,
            gesture->dy
        );
    }
}

static bool gesture_commit(
        OnyrionGestureRecognizer *gesture,
        OnyrionInputBindingSelector selector,
        OnyrionGestureDirection direction) {
    if (!gesture ||
            !gestures_allowed(gesture->server)) {
        return false;
    }

    const OnyrionKeyboardBinding *binding =
        onyrion_core_policy_match_gesture(
            gesture->server->policy,
            selector,
            gesture->fingers,
            direction
        );

    if (!binding) {
        wlr_log(
            WLR_DEBUG,
            "Gesture completed without binding: selector=%d fingers=%u direction=%d",
            (int)selector,
            gesture->fingers,
            (int)direction
        );
        return false;
    }

    const OnyrionActionRequest action = binding->action;

    gesture_reset(gesture);

    const bool applied =
        onyrion_action_execute(
            gesture->server,
            action
        );

    wlr_log(
        WLR_INFO,
        "Gesture action committed: selector=%d direction=%d action=%d applied=%s",
        (int)selector,
        (int)direction,
        (int)action.kind,
        applied ? "yes" : "no"
    );

    return applied;
}

static void handle_swipe_begin(
        struct wl_listener *listener,
        void *data) {
    OnyrionGestureRecognizer *gesture =
        wl_container_of(listener, gesture, swipe_begin);
    struct wlr_pointer_swipe_begin_event *event = data;

    cancel_standalone_modifier_arms(
        gesture->server,
        "gesture-swipe"
    );

    gesture_reset(gesture);
    gesture->kind = ONYRION_GESTURE_SWIPE;
    gesture->fingers = event->fingers;
    gesture->scale = 1.0;
    gesture->active = gestures_allowed(gesture->server);

    wlr_log(
        WLR_DEBUG,
        "Gesture swipe begin: fingers=%u active=%s",
        event->fingers,
        gesture->active ? "yes" : "no"
    );
}

static void handle_swipe_update(
        struct wl_listener *listener,
        void *data) {
    OnyrionGestureRecognizer *gesture =
        wl_container_of(listener, gesture, swipe_update);
    struct wlr_pointer_swipe_update_event *event = data;

    if (!gesture->active ||
            gesture->kind != ONYRION_GESTURE_SWIPE ||
            event->fingers != gesture->fingers) {
        gesture->cancelled = true;
        return;
    }

    gesture->dx += event->dx;
    gesture->dy += event->dy;
    gesture_update_axis_lock(gesture);
}

static void handle_swipe_end(
        struct wl_listener *listener,
        void *data) {
    OnyrionGestureRecognizer *gesture =
        wl_container_of(listener, gesture, swipe_end);
    struct wlr_pointer_swipe_end_event *event = data;

    if (!gesture->active ||
            gesture->kind != ONYRION_GESTURE_SWIPE ||
            gesture->cancelled ||
            event->cancelled) {
        gesture_reset(gesture);
        return;
    }

    const double ax = absolute_double(gesture->dx);
    const double ay = absolute_double(gesture->dy);

    if (gesture->axis_lock == ONYRION_GESTURE_LOCK_HORIZONTAL &&
            ax >= (double)ONYRION_GESTURE_SWIPE_COMMIT_THRESHOLD) {
        const OnyrionGestureDirection direction =
            gesture->dx < 0.0
                ? ONYRION_GESTURE_LEFT
                : ONYRION_GESTURE_RIGHT;

        if (!gesture_commit(
                gesture,
                ONYRION_INPUT_BINDING_GESTURE_SWIPE,
                direction)) {
            gesture_reset(gesture);
        }
        return;
    }

    if (gesture->axis_lock == ONYRION_GESTURE_LOCK_VERTICAL &&
            ay >= (double)ONYRION_GESTURE_SWIPE_COMMIT_THRESHOLD) {
        const OnyrionGestureDirection direction =
            gesture->dy < 0.0
                ? ONYRION_GESTURE_UP
                : ONYRION_GESTURE_DOWN;

        if (!gesture_commit(
                gesture,
                ONYRION_INPUT_BINDING_GESTURE_SWIPE,
                direction)) {
            gesture_reset(gesture);
        }
        return;
    }

    wlr_log(
        WLR_DEBUG,
        "Gesture swipe threshold cancelled: dx=%.3f dy=%.3f lock=%d",
        gesture->dx,
        gesture->dy,
        (int)gesture->axis_lock
    );

    gesture_reset(gesture);
}

static void handle_pinch_begin(
        struct wl_listener *listener,
        void *data) {
    OnyrionGestureRecognizer *gesture =
        wl_container_of(listener, gesture, pinch_begin);
    struct wlr_pointer_pinch_begin_event *event = data;

    cancel_standalone_modifier_arms(
        gesture->server,
        "gesture-pinch"
    );

    gesture_reset(gesture);
    gesture->kind = ONYRION_GESTURE_PINCH;
    gesture->fingers = event->fingers;
    gesture->scale = 1.0;
    gesture->active = gestures_allowed(gesture->server);

    wlr_log(
        WLR_DEBUG,
        "Gesture pinch begin: fingers=%u active=%s",
        event->fingers,
        gesture->active ? "yes" : "no"
    );
}

static void handle_pinch_update(
        struct wl_listener *listener,
        void *data) {
    OnyrionGestureRecognizer *gesture =
        wl_container_of(listener, gesture, pinch_update);
    struct wlr_pointer_pinch_update_event *event = data;

    if (!gesture->active ||
            gesture->kind != ONYRION_GESTURE_PINCH ||
            event->fingers != gesture->fingers) {
        gesture->cancelled = true;
        return;
    }

    gesture->dx += event->dx;
    gesture->dy += event->dy;
    gesture->scale = event->scale;
}

static void handle_pinch_end(
        struct wl_listener *listener,
        void *data) {
    OnyrionGestureRecognizer *gesture =
        wl_container_of(listener, gesture, pinch_end);
    struct wlr_pointer_pinch_end_event *event = data;

    if (!gesture->active ||
            gesture->kind != ONYRION_GESTURE_PINCH ||
            gesture->cancelled ||
            event->cancelled) {
        gesture_reset(gesture);
        return;
    }

    const double scale_delta =
        absolute_double(gesture->scale - 1.0);

    if (scale_delta < ONYRION_GESTURE_PINCH_COMMIT_THRESHOLD) {
        wlr_log(
            WLR_DEBUG,
            "Gesture pinch threshold cancelled: scale=%.4f",
            gesture->scale
        );
        gesture_reset(gesture);
        return;
    }

    const OnyrionGestureDirection direction =
        gesture->scale < 1.0
            ? ONYRION_GESTURE_IN
            : ONYRION_GESTURE_OUT;

    if (!gesture_commit(
            gesture,
            ONYRION_INPUT_BINDING_GESTURE_PINCH,
            direction)) {
        gesture_reset(gesture);
    }
}

static void handle_cursor_frame(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            cursor_frame
        );

    wlr_seat_pointer_notify_frame(
        server->seat
    );
}

static void handle_request_set_cursor(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            request_set_cursor
        );

    struct wlr_seat_pointer_request_set_cursor_event *event =
        data;

    if (server->seat->pointer_state.focused_client !=
            event->seat_client) {
        return;
    }

    wlr_cursor_set_surface(
        server->cursor,
        event->surface,
        event->hotspot_x,
        event->hotspot_y
    );
}

static void handle_request_set_selection(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            request_set_selection
        );

    struct wlr_seat_request_set_selection_event *event =
        data;

    wlr_seat_set_selection(
        server->seat,
        event->source,
        event->serial
    );

    wlr_log(
        WLR_DEBUG,
        "Clipboard selection accepted: serial=%u source=%s",
        event->serial,
        event->source ? "set" : "clear"
    );
}

static void handle_request_start_drag(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            request_start_drag
        );

    struct wlr_seat_request_start_drag_event *event =
        data;

    if (onyrion_session_lock_active(server)) {
        if (event->drag->source) {
            wlr_data_source_destroy(event->drag->source);
        }
        return;
    }

    if (wlr_seat_validate_pointer_grab_serial(
            server->seat,
            event->origin,
            event->serial)) {
        wlr_seat_start_pointer_drag(
            server->seat,
            event->drag,
            event->serial
        );

        wlr_log(
            WLR_DEBUG,
            "DnD pointer drag accepted: serial=%u",
            event->serial
        );

        return;
    }

    wlr_log(
        WLR_DEBUG,
        "DnD start rejected: invalid pointer serial=%u",
        event->serial
    );

    if (event->drag->source) {
        wlr_data_source_destroy(
            event->drag->source
        );
    }
}

static void handle_start_drag(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            start_drag
        );

    struct wlr_drag *drag =
        data;

    if (onyrion_session_lock_active(server) || !drag->icon) {
        return;
    }

    struct wlr_scene_tree *tree =
        wlr_scene_drag_icon_create(
            server->drag_icons,
            drag->icon
        );

    if (!tree) {
        wlr_log(
            WLR_ERROR,
            "Failed to create DnD drag icon scene"
        );

        return;
    }

    wlr_scene_node_set_position(
        &tree->node,
        (int)server->cursor->x,
        (int)server->cursor->y
    );

    wlr_scene_node_raise_to_top(
        &server->drag_icons->node
    );

    wlr_log(
        WLR_DEBUG,
        "DnD drag icon created"
    );
}

static uint32_t keyboard_modifier_mask(
        OnyrionKeyboard *keyboard) {
    uint32_t modifiers = 0;

    if (xkb_state_mod_name_is_active(
            keyboard->wlr_keyboard->xkb_state,
            XKB_MOD_NAME_SHIFT,
            XKB_STATE_MODS_EFFECTIVE) > 0) {
        modifiers |=
            ONYRION_KEYBOARD_MODIFIER_SHIFT;
    }

    if (xkb_state_mod_name_is_active(
            keyboard->wlr_keyboard->xkb_state,
            XKB_MOD_NAME_CTRL,
            XKB_STATE_MODS_EFFECTIVE) > 0) {
        modifiers |=
            ONYRION_KEYBOARD_MODIFIER_CONTROL;
    }

    if (xkb_state_mod_name_is_active(
            keyboard->wlr_keyboard->xkb_state,
            XKB_MOD_NAME_ALT,
            XKB_STATE_MODS_EFFECTIVE) > 0) {
        modifiers |=
            ONYRION_KEYBOARD_MODIFIER_ALT;
    }

    if (xkb_state_mod_name_is_active(
            keyboard->wlr_keyboard->xkb_state,
            XKB_MOD_NAME_LOGO,
            XKB_STATE_MODS_EFFECTIVE) > 0) {
        modifiers |=
            ONYRION_KEYBOARD_MODIFIER_SUPER;
    }

    return modifiers;
}

static bool keyboard_modifier_name_active(
        OnyrionKeyboard *keyboard,
        const char *name) {
    return
        keyboard->wlr_keyboard->xkb_state &&
        xkb_state_mod_name_is_active(
            keyboard->wlr_keyboard->xkb_state,
            name,
            XKB_STATE_MODS_EFFECTIVE
        ) > 0;
}

static bool fallback_super_binding_active(
        OnyrionKeyboard *keyboard,
        xkb_keysym_t keysym,
        xkb_keysym_t expected) {
    if (keysym != expected) {
        return false;
    }

    return
        keyboard_modifier_name_active(
            keyboard,
            XKB_MOD_NAME_LOGO
        ) &&
        !keyboard_modifier_name_active(
            keyboard,
            XKB_MOD_NAME_SHIFT
        ) &&
        !keyboard_modifier_name_active(
            keyboard,
            XKB_MOD_NAME_CTRL
        ) &&
        !keyboard_modifier_name_active(
            keyboard,
            XKB_MOD_NAME_ALT
        );
}

static bool fallback_terminal_binding_active(
        OnyrionKeyboard *keyboard,
        xkb_keysym_t keysym) {
    return fallback_super_binding_active(
        keyboard,
        keysym,
        XKB_KEY_Return
    );
}

static bool fallback_close_binding_active(
        OnyrionKeyboard *keyboard,
        xkb_keysym_t keysym) {
    return fallback_super_binding_active(
        keyboard,
        keysym,
        XKB_KEY_q
    );
}

static void handle_keyboard_key(
        struct wl_listener *listener,
        void *data) {
    OnyrionKeyboard *keyboard =
        wl_container_of(
            listener,
            keyboard,
            key
        );

    struct wlr_keyboard_key_event *event =
        data;

    xkb_keysym_t keysym =
        XKB_KEY_NoSymbol;

    if (keyboard->wlr_keyboard->xkb_state) {
        keysym =
            xkb_state_key_get_one_sym(
                keyboard->wlr_keyboard->xkb_state,
                event->keycode + 8
            );
    }

    if (keyboard->server->session &&
            keysym >= XKB_KEY_XF86Switch_VT_1 &&
            keysym <= XKB_KEY_XF86Switch_VT_12) {
        if (event->state ==
                WL_KEYBOARD_KEY_STATE_PRESSED) {
            cancel_standalone_modifier_arms(
                keyboard->server,
                "vt-switch"
            );

            const unsigned vt =
                (unsigned)(
                    keysym -
                    XKB_KEY_XF86Switch_VT_1
                ) + 1u;

            if (!wlr_session_change_vt(
                    keyboard->server->session,
                    vt)) {
                wlr_log(
                    WLR_ERROR,
                    "VT switch failed: vt=%u",
                    vt
                );
            } else {
                wlr_log(
                    WLR_INFO,
                    "VT switch requested: vt=%u",
                    vt
                );
            }
        }

        return;
    }

    const bool standalone_modifier =
        keysym == XKB_KEY_Super_L ||
        keysym == XKB_KEY_Super_R;

    if (standalone_modifier) {
        handle_standalone_modifier_key(
            keyboard,
            event,
            keysym
        );
    } else if (event->state ==
            WL_KEYBOARD_KEY_STATE_PRESSED) {
        cancel_standalone_modifier_arms(
            keyboard->server,
            "keyboard-chord"
        );
    }

    if (!onyrion_session_lock_active(keyboard->server) &&
            !onyrion_layer_shell_has_keyboard_focus(
                keyboard->server) &&
            keysym != XKB_KEY_NoSymbol) {
        if (keyboard->server->fallback_active) {
            if (fallback_terminal_binding_active(
                    keyboard,
                    keysym)) {
                if (event->state ==
                        WL_KEYBOARD_KEY_STATE_PRESSED) {
                    onyrion_fallback_expect_recovery_window(
                        keyboard->server
                    );

                    if (!onyrion_recovery_spawn_terminal(
                            keyboard->server->socket)) {
                        onyrion_fallback_cancel_recovery_window(
                            keyboard->server
                        );

                        wlr_log(
                            WLR_ERROR,
                            "Fallback terminal binding failed"
                        );
                    }
                }

                return;
            }

            if (fallback_close_binding_active(
                    keyboard,
                    keysym)) {
                if (event->state ==
                        WL_KEYBOARD_KEY_STATE_PRESSED) {
                    if (onyrion_window_close_active(
                            keyboard->server)) {
                        wlr_log(
                            WLR_INFO,
                            "Fallback close action applied"
                        );
                    } else {
                        wlr_log(
                            WLR_DEBUG,
                            "Fallback close action ignored:"
                            " no active window"
                        );
                    }
                }

                return;
            }
        } else {
            const uint32_t modifiers =
                keyboard_modifier_mask(
                    keyboard
                );

            const OnyrionKeyboardBinding *binding =
                NULL;

            const OnyrionKeyboardBindingMatch match =
                onyrion_core_policy_match_binding(
                    keyboard->server->policy,
                    (uint32_t)keysym,
                    event->keycode,
                    modifiers,
                    &binding
                );

            if (match !=
                    ONYRION_KEYBOARD_BINDING_MATCH_NONE) {
                if (event->state ==
                        WL_KEYBOARD_KEY_STATE_PRESSED &&
                        match ==
                            ONYRION_KEYBOARD_BINDING_MATCH_ACTION &&
                        binding) {
                    if (onyrion_action_execute(
                            keyboard->server,
                            binding->action)) {
                        wlr_log(
                            WLR_DEBUG,
                            "Keyboard binding action applied:"
                            " kind=%d keysym=%u modifiers=%u"
                            " wildcard=%d",
                            binding->action.kind,
                            binding->keysym,
                            binding->modifiers,
                            binding->modifiers_wildcard
                        );
                    }
                }

                if (match ==
                        ONYRION_KEYBOARD_BINDING_MATCH_CONSUMED &&
                        event->state ==
                            WL_KEYBOARD_KEY_STATE_PRESSED) {
                    wlr_log(
                        WLR_DEBUG,
                        "Keyboard binding chord ignored:"
                        " ambiguous modifiers keysym=%u"
                        " modifiers=%u",
                        (uint32_t)keysym,
                        modifiers
                    );
                }

                return;
            }
        }
    }

    wlr_seat_set_keyboard(
        keyboard->server->seat,
        keyboard->wlr_keyboard
    );

    wlr_seat_keyboard_notify_key(
        keyboard->server->seat,
        event->time_msec,
        event->keycode,
        event->state
    );
}



static void handle_keyboard_modifiers(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionKeyboard *keyboard =
        wl_container_of(
            listener,
            keyboard,
            modifiers
        );

    wlr_seat_set_keyboard(
        keyboard->server->seat,
        keyboard->wlr_keyboard
    );

    wlr_seat_keyboard_notify_modifiers(
        keyboard->server->seat,
        &keyboard->wlr_keyboard->modifiers
    );
}

static void handle_keyboard_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionKeyboard *keyboard =
        wl_container_of(
            listener,
            keyboard,
            destroy
        );

    OnyrionServer *server =
        keyboard->server;

    destroy_standalone_release_timer(
        keyboard,
        "device-destroy"
    );

    reset_standalone_modifier_state(
        &keyboard->standalone_super_left
    );

    reset_standalone_modifier_state(
        &keyboard->standalone_super_right
    );

    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);

    free(keyboard);

    update_seat_capabilities(server);

    wlr_log(
        WLR_INFO,
        "Keyboard removed"
    );
}

static void handle_pointer_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionPointer *pointer =
        wl_container_of(
            listener,
            pointer,
            destroy
        );

    OnyrionServer *server =
        pointer->server;

    wl_list_remove(&pointer->destroy.link);
    wl_list_remove(&pointer->link);

    free(pointer);

    update_seat_capabilities(server);

    wlr_log(
        WLR_INFO,
        "Pointer removed"
    );
}

static struct xkb_keymap *keyboard_keymap_from_policy(
        const OnyrionKeyboardPolicy *policy) {
    struct xkb_context *context =
        xkb_context_new(
            XKB_CONTEXT_NO_FLAGS
        );

    if (!context) {
        return NULL;
    }

    const struct xkb_rule_names names = {
        .model =
            policy->model[0]
                ? policy->model
                : NULL,
        .layout =
            policy->layout[0]
                ? policy->layout
                : NULL,
        .variant =
            policy->variant[0]
                ? policy->variant
                : NULL,
        .options =
            policy->options[0]
                ? policy->options
                : NULL,
    };

    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(
            context,
            &names,
            XKB_KEYMAP_COMPILE_NO_FLAGS
        );

    xkb_context_unref(context);

    return keymap;
}

bool onyrion_input_apply_keyboard_policy(
        struct onyrion_server *server,
        const struct onyrion_keyboard_policy *policy) {
    if (!server ||
            !server->policy ||
            !policy) {
        return false;
    }

    reset_all_standalone_modifier_states(
        server
    );

    struct xkb_keymap *candidate =
        keyboard_keymap_from_policy(
            policy
        );

    if (!candidate) {
        wlr_log(
            WLR_ERROR,
            "Failed to compile candidate XKB keymap"
        );

        return false;
    }

    struct xkb_keymap *rollback =
        keyboard_keymap_from_policy(
            &server->policy->
                input.keyboard
        );

    if (!rollback) {
        xkb_keymap_unref(candidate);

        wlr_log(
            WLR_ERROR,
            "Failed to compile rollback XKB keymap"
        );

        return false;
    }

    OnyrionKeyboard *keyboard;

    wl_list_for_each(
            keyboard,
            &server->keyboards,
            link) {
        if (!wlr_keyboard_set_keymap(
                keyboard->wlr_keyboard,
                candidate)) {
            wlr_log(
                WLR_ERROR,
                "Failed to apply candidate XKB keymap;"
                " rolling back keyboards"
            );

            OnyrionKeyboard *rollback_keyboard;

            wl_list_for_each(
                    rollback_keyboard,
                    &server->keyboards,
                    link) {
                if (!wlr_keyboard_set_keymap(
                        rollback_keyboard->
                            wlr_keyboard,
                        rollback)) {
                    wlr_log(
                        WLR_ERROR,
                        "Keyboard rollback keymap failed"
                    );
                }

                wlr_keyboard_set_repeat_info(
                    rollback_keyboard->
                        wlr_keyboard,
                    server->policy->
                        input.keyboard.repeat_rate,
                    server->policy->
                        input.keyboard.repeat_delay
                );
            }

            xkb_keymap_unref(rollback);
            xkb_keymap_unref(candidate);

            return false;
        }

        wlr_keyboard_set_repeat_info(
            keyboard->wlr_keyboard,
            policy->repeat_rate,
            policy->repeat_delay
        );
    }

    xkb_keymap_unref(rollback);
    xkb_keymap_unref(candidate);

    return true;
}

static bool add_keyboard(
        OnyrionServer *server,
        struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard =
        wlr_keyboard_from_input_device(device);

    OnyrionKeyboard *keyboard =
        calloc(1, sizeof(*keyboard));

    if (!keyboard) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate OnyrionKeyboard"
        );
        return false;
    }

    const OnyrionKeyboardPolicy *policy =
        &server->policy->input.keyboard;

    struct xkb_keymap *keymap =
        keyboard_keymap_from_policy(
            policy
        );

    if (!keymap) {
        wlr_log(
            WLR_ERROR,
            "Failed to create XKB keymap"
        );

        free(keyboard);
        return false;
    }

    if (!wlr_keyboard_set_keymap(
            wlr_keyboard,
            keymap)) {
        wlr_log(
            WLR_ERROR,
            "Failed to set keyboard keymap"
        );

        xkb_keymap_unref(keymap);
        free(keyboard);
        return false;
    }

    wlr_log(
        WLR_INFO,
        "Keyboard keymap configured:"
        " layout=%s model=%s variant=%s options=%s",
        policy->layout[0]
            ? policy->layout
            : "<default>",
        policy->model[0]
            ? policy->model
            : "<default>",
        policy->variant[0]
            ? policy->variant
            : "<default>",
        policy->options[0]
            ? policy->options
            : "<default>"
    );

    xkb_keymap_unref(keymap);

    wlr_keyboard_set_repeat_info(
        wlr_keyboard,
        policy->repeat_rate,
        policy->repeat_delay
    );

    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    keyboard->key.notify =
        handle_keyboard_key;

    wl_signal_add(
        &wlr_keyboard->events.key,
        &keyboard->key
    );

    keyboard->modifiers.notify =
        handle_keyboard_modifiers;

    wl_signal_add(
        &wlr_keyboard->events.modifiers,
        &keyboard->modifiers
    );

    keyboard->destroy.notify =
        handle_keyboard_destroy;

    wl_signal_add(
        &device->events.destroy,
        &keyboard->destroy
    );

    wl_list_insert(
        &server->keyboards,
        &keyboard->link
    );

    wlr_seat_set_keyboard(
        server->seat,
        wlr_keyboard
    );

    update_seat_capabilities(server);

    onyrion_input_focus_window(
        server,
        active_window(server)
    );

    wlr_log(
        WLR_INFO,
        "Keyboard added: %s",
        device->name
            ? device->name
            : "<unnamed>"
    );

    return true;
}

static OnyrionOutput *pointer_output(
        OnyrionServer *server,
        struct wlr_pointer *wlr_pointer) {
    if (!server ||
            !wlr_pointer ||
            !wlr_pointer->output_name ||
            wlr_pointer->output_name[0] == '\0') {
        return NULL;
    }

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        if (!output->wlr_output ||
                !output->wlr_output->name) {
            continue;
        }

        if (strcmp(
                output->wlr_output->name,
                wlr_pointer->output_name) == 0) {
            return output;
        }
    }

    return NULL;
}

static bool add_pointer(
        OnyrionServer *server,
        struct wlr_input_device *device) {
    OnyrionPointer *pointer =
        calloc(1, sizeof(*pointer));

    if (!pointer) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate OnyrionPointer"
        );
        return false;
    }

    pointer->server = server;
    pointer->device = device;
    pointer->scroll_factor = 1.0;

    if (!apply_pointer_policy_one(
            pointer,
            &server->policy->input)) {
        wlr_log(
            WLR_ERROR,
            "Failed to apply current pointer policy:"
            " device=%s",
            device->name
                ? device->name
                : "<unnamed>"
        );

        free(pointer);
        return false;
    }

    pointer->destroy.notify =
        handle_pointer_destroy;

    wl_signal_add(
        &device->events.destroy,
        &pointer->destroy
    );

    wl_list_insert(
        &server->pointers,
        &pointer->link
    );

    wlr_cursor_attach_input_device(
        server->cursor,
        device
    );

    struct wlr_pointer *wlr_pointer =
        wlr_pointer_from_input_device(device);

    OnyrionOutput *mapped_output =
        pointer_output(
            server,
            wlr_pointer
        );

    if (mapped_output) {
        wlr_cursor_map_input_to_output(
            server->cursor,
            device,
            mapped_output->wlr_output
        );

        wlr_log(
            WLR_INFO,
            "Pointer mapped: device=%s output=%s",
            device->name
                ? device->name
                : "<unnamed>",
            mapped_output->wlr_output->name
        );
    }

    update_seat_capabilities(server);

    wlr_log(
        WLR_INFO,
        "Pointer added: %s",
        device->name
            ? device->name
            : "<unnamed>"
    );

    return true;
}

static void handle_new_input(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            new_input
        );

    struct wlr_input_device *device =
        data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        if (!add_keyboard(server, device)) {
            wlr_log(
                WLR_ERROR,
                "Failed to initialize keyboard"
            );
        }
        break;

    case WLR_INPUT_DEVICE_POINTER:
        if (!add_pointer(server, device)) {
            wlr_log(
                WLR_ERROR,
                "Failed to initialize pointer"
            );
        }
        break;

    default:
        wlr_log(
            WLR_DEBUG,
            "Ignoring unsupported input device type=%d",
            device->type
        );
        break;
    }
}

bool onyrion_input_init(
        struct onyrion_server *server) {
    wl_list_init(&server->keyboards);
    wl_list_init(&server->pointers);

    server->seat =
        wlr_seat_create(
            server->display,
            "seat0"
        );

    if (!server->seat) {
        wlr_log(
            WLR_ERROR,
            "Failed to create seat"
        );
        return false;
    }

    server->cursor =
        wlr_cursor_create();

    if (!server->cursor) {
        wlr_log(
            WLR_ERROR,
            "Failed to create cursor"
        );
        return false;
    }

    wlr_cursor_attach_output_layout(
        server->cursor,
        server->output_layout
    );

    server->xcursor_manager =
        wlr_xcursor_manager_create(
            NULL,
            24
        );

    if (!server->xcursor_manager) {
        wlr_log(
            WLR_ERROR,
            "Failed to create XCursor manager"
        );

        return false;
    }

    if (!wlr_xcursor_manager_load(
            server->xcursor_manager,
            1.0f)) {
        wlr_log(
            WLR_ERROR,
            "Failed to load XCursor theme at scale=1"
        );

        return false;
    }

    if (!set_default_cursor(server)) {
        return false;
    }

    server->drag_icons =
        wlr_scene_tree_create(
            &server->scene->tree
        );

    if (!server->drag_icons) {
        wlr_log(
            WLR_ERROR,
            "Failed to create drag icon scene"
        );

        return false;
    }

    server->cursor_motion.notify =
        handle_cursor_motion;

    wl_signal_add(
        &server->cursor->events.motion,
        &server->cursor_motion
    );

    server->cursor_motion_absolute.notify =
        handle_cursor_motion_absolute;

    wl_signal_add(
        &server->cursor->events.motion_absolute,
        &server->cursor_motion_absolute
    );

    server->cursor_button.notify =
        handle_cursor_button;

    wl_signal_add(
        &server->cursor->events.button,
        &server->cursor_button
    );

    server->cursor_axis.notify =
        handle_cursor_axis;

    wl_signal_add(
        &server->cursor->events.axis,
        &server->cursor_axis
    );

    server->cursor_frame.notify =
        handle_cursor_frame;

    wl_signal_add(
        &server->cursor->events.frame,
        &server->cursor_frame
    );

    server->gesture_recognizer =
        calloc(1, sizeof(*server->gesture_recognizer));

    if (!server->gesture_recognizer) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate gesture recognizer"
        );
        return false;
    }

    OnyrionGestureRecognizer *gesture =
        server->gesture_recognizer;

    gesture->server = server;
    gesture_reset(gesture);

    gesture->swipe_begin.notify = handle_swipe_begin;
    gesture->swipe_update.notify = handle_swipe_update;
    gesture->swipe_end.notify = handle_swipe_end;
    gesture->pinch_begin.notify = handle_pinch_begin;
    gesture->pinch_update.notify = handle_pinch_update;
    gesture->pinch_end.notify = handle_pinch_end;

    wl_signal_add(&server->cursor->events.swipe_begin, &gesture->swipe_begin);
    wl_signal_add(&server->cursor->events.swipe_update, &gesture->swipe_update);
    wl_signal_add(&server->cursor->events.swipe_end, &gesture->swipe_end);
    wl_signal_add(&server->cursor->events.pinch_begin, &gesture->pinch_begin);
    wl_signal_add(&server->cursor->events.pinch_update, &gesture->pinch_update);
    wl_signal_add(&server->cursor->events.pinch_end, &gesture->pinch_end);

    server->request_set_cursor.notify =
        handle_request_set_cursor;

    wl_signal_add(
        &server->seat->events.request_set_cursor,
        &server->request_set_cursor
    );

    server->request_set_selection.notify =
        handle_request_set_selection;

    wl_signal_add(
        &server->seat->events.request_set_selection,
        &server->request_set_selection
    );

    server->request_start_drag.notify =
        handle_request_start_drag;

    wl_signal_add(
        &server->seat->events.request_start_drag,
        &server->request_start_drag
    );

    server->start_drag.notify =
        handle_start_drag;

    wl_signal_add(
        &server->seat->events.start_drag,
        &server->start_drag
    );

    server->new_input.notify =
        handle_new_input;

    wl_signal_add(
        &server->backend->events.new_input,
        &server->new_input
    );

    return true;
}

void onyrion_input_finish(
        struct onyrion_server *server) {
    if (server->new_input.notify) {
        wl_list_remove(
            &server->new_input.link
        );

        server->new_input.notify = NULL;
    }

    if (server->start_drag.notify) {
        wl_list_remove(
            &server->start_drag.link
        );

        server->start_drag.notify = NULL;
    }

    if (server->request_start_drag.notify) {
        wl_list_remove(
            &server->request_start_drag.link
        );

        server->request_start_drag.notify = NULL;
    }

    if (server->request_set_selection.notify) {
        wl_list_remove(
            &server->request_set_selection.link
        );

        server->request_set_selection.notify = NULL;
    }

    if (server->request_set_cursor.notify) {
        wl_list_remove(
            &server->request_set_cursor.link
        );

        server->request_set_cursor.notify = NULL;
    }

    if (server->cursor_motion.notify) {
        wl_list_remove(&server->cursor_motion.link);
        server->cursor_motion.notify = NULL;
    }

    if (server->cursor_motion_absolute.notify) {
        wl_list_remove(
            &server->cursor_motion_absolute.link
        );

        server->cursor_motion_absolute.notify = NULL;
    }

    if (server->cursor_button.notify) {
        wl_list_remove(&server->cursor_button.link);
        server->cursor_button.notify = NULL;
    }

    if (server->cursor_axis.notify) {
        wl_list_remove(&server->cursor_axis.link);
        server->cursor_axis.notify = NULL;
    }

    if (server->gesture_recognizer) {
        OnyrionGestureRecognizer *gesture =
            server->gesture_recognizer;

        struct wl_listener *listeners[] = {
            &gesture->swipe_begin,
            &gesture->swipe_update,
            &gesture->swipe_end,
            &gesture->pinch_begin,
            &gesture->pinch_update,
            &gesture->pinch_end,
        };

        for (size_t i = 0;
                i < sizeof(listeners) / sizeof(listeners[0]);
                i++) {
            if (listeners[i]->notify) {
                wl_list_remove(&listeners[i]->link);
                listeners[i]->notify = NULL;
            }
        }

        free(gesture);
        server->gesture_recognizer = NULL;
    }

    if (server->cursor_frame.notify) {
        wl_list_remove(&server->cursor_frame.link);
        server->cursor_frame.notify = NULL;
    }

    OnyrionKeyboard *keyboard;
    OnyrionKeyboard *keyboard_tmp;

    wl_list_for_each_safe(
            keyboard,
            keyboard_tmp,
            &server->keyboards,
            link) {
        destroy_standalone_release_timer(
            keyboard,
            "input-finish"
        );

        wl_list_remove(&keyboard->key.link);
        wl_list_remove(&keyboard->modifiers.link);
        wl_list_remove(&keyboard->destroy.link);
        wl_list_remove(&keyboard->link);

        free(keyboard);
    }

    OnyrionPointer *pointer;
    OnyrionPointer *pointer_tmp;

    wl_list_for_each_safe(
            pointer,
            pointer_tmp,
            &server->pointers,
            link) {
        wl_list_remove(&pointer->destroy.link);
        wl_list_remove(&pointer->link);

        free(pointer);
    }

    if (server->drag_icons) {
        wlr_scene_node_destroy(
            &server->drag_icons->node
        );

        server->drag_icons = NULL;
    }

    if (server->xcursor_manager) {
        wlr_xcursor_manager_destroy(
            server->xcursor_manager
        );

        server->xcursor_manager = NULL;
    }

    if (server->cursor) {
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
    }

    if (server->seat) {
        wlr_seat_destroy(server->seat);
        server->seat = NULL;
    }
}
