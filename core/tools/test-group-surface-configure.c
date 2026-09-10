#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "onyrion-shell-unstable-v1-client-protocol.h"

typedef struct test_state TestState;

typedef struct test_window {
    TestState *state;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    int fd;
    bool mapped_commit_sent;
    bool close_requested;
} TestWindow;

typedef struct test_group_record {
    char id[32];
    char workspace_id[32];
    uint32_t window_count;
} TestGroupRecord;

typedef struct test_window_record {
    char id[32];
    char group_id[32];
} TestWindowRecord;

struct test_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct onyrion_shell_unstable_v1 *shell;

    uint32_t shell_version;

    TestWindow windows[2];

    bool snapshot_done;
    char group_id[32];
    TestGroupRecord group_records[4];
    size_t group_record_count;
    TestWindowRecord window_records[4];
    size_t window_record_count;

    struct wl_surface *group_surface_wl;
    struct onyrion_group_surface_v1 *group_surface;
    unsigned configure_count;
    int32_t initial_width;
    int32_t initial_height;
    int32_t changed_width;
    int32_t changed_height;

    struct wl_surface *target_group_surface_wl;
    struct onyrion_group_surface_v1 *target_group_surface;
    unsigned target_configure_count;
    int32_t target_initial_width;
    int32_t target_initial_height;
    int32_t target_changed_width;
    int32_t target_changed_height;

    bool split_result_received;
    bool split_result_success;
    bool move_result_received;
    bool move_result_success;
    bool float_result_received;
    bool float_result_success;
    bool pin_result_received;
    bool pin_result_success;
    bool workspace_next_result_received;
    bool workspace_next_result_success;
};

static void die(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(EXIT_FAILURE);
}

static bool display_roundtrip(TestState *state) {
    return state &&
        state->display &&
        wl_display_roundtrip(state->display) >= 0 &&
        wl_display_get_error(state->display) == 0;
}

static const TestGroupRecord *find_group_record(
        const TestState *state,
        const char *group_id) {
    if (!state || !group_id) {
        return NULL;
    }

    for (size_t i = 0; i < state->group_record_count; ++i) {
        if (strcmp(state->group_records[i].id, group_id) == 0) {
            return &state->group_records[i];
        }
    }

    return NULL;
}

static int create_backing_file(size_t size) {
    char path[] = "/tmp/onyrion-group-surface-test-XXXXXX";
    int fd = mkstemp(path);

    if (fd < 0) {
        return -1;
    }

    unlink(path);

    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void handle_buffer_release(
        void *data,
        struct wl_buffer *buffer) {
    (void)data;
    (void)buffer;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = handle_buffer_release,
};

static void handle_wm_base_ping(
        void *data,
        struct xdg_wm_base *wm_base,
        uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = handle_wm_base_ping,
};

static void handle_xdg_toplevel_configure(
        void *data,
        struct xdg_toplevel *toplevel,
        int32_t width,
        int32_t height,
        struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void handle_xdg_toplevel_close(
        void *data,
        struct xdg_toplevel *toplevel) {
    (void)toplevel;
    TestWindow *window = data;

    if (window) {
        window->close_requested = true;
    }
}

static void handle_xdg_toplevel_configure_bounds(
        void *data,
        struct xdg_toplevel *toplevel,
        int32_t width,
        int32_t height) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

static void handle_xdg_toplevel_wm_capabilities(
        void *data,
        struct xdg_toplevel *toplevel,
        struct wl_array *capabilities) {
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = handle_xdg_toplevel_configure,
    .close = handle_xdg_toplevel_close,
    .configure_bounds = handle_xdg_toplevel_configure_bounds,
    .wm_capabilities = handle_xdg_toplevel_wm_capabilities,
};

static void handle_xdg_surface_configure(
        void *data,
        struct xdg_surface *xdg_surface,
        uint32_t serial) {
    TestWindow *window = data;

    xdg_surface_ack_configure(xdg_surface, serial);

    if (!window || window->mapped_commit_sent) {
        return;
    }

    wl_surface_attach(
        window->surface,
        window->buffer,
        0,
        0
    );

    wl_surface_damage(
        window->surface,
        0,
        0,
        64,
        64
    );

    wl_surface_commit(window->surface);
    window->mapped_commit_sent = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = handle_xdg_surface_configure,
};

static bool create_test_window(
        TestState *state,
        TestWindow *window,
        const char *title) {
    if (!state || !window || !state->compositor ||
            !state->shm || !state->wm_base) {
        return false;
    }

    *window = (TestWindow){
        .state = state,
        .fd = -1,
    };

    const int width = 64;
    const int height = 64;
    const int stride = width * 4;
    const size_t size = (size_t)stride * (size_t)height;

    window->fd = create_backing_file(size);

    if (window->fd < 0) {
        return false;
    }

    window->pool = wl_shm_create_pool(
        state->shm,
        window->fd,
        (int32_t)size
    );

    if (!window->pool) {
        return false;
    }

    window->buffer = wl_shm_pool_create_buffer(
        window->pool,
        0,
        width,
        height,
        stride,
        WL_SHM_FORMAT_XRGB8888
    );

    if (!window->buffer) {
        return false;
    }

    wl_buffer_add_listener(
        window->buffer,
        &buffer_listener,
        window
    );

    window->surface =
        wl_compositor_create_surface(
            state->compositor
        );

    if (!window->surface) {
        return false;
    }

    window->xdg_surface =
        xdg_wm_base_get_xdg_surface(
            state->wm_base,
            window->surface
        );

    if (!window->xdg_surface) {
        return false;
    }

    xdg_surface_add_listener(
        window->xdg_surface,
        &xdg_surface_listener,
        window
    );

    window->toplevel =
        xdg_surface_get_toplevel(
            window->xdg_surface
        );

    if (!window->toplevel) {
        return false;
    }

    xdg_toplevel_add_listener(
        window->toplevel,
        &toplevel_listener,
        window
    );

    xdg_toplevel_set_title(
        window->toplevel,
        title
    );

    xdg_toplevel_set_app_id(
        window->toplevel,
        "onyrion.core-group-surface-test"
    );

    wl_surface_commit(window->surface);
    return true;
}

static void destroy_test_window(TestWindow *window) {
    if (!window) {
        return;
    }

    if (window->toplevel) {
        xdg_toplevel_destroy(window->toplevel);
        window->toplevel = NULL;
    }

    if (window->xdg_surface) {
        xdg_surface_destroy(window->xdg_surface);
        window->xdg_surface = NULL;
    }

    if (window->surface) {
        wl_surface_destroy(window->surface);
        window->surface = NULL;
    }

    if (window->buffer) {
        wl_buffer_destroy(window->buffer);
        window->buffer = NULL;
    }

    if (window->pool) {
        wl_shm_pool_destroy(window->pool);
        window->pool = NULL;
    }

    if (window->fd >= 0) {
        close(window->fd);
        window->fd = -1;
    }
}

static void handle_state_begin(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation,
        uint32_t workspace_count) {
    (void)shell;
    (void)generation;
    (void)workspace_count;

    TestState *state = data;

    if (state) {
        state->snapshot_done = false;
        state->group_id[0] = '\0';
        state->group_record_count = 0;
        state->window_record_count = 0;
    }
}

static void handle_workspace(
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

static void handle_state_end(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation) {
    (void)shell;
    (void)generation;

    TestState *state = data;

    if (state) {
        state->snapshot_done = true;
    }
}

static void handle_changed(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t generation) {
    (void)data;
    (void)shell;
    (void)generation;
}

static void handle_action_result(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t action,
        uint32_t success) {
    (void)shell;

    TestState *state = data;

    if (!state) {
        return;
    }

    if (action == ONYRION_SHELL_UNSTABLE_V1_ACTION_SPLIT_ACTIVE) {
        state->split_result_received = true;
        state->split_result_success = success != 0;
        return;
    }

    if (action == ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_MOVE_TO_GROUP) {
        state->move_result_received = true;
        state->move_result_success = success != 0;
        return;
    }

    if (action == ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_FLOAT) {
        state->float_result_received = true;
        state->float_result_success = success != 0;
        return;
    }

    if (action == ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_SET_PINNED) {
        state->pin_result_received = true;
        state->pin_result_success = success != 0;
        return;
    }

    if (action == ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_NEXT) {
        state->workspace_next_result_received = true;
        state->workspace_next_result_success = success != 0;
    }
}

static void handle_group(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        const char *workspace_id,
        uint32_t active,
        uint32_t window_count) {
    (void)shell;

    TestState *state = data;

    if (!state || !id) {
        return;
    }

    if (state->group_record_count <
            sizeof(state->group_records) / sizeof(state->group_records[0])) {
        TestGroupRecord *record =
            &state->group_records[state->group_record_count++];

        snprintf(record->id, sizeof(record->id), "%s", id);
        snprintf(
            record->workspace_id,
            sizeof(record->workspace_id),
            "%s",
            workspace_id ? workspace_id : ""
        );
        record->window_count = window_count;
    }

    if (!active || window_count < 2) {
        return;
    }

    snprintf(
        state->group_id,
        sizeof(state->group_id),
        "%s",
        id
    );
}

static void handle_window(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *id,
        const char *group_id,
        uint32_t active,
        uint32_t fullscreen,
        const char *app_id,
        const char *title) {
    (void)shell;
    (void)active;
    (void)fullscreen;
    (void)app_id;
    (void)title;

    TestState *state = data;

    if (!state || !id || !group_id || group_id[0] == '\0') {
        return;
    }

    if (state->window_record_count <
            sizeof(state->window_records) / sizeof(state->window_records[0])) {
        TestWindowRecord *record =
            &state->window_records[state->window_record_count++];

        snprintf(record->id, sizeof(record->id), "%s", id);
        snprintf(
            record->group_id,
            sizeof(record->group_id),
            "%s",
            group_id
        );
    }
}

static void handle_controller_claim_result(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        uint32_t success) {
    (void)data;
    (void)shell;
    (void)success;
}

static void handle_controller_invoke(
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

static void handle_output(
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

static void handle_workspace_output(
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

static void handle_workspace_metadata(
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

static void handle_window_placement(
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

static void handle_group_placement(
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

static void handle_drag_surface_motion(
        void *data,
        struct onyrion_shell_unstable_v1 *shell,
        const char *group_id,
        const char *name_space,
        wl_fixed_t x,
        wl_fixed_t y) {
    (void)data;
    (void)shell;
    (void)group_id;
    (void)name_space;
    (void)x;
    (void)y;
}

static const struct onyrion_shell_unstable_v1_listener shell_listener = {
    .state_begin = handle_state_begin,
    .workspace = handle_workspace,
    .state_end = handle_state_end,
    .changed = handle_changed,
    .action_result = handle_action_result,
    .group = handle_group,
    .window = handle_window,
    .controller_claim_result = handle_controller_claim_result,
    .controller_invoke = handle_controller_invoke,
    .output = handle_output,
    .workspace_output = handle_workspace_output,
    .workspace_metadata = handle_workspace_metadata,
    .window_placement = handle_window_placement,
    .group_placement = handle_group_placement,
    .drag_surface_motion = handle_drag_surface_motion,
};

static void handle_group_surface_configure(
        void *data,
        struct onyrion_group_surface_v1 *group_surface,
        int32_t width,
        int32_t height) {
    TestState *state = data;

    if (!state) {
        return;
    }

    if (group_surface == state->group_surface) {
        state->configure_count++;

        if (state->configure_count == 1) {
            state->initial_width = width;
            state->initial_height = height;
        } else {
            state->changed_width = width;
            state->changed_height = height;
        }

        printf(
            "SOURCE_CONFIGURE count=%u width=%d height=%d\n",
            state->configure_count,
            width,
            height
        );
        return;
    }

    if (group_surface == state->target_group_surface) {
        state->target_configure_count++;

        if (state->target_configure_count == 1) {
            state->target_initial_width = width;
            state->target_initial_height = height;
        } else {
            state->target_changed_width = width;
            state->target_changed_height = height;
        }

        printf(
            "TARGET_CONFIGURE count=%u width=%d height=%d\n",
            state->target_configure_count,
            width,
            height
        );
        return;
    }

}

static const struct onyrion_group_surface_v1_listener
        group_surface_listener = {
    .configure = handle_group_surface_configure,
};

static void handle_registry_global(
        void *data,
        struct wl_registry *registry,
        uint32_t name,
        const char *interface,
        uint32_t version) {
    TestState *state = data;

    if (!state) {
        return;
    }

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            version < 4 ? version : 4
        );
        return;
    }

    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(
            registry,
            name,
            &wl_shm_interface,
            1
        );
        return;
    }

    if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->wm_base = wl_registry_bind(
            registry,
            name,
            &xdg_wm_base_interface,
            version < 6 ? version : 6
        );

        if (state->wm_base) {
            xdg_wm_base_add_listener(
                state->wm_base,
                &wm_base_listener,
                state
            );
        }

        return;
    }

    if (strcmp(
            interface,
            onyrion_shell_unstable_v1_interface.name) == 0) {
        state->shell_version = version;

        if (version >= 14) {
            state->shell = wl_registry_bind(
                registry,
                name,
                &onyrion_shell_unstable_v1_interface,
                14
            );

            if (state->shell) {
                onyrion_shell_unstable_v1_add_listener(
                    state->shell,
                    &shell_listener,
                    state
                );
            }
        }
    }
}

static void handle_registry_global_remove(
        void *data,
        struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_registry_global,
    .global_remove = handle_registry_global_remove,
};

static void destroy_state(TestState *state) {
    if (!state) {
        return;
    }

    if (state->target_group_surface) {
        onyrion_group_surface_v1_destroy(
            state->target_group_surface
        );
        state->target_group_surface = NULL;
    }

    if (state->target_group_surface_wl) {
        wl_surface_destroy(state->target_group_surface_wl);
        state->target_group_surface_wl = NULL;
    }

    if (state->group_surface) {
        onyrion_group_surface_v1_destroy(
            state->group_surface
        );
        state->group_surface = NULL;
    }

    if (state->group_surface_wl) {
        wl_surface_destroy(state->group_surface_wl);
        state->group_surface_wl = NULL;
    }

    destroy_test_window(&state->windows[1]);
    destroy_test_window(&state->windows[0]);

    if (state->shell) {
        onyrion_shell_unstable_v1_destroy(state->shell);
        state->shell = NULL;
    }

    if (state->wm_base) {
        xdg_wm_base_destroy(state->wm_base);
        state->wm_base = NULL;
    }

    if (state->shm) {
        wl_shm_destroy(state->shm);
        state->shm = NULL;
    }

    if (state->compositor) {
        wl_compositor_destroy(state->compositor);
        state->compositor = NULL;
    }

    if (state->registry) {
        wl_registry_destroy(state->registry);
        state->registry = NULL;
    }

    if (state->display) {
        wl_display_flush(state->display);
        wl_display_disconnect(state->display);
        state->display = NULL;
    }
}

int main(void) {
    TestState state = {0};

    state.windows[0].fd = -1;
    state.windows[1].fd = -1;

    state.display = wl_display_connect(NULL);

    if (!state.display) {
        die("cannot connect to Wayland display");
    }

    state.registry = wl_display_get_registry(
        state.display
    );

    if (!state.registry) {
        destroy_state(&state);
        die("cannot get registry");
    }

    wl_registry_add_listener(
        state.registry,
        &registry_listener,
        &state
    );

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("registry roundtrip failed");
    }

    if (!state.compositor || !state.shm || !state.wm_base) {
        destroy_state(&state);
        die("required core Wayland globals are missing");
    }

    if (state.shell_version < 14 || !state.shell) {
        destroy_state(&state);
        die("Onyrion protocol v14 is unavailable");
    }

    if (!create_test_window(
            &state,
            &state.windows[0],
            "group-surface-configure-a") ||
        !create_test_window(
            &state,
            &state.windows[1],
            "group-surface-configure-b")) {
        destroy_state(&state);
        die("cannot create seed XDG windows");
    }

    for (int i = 0; i < 8 &&
            (!state.windows[0].mapped_commit_sent ||
             !state.windows[1].mapped_commit_sent); ++i) {
        if (wl_display_dispatch(state.display) < 0) {
            destroy_state(&state);
            die("XDG configure dispatch failed");
        }
    }

    if (!state.windows[0].mapped_commit_sent ||
            !state.windows[1].mapped_commit_sent) {
        destroy_state(&state);
        die("seed XDG windows were not configured");
    }

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("mapped-window roundtrip failed");
    }

    onyrion_shell_unstable_v1_get_state(
        state.shell
    );

    if (!display_roundtrip(&state) ||
            !state.snapshot_done ||
            state.group_id[0] == '\0') {
        destroy_state(&state);
        die("cannot discover one active two-window Group");
    }

    printf("GROUP id=%s\n", state.group_id);

    char original_group_id[32];
    snprintf(
        original_group_id,
        sizeof(original_group_id),
        "%s",
        state.group_id
    );

    state.group_surface_wl =
        wl_compositor_create_surface(
            state.compositor
        );

    if (!state.group_surface_wl) {
        destroy_state(&state);
        die("cannot create Group wl_surface");
    }

    state.group_surface =
        onyrion_shell_unstable_v1_get_group_surface(
            state.shell,
            state.group_surface_wl,
            state.group_id,
            "onyrion.core-configure-test"
        );

    if (!state.group_surface) {
        destroy_state(&state);
        die("get_group_surface returned NULL");
    }

    onyrion_group_surface_v1_add_listener(
        state.group_surface,
        &group_surface_listener,
        &state
    );

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("initial Group-surface roundtrip failed");
    }

    if (state.configure_count != 1 ||
            state.initial_width != 1280 ||
            state.initial_height != 720) {
        fprintf(
            stderr,
            "FAIL: unexpected initial configure count=%u size=%dx%d\n",
            state.configure_count,
            state.initial_width,
            state.initial_height
        );
        destroy_state(&state);
        return EXIT_FAILURE;
    }

    onyrion_group_surface_v1_set_rect(
        state.group_surface,
        0,
        0,
        state.initial_width,
        32
    );

    onyrion_group_surface_v1_set_layer(
        state.group_surface,
        ONYRION_GROUP_SURFACE_V1_LAYER_ABOVE_CLIENT
    );

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("unchanged-size roundtrip failed");
    }

    if (state.configure_count != 1) {
        destroy_state(&state);
        die("unchanged Group size emitted duplicate configure");
    }

    onyrion_shell_unstable_v1_split_active(
        state.shell,
        ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_HORIZONTAL
    );

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("split-active roundtrip failed");
    }

    if (!state.split_result_received ||
            !state.split_result_success) {
        destroy_state(&state);
        die("split-active did not succeed");
    }

    if (state.configure_count != 2 ||
            state.changed_width != 640 ||
            state.changed_height != 720) {
        fprintf(
            stderr,
            "FAIL: unexpected changed configure count=%u size=%dx%d\n",
            state.configure_count,
            state.changed_width,
            state.changed_height
        );
        destroy_state(&state);
        return EXIT_FAILURE;
    }

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("post-change stability roundtrip failed");
    }

    if (state.configure_count != 2) {
        destroy_state(&state);
        die("unchanged post-split size emitted duplicate configure");
    }

    onyrion_shell_unstable_v1_get_state(state.shell);

    if (!display_roundtrip(&state) ||
            !state.snapshot_done ||
            state.group_record_count != 2 ||
            state.window_record_count != 2) {
        destroy_state(&state);
        die("cannot discover post-split Groups");
    }

    char source_window_id[32] = {0};
    char target_group_id[32] = {0};

    for (size_t i = 0; i < state.window_record_count; ++i) {
        TestWindowRecord *record = &state.window_records[i];

        if (strcmp(record->group_id, original_group_id) == 0) {
            snprintf(
                source_window_id,
                sizeof(source_window_id),
                "%s",
                record->id
            );
        } else {
            snprintf(
                target_group_id,
                sizeof(target_group_id),
                "%s",
                record->group_id
            );
        }
    }

    if (source_window_id[0] == '\0' ||
            target_group_id[0] == '\0' ||
            strcmp(target_group_id, original_group_id) == 0) {
        destroy_state(&state);
        die("cannot resolve source Window and target Group");
    }

    printf(
        "POST_SPLIT source_group=%s source_window=%s target_group=%s\n",
        original_group_id,
        source_window_id,
        target_group_id
    );

    state.target_group_surface_wl =
        wl_compositor_create_surface(state.compositor);

    if (!state.target_group_surface_wl) {
        destroy_state(&state);
        die("cannot create target Group wl_surface");
    }

    state.target_group_surface =
        onyrion_shell_unstable_v1_get_group_surface(
            state.shell,
            state.target_group_surface_wl,
            target_group_id,
            "onyrion.core-lifetime-target"
        );

    if (!state.target_group_surface) {
        destroy_state(&state);
        die("cannot create target Group role");
    }

    onyrion_group_surface_v1_add_listener(
        state.target_group_surface,
        &group_surface_listener,
        &state
    );

    if (!display_roundtrip(&state) ||
            state.target_configure_count != 1 ||
            state.target_initial_width != 640 ||
            state.target_initial_height != 720) {
        destroy_state(&state);
        die("unexpected target Group initial configure");
    }

    onyrion_group_surface_v1_set_rect(
        state.target_group_surface,
        0,
        0,
        state.target_initial_width,
        32
    );

    onyrion_group_surface_v1_set_layer(
        state.target_group_surface,
        ONYRION_GROUP_SURFACE_V1_LAYER_ABOVE_CLIENT
    );

    state.move_result_received = false;
    state.move_result_success = false;

    onyrion_shell_unstable_v1_move_window_to_group(
        state.shell,
        source_window_id,
        target_group_id
    );

    if (!display_roundtrip(&state) ||
            !state.move_result_received ||
            !state.move_result_success) {
        destroy_state(&state);
        die("move-window-to-group failed or disconnected client");
    }

    if (wl_display_get_error(state.display) != 0) {
        destroy_state(&state);
        die("Wayland client disconnected after source Group destruction");
    }

    if (state.configure_count != 2) {
        destroy_state(&state);
        die("destroyed source Group emitted unexpected configure");
    }

    if (state.target_configure_count != 2 ||
            state.target_changed_width != 1280 ||
            state.target_changed_height != 720) {
        destroy_state(&state);
        die("target Group surface did not continue after merge");
    }

    onyrion_shell_unstable_v1_get_state(state.shell);

    if (!display_roundtrip(&state) ||
            !state.snapshot_done ||
            state.group_record_count != 1 ||
            state.window_record_count != 2 ||
            strcmp(state.group_records[0].id, target_group_id) != 0 ||
            state.group_records[0].window_count != 2) {
        destroy_state(&state);
        die("post-merge state snapshot is inconsistent");
    }

    onyrion_group_surface_v1_set_rect(
        state.group_surface,
        0,
        0,
        320,
        32
    );

    onyrion_group_surface_v1_set_layer(
        state.group_surface,
        ONYRION_GROUP_SURFACE_V1_LAYER_ABOVE_CLIENT
    );

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("tombstoned Group surface rejected harmless requests");
    }

    onyrion_group_surface_v1_destroy(state.group_surface);
    state.group_surface = NULL;

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("late client Group-surface destructor disconnected client");
    }

    wl_surface_destroy(state.group_surface_wl);
    state.group_surface_wl = NULL;

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("source backing wl_surface cleanup failed");
    }

    onyrion_shell_unstable_v1_get_state(state.shell);

    if (!display_roundtrip(&state) || !state.snapshot_done) {
        destroy_state(&state);
        die("cannot snapshot target Group before pinned migration");
    }

    const TestGroupRecord *target_before =
        find_group_record(&state, target_group_id);

    if (!target_before || target_before->workspace_id[0] == '\0') {
        destroy_state(&state);
        die("target Group has no workspace before pinned migration");
    }

    char workspace_before[32];
    snprintf(
        workspace_before,
        sizeof(workspace_before),
        "%s",
        target_before->workspace_id
    );

    state.float_result_received = false;
    state.float_result_success = false;
    onyrion_shell_unstable_v1_float_group(
        state.shell,
        target_group_id
    );

    if (!display_roundtrip(&state) ||
            !state.float_result_received ||
            !state.float_result_success) {
        destroy_state(&state);
        die("target Group float failed");
    }

    state.pin_result_received = false;
    state.pin_result_success = false;
    onyrion_shell_unstable_v1_set_group_pinned(
        state.shell,
        target_group_id,
        1
    );

    if (!display_roundtrip(&state) ||
            !state.pin_result_received ||
            !state.pin_result_success) {
        destroy_state(&state);
        die("target Group pin failed");
    }

    state.workspace_next_result_received = false;
    state.workspace_next_result_success = false;
    onyrion_shell_unstable_v1_workspace_next(state.shell);

    if (!display_roundtrip(&state) ||
            !state.workspace_next_result_received ||
            !state.workspace_next_result_success) {
        destroy_state(&state);
        die("pinned workspace-next migration failed or Core disconnected");
    }

    onyrion_shell_unstable_v1_get_state(state.shell);

    if (!display_roundtrip(&state) || !state.snapshot_done) {
        destroy_state(&state);
        die("cannot snapshot target Group after pinned migration");
    }

    const TestGroupRecord *target_after =
        find_group_record(&state, target_group_id);

    if (!target_after ||
            target_after->window_count != 2 ||
            target_after->workspace_id[0] == '\0' ||
            strcmp(target_after->workspace_id, workspace_before) == 0) {
        destroy_state(&state);
        die("pinned Group did not follow workspace change");
    }

    onyrion_group_surface_v1_set_rect(
        state.target_group_surface,
        0,
        0,
        320,
        32
    );

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("reparented target Group surface is not live");
    }

    /*
     * Keep the valid Wayland teardown order: role object first, then the
     * backing wl_surface. This regression is about server-side Group death,
     * not the protocol-invalid wl_surface-before-role sequence.
     */
    onyrion_group_surface_v1_destroy(state.target_group_surface);
    state.target_group_surface = NULL;

    wl_surface_destroy(state.target_group_surface_wl);
    state.target_group_surface_wl = NULL;

    if (!display_roundtrip(&state)) {
        destroy_state(&state);
        die("target Group-surface final cleanup failed");
    }

    printf("PROTOCOL_VERSION=%u\n", state.shell_version);
    printf(
        "INITIAL_GROUP_OUTER=%dx%d\n",
        state.initial_width,
        state.initial_height
    );
    printf(
        "CHANGED_GROUP_OUTER=%dx%d\n",
        state.changed_width,
        state.changed_height
    );
    printf("CONFIGURE_COUNT=%u\n", state.configure_count);
    printf("UNCHANGED_DUPLICATE_CONFIGURE=0\n");
    printf("GROUP_SURFACE_DESTROY_CLEAN=1\n");
    printf("GROUP_DESTROY_TOMBSTONE_V=1\n");
    printf("LATE_CLIENT_GROUP_SURFACE_DESTROY_V=1\n");
    printf("NO_CLIENT_DISCONNECT_V=1\n");
    printf("TARGET_GROUP_SURFACE_CONTINUITY_V=1\n");
    printf("PINNED_WORKSPACE_REPARENT_V=1\n");
    printf("SCENE_PARENT_INVARIANT_V=1\n");
    printf("RESULT=PASS_GROUP_SURFACE_LIFETIME_AND_REPARENT_V14\n");

    destroy_state(&state);
    return EXIT_SUCCESS;
}
