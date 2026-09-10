#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include "onyrion-shell-unstable-v1-client-protocol.h"

#define ONYRION_REQUIRED_PROTOCOL_VERSION 14u
#define ONYRION_BOOTSTRAP_WIDTH 1
#define ONYRION_BOOTSTRAP_HEIGHT 1
#define ONYRION_BOOTSTRAP_LAYER ONYRION_GROUP_SURFACE_V1_LAYER_ABOVE_CLIENT
#define ONYRION_TABGROUP_NAMESPACE "onyrion.tabgroup"
#define ONYRION_WL_SEAT_VERSION 5u

typedef enum {
    ONYRION_DRAG_NONE = 0,
    ONYRION_DRAG_WINDOW,
    ONYRION_DRAG_GROUP,
} OnyrionDragKind;

typedef struct GtkLayerShellExternalRole GtkLayerShellExternalRole;

typedef gboolean (*ExternalRoleCreateFunc)(
    GtkLayerShellExternalRole *role,
    struct wl_display *display,
    struct wl_surface *surface,
    gpointer user_data
);

typedef void (*ExternalRoleDestroyFunc)(
    GtkLayerShellExternalRole *role,
    gpointer user_data
);

typedef GtkLayerShellExternalRole *(*ExternalRoleInitForWindowFn)(
    GtkWindow *window,
    ExternalRoleCreateFunc create_func,
    ExternalRoleDestroyFunc destroy_func,
    gpointer user_data,
    GDestroyNotify user_data_destroy
);

typedef void (*ExternalRoleConfigureFn)(
    GtkLayerShellExternalRole *role,
    int width,
    int height
);

/*
 * Implemented by the Rust TabGroup renderer in the same plugin DSO. These are
 * direct same-process callbacks: no HostProxy/EwwiiAPI re-entry.
 */
extern void onyrion_tabgroup_group_configure(
    uintptr_t gtk_window_ptr,
    int group_width,
    int group_height
);

extern void onyrion_tabgroup_role_released(
    uintptr_t gtk_window_ptr
);

extern void onyrion_tabgroup_drag_surface_motion(
    const char *group_id,
    double x,
    double y
);

typedef struct {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct onyrion_shell_unstable_v1 *shell;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_surface *pointer_focus_surface;
    struct wl_surface *primary_press_surface;
    uint32_t seat_global_name;
    uint32_t advertised_version;
    uint32_t primary_press_serial;
    guint dispatch_source_id;
    OnyrionDragKind drag_kind;
    bool primary_down;
    bool primary_drag_consumed;
    bool initialized;
    bool available;
    char *last_target_group_id;
    char *last_target_window_id;
    uint32_t last_target_kind;
    bool last_target_valid;
} OnyrionBridgeProtocol;

typedef struct {
    GtkWindow *window;
    GtkLayerShellExternalRole *external_role;
    struct wl_surface *wl_surface;
    struct onyrion_group_surface_v1 *group_surface;
    char *group_id;
    char *name_space;
    int x;
    int y;
    int width;
    int height;
    int group_outer_width;
    int group_outer_height;
    int ui_height;
    uint32_t layer;
    bool tabgroup;
    bool claimed;
} OnyrionWindowRole;

static OnyrionBridgeProtocol protocol_state = {0};
static GList *window_roles = NULL;

static ExternalRoleInitForWindowFn external_role_init = NULL;
static ExternalRoleConfigureFn external_role_configure = NULL;
static bool external_role_api_checked = false;

static void trace_line(
        const char *event,
        const OnyrionWindowRole *role) {
    fprintf(
        stderr,
        "[onyrion-group-surface-bridge] %s group=%s namespace=%s claimed=%d\n",
        event ? event : "UNKNOWN",
        role && role->group_id ? role->group_id : "",
        role && role->name_space ? role->name_space : "",
        role && role->claimed ? 1 : 0
    );
}

static void reset_drag_target_cache(
        OnyrionBridgeProtocol *state) {
    if (!state) {
        return;
    }

    free(state->last_target_group_id);
    free(state->last_target_window_id);
    state->last_target_group_id = NULL;
    state->last_target_window_id = NULL;
    state->last_target_kind =
        ONYRION_SHELL_UNSTABLE_V1_DRAG_TARGET_KIND_NONE;
    state->last_target_valid = false;
}

static void reset_primary_drag(
        OnyrionBridgeProtocol *state,
        bool clear_press) {
    if (!state) {
        return;
    }

    state->drag_kind = ONYRION_DRAG_NONE;
    state->primary_drag_consumed = false;
    reset_drag_target_cache(state);

    if (clear_press) {
        state->primary_down = false;
        state->primary_press_serial = 0;
        state->primary_press_surface = NULL;
    }
}

static void pointer_enter(
        void *data,
        struct wl_pointer *pointer,
        uint32_t serial,
        struct wl_surface *surface,
        wl_fixed_t surface_x,
        wl_fixed_t surface_y) {
    (void)pointer;
    (void)serial;
    (void)surface_x;
    (void)surface_y;

    OnyrionBridgeProtocol *state = data;
    if (state) {
        state->pointer_focus_surface = surface;
    }
}

static void pointer_leave(
        void *data,
        struct wl_pointer *pointer,
        uint32_t serial,
        struct wl_surface *surface) {
    (void)pointer;
    (void)serial;

    OnyrionBridgeProtocol *state = data;
    if (state &&
            state->pointer_focus_surface == surface &&
            !state->primary_down) {
        state->pointer_focus_surface = NULL;
    }
}

static void pointer_motion(
        void *data,
        struct wl_pointer *pointer,
        uint32_t time,
        wl_fixed_t surface_x,
        wl_fixed_t surface_y) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)surface_x;
    (void)surface_y;
}

static void pointer_button(
        void *data,
        struct wl_pointer *pointer,
        uint32_t serial,
        uint32_t time,
        uint32_t button,
        uint32_t button_state) {
    (void)pointer;
    (void)time;

    OnyrionBridgeProtocol *state = data;
    if (!state || button != BTN_LEFT) {
        return;
    }

    if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
        reset_primary_drag(state, false);
        state->primary_down = true;
        state->primary_press_serial = serial;
        state->primary_press_surface =
            state->pointer_focus_surface;

        fprintf(
            stderr,
            "[onyrion-group-surface-bridge] POINTER_PRIMARY_PRESS serial=%u surface=%p\n",
            serial,
            (void *)state->primary_press_surface
        );
        return;
    }

    if (button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_primary_drag(state, true);

        fprintf(
            stderr,
            "[onyrion-group-surface-bridge] POINTER_PRIMARY_RELEASE serial=%u\n",
            serial
        );
    }
}

static void pointer_axis(
        void *data,
        struct wl_pointer *pointer,
        uint32_t time,
        uint32_t axis,
        wl_fixed_t value) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    (void)value;
}

static void pointer_frame(
        void *data,
        struct wl_pointer *pointer) {
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(
        void *data,
        struct wl_pointer *pointer,
        uint32_t axis_source) {
    (void)data;
    (void)pointer;
    (void)axis_source;
}

static void pointer_axis_stop(
        void *data,
        struct wl_pointer *pointer,
        uint32_t time,
        uint32_t axis) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(
        void *data,
        struct wl_pointer *pointer,
        uint32_t axis,
        int32_t discrete) {
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(
        void *data,
        struct wl_seat *seat,
        uint32_t capabilities) {
    OnyrionBridgeProtocol *state = data;
    if (!state || seat != state->seat) {
        return;
    }

    const bool has_pointer =
        (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;

    if (has_pointer && !state->pointer) {
        state->pointer =
            wl_seat_get_pointer(seat);

        if (!state->pointer) {
            fprintf(
                stderr,
                "[onyrion-group-surface-bridge] POINTER_READY status=FAIL reason=get_pointer\n"
            );
            return;
        }

        wl_proxy_set_queue(
            (struct wl_proxy *)state->pointer,
            state->queue
        );

        if (wl_pointer_add_listener(
                state->pointer,
                &pointer_listener,
                state) != 0) {
            wl_pointer_release(state->pointer);
            state->pointer = NULL;

            fprintf(
                stderr,
                "[onyrion-group-surface-bridge] POINTER_READY status=FAIL reason=listener\n"
            );
            return;
        }

        fprintf(
            stderr,
            "[onyrion-group-surface-bridge] POINTER_READY status=PASS\n"
        );
        return;
    }

    if (!has_pointer && state->pointer) {
        wl_pointer_release(state->pointer);
        state->pointer = NULL;
        state->pointer_focus_surface = NULL;
        reset_primary_drag(state, true);

        fprintf(
            stderr,
            "[onyrion-group-surface-bridge] POINTER_READY status=REMOVED\n"
        );
    }
}

static void seat_name(
        void *data,
        struct wl_seat *seat,
        const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void shell_state_begin(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation,
        uint32_t workspace_count) {
    (void)data;
    (void)shell;
    (void)generation;
    (void)workspace_count;
}

static void shell_workspace(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        uint32_t active,
        uint32_t group_count,
        uint32_t tile_count,
        uint32_t window_count) {
    (void)data;
    (void)shell;
    (void)id;
    (void)active;
    (void)group_count;
    (void)tile_count;
    (void)window_count;
}

static void shell_state_end(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation) {
    (void)data;
    (void)shell;
    (void)generation;
}

static void shell_changed(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation) {
    (void)data;
    (void)shell;
    (void)generation;
}

static void shell_action_result(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t action,
        uint32_t success) {
    (void)data;
    (void)shell;
    (void)action;
    (void)success;
}

static void shell_group(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        const char *workspace_id,
        uint32_t active,
        uint32_t window_count) {
    (void)data;
    (void)shell;
    (void)id;
    (void)workspace_id;
    (void)active;
    (void)window_count;
}

static void shell_window(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        const char *group_id,
        uint32_t active,
        uint32_t fullscreen,
        const char *app_id,
        const char *title) {
    (void)data;
    (void)shell;
    (void)id;
    (void)group_id;
    (void)active;
    (void)fullscreen;
    (void)app_id;
    (void)title;
}

static void shell_controller_claim_result(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t success) {
    (void)data;
    (void)shell;
    (void)success;
}

static void shell_controller_invoke(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t serial,
        const char *capability,
        const char *action) {
    (void)data;
    (void)shell;
    (void)serial;
    (void)capability;
    (void)action;
}

static void shell_output(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *name,
        uint32_t focused,
        const char *visible_workspace_id,
        uint32_t assigned_workspace_count) {
    (void)data;
    (void)shell;
    (void)name;
    (void)focused;
    (void)visible_workspace_id;
    (void)assigned_workspace_count;
}

static void shell_workspace_output(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *workspace_id,
        const char *output_name,
        uint32_t visible) {
    (void)data;
    (void)shell;
    (void)workspace_id;
    (void)output_name;
    (void)visible;
}

static void shell_workspace_metadata(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *workspace_id,
        const char *name,
        const char *icon,
        uint32_t persistent,
        uint32_t startup,
        const char *output_affinity) {
    (void)data;
    (void)shell;
    (void)workspace_id;
    (void)name;
    (void)icon;
    (void)persistent;
    (void)startup;
    (void)output_affinity;
}

static void shell_window_placement(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *window_id,
        const char *workspace_id,
        uint32_t placement,
        const char *parent_window_id) {
    (void)data;
    (void)shell;
    (void)window_id;
    (void)workspace_id;
    (void)placement;
    (void)parent_window_id;
}

static void shell_group_placement(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *group_id,
        uint32_t placement,
        uint32_t pinned,
        const char *pinned_output_name) {
    (void)data;
    (void)shell;
    (void)group_id;
    (void)placement;
    (void)pinned;
    (void)pinned_output_name;
}

static void shell_drag_surface_motion(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *group_id,
        const char *name_space,
        wl_fixed_t x,
        wl_fixed_t y) {
    (void)data;
    (void)shell;

    if (protocol_state.drag_kind != ONYRION_DRAG_WINDOW ||
            !group_id ||
            !*group_id ||
            !name_space ||
            strcmp(
                name_space,
                ONYRION_TABGROUP_NAMESPACE
            ) != 0) {
        return;
    }

    onyrion_tabgroup_drag_surface_motion(
        group_id,
        wl_fixed_to_double(x),
        wl_fixed_to_double(y)
    );
}

static const struct onyrion_shell_unstable_v1_listener shell_listener = {
    .state_begin = shell_state_begin,
    .workspace = shell_workspace,
    .state_end = shell_state_end,
    .changed = shell_changed,
    .action_result = shell_action_result,
    .group = shell_group,
    .window = shell_window,
    .controller_claim_result = shell_controller_claim_result,
    .controller_invoke = shell_controller_invoke,
    .output = shell_output,
    .workspace_output = shell_workspace_output,
    .workspace_metadata = shell_workspace_metadata,
    .window_placement = shell_window_placement,
    .group_placement = shell_group_placement,
    .drag_surface_motion = shell_drag_surface_motion,
};

static void group_surface_configure(
        void *data,
        struct onyrion_group_surface_v1 *group_surface,
        int32_t width,
        int32_t height) {
    OnyrionWindowRole *role = data;
    (void)group_surface;

    if (!role ||
            width <= 0 ||
            height <= 0) {
        return;
    }

    role->group_outer_width = width;
    role->group_outer_height = height;

    fprintf(
        stderr,
        "[onyrion-group-surface-bridge] GROUP_CONFIGURE group=%s namespace=%s outer=%dx%d\n",
        role->group_id ? role->group_id : "",
        role->name_space ? role->name_space : "",
        width,
        height
    );

    if (!role->tabgroup ||
            !role->external_role ||
            !role->group_surface) {
        return;
    }

    const int effective_height =
        role->ui_height < height
            ? role->ui_height
            : height;

    if (effective_height <= 0) {
        return;
    }

    role->x = 0;
    role->y = 0;
    role->width = width;
    role->height = effective_height;

    /*
     * v14 outer geometry is authoritative input. Shell chooses the concrete
     * top TabGroup policy here: surface rect and content reservation are
     * separate requests even though they currently share the same height.
     */
    onyrion_group_surface_v1_set_rect(
        role->group_surface,
        0,
        0,
        width,
        effective_height
    );

    onyrion_group_surface_v1_set_layer(
        role->group_surface,
        role->layer
    );

    onyrion_shell_unstable_v1_set_group_content_insets(
        protocol_state.shell,
        role->group_id,
        (uint32_t)effective_height,
        0,
        0,
        0
    );

    external_role_configure(
        role->external_role,
        width,
        effective_height
    );

    onyrion_tabgroup_group_configure(
        (uintptr_t)role->window,
        width,
        height
    );

    if (protocol_state.display) {
        (void)wl_display_flush(
            protocol_state.display
        );
    }
}

static const struct onyrion_group_surface_v1_listener group_surface_listener = {
    .configure = group_surface_configure,
};

typedef struct {
    struct wl_event_queue *queue;
    struct onyrion_shell_unstable_v1 *shell;
    uint32_t advertised_version;
} RegistryProbe;

static void registry_global(
        void *data,
        struct wl_registry *registry,
        uint32_t name,
        const char *interface,
        uint32_t version) {
    RegistryProbe *probe = data;

    if (!probe || !interface) {
        return;
    }

    if (strcmp(
            interface,
            wl_seat_interface.name
        ) == 0) {
        if (!protocol_state.seat) {
            if (version >= ONYRION_WL_SEAT_VERSION) {
                protocol_state.seat =
                    wl_registry_bind(
                        registry,
                        name,
                        &wl_seat_interface,
                        ONYRION_WL_SEAT_VERSION
                    );

                if (protocol_state.seat) {
                    protocol_state.seat_global_name = name;

                    wl_proxy_set_queue(
                        (struct wl_proxy *)protocol_state.seat,
                        probe->queue
                    );

                    if (wl_seat_add_listener(
                            protocol_state.seat,
                            &seat_listener,
                            &protocol_state) != 0) {
                        wl_seat_release(protocol_state.seat);
                        protocol_state.seat = NULL;
                        protocol_state.seat_global_name = 0;
                    }
                }
            }
        }
        return;
    }

    if (strcmp(
            interface,
            onyrion_shell_unstable_v1_interface.name
        ) != 0) {
        return;
    }

    probe->advertised_version = version;

    if (version < ONYRION_REQUIRED_PROTOCOL_VERSION ||
            probe->shell) {
        return;
    }

    probe->shell = wl_registry_bind(
        registry,
        name,
        &onyrion_shell_unstable_v1_interface,
        ONYRION_REQUIRED_PROTOCOL_VERSION
    );

    if (!probe->shell) {
        return;
    }

    wl_proxy_set_queue(
        (struct wl_proxy *)probe->shell,
        probe->queue
    );

    if (onyrion_shell_unstable_v1_add_listener(
            probe->shell,
            &shell_listener,
            NULL) != 0) {
        onyrion_shell_unstable_v1_destroy(
            probe->shell
        );
        probe->shell = NULL;
    }
}

static void registry_global_remove(
        void *data,
        struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;

    if (name != protocol_state.seat_global_name) {
        return;
    }

    if (protocol_state.pointer) {
        wl_pointer_release(protocol_state.pointer);
        protocol_state.pointer = NULL;
    }

    if (protocol_state.seat) {
        wl_seat_release(protocol_state.seat);
        protocol_state.seat = NULL;
    }

    protocol_state.seat_global_name = 0;
    protocol_state.pointer_focus_surface = NULL;
    reset_primary_drag(&protocol_state, true);
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static bool load_external_role_api(void) {
    if (external_role_api_checked) {
        return external_role_init && external_role_configure;
    }

    external_role_api_checked = true;

    external_role_init =
        (ExternalRoleInitForWindowFn)dlsym(
            RTLD_DEFAULT,
            "gtk_layer_external_role_init_for_window"
        );

    external_role_configure =
        (ExternalRoleConfigureFn)dlsym(
            RTLD_DEFAULT,
            "gtk_layer_external_role_configure"
        );

    return external_role_init && external_role_configure;
}

static gboolean dispatch_protocol_queue(
        gpointer user_data) {
    OnyrionBridgeProtocol *state = user_data;

    if (!state ||
            !state->display ||
            !state->queue) {
        return G_SOURCE_REMOVE;
    }

    if (wl_display_dispatch_queue_pending(
            state->display,
            state->queue) < 0) {
        fprintf(
            stderr,
            "[onyrion-group-surface-bridge] protocol queue dispatch failed\n"
        );
        state->dispatch_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static int ensure_protocol(void) {
    if (protocol_state.initialized) {
        return protocol_state.available ? 1 : 0;
    }

    protocol_state.initialized = true;

    GdkDisplay *gdk_display =
        gdk_display_get_default();

    if (!gdk_display ||
            !GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        return 0;
    }

    struct wl_display *display =
        gdk_wayland_display_get_wl_display(
            gdk_display
        );

    if (!display) {
        return 0;
    }

    struct wl_event_queue *queue =
        wl_display_create_queue(display);

    if (!queue) {
        return -1;
    }

    protocol_state.display = display;
    protocol_state.queue = queue;

    struct wl_registry *registry =
        wl_display_get_registry(display);

    if (!registry) {
        protocol_state.display = NULL;
        protocol_state.queue = NULL;
        wl_event_queue_destroy(queue);
        return -1;
    }

    wl_proxy_set_queue(
        (struct wl_proxy *)registry,
        queue
    );

    RegistryProbe probe = {
        .queue = queue,
        .shell = NULL,
        .advertised_version = 0,
    };

    if (wl_registry_add_listener(
            registry,
            &registry_listener,
            &probe) != 0) {
        wl_registry_destroy(registry);
        protocol_state.display = NULL;
        protocol_state.queue = NULL;
        wl_event_queue_destroy(queue);
        return -1;
    }

    if (wl_display_roundtrip_queue(
            display,
            queue) < 0) {
        wl_registry_destroy(registry);

        if (protocol_state.pointer) {
            wl_pointer_release(protocol_state.pointer);
            protocol_state.pointer = NULL;
        }
        if (protocol_state.seat) {
            wl_seat_release(protocol_state.seat);
            protocol_state.seat = NULL;
        }

        protocol_state.display = NULL;
        protocol_state.queue = NULL;
        wl_event_queue_destroy(queue);
        return -1;
    }

    /*
     * The first roundtrip discovers/binds wl_seat. A second roundtrip makes
     * its capabilities deterministic before the pre-realize hook returns.
     * Pointer presence is optional for rendering; it is required only when a
     * real Shell-origin drag is attempted.
     */
    if (protocol_state.seat &&
            wl_display_roundtrip_queue(
                display,
                queue) < 0) {
        wl_registry_destroy(registry);

        if (protocol_state.pointer) {
            wl_pointer_release(protocol_state.pointer);
            protocol_state.pointer = NULL;
        }
        wl_seat_release(protocol_state.seat);
        protocol_state.seat = NULL;

        if (probe.shell) {
            onyrion_shell_unstable_v1_destroy(
                probe.shell
            );
            probe.shell = NULL;
        }

        protocol_state.display = NULL;
        protocol_state.queue = NULL;
        wl_event_queue_destroy(queue);
        return -1;
    }

    if (probe.shell) {
        (void)wl_display_dispatch_queue_pending(
            display,
            queue
        );
    }

    wl_registry_destroy(registry);
    (void)wl_display_flush(display);

    protocol_state.display = display;
    protocol_state.queue = probe.shell ? queue : NULL;
    protocol_state.shell = probe.shell;
    protocol_state.advertised_version =
        probe.advertised_version;
    protocol_state.available =
        probe.shell != NULL &&
        probe.advertised_version >=
            ONYRION_REQUIRED_PROTOCOL_VERSION;

    if (protocol_state.available) {
        protocol_state.dispatch_source_id =
            g_timeout_add(
                25,
                dispatch_protocol_queue,
                &protocol_state
            );
    } else {
        if (protocol_state.pointer) {
            wl_pointer_release(protocol_state.pointer);
            protocol_state.pointer = NULL;
        }
        if (protocol_state.seat) {
            wl_seat_release(protocol_state.seat);
            protocol_state.seat = NULL;
        }

        protocol_state.queue = NULL;
        wl_event_queue_destroy(queue);
    }

    fprintf(
        stderr,
        "[onyrion-group-surface-bridge] PROTOCOL advertised=%u available=%d\n",
        protocol_state.advertised_version,
        protocol_state.available ? 1 : 0
    );

    return protocol_state.available ? 1 : 0;
}

static OnyrionWindowRole *find_window_role(
        GtkWindow *window) {
    for (GList *it = window_roles; it; it = it->next) {
        OnyrionWindowRole *role = it->data;

        if (role && role->window == window) {
            return role;
        }
    }

    return NULL;
}

static gboolean external_role_create(
        GtkLayerShellExternalRole *external,
        struct wl_display *display,
        struct wl_surface *surface,
        gpointer user_data) {
    OnyrionWindowRole *role = user_data;

    if (!role ||
            !surface ||
            !display ||
            display != protocol_state.display ||
            !protocol_state.available ||
            !protocol_state.shell) {
        return FALSE;
    }

    role->wl_surface = surface;

    role->group_surface =
        onyrion_shell_unstable_v1_get_group_surface(
            protocol_state.shell,
            surface,
            role->group_id,
            role->name_space
        );

    if (!role->group_surface) {
        trace_line(
            "GET_GROUP_SURFACE_NULL",
            role
        );
        return FALSE;
    }

    if (onyrion_group_surface_v1_add_listener(
            role->group_surface,
            &group_surface_listener,
            role) != 0) {
        onyrion_group_surface_v1_destroy(
            role->group_surface
        );
        role->group_surface = NULL;
        role->wl_surface = NULL;
        trace_line(
            "GROUP_SURFACE_LISTENER_FAIL",
            role
        );
        return FALSE;
    }

    onyrion_group_surface_v1_set_rect(
        role->group_surface,
        role->x,
        role->y,
        role->width,
        role->height
    );

    onyrion_group_surface_v1_set_layer(
        role->group_surface,
        role->layer
    );

    /*
     * Feed a tiny pre-configure bootstrap to GTK's fake XDG facade. v14
     * immediately replaces this for real TabGroup windows from the
     * authoritative Group outer-size configure event. The bootstrap reserves
     * no client content and is never used to compute the viewport.
     */
    external_role_configure(
        external,
        role->width,
        role->height
    );

    role->claimed = true;

    (void)wl_display_flush(
        protocol_state.display
    );

    trace_line(
        "ROLE_CLAIMED",
        role
    );

    return TRUE;
}

static void external_role_destroy(
        GtkLayerShellExternalRole *external,
        gpointer user_data) {
    (void)external;

    OnyrionWindowRole *role = user_data;

    if (!role) {
        return;
    }

    if (role->group_surface) {
        onyrion_group_surface_v1_destroy(
            role->group_surface
        );
        role->group_surface = NULL;

        if (protocol_state.display) {
            (void)wl_display_flush(
                protocol_state.display
            );
        }
    }

    if (protocol_state.pointer_focus_surface == role->wl_surface) {
        protocol_state.pointer_focus_surface = NULL;
    }
    if (protocol_state.primary_press_surface == role->wl_surface) {
        reset_primary_drag(&protocol_state, true);
    }

    role->wl_surface = NULL;
    role->claimed = false;

    if (role->tabgroup) {
        onyrion_tabgroup_role_released(
            (uintptr_t)role->window
        );
    }

    trace_line(
        "ROLE_RELEASED",
        role
    );
}

static void window_role_free(
        gpointer user_data) {
    OnyrionWindowRole *role = user_data;

    if (!role) {
        return;
    }

    window_roles =
        g_list_remove(
            window_roles,
            role
        );

    free(role->group_id);
    free(role->name_space);
    free(role);
}

int onyrion_group_surface_bridge_prepare(
        uintptr_t gtk_window_ptr,
        const char *group_id,
        const char *name_space,
        int ui_height) {
    if (!gtk_window_ptr ||
            !group_id ||
            !*group_id ||
            !name_space ||
            !*name_space ||
            ui_height <= 0) {
        return -1;
    }

    if (!load_external_role_api()) {
        return 0;
    }

    const int protocol_ready =
        ensure_protocol();

    if (protocol_ready <= 0) {
        return protocol_ready;
    }

    GtkWindow *window =
        GTK_WINDOW(
            (gpointer)gtk_window_ptr
        );

    if (find_window_role(window)) {
        return 1;
    }

    OnyrionWindowRole *role =
        calloc(
            1,
            sizeof(*role)
        );

    if (!role) {
        return -1;
    }

    role->window = window;
    role->group_id = strdup(group_id);
    role->name_space = strdup(name_space);
    role->x = 0;
    role->y = 0;
    role->width = ONYRION_BOOTSTRAP_WIDTH;
    role->height = ONYRION_BOOTSTRAP_HEIGHT;
    role->group_outer_width = 0;
    role->group_outer_height = 0;
    role->ui_height = ui_height;
    role->layer = ONYRION_BOOTSTRAP_LAYER;
    role->tabgroup =
        strcmp(
            name_space,
            ONYRION_TABGROUP_NAMESPACE
        ) == 0;

    if (!role->group_id ||
            !role->name_space) {
        window_role_free(role);
        return -1;
    }

    window_roles =
        g_list_append(
            window_roles,
            role
        );

    role->external_role =
        external_role_init(
            window,
            external_role_create,
            external_role_destroy,
            role,
            window_role_free
        );

    if (!role->external_role) {
        window_roles =
            g_list_remove(
                window_roles,
                role
            );

        free(role->group_id);
        free(role->name_space);
        free(role);
        return -1;
    }

    trace_line(
        "ROLE_REGISTERED",
        role
    );

    return 1;
}

static int begin_core_drag(
        uintptr_t gtk_window_ptr,
        const char *target_id,
        OnyrionDragKind kind) {
    if (!gtk_window_ptr ||
            !target_id ||
            !*target_id ||
            (kind != ONYRION_DRAG_WINDOW &&
             kind != ONYRION_DRAG_GROUP) ||
            !protocol_state.available ||
            !protocol_state.shell) {
        return -1;
    }

    /*
     * GDK has already read the physical pointer event before the GTK motion
     * callback that crosses the drag threshold. Deliver any event that
     * libwayland routed to our private queue before validating its serial.
     */
    if (protocol_state.display &&
            protocol_state.queue) {
        (void)wl_display_dispatch_queue_pending(
            protocol_state.display,
            protocol_state.queue
        );
    }

    GtkWindow *window =
        GTK_WINDOW(
            (gpointer)gtk_window_ptr
        );

    OnyrionWindowRole *role =
        find_window_role(window);

    if (!role ||
            !role->claimed ||
            !role->wl_surface ||
            !protocol_state.pointer ||
            !protocol_state.primary_down ||
            protocol_state.primary_drag_consumed ||
            protocol_state.primary_press_serial == 0 ||
            protocol_state.primary_press_surface !=
                role->wl_surface) {
        return 0;
    }

    if (kind == ONYRION_DRAG_GROUP &&
            strcmp(target_id, role->group_id) != 0) {
        return -1;
    }

    if (kind == ONYRION_DRAG_WINDOW) {
        onyrion_shell_unstable_v1_begin_window_drag(
            protocol_state.shell,
            target_id,
            BTN_LEFT,
            protocol_state.primary_press_serial
        );
    } else {
        onyrion_shell_unstable_v1_begin_group_drag(
            protocol_state.shell,
            target_id,
            BTN_LEFT,
            protocol_state.primary_press_serial
        );
    }

    protocol_state.primary_drag_consumed = true;
    protocol_state.drag_kind = kind;
    reset_drag_target_cache(&protocol_state);

    if (protocol_state.display) {
        (void)wl_display_flush(
            protocol_state.display
        );
    }

    fprintf(
        stderr,
        "[onyrion-group-surface-bridge] DRAG_BEGIN kind=%s target=%s serial=%u button=%u\n",
        kind == ONYRION_DRAG_WINDOW
            ? "window"
            : "group",
        target_id,
        protocol_state.primary_press_serial,
        (unsigned)BTN_LEFT
    );

    return 1;
}

int onyrion_group_surface_bridge_begin_window_drag(
        uintptr_t gtk_window_ptr,
        const char *window_id) {
    return begin_core_drag(
        gtk_window_ptr,
        window_id,
        ONYRION_DRAG_WINDOW
    );
}

int onyrion_group_surface_bridge_begin_group_drag(
        uintptr_t gtk_window_ptr,
        const char *group_id) {
    return begin_core_drag(
        gtk_window_ptr,
        group_id,
        ONYRION_DRAG_GROUP
    );
}

static bool drag_target_cache_matches(
        const char *group_id,
        const char *window_id,
        uint32_t kind) {
    if (!protocol_state.last_target_valid ||
            protocol_state.last_target_kind != kind ||
            !protocol_state.last_target_group_id ||
            strcmp(
                protocol_state.last_target_group_id,
                group_id
            ) != 0) {
        return false;
    }

    if (!window_id) {
        return protocol_state.last_target_window_id == NULL;
    }

    return protocol_state.last_target_window_id &&
        strcmp(
            protocol_state.last_target_window_id,
            window_id
        ) == 0;
}

static int set_window_drag_target(
        const char *group_id,
        const char *window_id,
        uint32_t kind) {
    if (!group_id ||
            !*group_id ||
            protocol_state.drag_kind != ONYRION_DRAG_WINDOW ||
            !protocol_state.available ||
            !protocol_state.shell) {
        return 0;
    }

    if (drag_target_cache_matches(
            group_id,
            window_id,
            kind)) {
        return 1;
    }

    onyrion_shell_unstable_v1_set_window_drag_target(
        protocol_state.shell,
        group_id,
        window_id,
        kind
    );

    reset_drag_target_cache(&protocol_state);
    protocol_state.last_target_group_id =
        strdup(group_id);
    protocol_state.last_target_window_id =
        window_id
            ? strdup(window_id)
            : NULL;
    protocol_state.last_target_kind = kind;
    protocol_state.last_target_valid =
        protocol_state.last_target_group_id != NULL &&
        (!window_id ||
         protocol_state.last_target_window_id != NULL);

    if (protocol_state.display) {
        (void)wl_display_flush(
            protocol_state.display
        );
    }

    fprintf(
        stderr,
        "[onyrion-group-surface-bridge] DRAG_TARGET group=%s kind=%u reference=%s\n",
        group_id,
        kind,
        window_id ? window_id : ""
    );

    return 1;
}

int onyrion_group_surface_bridge_set_group_drag_target(
        const char *group_id) {
    return set_window_drag_target(
        group_id,
        NULL,
        ONYRION_SHELL_UNSTABLE_V1_DRAG_TARGET_KIND_GROUP
    );
}

int onyrion_group_surface_bridge_set_tab_drag_target(
        const char *group_id,
        const char *reference_window_id,
        int after) {
    if (!reference_window_id ||
            !*reference_window_id) {
        return -1;
    }

    return set_window_drag_target(
        group_id,
        reference_window_id,
        after
            ? ONYRION_SHELL_UNSTABLE_V1_DRAG_TARGET_KIND_TAB_AFTER
            : ONYRION_SHELL_UNSTABLE_V1_DRAG_TARGET_KIND_TAB_BEFORE
    );
}

int onyrion_group_surface_bridge_configure(
        uintptr_t gtk_window_ptr,
        int x,
        int y,
        int width,
        int height) {
    if (!gtk_window_ptr ||
            width <= 0 ||
            height <= 0) {
        return -1;
    }

    GtkWindow *window =
        GTK_WINDOW(
            (gpointer)gtk_window_ptr
        );

    OnyrionWindowRole *role =
        find_window_role(window);

    if (!role ||
            !role->external_role) {
        return 0;
    }

    role->x = x;
    role->y = y;
    role->width = width;
    role->height = height;

    external_role_configure(
        role->external_role,
        width,
        height
    );

    if (role->group_surface) {
        onyrion_group_surface_v1_set_rect(
            role->group_surface,
            x,
            y,
            width,
            height
        );

        if (protocol_state.display) {
            (void)wl_display_flush(
                protocol_state.display
            );
        }
    }

    return 1;
}

int onyrion_group_surface_bridge_set_layer(
        uintptr_t gtk_window_ptr,
        uint32_t layer) {
    if (!gtk_window_ptr ||
            layer >
                ONYRION_GROUP_SURFACE_V1_LAYER_ABOVE_CLIENT) {
        return -1;
    }

    GtkWindow *window =
        GTK_WINDOW(
            (gpointer)gtk_window_ptr
        );

    OnyrionWindowRole *role =
        find_window_role(window);

    if (!role) {
        return 0;
    }

    role->layer = layer;

    if (role->group_surface) {
        onyrion_group_surface_v1_set_layer(
            role->group_surface,
            layer
        );

        if (protocol_state.display) {
            (void)wl_display_flush(
                protocol_state.display
            );
        }
    }

    return 1;
}
