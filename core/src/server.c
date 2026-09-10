#include "server.h"
#include "config.h"
#include "config_runtime.h"
#include "fallback.h"
#include "output.h"
#include "session_lock.h"
#include "shell_protocol.h"
#include "group.h"
#include "group_surface.h"
#include "input.h"
#include "layout.h"
#include "layer_shell.h"
#include "window.h"
#include "workspace.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/util/log.h>

static bool valid_socket_name(
        const char *name) {
    if (!name ||
            !name[0] ||
            strcmp(name, ".") == 0 ||
            strcmp(name, "..") == 0) {
        return false;
    }

    for (const unsigned char *cursor =
            (const unsigned char *)name;
            *cursor;
            cursor++) {
        if (*cursor == '/' ||
                *cursor <= 0x20 ||
                *cursor == 0x7f) {
            return false;
        }
    }

    return true;
}

static bool add_display_socket(
        OnyrionServer *server,
        const char *socket_name) {
    if (!socket_name) {
        server->socket =
            wl_display_add_socket_auto(
                server->display
            );

        if (!server->socket) {
            fprintf(
                stderr,
                "FAIL: wl_display_add_socket_auto\n"
            );

            return false;
        }

        return true;
    }

    if (!valid_socket_name(socket_name)) {
        fprintf(
            stderr,
            "FAIL: invalid Wayland socket name '%s'\n",
            socket_name
        );

        return false;
    }

    errno = 0;

    if (wl_display_add_socket(
            server->display,
            socket_name) != 0) {
        const int saved_errno = errno;

        fprintf(
            stderr,
            "FAIL: cannot create Wayland socket '%s': %s\n",
            socket_name,
            saved_errno
                ? strerror(saved_errno)
                : "unknown error"
        );

        return false;
    }

    server->socket = socket_name;

    return true;
}

static void handle_exit_idle(void *data) {
    OnyrionServer *server = data;

    if (!server ||
            !server->display) {
        return;
    }

    wlr_log(
        WLR_INFO,
        "Session exit requested, terminating"
    );

    wl_display_terminate(server->display);
}

bool onyrion_server_request_exit(OnyrionServer *server) {
    if (!server ||
            !server->display) {
        return false;
    }

    if (server->exit_requested) {
        return true;
    }

    struct wl_event_loop *event_loop =
        wl_display_get_event_loop(server->display);

    if (!event_loop) {
        return false;
    }

    if (!wl_event_loop_add_idle(
            event_loop,
            handle_exit_idle,
            server)) {
        return false;
    }

    server->exit_requested = true;

    return true;
}

static int handle_signal(int signal_number, void *data) {
    OnyrionServer *server = data;

    wlr_log(
        WLR_INFO,
        "Received signal %d, terminating",
        signal_number
    );

    wl_display_terminate(server->display);

    return 0;
}

bool onyrion_server_init(
        OnyrionServer *server,
        struct onyrion_core_config *config,
        const char *config_path,
        const char *socket_name) {
    memset(server, 0, sizeof(*server));

    /*
     * server_finish() is also the unwind path for failed initialization.
     * Keep every intrusive registry valid even when initialization stops
     * before its owning subsystem's init function runs.
     */
    wl_list_init(&server->outputs);
    wl_list_init(&server->workspaces);
    wl_list_init(&server->shell_clients);
    wl_list_init(&server->shell_pending_invokes);
    wl_list_init(&server->groups);
    wl_list_init(&server->keyboards);
    wl_list_init(&server->pointers);
    wl_list_init(&server->windows);
    wl_list_init(&server->layer_surfaces);

    if (!config ||
            !config_path) {
        fprintf(stderr, "FAIL: core config unavailable\n");
        return false;
    }

    server->config =
        config;

    server->policy =
        &config->policy;

    wlr_log_init(WLR_DEBUG, NULL);

    server->display = wl_display_create();
    if (!server->display) {
        fprintf(stderr, "FAIL: wl_display_create\n");
        return false;
    }

    if (!add_display_socket(
            server,
            socket_name)) {
        return false;
    }

    struct wl_event_loop *event_loop =
        wl_display_get_event_loop(server->display);

    if (!event_loop) {
        fprintf(stderr, "FAIL: wl_display_get_event_loop\n");
        return false;
    }

    server->backend =
        wlr_backend_autocreate(event_loop, &server->session);

    if (!server->backend) {
        fprintf(stderr, "FAIL: wlr_backend_autocreate\n");
        return false;
    }

    server->renderer =
        wlr_renderer_autocreate(server->backend);

    if (!server->renderer) {
        fprintf(stderr, "FAIL: wlr_renderer_autocreate\n");
        return false;
    }

    if (!wlr_renderer_init_wl_display(
            server->renderer,
            server->display)) {
        fprintf(stderr, "FAIL: wlr_renderer_init_wl_display\n");
        return false;
    }

    server->allocator =
        wlr_allocator_autocreate(
            server->backend,
            server->renderer
        );

    if (!server->allocator) {
        fprintf(stderr, "FAIL: wlr_allocator_autocreate\n");
        return false;
    }

    server->compositor =
        wlr_compositor_create(
            server->display,
            6,
            server->renderer
        );

    if (!server->compositor) {
        fprintf(stderr, "FAIL: wlr_compositor_create\n");
        return false;
    }

    server->subcompositor =
        wlr_subcompositor_create(server->display);

    if (!server->subcompositor) {
        fprintf(stderr, "FAIL: wlr_subcompositor_create\n");
        return false;
    }

    server->viewporter =
        wlr_viewporter_create(
            server->display
        );

    if (!server->viewporter) {
        fprintf(
            stderr,
            "FAIL: wlr_viewporter_create\n"
        );

        return false;
    }

    server->data_device_manager =
        wlr_data_device_manager_create(
            server->display
        );

    if (!server->data_device_manager) {
        fprintf(
            stderr,
            "FAIL: wlr_data_device_manager_create\n"
        );
        return false;
    }

    if (!wl_event_loop_add_signal(
            event_loop,
            SIGINT,
            handle_signal,
            server)) {
        fprintf(stderr, "FAIL: SIGINT handler\n");
        return false;
    }

    if (!wl_event_loop_add_signal(
            event_loop,
            SIGTERM,
            handle_signal,
            server)) {
        fprintf(stderr, "FAIL: SIGTERM handler\n");
        return false;
    }

    if (!wlr_screencopy_manager_v1_create(server->display)) {
        wlr_log(
            WLR_ERROR,
            "Failed to create screencopy manager"
        );
        return false;
    }

    if (!onyrion_output_init(server)) {
        fprintf(stderr, "FAIL: onyrion_output_init\n");
        return false;
    }

    if (!onyrion_layout_init(server)) {
        fprintf(stderr, "FAIL: onyrion_layout_init\n");
        return false;
    }

    if (!onyrion_layer_shell_init(server)) {
        fprintf(stderr, "FAIL: onyrion_layer_shell_init\n");
        return false;
    }

    if (!onyrion_fallback_init(server)) {
        fprintf(stderr, "FAIL: onyrion_fallback_init\n");
        return false;
    }

    onyrion_group_init(server);

    if (!onyrion_group_surface_init(server)) {
        fprintf(stderr, "FAIL: onyrion_group_surface_init\n");
        return false;
    }

    if (!onyrion_workspace_init(server)) {
        fprintf(stderr, "FAIL: onyrion_workspace_init\n");
        return false;
    }

    if (!onyrion_shell_protocol_init(server)) {
        fprintf(stderr, "FAIL: onyrion_shell_protocol_init\n");
        return false;
    }

    if (!onyrion_window_init(server)) {
        fprintf(stderr, "FAIL: onyrion_window_init\n");
        return false;
    }

    if (!onyrion_input_init(server)) {
        fprintf(stderr, "FAIL: onyrion_input_init\n");
        return false;
    }

    if (!onyrion_config_runtime_init(
            server,
            config,
            config_path)) {
        fprintf(stderr, "FAIL: onyrion_config_runtime_init\n");
        return false;
    }

    if (!onyrion_session_lock_init(server)) {
        fprintf(stderr, "FAIL: onyrion_session_lock_init\n");
        return false;
    }

    if (!wlr_backend_start(server->backend)) {
        fprintf(stderr, "FAIL: wlr_backend_start\n");
        return false;
    }

    return true;
}

int onyrion_server_run(OnyrionServer *server) {
    printf("PASS: Onyrion bootstrap started\n");
    printf("WAYLAND_DISPLAY=%s\n", server->socket);
    fflush(stdout);

    wl_display_run(server->display);

    printf("PASS: event loop stopped\n");

    return 0;
}

void onyrion_server_finish(OnyrionServer *server) {
    if (server->display) {
        wl_display_destroy_clients(server->display);
    }

    onyrion_shell_protocol_finish(server);

    onyrion_session_lock_finish(server);

    onyrion_fallback_finish(server);
    onyrion_layer_shell_finish(server);

    onyrion_config_runtime_finish(server);
    onyrion_input_finish(server);

    onyrion_window_finish(server);
    onyrion_group_surface_finish(server);
    onyrion_group_finish(server);

    onyrion_workspace_finish(server);
    onyrion_layout_finish(server);
    onyrion_output_finish(server);

    if (server->allocator) {
        wlr_allocator_destroy(server->allocator);
    }

    if (server->renderer) {
        wlr_renderer_destroy(server->renderer);
    }

    if (server->backend) {
        wlr_backend_destroy(server->backend);
    }

    if (server->display) {
        wl_display_destroy(server->display);
    }

    memset(server, 0, sizeof(*server));

    printf("PASS: clean shutdown\n");
}
