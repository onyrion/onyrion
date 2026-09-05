#include "output.h"

#include "layer_shell.h"
#include "server.h"
#include "session_lock.h"
#include "shell_protocol.h"
#include "workspace.h"

#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

OnyrionOutput *onyrion_output_first(
        struct onyrion_server *server) {
    if (!server ||
            wl_list_empty(&server->outputs)) {
        return NULL;
    }

    OnyrionOutput *output =
        wl_container_of(
            server->outputs.next,
            output,
            link
        );

    return output;
}

OnyrionOutput *onyrion_output_from_wlr(
        struct onyrion_server *server,
        struct wlr_output *wlr_output) {
    if (!server || !wlr_output) {
        return NULL;
    }

    OnyrionOutput *output;

    wl_list_for_each(
            output,
            &server->outputs,
            link) {
        if (output->wlr_output == wlr_output) {
            return output;
        }
    }

    return NULL;
}

OnyrionOutput *onyrion_output_at(
        struct onyrion_server *server,
        double lx,
        double ly) {
    if (!server || !server->output_layout) {
        return NULL;
    }

    struct wlr_output *wlr_output =
        wlr_output_layout_output_at(
            server->output_layout,
            lx,
            ly
        );

    return onyrion_output_from_wlr(
        server,
        wlr_output
    );
}

void onyrion_output_focus(
        struct onyrion_server *server,
        OnyrionOutput *output) {
    if (!server ||
            !output ||
            output->server != server ||
            server->focused_output == output) {
        return;
    }

    server->focused_output = output;

    wlr_log(
        WLR_INFO,
        "Focused output: %s",
        output->wlr_output &&
                output->wlr_output->name
            ? output->wlr_output->name
            : "<unnamed>"
    );

    onyrion_shell_protocol_mark_changed(
        server
    );
}

static void handle_output_frame(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionOutput *output =
        wl_container_of(listener, output, frame);

    const uint32_t next_commit_seq =
        output->wlr_output->commit_seq + 1;

    onyrion_session_lock_output_commit_pending(
        output->server,
        output->wlr_output,
        next_commit_seq
    );

    if (!wlr_scene_output_commit(
            output->scene_output,
            NULL)) {
        onyrion_session_lock_output_commit_failed(
            output->server,
            output->wlr_output,
            next_commit_seq
        );

        wlr_log(
            WLR_ERROR,
            "Failed to commit scene output"
        );
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    wlr_scene_output_send_frame_done(
        output->scene_output,
        &now
    );
}

static void handle_output_present(
        struct wl_listener *listener,
        void *data) {
    OnyrionOutput *output =
        wl_container_of(listener, output, present);

    struct wlr_output_event_present *event =
        data;

    onyrion_session_lock_output_presented(
        output->server,
        output->wlr_output,
        event->commit_seq,
        event->presented
    );
}

static void handle_output_destroy(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionOutput *output =
        wl_container_of(listener, output, destroy);

    OnyrionServer *server =
        output->server;

    onyrion_session_lock_output_remove(
        server,
        output->wlr_output
    );

    onyrion_layer_shell_output_destroy(
        server,
        output->wlr_output
    );

    if (output->frame.notify) {
        wl_list_remove(&output->frame.link);
        output->frame.notify = NULL;
    }

    if (output->present.notify) {
        wl_list_remove(&output->present.link);
        output->present.notify = NULL;
    }

    if (output->scene_output) {
        wlr_scene_output_destroy(
            output->scene_output
        );
        output->scene_output = NULL;
    }

    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);

    onyrion_workspace_output_removed(
        server,
        output
    );

    free(output);
}

static void handle_new_output(
        struct wl_listener *listener,
        void *data) {
    OnyrionServer *server =
        wl_container_of(
            listener,
            server,
            new_output
        );

    struct wlr_output *wlr_output = data;

    OnyrionOutput *output =
        calloc(1, sizeof(*output));

    if (!output) {
        wlr_log(
            WLR_ERROR,
            "Failed to allocate OnyrionOutput"
        );
        return;
    }

    output->server = server;
    output->wlr_output = wlr_output;
    wl_list_init(&output->assigned_workspaces);

    if (!wlr_output_init_render(
            wlr_output,
            server->allocator,
            server->renderer)) {
        wlr_log(
            WLR_ERROR,
            "Failed to initialize output rendering"
        );

        free(output);
        return;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    wlr_output_state_set_enabled(
        &state,
        true
    );

    struct wlr_output_mode *mode =
        wlr_output_preferred_mode(
            wlr_output
        );

    if (mode) {
        wlr_output_state_set_mode(
            &state,
            mode
        );
    } else {
        wlr_output_state_set_custom_mode(
            &state,
            1280,
            720,
            0
        );
    }

    const bool committed =
        wlr_output_commit_state(
            wlr_output,
            &state
        );

    wlr_output_state_finish(&state);

    if (!committed) {
        wlr_log(
            WLR_ERROR,
            "Failed to enable output"
        );

        free(output);
        return;
    }

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(
            server->output_layout,
            wlr_output
        );

    if (!layout_output) {
        wlr_log(
            WLR_ERROR,
            "Failed to add output to layout"
        );

        free(output);
        return;
    }

    output->scene_output =
        wlr_scene_output_create(
            server->scene,
            wlr_output
        );

    if (!output->scene_output) {
        wlr_log(
            WLR_ERROR,
            "Failed to create scene output"
        );

        wlr_output_layout_remove(
            server->output_layout,
            wlr_output
        );

        free(output);
        return;
    }

    output->frame.notify =
        handle_output_frame;

    wl_signal_add(
        &wlr_output->events.frame,
        &output->frame
    );

    output->present.notify =
        handle_output_present;

    wl_signal_add(
        &wlr_output->events.present,
        &output->present
    );

    wlr_output_schedule_frame(
        wlr_output
    );

    wlr_scene_output_layout_add_output(
        server->scene_layout,
        layout_output,
        output->scene_output
    );

    output->destroy.notify =
        handle_output_destroy;

    wl_signal_add(
        &wlr_output->events.destroy,
        &output->destroy
    );

    wl_list_insert(
        server->outputs.prev,
        &output->link
    );

    wlr_output_layout_get_box(
        server->output_layout,
        wlr_output,
        &output->usable_box
    );

    onyrion_session_lock_output_add(
        server,
        wlr_output
    );

    onyrion_workspace_output_added(
        server,
        output
    );

    onyrion_layer_shell_arrange(
        server
    );

    wlr_log(
        WLR_INFO,
        "New output: %s",
        wlr_output->name
            ? wlr_output->name
            : "<unnamed>"
    );
}

bool onyrion_output_init(
        struct onyrion_server *server) {
    wl_list_init(&server->outputs);

    server->output_layout =
        wlr_output_layout_create(
            server->display
        );

    if (!server->output_layout) {
        wlr_log(
            WLR_ERROR,
            "Failed to create output layout"
        );
        return false;
    }

    server->scene =
        wlr_scene_create();

    if (!server->scene) {
        wlr_log(
            WLR_ERROR,
            "Failed to create scene"
        );
        return false;
    }

    server->scene_layout =
        wlr_scene_attach_output_layout(
            server->scene,
            server->output_layout
        );

    if (!server->scene_layout) {
        wlr_log(
            WLR_ERROR,
            "Failed to attach scene to output layout"
        );
        return false;
    }

    server->new_output.notify =
        handle_new_output;

    wl_signal_add(
        &server->backend->events.new_output,
        &server->new_output
    );

    return true;
}

void onyrion_output_finish(
        struct onyrion_server *server) {
    if (!server->new_output.notify) {
        return;
    }

    wl_list_remove(
        &server->new_output.link
    );
    server->new_output.notify = NULL;

    OnyrionOutput *output;
    OnyrionOutput *tmp;

    wl_list_for_each_safe(
            output,
            tmp,
            &server->outputs,
            link) {
        if (output->frame.notify) {
            wl_list_remove(
                &output->frame.link
            );
            output->frame.notify = NULL;
        }

        if (output->present.notify) {
            wl_list_remove(
                &output->present.link
            );
            output->present.notify = NULL;
        }

        if (output->scene_output) {
            wlr_scene_output_destroy(
                output->scene_output
            );
            output->scene_output = NULL;
        }

        wl_list_remove(
            &output->destroy.link
        );
        wl_list_remove(
            &output->link
        );

        onyrion_workspace_output_removed(
            server,
            output
        );

        free(output);
    }

    server->focused_output = NULL;
}
