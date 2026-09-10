#include "compiler.h"
#include "config_runtime.h"
#include "config_editor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <wayland-client.h>

#include "onyrion-shell-unstable-v1-client-protocol.h"
#include "provider_runtime.h"
#include "shell_config.h"
#include "shell_state.h"
#include "ui_actions_v11.h"
#include "power_actions.h"
#include "tray_sni.h"
#include "ui_daily_v11.h"
#include "ui_runtime.h"
#include "widget_model.h"
#include "ewwii_adapter.h"

extern char **environ;

typedef enum shell_command {
    COMMAND_DAEMON,
    COMMAND_CONFIG_CHECK,
    COMMAND_CONFIG_PERSIST,
    COMMAND_STATE,
    COMMAND_WATCH_STATE,
    COMMAND_WATCH_STATUS,
    COMMAND_INVOKE,
    COMMAND_LAUNCH,
    COMMAND_NEXT,
    COMMAND_PREVIOUS,
    COMMAND_SPLIT,
    COMMAND_FOCUS,
    COMMAND_MOVE,
    COMMAND_RESIZE,
    COMMAND_FLIP,
    COMMAND_FULLSCREEN,
    COMMAND_CLOSE,
    COMMAND_WORKSPACE_ACTIVATE,
    COMMAND_GROUP_FOCUS,
    COMMAND_WINDOW_FOCUS,
    COMMAND_WINDOW_NEXT,
    COMMAND_WINDOW_PREVIOUS,
    COMMAND_GROUP_MERGE,
    COMMAND_GROUP_MOVE_TO_WORKSPACE,
    COMMAND_GROUP_SPLIT_AT,
    COMMAND_GROUP_FLOAT,
    COMMAND_GROUP_TILE,
    COMMAND_GROUP_PIN,
    COMMAND_GROUP_UNPIN,
    COMMAND_WINDOW_MOVE_TO_GROUP,
    COMMAND_WINDOW_SPLIT,
    COMMAND_WINDOW_RANGE_MOVE,
    COMMAND_WINDOW_MOVE_TO_WORKSPACE,
    COMMAND_WINDOW_FLOAT,
    COMMAND_WINDOW_TILE,
    COMMAND_SESSION_EXIT,
    COMMAND_TERMINAL,
    COMMAND_LOCK,
    COMMAND_POWER,
} ShellCommand;

typedef enum ctl_watch_kind {
    CTL_WATCH_STATE,
    CTL_WATCH_WORKSPACE,
    CTL_WATCH_GROUP,
    CTL_WATCH_WINDOW,
    CTL_WATCH_OUTPUT,
} CtlWatchKind;

typedef struct shell_request {
    ShellCommand command;
    uint32_t direction;
    uint32_t orientation;
    char **launch_argv;
    const char *config_path;
    const char *provider_capability;
    const char *provider_action;
    const char *object_id;
    const char *object_id2;
    const char *object_id3;
    const char *power_action;
    const char *ewwii_config_dir;
    const char *ctl_namespace;
    const char *ctl_name;
    const char *ctl_verb;
    CtlWatchKind ctl_watch_kind;
    ShellConfigEditRequest config_edit;
    bool config_explicit;
    bool state_ewwii_actions;
    bool ctl_mode;
    bool ctl_json;
    bool ctl_persist;
} ShellRequest;

typedef struct shell {
    struct wl_display *display;
    struct wl_registry *registry;
    struct onyrion_shell_unstable_v1 *core;
    uint32_t core_version;

    uint32_t expected_action;

    bool ctl_mode;
    bool ctl_json;
    bool result_received;
    bool succeeded;

    bool watch_state;
    bool state_request_outstanding;
    bool protocol_failed;
    bool controller_claim_received;
    bool controller_owned;
    uint32_t latest_change_generation;

    ShellState state;
    GMainLoop *loop;

    ProviderManager *providers;
    int control_fd;
    char *control_path;
    GArray *state_waiters;
} Shell;

typedef struct state_waiter {
    int fd;
    uint32_t after_generation;
} StateWaiter;

static bool shell_request_state(
    Shell *shell
);

static void state_waiters_notify(
    Shell *shell
);

static char *snapshot_to_json(
        const ShellSnapshot *snapshot) {
    JsonBuilder *builder = json_builder_new();
    if (!builder) return NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "generation");
    json_builder_add_int_value(builder, snapshot->generation);

    json_builder_set_member_name(builder, "outputs");
    json_builder_begin_array(builder);
    for (size_t i = 0; i < snapshot->output_count; i++) {
        const ShellOutputState *output = &snapshot->outputs[i];
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, output->name);
        json_builder_set_member_name(builder, "focused");
        json_builder_add_boolean_value(builder, output->focused);
        json_builder_set_member_name(builder, "visible_workspace_id");
        json_builder_add_string_value(builder, output->visible_workspace_id);
        json_builder_set_member_name(builder, "assigned_workspace_count");
        json_builder_add_int_value(builder, output->assigned_workspace_count);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "workspaces");
    json_builder_begin_array(builder);
    for (size_t i = 0; i < snapshot->workspace_count; i++) {
        const ShellWorkspaceState *workspace = &snapshot->workspaces[i];
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, workspace->id);
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, workspace->active);
        json_builder_set_member_name(builder, "groups");
        json_builder_add_int_value(builder, workspace->group_count);
        json_builder_set_member_name(builder, "tiles");
        json_builder_add_int_value(builder, workspace->tile_count);
        json_builder_set_member_name(builder, "windows");
        json_builder_add_int_value(builder, workspace->window_count);
        json_builder_set_member_name(builder, "output_name");
        json_builder_add_string_value(builder, workspace->output_name);
        json_builder_set_member_name(builder, "visible");
        json_builder_add_boolean_value(builder, workspace->visible);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, workspace->name);
        json_builder_set_member_name(builder, "icon");
        json_builder_add_string_value(builder, workspace->icon);
        json_builder_set_member_name(builder, "persistent");
        json_builder_add_boolean_value(builder, workspace->persistent);
        json_builder_set_member_name(builder, "startup");
        json_builder_add_boolean_value(builder, workspace->startup);
        json_builder_set_member_name(builder, "output_affinity");
        json_builder_add_string_value(builder, workspace->output_affinity);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "groups");
    json_builder_begin_array(builder);
    for (size_t i = 0; i < snapshot->group_count; i++) {
        const ShellGroupState *group = &snapshot->groups[i];
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, group->id);
        json_builder_set_member_name(builder, "workspace_id");
        json_builder_add_string_value(builder, group->workspace_id);
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, group->active);
        json_builder_set_member_name(builder, "window_count");
        json_builder_add_int_value(builder, group->window_count);
        json_builder_set_member_name(builder, "placement");
        json_builder_add_string_value(builder, shell_group_placement_name(group->placement));
        json_builder_set_member_name(builder, "pinned");
        json_builder_add_boolean_value(builder, group->pinned);
        json_builder_set_member_name(builder, "pinned_output_name");
        json_builder_add_string_value(builder,
            group->pinned_output_name ? group->pinned_output_name : "");
        json_builder_set_member_name(builder, "placement_seen");
        json_builder_add_boolean_value(builder, group->placement_seen);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "windows");
    json_builder_begin_array(builder);
    for (size_t i = 0; i < snapshot->window_count; i++) {
        const ShellWindowState *window = &snapshot->windows[i];
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, window->id);
        json_builder_set_member_name(builder, "group_id");
        json_builder_add_string_value(builder, window->group_id);
        json_builder_set_member_name(builder, "active");
        json_builder_add_boolean_value(builder, window->active);
        json_builder_set_member_name(builder, "fullscreen");
        json_builder_add_boolean_value(builder, window->fullscreen);
        json_builder_set_member_name(builder, "app_id");
        json_builder_add_string_value(builder, window->app_id);
        json_builder_set_member_name(builder, "title");
        json_builder_add_string_value(builder, window->title);
        json_builder_set_member_name(builder, "workspace_id");
        json_builder_add_string_value(builder, window->workspace_id);
        json_builder_set_member_name(builder, "placement");
        json_builder_add_string_value(builder, shell_window_placement_name(window->placement));
        json_builder_set_member_name(builder, "parent_window_id");
        json_builder_add_string_value(builder, window->parent_window_id);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *generator = json_generator_new();
    if (!root || !generator) {
        if (root) json_node_free(root);
        if (generator) g_object_unref(generator);
        g_object_unref(builder);
        return NULL;
    }
    json_generator_set_root(generator, root);
    char *json = json_generator_to_data(generator, NULL);
    g_object_unref(generator);
    json_node_free(root);
    g_object_unref(builder);
    return json;
}

static bool send_all_no_sigpipe(
        int fd,
        const char *data,
        size_t length) {
    size_t offset = 0;

    while (offset < length) {
        const ssize_t written =
            send(
                fd,
                data + offset,
                length - offset,
                MSG_NOSIGNAL
            );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (written == 0) {
            errno = EPIPE;
            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static bool send_state_waiter_response(
        int fd,
        const ShellSnapshot *snapshot) {
    g_autofree char *json =
        snapshot_to_json(snapshot);

    if (!json) {
        return false;
    }

    g_autofree char *response =
        g_strdup_printf(
            "STATE\t%u\t%s\n",
            snapshot->generation,
            json
        );

    if (!response) {
        return false;
    }

    return send_all_no_sigpipe(
        fd,
        response,
        strlen(response)
    );
}

static bool state_waiter_set_cloexec(
        int fd) {
    const int flags =
        fcntl(
            fd,
            F_GETFD
        );

    if (flags < 0) {
        return false;
    }

    return fcntl(
        fd,
        F_SETFD,
        flags | FD_CLOEXEC
    ) == 0;
}

static bool state_waiter_store(
        Shell *shell,
        int fd,
        uint32_t after_generation) {
    if (!state_waiter_set_cloexec(fd)) {
        return false;
    }

    if (!shell->state_waiters) {
        shell->state_waiters =
            g_array_new(
                false,
                false,
                sizeof(StateWaiter)
            );
    }

    StateWaiter waiter = {
        .fd = fd,
        .after_generation = after_generation,
    };

    g_array_append_val(
        shell->state_waiters,
        waiter
    );

    return true;
}

static void state_waiters_notify(
        Shell *shell) {
    if (!shell->state_waiters ||
            shell->state_waiters->len == 0) {
        return;
    }

    const ShellSnapshot *snapshot =
        shell_state_snapshot(
            &shell->state
        );

    if (snapshot->generation == 0) {
        return;
    }

    guint i = 0;

    while (i < shell->state_waiters->len) {
        const StateWaiter waiter =
            g_array_index(
                shell->state_waiters,
                StateWaiter,
                i
            );

        if (snapshot->generation <=
                waiter.after_generation) {
            i++;
            continue;
        }

        errno = 0;

        const bool sent =
            send_state_waiter_response(
                waiter.fd,
                snapshot
            );

        const int saved_errno = errno;

        close(waiter.fd);

        g_array_remove_index(
            shell->state_waiters,
            i
        );

        if (sent) {
            printf(
                "CONTROL WATCH_STATE SEND generation=%u subscribers=%u\n",
                snapshot->generation,
                shell->state_waiters->len
            );
        } else {
            printf(
                "CONTROL WATCH_STATE DROP generation=%u subscribers=%u error=%s\n",
                snapshot->generation,
                shell->state_waiters->len,
                saved_errno
                    ? strerror(saved_errno)
                    : "send failed"
            );
        }

        fflush(stdout);
    }
}

static void state_waiters_finish(
        Shell *shell) {
    if (!shell->state_waiters) {
        return;
    }

    for (guint i = 0;
            i < shell->state_waiters->len;
            i++) {
        const StateWaiter waiter =
            g_array_index(
                shell->state_waiters,
                StateWaiter,
                i
            );

        close(waiter.fd);
    }

    g_array_free(
        shell->state_waiters,
        true
    );

    shell->state_waiters = NULL;
}

static void print_snapshot(
        const ShellSnapshot *snapshot) {
    printf("STATE generation=%u outputs=%zu workspaces=%zu groups=%zu windows=%zu\n",
        snapshot->generation,
        snapshot->output_count,
        snapshot->workspace_count,
        snapshot->group_count,
        snapshot->window_count);

    for (size_t i = 0; i < snapshot->output_count; i++) {
        const ShellOutputState *output = &snapshot->outputs[i];
        printf("OUTPUT name=%s focused=%u visible_workspace=%s assigned=%u\n",
            output->name, output->focused ? 1U : 0U,
            output->visible_workspace_id, output->assigned_workspace_count);
    }

    for (size_t i = 0; i < snapshot->workspace_count; i++) {
        const ShellWorkspaceState *workspace = &snapshot->workspaces[i];
        printf("WORKSPACE id=%s output=%s visible=%u active=%u name=%s icon=%s persistent=%u startup=%u affinity=%s groups=%u tiles=%u windows=%u\n",
            workspace->id, workspace->output_name,
            workspace->visible ? 1U : 0U, workspace->active ? 1U : 0U,
            workspace->name, workspace->icon,
            workspace->persistent ? 1U : 0U, workspace->startup ? 1U : 0U,
            workspace->output_affinity, workspace->group_count,
            workspace->tile_count, workspace->window_count);
    }

    for (size_t i = 0; i < snapshot->group_count; i++) {
        const ShellGroupState *group = &snapshot->groups[i];
        printf("GROUP id=%s workspace_id=%s active=%u windows=%u placement=%s pinned=%u pinned_output=%s placement_seen=%u\n",
            group->id, group->workspace_id,
            group->active ? 1U : 0U, group->window_count,
            shell_group_placement_name(group->placement),
            group->pinned ? 1U : 0U,
            group->pinned_output_name ? group->pinned_output_name : "",
            group->placement_seen ? 1U : 0U);
    }

    for (size_t i = 0; i < snapshot->window_count; i++) {
        const ShellWindowState *window = &snapshot->windows[i];
        printf("WINDOW id=%s workspace_id=%s group_id=%s placement=%s parent=%s active=%u fullscreen=%u app_id=%s title=%s\n",
            window->id, window->workspace_id, window->group_id,
            shell_window_placement_name(window->placement),
            window->parent_window_id,
            window->active ? 1U : 0U, window->fullscreen ? 1U : 0U,
            window->app_id, window->title);
    }
    fflush(stdout);
}

static void handle_state_begin(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        uint32_t generation,
        uint32_t workspace_count) {
    (void)core;

    Shell *shell = data;

    if (!shell->watch_state) {
        return;
    }

    if (!shell_state_begin(
            &shell->state,
            generation,
            workspace_count)) {
        fprintf(
            stderr,
            "FAIL: cannot begin shell state snapshot\n"
        );

        shell->protocol_failed = true;
    }
}

static void handle_workspace(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *id,
        uint32_t active,
        uint32_t group_count,
        uint32_t tile_count,
        uint32_t window_count) {
    (void)core;

    Shell *shell = data;

    if (!shell->watch_state) {
        return;
    }

    if (!shell_state_add_workspace(
            &shell->state,
            id,
            active != 0,
            group_count,
            tile_count,
            window_count)) {
        fprintf(
            stderr,
            "FAIL: invalid workspace in shell state snapshot\n"
        );

        shell->protocol_failed = true;
    }
}

static void handle_group(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *id,
        const char *workspace_id,
        uint32_t active,
        uint32_t window_count) {
    (void)core;

    Shell *shell = data;

    if (!shell->watch_state) {
        return;
    }

    if (!shell_state_add_group(
            &shell->state,
            id,
            workspace_id,
            active != 0,
            window_count)) {
        fprintf(
            stderr,
            "FAIL: invalid group in shell state snapshot\n"
        );

        shell->protocol_failed = true;
    }
}

static void handle_group_placement(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *group_id,
        uint32_t placement,
        uint32_t pinned,
        const char *pinned_output_name) {
    (void)core;

    Shell *shell = data;
    if (!shell->watch_state) {
        return;
    }

    if (placement > ONYRION_SHELL_UNSTABLE_V1_GROUP_PLACEMENT_FLOATING ||
            !shell_state_set_group_placement(
                &shell->state,
                group_id,
                (ShellGroupPlacement)placement,
                pinned != 0,
                pinned_output_name)) {
        fprintf(stderr, "FAIL: invalid group_placement in shell state snapshot\n");
        shell->protocol_failed = true;
    }
}

static void handle_drag_surface_motion(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *group_id,
        const char *namespace_name,
        wl_fixed_t x,
        wl_fixed_t y) {
    (void)data;
    (void)core;
    (void)group_id;
    (void)namespace_name;
    (void)x;
    (void)y;

    /* The persistent Shell daemon does not own Ewwii wl_surfaces and does not
     * begin Group-surface drags.  Keep the v13 listener total so an unexpected
     * event cannot hit a NULL callback; the same-client Ewwii bridge will own
     * the real drag-surface-motion consumer. */
}

static void handle_window(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *id,
        const char *group_id,
        uint32_t active,
        uint32_t fullscreen,
        const char *app_id,
        const char *title) {
    (void)core;

    Shell *shell = data;

    if (!shell->watch_state) {
        return;
    }

    if (!shell_state_add_window(
            &shell->state,
            id,
            group_id,
            active != 0,
            fullscreen != 0,
            app_id,
            title)) {
        fprintf(
            stderr,
            "FAIL: invalid window in shell state snapshot\n"
        );

        shell->protocol_failed = true;
    }
}

static void handle_output(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *name,
        uint32_t focused,
        const char *visible_workspace_id,
        uint32_t assigned_workspace_count) {
    (void)core;
    Shell *shell = data;
    if (!shell->watch_state) return;
    if (!shell_state_add_output(&shell->state, name, focused != 0,
            visible_workspace_id, assigned_workspace_count)) {
        fprintf(stderr, "FAIL: invalid output in shell state snapshot\n");
        shell->protocol_failed = true;
    }
}

static void handle_workspace_output(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *workspace_id,
        const char *output_name,
        uint32_t visible) {
    (void)core;
    Shell *shell = data;
    if (!shell->watch_state) return;
    if (!shell_state_set_workspace_output(&shell->state, workspace_id,
            output_name, visible != 0)) {
        fprintf(stderr, "FAIL: invalid workspace_output in shell state snapshot\n");
        shell->protocol_failed = true;
    }
}

static void handle_workspace_metadata(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *workspace_id,
        const char *name,
        const char *icon,
        uint32_t persistent,
        uint32_t startup,
        const char *output_affinity) {
    (void)core;
    Shell *shell = data;
    if (!shell->watch_state) return;
    if (!shell_state_set_workspace_metadata(&shell->state, workspace_id,
            name, icon, persistent != 0, startup != 0, output_affinity)) {
        fprintf(stderr, "FAIL: invalid workspace_metadata in shell state snapshot\n");
        shell->protocol_failed = true;
    }
}

static void handle_window_placement(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        const char *window_id,
        const char *workspace_id,
        uint32_t placement,
        const char *parent_window_id) {
    (void)core;
    Shell *shell = data;
    if (!shell->watch_state) return;
    if (placement > ONYRION_SHELL_UNSTABLE_V1_WINDOW_PLACEMENT_FLOATING ||
            !shell_state_set_window_placement(&shell->state, window_id,
                workspace_id, (ShellWindowPlacement)placement,
                parent_window_id)) {
        fprintf(stderr, "FAIL: invalid window_placement in shell state snapshot\n");
        shell->protocol_failed = true;
    }
}

static void handle_state_end(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        uint32_t generation) {
    (void)core;

    Shell *shell = data;

    if (!shell->watch_state) {
        return;
    }

    shell->state_request_outstanding = false;

    if (!shell_state_end(
            &shell->state,
            generation)) {
        fprintf(
            stderr,
            "FAIL: invalid shell state snapshot end generation=%u\n",
            generation
        );

        shell->protocol_failed = true;
        return;
    }

    print_snapshot(
        shell_state_snapshot(
            &shell->state
        )
    );

    state_waiters_notify(shell);

    if (shell->latest_change_generation > generation) {
        if (!shell_request_state(shell)) {
            shell->protocol_failed = true;
        }
    }
}

static void handle_changed(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        uint32_t generation) {
    (void)core;

    Shell *shell = data;

    if (!shell->watch_state) {
        return;
    }

    if (generation > shell->latest_change_generation) {
        shell->latest_change_generation = generation;
    }

    if (!shell->state_request_outstanding) {
        if (!shell_request_state(shell)) {
            shell->protocol_failed = true;
        }
    }
}

static void handle_action_result(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        uint32_t action,
        uint32_t success) {
    (void)core;

    Shell *shell = data;

    shell->result_received = true;

    if (action != shell->expected_action) {
        fprintf(
            stderr,
            "FAIL: unexpected action=%u expected=%u\n",
            action,
            shell->expected_action
        );

        shell->succeeded = false;
        return;
    }

    shell->succeeded =
        success != 0;

    if (!shell->ctl_mode) {
        printf(
            "ACTION RESULT action=%u success=%u\n",
            action,
            success
        );
    }
}

static void handle_controller_claim_result(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        uint32_t success) {
    (void)core;

    Shell *shell = data;

    shell->controller_claim_received = true;
    shell->controller_owned =
        success != 0;

    printf(
        "CONTROLLER CLAIM success=%u\n",
        success != 0 ? 1U : 0U
    );
    fflush(stdout);
}

static bool shell_invoke_action(
        Shell *shell,
        const char *capability,
        const char *action,
        const char **provider_label,
        GError **error) {
    if (provider_label) {
        *provider_label = "-";
    }

    if (!shell ||
            !capability ||
            !action) {
        return false;
    }

    if (strcmp(
            capability,
            "ui:dismiss-transients"
        ) == 0 &&
            strcmp(
                action,
                "pointer-focus"
            ) == 0) {
        const char *ui_dir =
            g_getenv("ONYRION_UI_DIR");

        if (!ui_dir || ui_dir[0] == '\0') {
            ui_dir =
                "/usr/share/onyrion/ui/ewwii";
        }

        if (provider_label) {
            *provider_label =
                "shell-transient";
        }

        return ui_actions_v11_dismiss_transients(
            ui_dir
        );
    }

    if (strcmp(
            capability,
            "ui:context-actions"
        ) == 0) {
        const char *subject_kind = NULL;
        const char *subject_id = NULL;

        if (g_str_has_prefix(
                action,
                "window:")) {
            subject_kind = "window";
            subject_id =
                action + strlen("window:");
        } else if (g_str_has_prefix(
                action,
                "group:")) {
            subject_kind = "group";
            subject_id =
                action + strlen("group:");
        }

        const ShellSnapshot *snapshot =
            shell_state_snapshot(
                &shell->state
            );
        g_autofree char *json =
            snapshot &&
            snapshot->generation != 0
                ? snapshot_to_json(snapshot)
                : NULL;
        const char *ui_dir =
            g_getenv("ONYRION_UI_DIR");

        if (!ui_dir || ui_dir[0] == '\0') {
            ui_dir =
                "/usr/share/onyrion/ui/ewwii";
        }

        if (provider_label) {
            *provider_label =
                "shell-context";
        }

        return subject_kind &&
            subject_id &&
            json &&
            ui_actions_v11_context_open(
                ui_dir,
                json,
                subject_kind,
                subject_id
            );
    }

    if (!shell->providers) {
        return false;
    }

    const ProviderConfig *used_provider = NULL;
    const bool invoked =
        provider_manager_invoke(
            shell->providers,
            capability,
            action,
            &used_provider,
            error
        );

    if (provider_label &&
            used_provider) {
        *provider_label =
            used_provider->id;
    }

    return invoked;
}

static void handle_controller_invoke(
        void *data,
        struct onyrion_shell_unstable_v1 *core,
        uint32_t serial,
        const char *capability,
        const char *action) {
    Shell *shell = data;

    GError *error = NULL;
    const char *provider_label = "-";
    const bool invoked =
        shell->controller_owned &&
        shell_invoke_action(
            shell,
            capability,
            action,
            &provider_label,
            &error
        );

    printf(
        "CONTROLLER INVOKE serial=%u capability=%s action=%s "
        "success=%u provider=%s\n",
        serial,
        capability ? capability : "-",
        action ? action : "-",
        invoked ? 1U : 0U,
        provider_label
    );

    if (!invoked && error) {
        fprintf(
            stderr,
            "CONTROLLER INVOKE FAIL serial=%u error=%s\n",
            serial,
            error->message
        );
    }

    g_clear_error(&error);

    onyrion_shell_unstable_v1_controller_result(
        core,
        serial,
        invoked ? 1U : 0U
    );

    if (wl_display_flush(
            shell->display) < 0 &&
            errno != EAGAIN) {
        fprintf(
            stderr,
            "FAIL: cannot flush controller result: %s\n",
            strerror(errno)
        );

        shell->protocol_failed = true;

        if (shell->loop) {
            g_main_loop_quit(
                shell->loop
            );
        }
    }

    fflush(stdout);
}

static const struct onyrion_shell_unstable_v1_listener
core_listener = {
    .state_begin = handle_state_begin,
    .workspace = handle_workspace,
    .state_end = handle_state_end,
    .changed = handle_changed,
    .action_result = handle_action_result,
    .group = handle_group,
    .window = handle_window,
    .output = handle_output,
    .workspace_output = handle_workspace_output,
    .workspace_metadata = handle_workspace_metadata,
    .window_placement = handle_window_placement,
    .group_placement = handle_group_placement,
    .drag_surface_motion = handle_drag_surface_motion,
    .controller_claim_result =
        handle_controller_claim_result,
    .controller_invoke =
        handle_controller_invoke,
};

static void handle_global(
        void *data,
        struct wl_registry *registry,
        uint32_t name,
        const char *interface,
        uint32_t version) {
    Shell *shell = data;

    if (strcmp(
            interface,
            onyrion_shell_unstable_v1_interface.name) != 0) {
        return;
    }

    const uint32_t supported_version =
        (uint32_t)
            onyrion_shell_unstable_v1_interface.version;

    const uint32_t bind_version =
        version < supported_version
            ? version
            : supported_version;

    shell->core =
        wl_registry_bind(
            registry,
            name,
            &onyrion_shell_unstable_v1_interface,
            bind_version
        );

    if (shell->core) {
        shell->core_version = bind_version;
        onyrion_shell_unstable_v1_add_listener(
            shell->core,
            &core_listener,
            shell
        );
    }
}

static void handle_global_remove(
        void *data,
        struct wl_registry *registry,
        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener
registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static bool shell_connect(
        Shell *shell) {
    shell->display =
        wl_display_connect(NULL);

    if (!shell->display) {
        fprintf(
            stderr,
            "FAIL: cannot connect to WAYLAND_DISPLAY\n"
        );

        return false;
    }

    shell->registry =
        wl_display_get_registry(
            shell->display
        );

    if (!shell->registry) {
        return false;
    }

    if (wl_registry_add_listener(
            shell->registry,
            &registry_listener,
            shell) < 0) {
        return false;
    }

    if (wl_display_roundtrip(
            shell->display) < 0) {
        return false;
    }

    if (!shell->core) {
        fprintf(
            stderr,
            "FAIL: onyrion_shell_unstable_v1 unavailable\n"
        );

        return false;
    }

    return true;
}

static void shell_finish(
        Shell *shell) {
    if (shell->core) {
        onyrion_shell_unstable_v1_destroy(
            shell->core
        );
    }

    if (shell->registry) {
        wl_registry_destroy(
            shell->registry
        );
    }

    if (shell->display) {
        wl_display_disconnect(
            shell->display
        );
    }

    shell_state_finish(&shell->state);
}

static bool shell_request_state(
        Shell *shell) {
    if (!shell->core) {
        return false;
    }

    onyrion_shell_unstable_v1_get_state(
        shell->core
    );

    shell->state_request_outstanding = true;

    if (wl_display_flush(
            shell->display) < 0 &&
            errno != EAGAIN) {
        fprintf(
            stderr,
            "FAIL: cannot flush state request: %s\n",
            strerror(errno)
        );

        return false;
    }

    return true;
}

static bool parse_direction(
        const char *value,
        uint32_t *direction) {
    if (strcmp(value, "left") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_LEFT;
    } else if (strcmp(value, "right") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_RIGHT;
    } else if (strcmp(value, "up") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_UP;
    } else if (strcmp(value, "down") == 0) {
        *direction =
            ONYRION_SHELL_UNSTABLE_V1_DIRECTION_DOWN;
    } else {
        return false;
    }

    return true;
}

static bool parse_orientation(
        const char *value,
        uint32_t *orientation) {
    if (strcmp(value, "horizontal") == 0) {
        *orientation =
            ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_HORIZONTAL;
    } else if (strcmp(value, "vertical") == 0) {
        *orientation =
            ONYRION_SHELL_UNSTABLE_V1_SPLIT_ORIENTATION_VERTICAL;
    } else {
        return false;
    }

    return true;
}

static bool parse_config_option(
        int argc,
        char **argv,
        const char **config_path,
        bool *config_explicit) {
    if (argc == 2) {
        *config_explicit = false;
        return true;
    }

    if (argc == 4 &&
            strcmp(argv[2], "--config") == 0) {
        *config_path = argv[3];
        *config_explicit = true;
        return true;
    }

    return false;
}

static bool ctl_name_valid(
        const char *value) {
    if (!value || value[0] == '\0') {
        return false;
    }

    for (const unsigned char *cursor =
            (const unsigned char *)value;
            *cursor;
            cursor++) {
        const unsigned char c = *cursor;

        if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '_' ||
                c == '-') {
            continue;
        }

        return false;
    }

    return true;
}

static bool parse_ctl_request(
        int argc,
        char **argv,
        ShellRequest *request) {
    request->ctl_mode = true;

    int index = 1;

    while (index < argc &&
            strncmp(argv[index], "--", 2) == 0) {
        if (strcmp(argv[index], "--json") == 0 &&
                !request->ctl_json) {
            request->ctl_json = true;
            index++;
            continue;
        }

        if (strcmp(argv[index], "--persist") == 0 &&
                !request->ctl_persist) {
            request->ctl_persist = true;
            index++;
            continue;
        }

        return false;
    }

    if (index >= argc) {
        return false;
    }

    const char *object = argv[index++];

    if (strcmp(object, "config") == 0) {
        if (!request->ctl_persist) {
            fprintf(
                stderr,
                "FAIL: config mutation requires explicit --persist\n"
            );
            return false;
        }

        if (index >= argc) {
            return false;
        }

        const char *scope = argv[index++];

        if (strcmp(scope, "appearance") == 0) {
            if (index + 2 != argc) {
                return false;
            }

            const char *field = argv[index++];

            if (strcmp(field, "wallpaper") != 0) {
                return false;
            }

            request->config_edit.field =
                SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER;
            request->config_edit.value = argv[index];
            request->command = COMMAND_CONFIG_PERSIST;
            request->ctl_namespace = "config";
            request->ctl_name = "appearance";
            request->ctl_verb = "wallpaper";
            return request->config_edit.value[0] != '\0';
        }

        if (strcmp(scope, "fallback") == 0) {
            if (index + 2 != argc) {
                return false;
            }

            const char *field = argv[index++];

            if (strcmp(field, "enabled") == 0) {
                request->config_edit.field =
                    SHELL_CONFIG_EDIT_FALLBACK_ENABLED;
            } else if (strcmp(field, "title") == 0) {
                request->config_edit.field =
                    SHELL_CONFIG_EDIT_FALLBACK_TITLE;
            } else {
                return false;
            }

            request->config_edit.value = argv[index];
            request->command = COMMAND_CONFIG_PERSIST;
            request->ctl_namespace = "config";
            request->ctl_name = "fallback";
            request->ctl_verb = field;
            return request->config_edit.value[0] != '\0';
        }

        if (strcmp(scope, "provider") == 0) {
            if (index + 3 != argc ||
                    !ctl_name_valid(argv[index])) {
                return false;
            }

            request->config_edit.provider_id =
                argv[index++];

            const char *field = argv[index++];

            if (strcmp(field, "priority") == 0) {
                request->config_edit.field =
                    SHELL_CONFIG_EDIT_PROVIDER_PRIORITY;
            } else if (strcmp(field, "autostart") == 0) {
                request->config_edit.field =
                    SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART;
            } else if (strcmp(field, "required") == 0) {
                request->config_edit.field =
                    SHELL_CONFIG_EDIT_PROVIDER_REQUIRED;
            } else {
                return false;
            }

            request->config_edit.value = argv[index];
            request->command = COMMAND_CONFIG_PERSIST;
            request->ctl_namespace = "config";
            request->ctl_name = request->config_edit.provider_id;
            request->ctl_verb = field;
            return request->config_edit.value[0] != '\0';
        }

        return false;
    }

    if (request->ctl_persist) {
        fprintf(
            stderr,
            "FAIL: --persist is only valid with semantic config edits; "
            "runtime actions remain runtime-only\n"
        );
        return false;
    }

    if (strcmp(object, "state") == 0) {
        if (index != argc) {
            return false;
        }

        request->command = COMMAND_STATE;
        return true;
    }

    if (strcmp(object, "watch") == 0) {
        if (index + 1 != argc) {
            return false;
        }

        const char *kind = argv[index];

        if (strcmp(kind, "state") == 0) {
            request->ctl_watch_kind = CTL_WATCH_STATE;
        } else if (strcmp(kind, "workspace") == 0) {
            request->ctl_watch_kind = CTL_WATCH_WORKSPACE;
        } else if (strcmp(kind, "group") == 0) {
            request->ctl_watch_kind = CTL_WATCH_GROUP;
        } else if (strcmp(kind, "window") == 0) {
            request->ctl_watch_kind = CTL_WATCH_WINDOW;
        } else if (strcmp(kind, "output") == 0) {
            request->ctl_watch_kind = CTL_WATCH_OUTPUT;
        } else if (strcmp(kind, "provider") == 0) {
            fprintf(
                stderr,
                "FAIL: watch provider requires a Shell provider event contract\n"
            );
            return false;
        } else if (strcmp(kind, "config") == 0) {
            fprintf(
                stderr,
                "FAIL: watch config event streaming is not part of this foundation slice\n"
            );
            return false;
        } else {
            return false;
        }

        request->command = COMMAND_WATCH_STATE;
        return true;
    }

    if (strcmp(object, "workspace") == 0) {
        if (index >= argc) {
            return false;
        }

        const char *verb = argv[index++];

        if (strcmp(verb, "activate") == 0) {
            if (index + 1 != argc) {
                return false;
            }

            request->command = COMMAND_WORKSPACE_ACTIVATE;
            request->object_id = argv[index];
            request->ctl_namespace = "workspace";
            request->ctl_verb = "activate";
            return request->object_id[0] != '\0';
        }

        if (index != argc) {
            return false;
        }

        if (strcmp(verb, "next") == 0) {
            request->command = COMMAND_NEXT;
        } else if (strcmp(verb, "previous") == 0) {
            request->command = COMMAND_PREVIOUS;
        } else {
            return false;
        }

        request->ctl_namespace = "workspace";
        request->ctl_verb = verb;
        return true;
    }

    if (strcmp(object, "group") == 0) {
        if (index >= argc) {
            return false;
        }

        const char *verb = argv[index++];

        if (strcmp(verb, "focus") == 0) {
            if (index + 1 != argc) {
                return false;
            }

            request->command = COMMAND_GROUP_FOCUS;
            request->object_id = argv[index];
            request->ctl_namespace = "group";
            request->ctl_verb = "focus";
            return request->object_id[0] != '\0';
        }

        if (strcmp(verb, "split") == 0) {
            if (index + 1 != argc ||
                    !parse_orientation(
                        argv[index],
                        &request->orientation)) {
                return false;
            }

            request->command = COMMAND_SPLIT;
            request->ctl_namespace = "group";
            request->ctl_verb = "split";
            request->ctl_name = argv[index];
            return true;
        }

        if (strcmp(verb, "merge") == 0) {
            request->ctl_namespace = "group";
            if (index + 2 != argc) return false;
            request->command = COMMAND_GROUP_MERGE;
            request->object_id = argv[index];
            request->object_id2 = argv[index + 1];
            request->ctl_verb = "merge";
            return request->object_id[0] != '\0' && request->object_id2[0] != '\0';
        }

        if (strcmp(verb, "move-to-workspace") == 0) {
            request->ctl_namespace = "group";
            if (index + 2 != argc) return false;
            request->command = COMMAND_GROUP_MOVE_TO_WORKSPACE;
            request->object_id = argv[index];
            request->object_id2 = argv[index + 1];
            request->ctl_verb = "move-to-workspace";
            return request->object_id[0] != '\0' && request->object_id2[0] != '\0';
        }

        if (strcmp(verb, "split-at") == 0) {
            request->ctl_namespace = "group";
            if (index + 2 != argc ||
                    !parse_orientation(argv[index + 1], &request->orientation)) return false;
            request->command = COMMAND_GROUP_SPLIT_AT;
            request->object_id = argv[index];
            request->ctl_name = argv[index + 1];
            request->ctl_verb = "split-at";
            return request->object_id[0] != '\0';
        }

        if (strcmp(verb, "float") == 0 ||
                strcmp(verb, "tile") == 0 ||
                strcmp(verb, "pin") == 0 ||
                strcmp(verb, "unpin") == 0) {
            if (index + 1 != argc) {
                return false;
            }

            request->object_id = argv[index];
            request->ctl_namespace = "group";
            request->ctl_verb = verb;

            if (strcmp(verb, "float") == 0) {
                request->command = COMMAND_GROUP_FLOAT;
            } else if (strcmp(verb, "tile") == 0) {
                request->command = COMMAND_GROUP_TILE;
            } else if (strcmp(verb, "pin") == 0) {
                request->command = COMMAND_GROUP_PIN;
            } else {
                request->command = COMMAND_GROUP_UNPIN;
            }

            return request->object_id[0] != '\0';
        }

        return false;
    }

    if (strcmp(object, "window") == 0) {
        if (index >= argc) {
            return false;
        }

        const char *verb = argv[index++];

        request->ctl_namespace = "window";
        request->ctl_verb = verb;

        if (strcmp(verb, "focus") == 0) {
            if (index + 1 != argc) {
                return false;
            }

            request->command = COMMAND_WINDOW_FOCUS;
            request->object_id = argv[index];
            return request->object_id[0] != '\0';
        }

        if (strcmp(verb, "move") == 0 ||
                strcmp(verb, "resize") == 0) {
            if (index + 1 != argc ||
                    !parse_direction(
                        argv[index],
                        &request->direction)) {
                return false;
            }

            request->command =
                strcmp(verb, "move") == 0
                    ? COMMAND_MOVE
                    : COMMAND_RESIZE;
            request->ctl_name = argv[index];
            return true;
        }

        if (strcmp(verb, "move-to-group") == 0) {
            if (index + 2 != argc) return false;
            request->command = COMMAND_WINDOW_MOVE_TO_GROUP;
            request->object_id = argv[index];
            request->object_id2 = argv[index + 1];
            return request->object_id[0] != '\0' && request->object_id2[0] != '\0';
        }

        if (strcmp(verb, "split") == 0) {
            if (index + 2 != argc ||
                    !parse_orientation(argv[index + 1], &request->orientation)) return false;
            request->command = COMMAND_WINDOW_SPLIT;
            request->object_id = argv[index];
            request->ctl_name = argv[index + 1];
            return request->object_id[0] != '\0';
        }

        if (strcmp(verb, "move-range") == 0) {
            if (index + 3 != argc) return false;
            request->command = COMMAND_WINDOW_RANGE_MOVE;
            request->object_id = argv[index];
            request->object_id2 = argv[index + 1];
            request->object_id3 = argv[index + 2];
            return request->object_id[0] != '\0' && request->object_id2[0] != '\0' && request->object_id3[0] != '\0';
        }

        if (strcmp(verb, "move-to-workspace") == 0) {
            if (index + 2 != argc) return false;
            request->command = COMMAND_WINDOW_MOVE_TO_WORKSPACE;
            request->object_id = argv[index];
            request->object_id2 = argv[index + 1];
            return request->object_id[0] != '\0' && request->object_id2[0] != '\0';
        }

        if (strcmp(verb, "float") == 0 || strcmp(verb, "tile") == 0) {
            if (index + 1 != argc) return false;
            request->command = strcmp(verb, "float") == 0
                ? COMMAND_WINDOW_FLOAT : COMMAND_WINDOW_TILE;
            request->object_id = argv[index];
            return request->object_id[0] != '\0';
        }

        if (index != argc) {
            return false;
        }

        if (strcmp(verb, "close") == 0) {
            request->command = COMMAND_CLOSE;
        } else if (strcmp(verb, "fullscreen") == 0) {
            request->command = COMMAND_FULLSCREEN;
        } else if (strcmp(verb, "next") == 0) {
            request->command = COMMAND_WINDOW_NEXT;
        } else if (strcmp(verb, "previous") == 0) {
            request->command = COMMAND_WINDOW_PREVIOUS;
        } else {
            return false;
        }

        return true;
    }

    if (strcmp(object, "focus") == 0) {
        if (index + 1 != argc ||
                !parse_direction(
                    argv[index],
                    &request->direction)) {
            return false;
        }

        request->command = COMMAND_FOCUS;
        request->ctl_namespace = "focus";
        request->ctl_verb = argv[index];
        return true;
    }

    if (strcmp(object, "ui") == 0 ||
            strcmp(object, "app") == 0) {
        if (index + 2 != argc ||
                !ctl_name_valid(argv[index]) ||
                !ctl_name_valid(argv[index + 1])) {
            return false;
        }

        request->command = COMMAND_INVOKE;
        request->ctl_namespace = object;
        request->ctl_name = argv[index];
        request->ctl_verb = argv[index + 1];
        return true;
    }

    if (strcmp(object, "invoke") == 0) {
        if (index + 2 != argc) {
            return false;
        }

        request->command = COMMAND_INVOKE;
        request->provider_capability = argv[index];
        request->provider_action = argv[index + 1];
        request->ctl_namespace = "invoke";
        request->ctl_name = argv[index];
        request->ctl_verb = argv[index + 1];
        return true;
    }

    if (strcmp(object, "session") == 0) {
        if (index + 1 != argc ||
                strcmp(argv[index], "logout") != 0) {
            return false;
        }

        request->command = COMMAND_SESSION_EXIT;
        request->ctl_namespace = "session";
        request->ctl_verb = "logout";
        return true;
    }

    return false;
}

static void print_ctl_usage(
        const char *argv0) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s [--json] state\n"
        "  %s [--json] watch state|output|workspace|group|window\n"
        "  %s [--json] workspace activate ID|next|previous\n"
        "  %s [--json] group focus ID | split horizontal|vertical | merge SOURCE TARGET | move-to-workspace GROUP WORKSPACE | split-at WINDOW horizontal|vertical\n"
        "  %s [--json] group float|tile|pin|unpin GROUP\n"
        "  %s [--json] window focus ID|close|fullscreen|next|previous\n"
        "  %s [--json] window move|resize left|right|up|down | move-to-group WINDOW GROUP | split WINDOW horizontal|vertical | move-range FIRST LAST GROUP | move-to-workspace WINDOW WORKSPACE\n"
        "  %s [--json] window float|tile WINDOW\n"
        "  %s [--json] focus left|right|up|down\n"
        "  %s [--json] ui NAME ACTION\n"
        "  %s [--json] app NAME ACTION\n"
        "  %s [--json] invoke CAPABILITY ACTION\n"
        "  %s [--json] session logout\n"
        "  %s [--json] --persist config appearance wallpaper auto|/absolute/path|~/path\n"
        "  %s [--json] --persist config fallback enabled true|false\n"
        "  %s [--json] --persist config fallback title TEXT\n"
        "  %s [--json] --persist config provider NAME priority N\n"
        "  %s [--json] --persist config provider NAME autostart true|false\n"
        "  %s [--json] --persist config provider NAME required true|false\n"
        "\n"
        "runtime actions are runtime-only; config mutation requires explicit --persist\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0
    );
}

static void print_usage(
        const char *argv0) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s daemon [--config path]\n"
        "  %s config-check [--config path]\n"
        "  %s state [--ewwii config-dir|--ewwii-actions config-dir]\n"
        "  %s watch-state [--ewwii config-dir]\n"
        "  %s watch-status\n"
        "  %s invoke capability action\n"
        "  %s launch [--] command [args...]\n"
        "  %s workspace ID\n"
        "  %s group ID\n"
        "  %s window ID\n"
        "  %s window-next\n"
        "  %s window-previous\n"
        "  %s session-exit\n"
        "  %s terminal\n"
        "  %s lock\n"
        "  %s power logout|suspend|reboot|poweroff\n"
        "  %s focus left|right|up|down\n"
        "  %s move left|right|up|down\n"
        "  %s resize left|right|up|down\n"
        "  %s split horizontal|vertical\n"
        "  %s flip\n"
        "  %s fullscreen\n"
        "  %s close\n"
        "  %s next\n"
        "  %s previous\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0
    );
}

static bool parse_request(
        int argc,
        char **argv,
        ShellRequest *request) {
    if (argc < 2) {
        return false;
    }

    if (strcmp(argv[1], "daemon") == 0) {
        request->command =
            COMMAND_DAEMON;

        return parse_config_option(
            argc,
            argv,
            &request->config_path,
            &request->config_explicit
        );
    }

    if (strcmp(argv[1], "config-check") == 0) {
        request->command =
            COMMAND_CONFIG_CHECK;

        return parse_config_option(
            argc,
            argv,
            &request->config_path,
            &request->config_explicit
        );
    }

    if (strcmp(argv[1], "state") == 0) {
        if (argc == 2) {
            request->command =
                COMMAND_STATE;

            return true;
        }

        if (argc == 4 &&
                (strcmp(
                    argv[2],
                    "--ewwii"
                ) == 0 ||
                strcmp(
                    argv[2],
                    "--ewwii-actions"
                ) == 0) &&
                argv[3][0] != '\0') {
            request->command =
                COMMAND_STATE;
            request->ewwii_config_dir =
                argv[3];
            request->state_ewwii_actions =
                strcmp(
                    argv[2],
                    "--ewwii-actions"
                ) == 0;

            return true;
        }

        return false;
    }

    if (strcmp(argv[1], "watch-state") == 0) {
        if (argc == 2) {
            request->command =
                COMMAND_WATCH_STATE;

            return true;
        }

        if (argc == 4 &&
                strcmp(argv[2], "--ewwii") == 0 &&
                argv[3][0] != '\0') {
            request->command =
                COMMAND_WATCH_STATE;
            request->ewwii_config_dir =
                argv[3];

            return true;
        }

        return false;
    }

    if (strcmp(argv[1], "watch-status") == 0) {
        if (argc != 2) {
            return false;
        }

        request->command =
            COMMAND_WATCH_STATUS;

        return true;
    }

    if (strcmp(argv[1], "invoke") == 0) {
        if (argc != 4) {
            return false;
        }

        request->command =
            COMMAND_INVOKE;

        request->provider_capability = argv[2];
        request->provider_action = argv[3];

        return true;
    }

    if (strcmp(argv[1], "launch") == 0) {
        int first = 2;

        if (first < argc &&
                strcmp(argv[first], "--") == 0) {
            first++;
        }

        if (first >= argc) {
            return false;
        }

        request->command =
            COMMAND_LAUNCH;

        request->launch_argv =
            &argv[first];

        return true;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "next") == 0) {
            request->command = COMMAND_NEXT;
        } else if (strcmp(argv[1], "previous") == 0) {
            request->command = COMMAND_PREVIOUS;
        } else if (strcmp(argv[1], "flip") == 0) {
            request->command = COMMAND_FLIP;
        } else if (strcmp(argv[1], "fullscreen") == 0) {
            request->command = COMMAND_FULLSCREEN;
        } else if (strcmp(argv[1], "close") == 0) {
            request->command = COMMAND_CLOSE;
        } else if (strcmp(argv[1], "window-next") == 0) {
            request->command =
                COMMAND_WINDOW_NEXT;
        } else if (strcmp(argv[1], "window-previous") == 0) {
            request->command =
                COMMAND_WINDOW_PREVIOUS;
        } else if (strcmp(argv[1], "session-exit") == 0) {
            request->command =
                COMMAND_SESSION_EXIT;
        } else if (strcmp(argv[1], "terminal") == 0) {
            request->command =
                COMMAND_TERMINAL;
        } else if (strcmp(argv[1], "lock") == 0) {
            request->command =
                COMMAND_LOCK;
        } else {
            return false;
        }

        return true;
    }

    if (argc != 3) {
        return false;
    }

    if (strcmp(argv[1], "power") == 0) {
        if (strcmp(argv[2], "logout") == 0) {
            request->command =
                COMMAND_SESSION_EXIT;
            return true;
        }

        if (strcmp(argv[2], "suspend") != 0 &&
                strcmp(argv[2], "reboot") != 0 &&
                strcmp(argv[2], "poweroff") != 0) {
            return false;
        }

        request->command =
            COMMAND_POWER;
        request->power_action = argv[2];
        return true;
    }

    if (strcmp(argv[1], "workspace") == 0) {
        request->command =
            COMMAND_WORKSPACE_ACTIVATE;
        request->object_id = argv[2];
        return argv[2][0] != '\0';
    }

    if (strcmp(argv[1], "group") == 0) {
        request->command =
            COMMAND_GROUP_FOCUS;
        request->object_id = argv[2];
        return argv[2][0] != '\0';
    }

    if (strcmp(argv[1], "window") == 0) {
        request->command =
            COMMAND_WINDOW_FOCUS;
        request->object_id = argv[2];
        return argv[2][0] != '\0';
    }

    if (strcmp(argv[1], "focus") == 0) {
        request->command = COMMAND_FOCUS;

        return parse_direction(
            argv[2],
            &request->direction
        );
    }

    if (strcmp(argv[1], "move") == 0) {
        request->command = COMMAND_MOVE;

        return parse_direction(
            argv[2],
            &request->direction
        );
    }

    if (strcmp(argv[1], "resize") == 0) {
        request->command = COMMAND_RESIZE;

        return parse_direction(
            argv[2],
            &request->direction
        );
    }

    if (strcmp(argv[1], "split") == 0) {
        request->command = COMMAND_SPLIT;

        return parse_orientation(
            argv[2],
            &request->orientation
        );
    }

    return false;
}

static bool spawn_argv(
        char *const argv[]) {
    pid_t pid = 0;

    const int rc =
        posix_spawnp(
            &pid,
            argv[0],
            NULL,
            NULL,
            argv,
            environ
        );

    if (rc != 0) {
        fprintf(
            stderr,
            "FAIL: spawn %s: %s\n",
            argv[0],
            strerror(rc)
        );

        return false;
    }

    printf(
        "SPAWN pid=%ld command=%s\n",
        (long)pid,
        argv[0]
    );

    return true;
}

static bool terminal_name_valid(
        const char *value) {
    if (!value || value[0] == '\0') {
        return false;
    }

    for (const char *cursor = value;
            *cursor != '\0';
            cursor++) {
        if (*cursor == ' ' ||
                *cursor == '\t' ||
                *cursor == '\n' ||
                *cursor == '\r') {
            return false;
        }
    }

    return true;
}

static bool launch_terminal(void) {
    const char *configured =
        g_getenv("TERMINAL");

    const char *candidates[] = {
        configured,
        "foot",
        "kitty",
        "alacritty",
        "wezterm",
        "kgx",
        "gnome-terminal",
        "konsole",
        "xterm",
    };

    for (size_t i = 0;
            i < G_N_ELEMENTS(candidates);
            i++) {
        const char *candidate =
            candidates[i];

        if (!terminal_name_valid(
                candidate)) {
            continue;
        }

        g_autofree char *path =
            g_find_program_in_path(
                candidate
            );

        if (!path) {
            continue;
        }

        char *const argv[] = {
            path,
            NULL,
        };

        printf(
            "TERMINAL command=%s\n",
            path
        );

        return spawn_argv(argv);
    }

    fprintf(
        stderr,
        "FAIL: no terminal command available\n"
    );

    return false;
}

static bool run_lock_action(void) {
    char *const argv[] = {
        "/usr/bin/hyprlock",
        "--config",
        "/usr/share/onyrion/config/hyprlock.conf",
        "--immediate-render",
        NULL,
    };

    printf(
        "LOCK backend=hyprlock binary=%s config=%s\n",
        argv[0],
        argv[2]
    );

    return spawn_argv(argv);
}

static bool run_power_action(
        const char *action) {
    g_autoptr(GError) error = NULL;

    printf(
        "POWER action=%s backend=logind-dbus\n",
        action
    );

    if (!onyrion_power_action(
                action,
                true,
                &error)) {
        fprintf(
            stderr,
            "FAIL: power action=%s backend=logind-dbus error=%s\n",
            action,
            error ? error->message : "unknown"
        );
        return false;
    }

    return true;
}

static bool launch_application(
        Shell *shell,
        const ShellRequest *request) {
    posix_spawn_file_actions_t actions;

    int rc =
        posix_spawn_file_actions_init(
            &actions
        );

    if (rc != 0) {
        fprintf(
            stderr,
            "FAIL: spawn actions: %s\n",
            strerror(rc)
        );

        return false;
    }

    rc =
        posix_spawn_file_actions_addclose(
            &actions,
            wl_display_get_fd(
                shell->display
            )
        );

    if (rc != 0) {
        posix_spawn_file_actions_destroy(
            &actions
        );

        return false;
    }

    pid_t pid = 0;

    rc =
        posix_spawnp(
            &pid,
            request->launch_argv[0],
            &actions,
            NULL,
            request->launch_argv,
            environ
        );

    posix_spawn_file_actions_destroy(
        &actions
    );

    if (rc != 0) {
        fprintf(
            stderr,
            "FAIL: launch %s: %s\n",
            request->launch_argv[0],
            strerror(rc)
        );

        return false;
    }

    printf(
        "LAUNCH pid=%ld command=%s\n",
        (long)pid,
        request->launch_argv[0]
    );

    return true;
}

static bool send_action(
        Shell *shell,
        const ShellRequest *request) {
    const bool needs_v12_group_control =
        request->command == COMMAND_GROUP_FLOAT ||
        request->command == COMMAND_GROUP_TILE ||
        request->command == COMMAND_GROUP_PIN ||
        request->command == COMMAND_GROUP_UNPIN;

    if (needs_v12_group_control && shell->core_version < 12) {
        fprintf(
            stderr,
            "FAIL: Core protocol v12 required for group placement/pin (negotiated=%u)\n",
            shell->core_version
        );
        return false;
    }

    switch (request->command) {
    case COMMAND_NEXT:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_NEXT;

        onyrion_shell_unstable_v1_workspace_next(
            shell->core
        );
        break;

    case COMMAND_PREVIOUS:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_PREVIOUS;

        onyrion_shell_unstable_v1_workspace_previous(
            shell->core
        );
        break;

    case COMMAND_SPLIT:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_SPLIT_ACTIVE;

        onyrion_shell_unstable_v1_split_active(
            shell->core,
            request->orientation
        );
        break;

    case COMMAND_FOCUS:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_FOCUS_DIRECTION;

        onyrion_shell_unstable_v1_focus_direction(
            shell->core,
            request->direction
        );
        break;

    case COMMAND_MOVE:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_MOVE_DIRECTION;

        onyrion_shell_unstable_v1_move_direction(
            shell->core,
            request->direction
        );
        break;

    case COMMAND_RESIZE:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_RESIZE_DIRECTION;

        onyrion_shell_unstable_v1_resize_direction(
            shell->core,
            request->direction
        );
        break;

    case COMMAND_FLIP:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_FLIP_ACTIVE_SPLIT;

        onyrion_shell_unstable_v1_flip_active_split(
            shell->core
        );
        break;

    case COMMAND_FULLSCREEN:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_TOGGLE_FULLSCREEN_ACTIVE;

        onyrion_shell_unstable_v1_toggle_fullscreen_active(
            shell->core
        );
        break;

    case COMMAND_CLOSE:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_CLOSE_ACTIVE;

        onyrion_shell_unstable_v1_close_active(
            shell->core
        );
        break;

    case COMMAND_WORKSPACE_ACTIVATE:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_WORKSPACE_ACTIVATE;

        onyrion_shell_unstable_v1_activate_workspace(
            shell->core,
            request->object_id
        );
        break;

    case COMMAND_GROUP_FOCUS:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_FOCUS;

        onyrion_shell_unstable_v1_focus_group(
            shell->core,
            request->object_id
        );
        break;

    case COMMAND_WINDOW_FOCUS:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_FOCUS;

        onyrion_shell_unstable_v1_focus_window(
            shell->core,
            request->object_id
        );
        break;

    case COMMAND_WINDOW_NEXT:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_NEXT;

        onyrion_shell_unstable_v1_window_next(
            shell->core
        );
        break;

    case COMMAND_WINDOW_PREVIOUS:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_PREVIOUS;

        onyrion_shell_unstable_v1_window_previous(
            shell->core
        );
        break;

    case COMMAND_GROUP_MERGE:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_MERGE;
        onyrion_shell_unstable_v1_merge_group(shell->core, request->object_id, request->object_id2);
        break;

    case COMMAND_GROUP_MOVE_TO_WORKSPACE:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_MOVE_TO_WORKSPACE;
        onyrion_shell_unstable_v1_move_group_to_workspace(shell->core, request->object_id, request->object_id2);
        break;

    case COMMAND_GROUP_SPLIT_AT:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_SPLIT_AT;
        onyrion_shell_unstable_v1_split_group_at(shell->core, request->object_id, request->orientation);
        break;

    case COMMAND_GROUP_FLOAT:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_FLOAT;
        onyrion_shell_unstable_v1_float_group(shell->core, request->object_id);
        break;

    case COMMAND_GROUP_TILE:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_TILE;
        onyrion_shell_unstable_v1_tile_group(shell->core, request->object_id);
        break;

    case COMMAND_GROUP_PIN:
    case COMMAND_GROUP_UNPIN:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_GROUP_SET_PINNED;
        onyrion_shell_unstable_v1_set_group_pinned(
            shell->core,
            request->object_id,
            request->command == COMMAND_GROUP_PIN ? 1U : 0U);
        break;

    case COMMAND_WINDOW_MOVE_TO_GROUP:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_MOVE_TO_GROUP;
        onyrion_shell_unstable_v1_move_window_to_group(shell->core, request->object_id, request->object_id2);
        break;

    case COMMAND_WINDOW_SPLIT:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_SPLIT;
        onyrion_shell_unstable_v1_split_window(shell->core, request->object_id, request->orientation);
        break;

    case COMMAND_WINDOW_RANGE_MOVE:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_RANGE_MOVE;
        onyrion_shell_unstable_v1_move_window_range(shell->core, request->object_id, request->object_id2, request->object_id3);
        break;

    case COMMAND_WINDOW_MOVE_TO_WORKSPACE:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_MOVE_TO_WORKSPACE;
        onyrion_shell_unstable_v1_move_window_to_workspace(shell->core, request->object_id, request->object_id2);
        break;

    case COMMAND_WINDOW_FLOAT:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_FLOAT;
        onyrion_shell_unstable_v1_float_window(shell->core, request->object_id);
        break;

    case COMMAND_WINDOW_TILE:
        shell->expected_action = ONYRION_SHELL_UNSTABLE_V1_ACTION_WINDOW_TILE;
        onyrion_shell_unstable_v1_tile_window(shell->core, request->object_id);
        break;

    case COMMAND_SESSION_EXIT:
        shell->expected_action =
            ONYRION_SHELL_UNSTABLE_V1_ACTION_SESSION_EXIT;

        onyrion_shell_unstable_v1_session_exit(
            shell->core
        );
        break;

    case COMMAND_DAEMON:
    case COMMAND_CONFIG_CHECK:
    case COMMAND_CONFIG_PERSIST:
    case COMMAND_STATE:
    case COMMAND_WATCH_STATE:
    case COMMAND_WATCH_STATUS:
    case COMMAND_INVOKE:
    case COMMAND_LAUNCH:
    case COMMAND_TERMINAL:
    case COMMAND_LOCK:
    case COMMAND_POWER:
        return false;
    }

    if (wl_display_flush(
            shell->display) < 0 &&
            errno != EAGAIN) {
        return false;
    }

    while (!shell->result_received) {
        if (wl_display_dispatch(
                shell->display) < 0) {
            return false;
        }
    }

    return shell->succeeded;
}

static char *default_config_path(void) {
    return g_build_filename(
        g_get_user_config_dir(),
        "onyrion",
        "shell.kdl",
        NULL
    );
}

static char *persist_config_path(void) {
    const char *explicit_path =
        getenv("ONYRION_SHELL_CONFIG");

    if (explicit_path &&
            explicit_path[0] != '\0') {
        return g_strdup(
            explicit_path
        );
    }

    return default_config_path();
}

static const char *config_edit_scope_name(
        const ShellConfigEditRequest *edit) {
    switch (edit->field) {
    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
        return "appearance";

    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
    case SHELL_CONFIG_EDIT_FALLBACK_TITLE:
        return "fallback";

    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED:
        return "provider";
    }

    return "unknown";
}

static const char *config_edit_field_name(
        const ShellConfigEditRequest *edit) {
    switch (edit->field) {
    case SHELL_CONFIG_EDIT_APPEARANCE_WALLPAPER:
        return "wallpaper";
    case SHELL_CONFIG_EDIT_FALLBACK_ENABLED:
        return "enabled";
    case SHELL_CONFIG_EDIT_FALLBACK_TITLE:
        return "title";
    case SHELL_CONFIG_EDIT_PROVIDER_PRIORITY:
        return "priority";
    case SHELL_CONFIG_EDIT_PROVIDER_AUTOSTART:
        return "autostart";
    case SHELL_CONFIG_EDIT_PROVIDER_REQUIRED:
        return "required";
    }

    return "unknown";
}

static bool ctl_persist_config(
        const ShellRequest *request,
        const char *path) {
    ShellConfigEditResult result = {0};
    GError *error = NULL;

    if (!shell_config_edit_persist(
            path,
            &request->config_edit,
            &result,
            &error)) {
        fprintf(
            stderr,
            "FAIL: persist config %s %s: %s\n",
            config_edit_scope_name(
                &request->config_edit
            ),
            config_edit_field_name(
                &request->config_edit
            ),
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        shell_config_edit_result_finish(
            &result
        );
        return false;
    }

    if (request->ctl_json) {
        JsonBuilder *builder =
            json_builder_new();

        if (!builder) {
            shell_config_edit_result_finish(
                &result
            );
            return false;
        }

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "ok");
        json_builder_add_boolean_value(builder, true);

        json_builder_set_member_name(builder, "transport");
        json_builder_add_string_value(builder, "config");

        json_builder_set_member_name(builder, "persisted");
        json_builder_add_boolean_value(builder, true);

        json_builder_set_member_name(builder, "changed");
        json_builder_add_boolean_value(builder, result.changed);

        json_builder_set_member_name(builder, "scope");
        json_builder_add_string_value(
            builder,
            config_edit_scope_name(
                &request->config_edit
            )
        );

        if (request->config_edit.provider_id) {
            json_builder_set_member_name(builder, "provider");
            json_builder_add_string_value(
                builder,
                request->config_edit.provider_id
            );
        }

        json_builder_set_member_name(builder, "field");
        json_builder_add_string_value(
            builder,
            config_edit_field_name(
                &request->config_edit
            )
        );

        json_builder_set_member_name(builder, "old");
        json_builder_add_string_value(
            builder,
            result.old_value
        );

        json_builder_set_member_name(builder, "value");
        json_builder_add_string_value(
            builder,
            result.new_value
        );

        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(
            builder,
            result.path
        );

        json_builder_end_object(builder);

        JsonGenerator *generator =
            json_generator_new();
        JsonNode *root =
            json_builder_get_root(builder);

        if (!generator ||
                !root) {
            if (root) {
                json_node_free(root);
            }
            if (generator) {
                g_object_unref(generator);
            }
            g_object_unref(builder);
            shell_config_edit_result_finish(
                &result
            );
            return false;
        }

        json_generator_set_root(
            generator,
            root
        );

        g_autofree char *json =
            json_generator_to_data(
                generator,
                NULL
            );

        json_node_free(root);
        g_object_unref(generator);
        g_object_unref(builder);

        if (!json) {
            shell_config_edit_result_finish(
                &result
            );
            return false;
        }

        printf("%s\n", json);
    } else {
        printf(
            "ok persist config %s%s%s %s=%s changed=%u path=%s\n",
            config_edit_scope_name(
                &request->config_edit
            ),
            request->config_edit.provider_id
                ? ":"
                : "",
            request->config_edit.provider_id
                ? request->config_edit.provider_id
                : "",
            config_edit_field_name(
                &request->config_edit
            ),
            result.new_value,
            result.changed ? 1U : 0U,
            result.path
        );
    }

    shell_config_edit_result_finish(
        &result
    );
    return true;
}


static bool load_config(
        const char *path,
        bool allow_missing,
        ShellConfig *config) {
    GError *error = NULL;

    if (!shell_config_load(
            config,
            path,
            allow_missing,
            &error)) {
        fprintf(
            stderr,
            "FAIL: config %s: %s\n",
            path,
            error
                ? error->message
                : "unknown error"
        );

        g_clear_error(&error);
        return false;
    }

    printf(
        "CONFIG path=%s loaded=%u version=%u providers=%zu "
        "fallback=%u\n",
        path,
        config->loaded_from_file ? 1U : 0U,
        config->version,
        config->provider_count,
        config->fallback.enabled ? 1U : 0U
    );

    for (size_t i = 0; i < config->provider_count; i++) {
        const ProviderConfig *provider = &config->providers[i];

        printf(
            "PROVIDER id=%s type=%s priority=%d "
            "autostart=%u required=%u capabilities=%zu "
            "restart=%s restart_delay_ms=%u "
            "restart_max_delay_ms=%u health=%s\n",
            provider->id,
            provider->type,
            provider->priority,
            provider->autostart ? 1U : 0U,
            provider->required ? 1U : 0U,
            provider->capability_count,
            provider_restart_policy_name(provider->restart_policy),
            provider->restart_delay_ms,
            provider->restart_max_delay_ms,
            provider_health_mode_name(provider->health_mode)
        );

        if (provider->health_mode == PROVIDER_HEALTH_COMMAND) {
            printf(
                "HEALTH provider=%s argc=%zu interval_ms=%u "
                "timeout_ms=%u startup_timeout_ms=%u\n",
                provider->id,
                provider->health_argc,
                provider->health_interval_ms,
                provider->health_timeout_ms,
                provider->health_startup_timeout_ms
            );
        }

        for (size_t j = 0; j < provider->capability_count; j++) {
            const ProviderCapabilityConfig *capability = &provider->capabilities[j];
            printf(
                "CAPABILITY provider=%s id=%s actions=%zu\n",
                provider->id,
                capability->id,
                capability->action_count
            );

            for (size_t k = 0; k < capability->action_count; k++) {
                printf(
                    "ACTION provider=%s capability=%s action=%s argc=%zu\n",
                    provider->id,
                    capability->id,
                    capability->actions[k].name,
                    capability->actions[k].argc
                );
            }
        }
    }

    return true;
}

static bool run_config_check(
        const char *path,
        bool allow_missing) {
    ShellConfig config;
    shell_config_init(&config);

    const bool success = load_config(path, allow_missing, &config);

    if (success) {
        for (size_t i = 0; i < config.provider_count; i++) {
            const ProviderConfig *provider = &config.providers[i];
            for (size_t j = 0; j < provider->capability_count; j++) {
                const ProviderCapabilityConfig *capability = &provider->capabilities[j];
                for (size_t k = 0; k < capability->action_count; k++) {
                    const char *action = capability->actions[k].name;
                    const ProviderConfig *resolved =
                        shell_config_provider_for_capability_action(
                            &config, capability->id, action
                        );
                    if (resolved) {
                        printf(
                            "RESOLVE capability=%s action=%s provider=%s priority=%d\n",
                            capability->id, action, resolved->id, resolved->priority
                        );
                    }
                }
            }
        }
        printf("PASS: config model valid\n");
    }

    shell_config_finish(&config);
    return success;
}

static gboolean handle_wayland_ready(
        gint fd,
        GIOCondition condition,
        gpointer data) {
    (void)fd;

    Shell *shell = data;

    if (condition &
            (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        fprintf(
            stderr,
            "FAIL: Wayland connection condition=0x%x\n",
            (unsigned)condition
        );

        shell->protocol_failed = true;
        g_main_loop_quit(shell->loop);

        return G_SOURCE_CONTINUE;
    }

    if (condition & G_IO_IN) {
        if (wl_display_dispatch(
                shell->display) < 0) {
            fprintf(
                stderr,
                "FAIL: Wayland dispatch: %s\n",
                strerror(errno)
            );

            shell->protocol_failed = true;
            g_main_loop_quit(shell->loop);
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean handle_shutdown_signal(
        gpointer data) {
    Shell *shell = data;

    g_main_loop_quit(shell->loop);

    return G_SOURCE_CONTINUE;
}

static char *control_socket_path(void) {
    const char *runtime =
        g_get_user_runtime_dir();

    if (!runtime || runtime[0] == '\0') {
        return NULL;
    }

    return g_build_filename(
        runtime,
        "onyrion-shell",
        "control.sock",
        NULL
    );
}

static bool valid_control_token(
        const char *value) {
    if (!value || value[0] == '\0') {
        return false;
    }

    for (const unsigned char *cursor =
            (const unsigned char *)value;
            *cursor;
            cursor++) {
        const unsigned char c = *cursor;

        if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == ':' ||
                c == '_' ||
                c == '-') {
            continue;
        }

        return false;
    }

    return true;
}

static bool write_all(
        int fd,
        const char *data) {
    const size_t length = strlen(data);
    size_t offset = 0;

    while (offset < length) {
        const ssize_t written =
            write(
                fd,
                data + offset,
                length - offset
            );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        offset += (size_t)written;
    }

    return true;
}

static bool handle_control_client(
        Shell *shell,
        int client_fd) {
    char buffer[1024];

    const ssize_t length =
        read(
            client_fd,
            buffer,
            sizeof(buffer) - 1
        );

    if (length <= 0) {
        return false;
    }

    buffer[length] = '\0';

    char *newline = strchr(buffer, '\n');

    if (!newline) {
        (void)write_all(
            client_fd,
            "ERR incomplete\n"
        );
        return false;
    }

    *newline = '\0';

    if (strcmp(buffer, "STATE") == 0) {
        const ShellSnapshot *snapshot =
            shell_state_snapshot(
                &shell->state
            );

        if (snapshot->generation == 0) {
            (void)write_all(
                client_fd,
                "ERR\tstate unavailable\n"
            );
            return false;
        }

        g_autofree char *json =
            snapshot_to_json(
                snapshot
            );

        if (!json) {
            (void)write_all(
                client_fd,
                "ERR\tstate serialization failed\n"
            );
            return false;
        }

        g_autofree char *response =
            g_strconcat(
                "OK\t",
                json,
                "\n",
                NULL
            );

        if (!response) {
            (void)write_all(
                client_fd,
                "ERR\tstate serialization failed\n"
            );
            return false;
        }

        (void)write_all(
            client_fd,
            response
        );

        printf(
            "CONTROL STATE generation=%u workspaces=%zu\n",
            snapshot->generation,
            snapshot->workspace_count
        );
        fflush(stdout);

        return false;
    }

    static const char watch_prefix[] =
        "WATCH_STATE\t";

    if (g_str_has_prefix(
            buffer,
            watch_prefix)) {
        const char *raw_generation =
            buffer +
            strlen(watch_prefix);

        errno = 0;
        char *end = NULL;

        const guint64 parsed_generation =
            g_ascii_strtoull(
                raw_generation,
                &end,
                10
            );

        if (errno != 0 ||
                end == raw_generation ||
                !end ||
                *end != '\0' ||
                parsed_generation > UINT32_MAX) {
            (void)write_all(
                client_fd,
                "ERR\tinvalid generation\n"
            );
            return false;
        }

        const uint32_t after_generation =
            (uint32_t)parsed_generation;

        const ShellSnapshot *snapshot =
            shell_state_snapshot(
                &shell->state
            );

        if (snapshot->generation >
                after_generation) {
            const bool sent =
                send_state_waiter_response(
                    client_fd,
                    snapshot
                );

            printf(
                "CONTROL WATCH_STATE %s generation=%u subscribers=%u\n",
                sent ? "IMMEDIATE" : "DROP",
                snapshot->generation,
                shell->state_waiters
                    ? shell->state_waiters->len
                    : 0U
            );
            fflush(stdout);

            return false;
        }

        if (!state_waiter_store(
                shell,
                client_fd,
                after_generation)) {
            (void)write_all(
                client_fd,
                "ERR\tcannot retain state waiter\n"
            );
            return false;
        }

        printf(
            "CONTROL WATCH_STATE WAIT after=%u subscribers=%u\n",
            after_generation,
            shell->state_waiters->len
        );
        fflush(stdout);

        return true;
    }

    char **parts =
        g_strsplit(
            buffer,
            "\t",
            0
        );

    if (!parts ||
            g_strv_length(parts) != 3 ||
            strcmp(parts[0], "INVOKE") != 0 ||
            !valid_control_token(parts[1]) ||
            !valid_control_token(parts[2])) {
        (void)write_all(
            client_fd,
            "ERR request\n"
        );

        g_strfreev(parts);
        return false;
    }

    GError *error = NULL;
    const char *provider_label = "-";

    const bool invoked =
        shell_invoke_action(
            shell,
            parts[1],
            parts[2],
            &provider_label,
            &error
        );

    if (invoked) {
        g_autofree char *response =
            g_strdup_printf(
                "OK\t%s\n",
                provider_label
            );

        (void)write_all(
            client_fd,
            response
        );
    } else {
        g_autofree char *response =
            g_strdup_printf(
                "ERR\t%s\n",
                error
                    ? error->message
                    : "invoke failed"
            );

        (void)write_all(
            client_fd,
            response
        );
    }

    g_clear_error(&error);
    g_strfreev(parts);

    return false;
}

static gboolean handle_control_ready(
        gint fd,
        GIOCondition condition,
        gpointer data) {
    Shell *shell = data;

    if (condition &
            (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        fprintf(
            stderr,
            "CONTROL FAIL condition=0x%x\n",
            (unsigned)condition
        );

        shell->protocol_failed = true;
        g_main_loop_quit(shell->loop);
        return G_SOURCE_CONTINUE;
    }

    if (!(condition & G_IO_IN)) {
        return G_SOURCE_CONTINUE;
    }

    const int client_fd =
        accept(
            fd,
            NULL,
            NULL
        );

    if (client_fd < 0) {
        if (errno != EINTR && errno != EAGAIN) {
            fprintf(
                stderr,
                "CONTROL FAIL accept=%s\n",
                strerror(errno)
            );
        }

        return G_SOURCE_CONTINUE;
    }

    const bool retained =
        handle_control_client(
            shell,
            client_fd
        );

    if (!retained) {
        close(client_fd);
    }

    return G_SOURCE_CONTINUE;
}

static void control_socket_cleanup_partial(
        Shell *shell) {
    if (shell->control_fd >= 0) {
        close(shell->control_fd);
        shell->control_fd = -1;
    }

    if (shell->control_path) {
        (void)unlink(shell->control_path);
    }
}

static bool prepare_control_socket_path(
        const char *path) {
    struct stat status;

    if (lstat(path, &status) < 0) {
        return errno == ENOENT;
    }

    if (!S_ISSOCK(status.st_mode)) {
        fprintf(
            stderr,
            "FAIL: control path exists and is not a socket: %s\n",
            path
        );
        return false;
    }

    const int probe_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (probe_fd < 0) {
        return false;
    }

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };

    if (strlen(path) >= sizeof(address.sun_path)) {
        close(probe_fd);
        return false;
    }

    strcpy(address.sun_path, path);

    if (connect(
            probe_fd,
            (const struct sockaddr *)&address,
            sizeof(address)) == 0) {
        close(probe_fd);
        fprintf(
            stderr,
            "FAIL: another Onyrion shell control socket is active\n"
        );
        return false;
    }

    const int connect_errno = errno;
    close(probe_fd);

    if (connect_errno != ECONNREFUSED &&
            connect_errno != ENOENT) {
        fprintf(
            stderr,
            "FAIL: cannot validate existing control socket: %s\n",
            strerror(connect_errno)
        );
        return false;
    }

    if (unlink(path) < 0 && errno != ENOENT) {
        fprintf(
            stderr,
            "FAIL: cannot remove stale control socket: %s\n",
            strerror(errno)
        );
        return false;
    }

    return true;
}

static bool control_server_start(
        Shell *shell,
        guint *source_id) {
    shell->control_path = control_socket_path();

    if (!shell->control_path) {
        fprintf(
            stderr,
            "FAIL: cannot construct control socket path\n"
        );
        return false;
    }

    g_autofree char *directory =
        g_path_get_dirname(
            shell->control_path
        );

    if (g_mkdir_with_parents(
            directory,
            0700) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot create control directory: %s\n",
            strerror(errno)
        );
        return false;
    }

    if (chmod(directory, 0700) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot secure control directory: %s\n",
            strerror(errno)
        );
        return false;
    }

    if (!prepare_control_socket_path(shell->control_path)) {
        return false;
    }

    shell->control_fd =
        socket(
            AF_UNIX,
            SOCK_STREAM,
            0
        );

    if (shell->control_fd < 0) {
        fprintf(
            stderr,
            "FAIL: cannot create control socket: %s\n",
            strerror(errno)
        );
        return false;
    }

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };

    if (strlen(shell->control_path) >= sizeof(address.sun_path)) {
        fprintf(
            stderr,
            "FAIL: control socket path too long\n"
        );
        control_socket_cleanup_partial(shell);
        return false;
    }

    strcpy(address.sun_path, shell->control_path);

    if (bind(
            shell->control_fd,
            (const struct sockaddr *)&address,
            sizeof(address)) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot bind control socket: %s\n",
            strerror(errno)
        );
        control_socket_cleanup_partial(shell);
        return false;
    }

    if (listen(shell->control_fd, 16) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot listen on control socket: %s\n",
            strerror(errno)
        );
        control_socket_cleanup_partial(shell);
        return false;
    }

    *source_id =
        g_unix_fd_add(
            shell->control_fd,
            G_IO_IN |
                G_IO_ERR |
                G_IO_HUP |
                G_IO_NVAL,
            handle_control_ready,
            shell
        );

    if (*source_id == 0) {
        fprintf(
            stderr,
            "FAIL: cannot register control socket source\n"
        );
        control_socket_cleanup_partial(shell);
        return false;
    }

    printf(
        "CONTROL READY path=%s\n",
        shell->control_path
    );
    fflush(stdout);

    return true;
}

static void control_server_stop(
        Shell *shell,
        guint source_id) {
    state_waiters_finish(shell);

    if (source_id != 0) {
        g_source_remove(source_id);
    }

    if (shell->control_fd >= 0) {
        close(shell->control_fd);
        shell->control_fd = -1;
    }

    if (shell->control_path) {
        (void)unlink(shell->control_path);
        g_free(shell->control_path);
        shell->control_path = NULL;
    }
}

static bool query_daemon_state(
        const char *ewwii_dir,
        bool actions_only) {
    g_autofree char *path =
        control_socket_path();

    if (!path) {
        fprintf(
            stderr,
            "FAIL: cannot construct control socket path\n"
        );
        return false;
    }

    const int fd =
        socket(
            AF_UNIX,
            SOCK_STREAM,
            0
        );

    if (fd < 0) {
        fprintf(
            stderr,
            "FAIL: cannot create control client socket: %s\n",
            strerror(errno)
        );
        return false;
    }

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };

    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        fprintf(
            stderr,
            "FAIL: control socket path too long\n"
        );
        return false;
    }

    strcpy(
        address.sun_path,
        path
    );

    if (connect(
            fd,
            (const struct sockaddr *)&address,
            sizeof(address)) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot connect to shell daemon: %s\n",
            strerror(errno)
        );
        close(fd);
        return false;
    }

    if (!write_all(
            fd,
            "STATE\n"
        )) {
        fprintf(
            stderr,
            "FAIL: cannot send state request\n"
        );
        close(fd);
        return false;
    }

    char response[16384];
    const ssize_t length =
        read(
            fd,
            response,
            sizeof(response) - 1
        );

    close(fd);

    if (length <= 0) {
        fprintf(
            stderr,
            "FAIL: empty state response\n"
        );
        return false;
    }

    response[length] = '\0';

    if (!g_str_has_prefix(
            response,
            "OK\t")) {
        fprintf(
            stderr,
            "FAIL: state %s",
            response
        );
        return false;
    }

    const char *json =
        response + 3;

    if (ewwii_dir) {
        if (actions_only) {
            return ui_actions_v11_sync_apply(
                ewwii_dir,
                json
            );
        }

        GError *projection_error = NULL;
        g_autofree char *projected =
            widget_block_project_json(
                "bar",
                json,
                &projection_error
            );

        if (!projected) {
            fprintf(
                stderr,
                "FAIL: cannot project Ewwii bar bootstrap state: %s\n",
                projection_error
                    ? projection_error->message
                    : "unknown"
            );
            g_clear_error(&projection_error);
            return false;
        }

        const bool reconciled =
            ewwii_adapter_reconcile_bar_roots(
                ewwii_dir,
                projected
            );
        g_clear_error(&projection_error);
        return reconciled;
    }

    fputs(
        json,
        stdout
    );

    return true;
}

static bool read_control_line(
        int fd,
        GString *line) {
    char buffer[4096];

    while (!strchr(line->str, '\n')) {
        const ssize_t length =
            read(
                fd,
                buffer,
                sizeof(buffer)
            );

        if (length == 0) {
            errno = 0;
            return false;
        }

        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        g_string_append_len(
            line,
            buffer,
            length
        );

        if (line->len > 1024U * 1024U) {
            errno = EMSGSIZE;
            return false;
        }
    }

    return true;
}

static int ctl_control_connect(
        const char *purpose) {
    g_autofree char *path =
        control_socket_path();

    if (!path) {
        fprintf(
            stderr,
            "FAIL: cannot construct control socket path\n"
        );
        return -1;
    }

    const int fd =
        socket(
            AF_UNIX,
            SOCK_STREAM,
            0
        );

    if (fd < 0) {
        fprintf(
            stderr,
            "FAIL: cannot create %s socket: %s\n",
            purpose,
            strerror(errno)
        );
        return -1;
    }

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };

    if (strlen(path) >= sizeof(address.sun_path)) {
        fprintf(
            stderr,
            "FAIL: control socket path too long\n"
        );
        close(fd);
        return -1;
    }

    strcpy(
        address.sun_path,
        path
    );

    if (connect(
            fd,
            (const struct sockaddr *)&address,
            sizeof(address)) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot connect %s to Shell daemon: %s\n",
            purpose,
            strerror(errno)
        );
        close(fd);
        return -1;
    }

    return fd;
}

static bool ctl_parse_state_json(
        const char *json,
        JsonParser **parser_out,
        JsonObject **root_out) {
    JsonParser *parser =
        json_parser_new();

    if (!parser) {
        return false;
    }

    GError *error = NULL;

    if (!json_parser_load_from_data(
            parser,
            json,
            -1,
            &error)) {
        fprintf(
            stderr,
            "FAIL: invalid Shell state JSON: %s\n",
            error
                ? error->message
                : "unknown error"
        );
        g_clear_error(&error);
        g_object_unref(parser);
        return false;
    }

    JsonNode *root =
        json_parser_get_root(parser);

    if (!root ||
            !JSON_NODE_HOLDS_OBJECT(root)) {
        fprintf(
            stderr,
            "FAIL: Shell state JSON root is not an object\n"
        );
        g_object_unref(parser);
        return false;
    }

    *parser_out = parser;
    *root_out =
        json_node_get_object(root);
    return true;
}

static const char *ctl_watch_member(
        CtlWatchKind kind) {
    switch (kind) {
    case CTL_WATCH_WORKSPACE:
        return "workspaces";
    case CTL_WATCH_GROUP:
        return "groups";
    case CTL_WATCH_WINDOW:
        return "windows";
    case CTL_WATCH_OUTPUT:
        return "outputs";
    case CTL_WATCH_STATE:
        return NULL;
    }

    return NULL;
}

static const char *ctl_watch_name(
        CtlWatchKind kind) {
    switch (kind) {
    case CTL_WATCH_WORKSPACE:
        return "workspace";
    case CTL_WATCH_GROUP:
        return "group";
    case CTL_WATCH_WINDOW:
        return "window";
    case CTL_WATCH_OUTPUT:
        return "output";
    case CTL_WATCH_STATE:
        return "state";
    }

    return "state";
}

static bool ctl_emit_filtered_json(
        JsonObject *root,
        CtlWatchKind kind) {
    const char *member =
        ctl_watch_member(kind);

    if (!member) {
        return false;
    }

    if (!json_object_has_member(
            root,
            "generation") ||
            !json_object_has_member(
                root,
                member)) {
        return false;
    }

    JsonArray *items =
        json_object_get_array_member(
            root,
            member
        );

    if (!items) {
        return false;
    }

    JsonBuilder *builder =
        json_builder_new();

    if (!builder) {
        return false;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(
        builder,
        "type"
    );
    json_builder_add_string_value(
        builder,
        ctl_watch_name(kind)
    );

    json_builder_set_member_name(
        builder,
        "generation"
    );
    json_builder_add_int_value(
        builder,
        json_object_get_int_member(
            root,
            "generation"
        )
    );

    json_builder_set_member_name(
        builder,
        "items"
    );
    json_builder_begin_array(builder);

    for (guint i = 0;
            i < json_array_get_length(items);
            i++) {
        JsonNode *item =
            json_array_get_element(
                items,
                i
            );

        JsonNode *copy =
            item
                ? json_node_copy(item)
                : NULL;

        if (!copy) {
            g_object_unref(builder);
            return false;
        }

        json_builder_add_value(
            builder,
            copy
        );
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonNode *node =
        json_builder_get_root(builder);
    JsonGenerator *generator =
        json_generator_new();

    if (!node || !generator) {
        if (node) {
            json_node_free(node);
        }

        if (generator) {
            g_object_unref(generator);
        }

        g_object_unref(builder);
        return false;
    }

    json_generator_set_root(
        generator,
        node
    );

    g_autofree char *data =
        json_generator_to_data(
            generator,
            NULL
        );

    if (!data) {
        g_object_unref(generator);
        json_node_free(node);
        g_object_unref(builder);
        return false;
    }

    printf("%s\n", data);
    fflush(stdout);

    g_object_unref(generator);
    json_node_free(node);
    g_object_unref(builder);
    return true;
}

static bool ctl_emit_human_state(
        JsonObject *root,
        CtlWatchKind kind,
        bool watch) {
    if (!json_object_has_member(
            root,
            "generation")) {
        return false;
    }

    const gint64 generation =
        json_object_get_int_member(
            root,
            "generation"
        );

    if (kind == CTL_WATCH_STATE) {
        JsonArray *workspaces =
            json_object_get_array_member(
                root,
                "workspaces"
            );
        JsonArray *groups =
            json_object_get_array_member(
                root,
                "groups"
            );
        JsonArray *windows =
            json_object_get_array_member(
                root,
                "windows"
            );

        if (!workspaces ||
                !groups ||
                !windows) {
            return false;
        }

        printf(
            "%s state generation=%lld workspaces=%u groups=%u windows=%u\n",
            watch ? "event" : "current",
            (long long)generation,
            json_array_get_length(workspaces),
            json_array_get_length(groups),
            json_array_get_length(windows)
        );

        return true;
    }

    const char *member =
        ctl_watch_member(kind);
    JsonArray *items =
        member
            ? json_object_get_array_member(
                root,
                member
            )
            : NULL;

    if (!items) {
        return false;
    }

    printf(
        "%s %s generation=%lld count=%u\n",
        watch ? "event" : "current",
        ctl_watch_name(kind),
        (long long)generation,
        json_array_get_length(items)
    );

    for (guint i = 0;
            i < json_array_get_length(items);
            i++) {
        JsonObject *item =
            json_array_get_object_element(
                items,
                i
            );

        if (!item) {
            return false;
        }

        if (kind == CTL_WATCH_WORKSPACE) {
            printf(
                "  workspace %s%s groups=%lld tiles=%lld windows=%lld\n",
                json_object_get_string_member(item, "id"),
                json_object_get_boolean_member(item, "active")
                    ? " active"
                    : "",
                (long long)json_object_get_int_member(item, "groups"),
                (long long)json_object_get_int_member(item, "tiles"),
                (long long)json_object_get_int_member(item, "windows")
            );
        } else if (kind == CTL_WATCH_GROUP) {
            printf(
                "  group %s workspace=%s%s windows=%lld\n",
                json_object_get_string_member(item, "id"),
                json_object_get_string_member(item, "workspace_id"),
                json_object_get_boolean_member(item, "active")
                    ? " active"
                    : "",
                (long long)json_object_get_int_member(item, "window_count")
            );
        } else if (kind == CTL_WATCH_WINDOW) {
            printf(
                "  window %s group=%s%s%s app=%s title=%s\n",
                json_object_get_string_member(item, "id"),
                json_object_get_string_member(item, "group_id"),
                json_object_get_boolean_member(item, "active")
                    ? " active"
                    : "",
                json_object_get_boolean_member(item, "fullscreen")
                    ? " fullscreen"
                    : "",
                json_object_get_string_member(item, "app_id"),
                json_object_get_string_member(item, "title")
            );
        }
    }

    fflush(stdout);
    return true;
}

static bool ctl_emit_state_payload(
        const char *json,
        CtlWatchKind kind,
        bool json_output,
        bool watch) {
    JsonParser *parser = NULL;
    JsonObject *root = NULL;

    if (!ctl_parse_state_json(
            json,
            &parser,
            &root)) {
        return false;
    }

    bool ok = false;

    if (json_output) {
        if (kind == CTL_WATCH_STATE) {
            printf("%s\n", json);
            fflush(stdout);
            ok = true;
        } else {
            ok =
                ctl_emit_filtered_json(
                    root,
                    kind
                );
        }
    } else {
        ok =
            ctl_emit_human_state(
                root,
                kind,
                watch
            );
    }

    g_object_unref(parser);
    return ok;
}

static bool ctl_query_state(
        bool json_output) {
    const int fd =
        ctl_control_connect(
            "onyrionctl state"
        );

    if (fd < 0) {
        return false;
    }

    if (!write_all(
            fd,
            "STATE\n"
        )) {
        fprintf(
            stderr,
            "FAIL: cannot send state request\n"
        );
        close(fd);
        return false;
    }

    GString *line =
        g_string_new(NULL);

    if (!line) {
        close(fd);
        return false;
    }

    errno = 0;

    const bool read_ok =
        read_control_line(
            fd,
            line
        );

    const int saved_errno = errno;
    close(fd);

    if (!read_ok) {
        fprintf(
            stderr,
            "FAIL: state response ended: %s\n",
            saved_errno
                ? strerror(saved_errno)
                : "EOF"
        );
        g_string_free(line, true);
        return false;
    }

    char *newline =
        strchr(
            line->str,
            '\n'
        );

    if (!newline) {
        g_string_free(line, true);
        return false;
    }

    *newline = '\0';

    if (!g_str_has_prefix(
            line->str,
            "OK\t")) {
        fprintf(
            stderr,
            "FAIL: state %s\n",
            line->str
        );
        g_string_free(line, true);
        return false;
    }

    const bool ok =
        ctl_emit_state_payload(
            line->str + 3,
            CTL_WATCH_STATE,
            json_output,
            false
        );

    g_string_free(line, true);
    return ok;
}

static bool ctl_watch_state(
        CtlWatchKind kind,
        bool json_output) {
    uint32_t generation = 0;
    bool received_state = false;
    guint startup_retries = 0;

    for (;;) {
        const int fd =
            ctl_control_connect(
                "onyrionctl watch"
            );

        if (fd < 0) {
            if (!received_state &&
                    startup_retries < 100U) {
                startup_retries++;
                g_usleep(50U * 1000U);
                continue;
            }

            return false;
        }

        startup_retries = 0;

        g_autofree char *request =
            g_strdup_printf(
                "WATCH_STATE\t%u\n",
                generation
            );

        if (!request ||
                !write_all(
                    fd,
                    request
                )) {
            close(fd);
            return false;
        }

        GString *line =
            g_string_new(NULL);

        if (!line) {
            close(fd);
            return false;
        }

        errno = 0;

        if (!read_control_line(
                fd,
                line)) {
            const int saved_errno = errno;
            close(fd);
            g_string_free(line, true);

            if (received_state &&
                    saved_errno == 0) {
                return true;
            }

            fprintf(
                stderr,
                "FAIL: watch response ended: %s\n",
                saved_errno
                    ? strerror(saved_errno)
                    : "EOF"
            );
            return false;
        }

        close(fd);

        char *newline =
            strchr(
                line->str,
                '\n'
            );

        if (!newline) {
            g_string_free(line, true);
            return false;
        }

        *newline = '\0';

        char **parts =
            g_strsplit(
                line->str,
                "\t",
                3
            );

        if (!parts ||
                g_strv_length(parts) != 3 ||
                strcmp(parts[0], "STATE") != 0) {
            fprintf(
                stderr,
                "FAIL: invalid watch response: %s\n",
                line->str
            );
            g_strfreev(parts);
            g_string_free(line, true);
            return false;
        }

        errno = 0;
        char *end = NULL;

        const guint64 parsed_generation =
            g_ascii_strtoull(
                parts[1],
                &end,
                10
            );

        if (errno != 0 ||
                end == parts[1] ||
                !end ||
                *end != '\0' ||
                parsed_generation > UINT32_MAX ||
                parsed_generation <= generation) {
            g_strfreev(parts);
            g_string_free(line, true);
            return false;
        }

        generation =
            (uint32_t)parsed_generation;

        const bool emitted =
            ctl_emit_state_payload(
                parts[2],
                kind,
                json_output,
                true
            );

        g_strfreev(parts);
        g_string_free(line, true);

        if (!emitted) {
            return false;
        }

        received_state = true;
    }
}

static bool ctl_invoke(
        const ShellRequest *request) {
    g_autofree char *capability = NULL;

    if (request->provider_capability) {
        capability =
            g_strdup(
                request->provider_capability
            );
    } else if (request->ctl_namespace &&
            request->ctl_name) {
        capability =
            g_strdup_printf(
                "%s:%s",
                request->ctl_namespace,
                request->ctl_name
            );
    }

    const char *action =
        request->provider_action
            ? request->provider_action
            : request->ctl_verb;

    if (!capability ||
            !valid_control_token(capability) ||
            !valid_control_token(action)) {
        fprintf(
            stderr,
            "FAIL: invalid capability/action\n"
        );
        return false;
    }

    const int fd =
        ctl_control_connect(
            "onyrionctl invoke"
        );

    if (fd < 0) {
        return false;
    }

    g_autofree char *wire =
        g_strdup_printf(
            "INVOKE\t%s\t%s\n",
            capability,
            action
        );

    if (!wire ||
            !write_all(
                fd,
                wire
            )) {
        close(fd);
        return false;
    }

    GString *line =
        g_string_new(NULL);

    if (!line) {
        close(fd);
        return false;
    }

    errno = 0;

    const bool read_ok =
        read_control_line(
            fd,
            line
        );

    const int saved_errno = errno;
    close(fd);

    if (!read_ok) {
        fprintf(
            stderr,
            "FAIL: invoke response ended: %s\n",
            saved_errno
                ? strerror(saved_errno)
                : "EOF"
        );
        g_string_free(line, true);
        return false;
    }

    char *newline =
        strchr(
            line->str,
            '\n'
        );

    if (newline) {
        *newline = '\0';
    }

    if (g_str_has_prefix(
            line->str,
            "OK\t")) {
        const char *provider =
            line->str + 3;

        if (request->ctl_json) {
            JsonBuilder *builder =
                json_builder_new();
            JsonGenerator *generator =
                json_generator_new();

            if (!builder ||
                    !generator) {
                if (builder) {
                    g_object_unref(builder);
                }

                if (generator) {
                    g_object_unref(generator);
                }

                g_string_free(line, true);
                return false;
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "ok");
            json_builder_add_boolean_value(builder, true);
            json_builder_set_member_name(builder, "transport");
            json_builder_add_string_value(builder, "shell");
            json_builder_set_member_name(builder, "capability");
            json_builder_add_string_value(builder, capability);
            json_builder_set_member_name(builder, "action");
            json_builder_add_string_value(builder, action);
            json_builder_set_member_name(builder, "provider");
            json_builder_add_string_value(builder, provider);
            json_builder_end_object(builder);

            JsonNode *root =
                json_builder_get_root(builder);

            json_generator_set_root(
                generator,
                root
            );

            g_autofree char *json =
                json_generator_to_data(
                    generator,
                    NULL
                );

            if (json) {
                printf("%s\n", json);
            }

            json_node_free(root);
            g_object_unref(generator);
            g_object_unref(builder);

            if (!json) {
                g_string_free(line, true);
                return false;
            }
        } else {
            printf(
                "ok %s %s provider=%s transport=shell\n",
                capability,
                action,
                provider
            );
        }

        fflush(stdout);
        g_string_free(line, true);
        return true;
    }

    fprintf(
        stderr,
        "FAIL: invoke %s\n",
        line->str
    );
    g_string_free(line, true);
    return false;
}

static bool ctl_emit_core_result(
        const ShellRequest *request,
        bool success) {
    const char *scope =
        request->ctl_namespace
            ? request->ctl_namespace
            : "core";
    const char *verb =
        request->ctl_verb
            ? request->ctl_verb
            : "action";

    if (request->ctl_json) {
        JsonBuilder *builder =
            json_builder_new();
        JsonGenerator *generator =
            json_generator_new();

        if (!builder ||
                !generator) {
            if (builder) {
                g_object_unref(builder);
            }

            if (generator) {
                g_object_unref(generator);
            }

            return false;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "ok");
        json_builder_add_boolean_value(builder, success);
        json_builder_set_member_name(builder, "transport");
        json_builder_add_string_value(builder, "core");
        json_builder_set_member_name(builder, "scope");
        json_builder_add_string_value(builder, scope);
        json_builder_set_member_name(builder, "action");
        json_builder_add_string_value(builder, verb);

        if (request->object_id) {
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(
                builder,
                request->object_id
            );
        }

        if (request->command == COMMAND_WINDOW_RANGE_MOVE) {
            json_builder_set_member_name(builder, "last_id");
            json_builder_add_string_value(builder, request->object_id2);
            json_builder_set_member_name(builder, "target_id");
            json_builder_add_string_value(builder, request->object_id3);
        } else if (request->object_id2) {
            json_builder_set_member_name(builder, "target_id");
            json_builder_add_string_value(builder, request->object_id2);
        }

        if (request->ctl_name) {
            json_builder_set_member_name(builder, "value");
            json_builder_add_string_value(
                builder,
                request->ctl_name
            );
        }

        json_builder_end_object(builder);

        JsonNode *root =
            json_builder_get_root(builder);

        json_generator_set_root(
            generator,
            root
        );

        g_autofree char *json =
            json_generator_to_data(
                generator,
                NULL
            );

        if (json) {
            printf("%s\n", json);
        }

        json_node_free(root);
        g_object_unref(generator);
        g_object_unref(builder);

        if (!json) {
            return false;
        }
    } else {
        printf(
            "%s %s %s",
            success ? "ok" : "failed",
            scope,
            verb
        );

        if (request->object_id) {
            printf(
                " id=%s",
                request->object_id
            );
        }

        if (request->ctl_name) {
            printf(
                " value=%s",
                request->ctl_name
            );
        }

        printf(" transport=core\n");
    }

    fflush(stdout);
    return true;
}

static bool watch_daemon_state(
        const char *ewwii_config_dir) {
    uint32_t generation = 0;
    bool received_state = false;
    guint startup_retries = 0;

    for (;;) {
        g_autofree char *path =
            control_socket_path();

        if (!path) {
            fprintf(
                stderr,
                "FAIL: cannot construct control socket path\n"
            );
            return false;
        }

        const int fd =
            socket(
                AF_UNIX,
                SOCK_STREAM,
                0
            );

        if (fd < 0) {
            fprintf(
                stderr,
                "FAIL: cannot create watch-state socket: %s\n",
                strerror(errno)
            );
            return false;
        }

        struct sockaddr_un address = {
            .sun_family = AF_UNIX,
        };

        if (strlen(path) >= sizeof(address.sun_path)) {
            close(fd);
            fprintf(
                stderr,
                "FAIL: control socket path too long\n"
            );
            return false;
        }

        strcpy(
            address.sun_path,
            path
        );

        if (connect(
                fd,
                (const struct sockaddr *)&address,
                sizeof(address)) < 0) {
            const int saved_errno = errno;
            close(fd);

            if (!received_state &&
                    (saved_errno == ENOENT ||
                        saved_errno == ECONNREFUSED) &&
                    startup_retries < 100U) {
                startup_retries++;
                g_usleep(50U * 1000U);
                continue;
            }

            fprintf(
                stderr,
                "FAIL: cannot connect watch-state to shell daemon: %s\n",
                strerror(saved_errno)
            );
            return false;
        }

        startup_retries = 0;

        g_autofree char *request =
            g_strdup_printf(
                "WATCH_STATE\t%u\n",
                generation
            );

        if (!request ||
                !write_all(
                    fd,
                    request
                )) {
            fprintf(
                stderr,
                "FAIL: cannot send watch-state request\n"
            );
            close(fd);
            return false;
        }

        GString *line =
            g_string_new(NULL);

        errno = 0;

        if (!read_control_line(
                fd,
                line)) {
            const int saved_errno = errno;
            close(fd);
            g_string_free(line, true);

            if (received_state &&
                    saved_errno == 0) {
                return true;
            }

            fprintf(
                stderr,
                "FAIL: watch-state response ended: %s\n",
                saved_errno
                    ? strerror(saved_errno)
                    : "EOF"
            );
            return false;
        }

        close(fd);

        char *newline =
            strchr(
                line->str,
                '\n'
            );

        if (!newline) {
            g_string_free(line, true);
            fprintf(
                stderr,
                "FAIL: watch-state response missing newline\n"
            );
            return false;
        }

        *newline = '\0';

        char **parts =
            g_strsplit(
                line->str,
                "\t",
                3
            );

        if (!parts ||
                g_strv_length(parts) != 3 ||
                strcmp(parts[0], "STATE") != 0) {
            fprintf(
                stderr,
                "FAIL: invalid watch-state response: %s\n",
                line->str
            );
            g_strfreev(parts);
            g_string_free(line, true);
            return false;
        }

        errno = 0;
        char *end = NULL;

        const guint64 parsed_generation =
            g_ascii_strtoull(
                parts[1],
                &end,
                10
            );

        if (errno != 0 ||
                end == parts[1] ||
                !end ||
                *end != '\0' ||
                parsed_generation > UINT32_MAX ||
                parsed_generation <= generation) {
            fprintf(
                stderr,
                "FAIL: invalid watch-state generation=%s\n",
                parts[1]
            );
            g_strfreev(parts);
            g_string_free(line, true);
            return false;
        }

        generation =
            (uint32_t)parsed_generation;

        const char *stream_json = parts[2];
        g_autofree char *projected = NULL;

        if (ewwii_config_dir) {
            GError *projection_error = NULL;
            const gint64 started_us = g_get_monotonic_time();

            projected =
                widget_block_project_json(
                    "bar",
                    parts[2],
                    &projection_error
                );

            if (!projected) {
                fprintf(
                    stderr,
                    "FAIL: cannot project Ewwii bar state generation=%u: %s\n",
                    generation,
                    projection_error
                        ? projection_error->message
                        : "unknown"
                );
                g_clear_error(&projection_error);
                g_strfreev(parts);
                g_string_free(line, true);
                return false;
            }

            if (!ewwii_adapter_reconcile_bar_roots(
                    ewwii_config_dir,
                    projected)) {
                fprintf(
                    stderr,
                    "FAIL: Ewwii root lifecycle reconcile failed generation=%u\n",
                    generation
                );
                g_clear_error(&projection_error);
                g_strfreev(parts);
                g_string_free(line, true);
                return false;
            }

            const gint64 elapsed_us =
                g_get_monotonic_time() - started_us;
            fprintf(
                stderr,
                "WIDGET BLOCK bar generation=%u bytes=%zu project_and_roots_us=%" G_GINT64_FORMAT "\n",
                generation,
                strlen(projected),
                elapsed_us
            );
            g_clear_error(&projection_error);
            stream_json = projected;
        }

        fputs(
            stream_json,
            stdout
        );
        fputc(
            '\n',
            stdout
        );
        fflush(stdout);

        received_state = true;

        g_strfreev(parts);
        g_string_free(line, true);
    }
}

static bool invoke_daemon(
        const char *capability,
        const char *action) {
    if (!valid_control_token(capability) ||
            !valid_control_token(action)) {
        fprintf(
            stderr,
            "FAIL: invalid invoke capability/action\n"
        );
        return false;
    }

    g_autofree char *path = control_socket_path();

    if (!path) {
        fprintf(
            stderr,
            "FAIL: cannot construct control socket path\n"
        );
        return false;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        fprintf(
            stderr,
            "FAIL: cannot create control client socket: %s\n",
            strerror(errno)
        );
        return false;
    }

    struct sockaddr_un address = {
        .sun_family = AF_UNIX,
    };

    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        fprintf(
            stderr,
            "FAIL: control socket path too long\n"
        );
        return false;
    }

    strcpy(address.sun_path, path);

    if (connect(
            fd,
            (const struct sockaddr *)&address,
            sizeof(address)) < 0) {
        fprintf(
            stderr,
            "FAIL: cannot connect to shell daemon: %s\n",
            strerror(errno)
        );
        close(fd);
        return false;
    }

    g_autofree char *request =
        g_strdup_printf(
            "INVOKE\t%s\t%s\n",
            capability,
            action
        );

    if (!request || !write_all(fd, request)) {
        fprintf(
            stderr,
            "FAIL: cannot send invoke request\n"
        );
        close(fd);
        return false;
    }

    char response[1024];
    const ssize_t length =
        read(
            fd,
            response,
            sizeof(response) - 1
        );

    close(fd);

    if (length <= 0) {
        fprintf(
            stderr,
            "FAIL: empty invoke response\n"
        );
        return false;
    }

    response[length] = '\0';

    if (g_str_has_prefix(response, "OK\t")) {
        printf("INVOKE %s", response);
        return true;
    }

    fprintf(stderr, "FAIL: invoke %s", response);
    return false;
}

static bool run_daemon(
        Shell *shell,
        const char *config_path) {
    ShellConfig config;
    shell_config_init(&config);

    g_autofree char *lkg_path = NULL;

    if (!shell_config_runtime_bootstrap(
            &config,
            config_path,
            &lkg_path)) {
        shell_config_finish(&config);
        return false;
    }

    if (!shell_config_runtime_publish_environment(
            &config)) {
        shell_config_finish(&config);
        return false;
    }

    shell->watch_state = true;

    shell->loop =
        g_main_loop_new(
            NULL,
            false
        );

    if (!shell->loop) {
        fprintf(
            stderr,
            "FAIL: cannot create GLib main loop\n"
        );

        shell_config_finish(&config);
        return false;
    }

    const guint wayland_source =
        g_unix_fd_add(
            wl_display_get_fd(
                shell->display
            ),
            G_IO_IN |
                G_IO_ERR |
                G_IO_HUP |
                G_IO_NVAL,
            handle_wayland_ready,
            shell
        );

    const guint sigint_source =
        g_unix_signal_add(
            SIGINT,
            handle_shutdown_signal,
            shell
        );

    const guint sigterm_source =
        g_unix_signal_add(
            SIGTERM,
            handle_shutdown_signal,
            shell
        );

    if (wayland_source == 0 ||
            sigint_source == 0 ||
            sigterm_source == 0) {
        fprintf(
            stderr,
            "FAIL: cannot install daemon event sources\n"
        );

        if (wayland_source != 0) {
            g_source_remove(wayland_source);
        }

        if (sigint_source != 0) {
            g_source_remove(sigint_source);
        }

        if (sigterm_source != 0) {
            g_source_remove(sigterm_source);
        }

        g_main_loop_unref(shell->loop);
        shell->loop = NULL;

        shell_config_finish(&config);
        return false;
    }

    ProviderManager providers;
    provider_manager_init(
        &providers,
        &config
    );

    shell->providers = &providers;

    if (!provider_manager_start(&providers)) {
        fprintf(
            stderr,
            "PROVIDER WARN: one or more required providers failed to start\n"
        );
    }

    guint control_source = 0;

    if (!control_server_start(
            shell,
            &control_source)) {
        shell->providers = NULL;
        provider_manager_finish(&providers);

        g_source_remove(wayland_source);
        g_source_remove(sigint_source);
        g_source_remove(sigterm_source);

        g_main_loop_unref(shell->loop);
        shell->loop = NULL;

        shell_config_finish(&config);
        return false;
    }

    if (!shell_request_state(shell)) {
        control_server_stop(
            shell,
            control_source
        );

        shell->providers = NULL;
        provider_manager_finish(&providers);

        g_source_remove(wayland_source);
        g_source_remove(sigint_source);
        g_source_remove(sigterm_source);

        g_main_loop_unref(shell->loop);
        shell->loop = NULL;

        shell_config_finish(&config);
        return false;
    }

    shell->controller_claim_received = false;
    shell->controller_owned = false;

    onyrion_shell_unstable_v1_claim_controller(
        shell->core
    );

    if (wl_display_flush(
            shell->display) < 0 &&
            errno != EAGAIN) {
        fprintf(
            stderr,
            "FAIL: cannot flush controller claim: %s\n",
            strerror(errno)
        );

        control_server_stop(
            shell,
            control_source
        );

        shell->providers = NULL;
        provider_manager_finish(&providers);

        g_source_remove(wayland_source);
        g_source_remove(sigint_source);
        g_source_remove(sigterm_source);

        g_main_loop_unref(shell->loop);
        shell->loop = NULL;

        shell_config_finish(&config);
        return false;
    }

    while (!shell->controller_claim_received ||
            shell->state_request_outstanding) {
        if (wl_display_dispatch(
                shell->display) < 0) {
            fprintf(
                stderr,
                "FAIL: controller/state startup dispatch: %s\n",
                strerror(errno)
            );

            shell->protocol_failed = true;
            break;
        }
    }

    if (!shell->controller_owned ||
            shell->protocol_failed) {
        fprintf(
            stderr,
            "FAIL: daemon did not acquire canonical controller role\n"
        );

        control_server_stop(
            shell,
            control_source
        );

        shell->providers = NULL;
        provider_manager_finish(&providers);

        g_source_remove(wayland_source);
        g_source_remove(sigint_source);
        g_source_remove(sigterm_source);

        g_main_loop_unref(shell->loop);
        shell->loop = NULL;

        shell_config_finish(&config);
        return false;
    }

    ShellConfigRuntime *config_runtime =
        shell_config_runtime_start(
            config_path,
            lkg_path,
            &config,
            &providers
        );

    if (!config_runtime) {
        fprintf(
            stderr,
            "FAIL: Shell realtime config runtime unavailable\n"
        );

        if (shell->controller_owned) {
            onyrion_shell_unstable_v1_release_controller(
                shell->core
            );
            (void)wl_display_flush(
                shell->display
            );
            shell->controller_owned = false;
        }

        control_server_stop(
            shell,
            control_source
        );

        shell->providers = NULL;
        provider_manager_finish(&providers);

        g_source_remove(wayland_source);
        g_source_remove(sigint_source);
        g_source_remove(sigterm_source);

        g_main_loop_unref(shell->loop);
        shell->loop = NULL;

        shell_config_finish(&config);
        return false;
    }

    printf("DAEMON READY controller=1\n");
    fflush(stdout);

    g_main_loop_run(shell->loop);

    const bool success =
        !shell->protocol_failed;

    if (shell->controller_owned) {
        onyrion_shell_unstable_v1_release_controller(
            shell->core
        );

        (void)wl_display_flush(
            shell->display
        );

        shell->controller_owned = false;
    }

    shell_config_runtime_finish(
        config_runtime
    );

    control_server_stop(
        shell,
        control_source
    );

    shell->providers = NULL;
    provider_manager_finish(&providers);

    g_source_remove(wayland_source);
    g_source_remove(sigint_source);
    g_source_remove(sigterm_source);

    g_main_loop_unref(shell->loop);
    shell->loop = NULL;

    printf(
        "DAEMON STOP success=%u\n",
        success ? 1U : 0U
    );

    shell_config_finish(&config);
    return success;
}

int main(
        int argc,
        char **argv) {
    if (argc >= 3 && strcmp(argv[1], "tray") == 0) {
        return onyrion_tray_cli(argc - 2, &argv[2]);
    }

    ShellRequest request = {0};

    g_autofree char *program =
        g_path_get_basename(
            argv[0]
        );

    const bool ctl_mode =
        program &&
        strcmp(
            program,
            "onyrionctl"
        ) == 0;

    const bool parsed =
        ctl_mode
            ? parse_ctl_request(
                argc,
                argv,
                &request
            )
            : parse_request(
                argc,
                argv,
                &request
            );

    if (!parsed) {
        if (ctl_mode) {
            print_ctl_usage(argv[0]);
        } else {
            print_usage(argv[0]);
        }

        return EXIT_FAILURE;
    }

    g_autofree char *default_path = NULL;

    if ((request.command == COMMAND_DAEMON ||
            request.command == COMMAND_CONFIG_CHECK ||
            request.command == COMMAND_CONFIG_PERSIST) &&
            !request.config_path) {
        default_path =
            request.command == COMMAND_CONFIG_PERSIST
                ? persist_config_path()
                : default_config_path();

        if (!default_path) {
            fprintf(
                stderr,
                "FAIL: cannot construct default config path\n"
            );

            return EXIT_FAILURE;
        }

        request.config_path = default_path;
    }

    if (request.command == COMMAND_CONFIG_CHECK) {
        return run_config_check(
            request.config_path,
            !request.config_explicit
        )
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    if (request.command == COMMAND_CONFIG_PERSIST) {
        return ctl_persist_config(
            &request,
            request.config_path
        )
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    if (request.command == COMMAND_STATE) {
        return request.ctl_mode
            ? (ctl_query_state(
                    request.ctl_json)
                ? EXIT_SUCCESS
                : EXIT_FAILURE)
            : (query_daemon_state(
                    request.ewwii_config_dir,
                    request.state_ewwii_actions)
                ? EXIT_SUCCESS
                : EXIT_FAILURE);
    }

    if (request.command == COMMAND_WATCH_STATE) {
        return request.ctl_mode
            ? (ctl_watch_state(
                    request.ctl_watch_kind,
                    request.ctl_json)
                ? EXIT_SUCCESS
                : EXIT_FAILURE)
            : (watch_daemon_state(
                    request.ewwii_config_dir)
                ? EXIT_SUCCESS
                : EXIT_FAILURE);
    }

    if (request.command == COMMAND_WATCH_STATUS) {
        return watch_status_stream()
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    if (request.command == COMMAND_INVOKE) {
        return request.ctl_mode
            ? (ctl_invoke(
                    &request)
                ? EXIT_SUCCESS
                : EXIT_FAILURE)
            : (invoke_daemon(
                    request.provider_capability,
                    request.provider_action)
                ? EXIT_SUCCESS
                : EXIT_FAILURE);
    }

    if (request.command == COMMAND_TERMINAL) {
        return launch_terminal()
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    if (request.command == COMMAND_LOCK) {
        return run_lock_action()
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    if (request.command == COMMAND_POWER) {
        return run_power_action(
            request.power_action
        )
            ? EXIT_SUCCESS
            : EXIT_FAILURE;
    }

    Shell shell = {
        .ctl_mode = request.ctl_mode,
        .ctl_json = request.ctl_json,
        .control_fd = -1,
    };
    shell_state_init(&shell.state);

    if (!shell_connect(&shell)) {
        shell_finish(&shell);
        return EXIT_FAILURE;
    }

    bool success = false;

    if (request.command == COMMAND_DAEMON) {
        success =
            run_daemon(
                &shell,
                request.config_path
            );
    } else if (request.command == COMMAND_LAUNCH) {
        success =
            launch_application(
                &shell,
                &request
            );
    } else {
        success =
            send_action(
                &shell,
                &request
            );

        if (request.ctl_mode &&
                !ctl_emit_core_result(
                    &request,
                    success)) {
            success = false;
        }
    }

    shell_finish(&shell);

    return success
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
