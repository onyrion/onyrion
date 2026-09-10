#pragma once

#include "compiler.h"
#include "input.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>


struct onyrion_config_runtime;
struct onyrion_core_config;
struct onyrion_core_policy;
struct onyrion_fallback_controller;
struct onyrion_gesture_recognizer;
struct onyrion_group;
struct onyrion_output;
struct onyrion_session_lock_controller;
struct onyrion_workspace;

struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_cursor;
struct wlr_data_device_manager;
struct wlr_layer_shell_v1;
struct wlr_viewporter;
struct wlr_xcursor_manager;
struct wlr_xdg_activation_v1;
struct wlr_subcompositor;
struct wlr_renderer;
struct wlr_seat;
struct wlr_session;
struct wlr_output_layout;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_output_layout;
struct wlr_xdg_shell;

typedef enum onyrion_chrome_drag_subject {
    ONYRION_CHROME_DRAG_NONE,
    ONYRION_CHROME_DRAG_WINDOW,
    ONYRION_CHROME_DRAG_GROUP,
} OnyrionChromeDragSubject;

typedef struct onyrion_server {
    struct wl_display *display;
    struct onyrion_core_config *config;
    const struct onyrion_core_policy *policy;
    struct onyrion_config_runtime *config_runtime;

    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_subcompositor *subcompositor;
    struct wlr_data_device_manager *data_device_manager;
    struct wlr_viewporter *viewporter;

    struct wlr_output_layout *output_layout;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_scene_tree *scene_background;
    struct wlr_scene_tree *scene_bottom;
    struct wlr_scene_tree *scene_content;
    struct wlr_scene_tree *scene_top;
    struct wlr_scene_tree *scene_overlay;
    struct wlr_scene_tree *drag_icons;
    struct onyrion_session_lock_controller *session_lock;

    struct wl_list outputs;
    struct wl_listener new_output;

    struct wl_listener layout_change;

    struct wl_list workspaces;
    uint64_t next_workspace_id;
    struct onyrion_output *focused_output;

    struct wl_global *shell_global;
    struct wl_list shell_clients;
    struct wl_list shell_pending_invokes;
    uint32_t shell_generation;
    uint32_t shell_next_invoke_serial;
    bool fallback_active;
    struct onyrion_fallback_controller *fallback;

    struct wl_list groups;
    struct wl_list group_surfaces;
    struct onyrion_group *active_group;
    uint64_t next_group_id;

    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor_manager;

    struct wl_list keyboards;
    struct wl_list pointers;
    struct wl_listener new_input;

    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct onyrion_gesture_recognizer *gesture_recognizer;
    struct wl_listener request_set_cursor;
    struct wl_listener request_set_selection;
    struct wl_listener request_start_drag;
    struct wl_listener start_drag;

    bool chrome_pointer_grab;
    uint32_t chrome_pointer_button;
    OnyrionChromeDragSubject chrome_drag_subject;
    uint64_t chrome_drag_window_id;
    uint64_t chrome_drag_group_id;
    double chrome_drag_origin_x;
    double chrome_drag_origin_y;
    bool chrome_drag_active;
    bool chrome_drag_shell_origin;
    struct wl_client *chrome_drag_shell_client;
    struct wl_resource *chrome_drag_shell_resource;
    uint64_t chrome_drag_hint_group_id;
    uint64_t chrome_drag_hint_reference_window_id;
    int chrome_drag_hint_kind;

    bool layout_resize_pointer_grab;
    bool layout_resize_cursor_owned;
    uint32_t layout_resize_pointer_button;
    uint64_t layout_resize_first_group_id;
    uint64_t layout_resize_second_group_id;
    int layout_resize_orientation;

    OnyrionWindowInteraction window_interaction;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener request_activate;

    struct wl_list windows;
    uint64_t next_window_id;
    struct wl_listener new_toplevel;

    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_list layer_surfaces;
    struct wl_listener new_layer_surface;

    int usable_x;
    int usable_y;
    int usable_width;
    int usable_height;

    const char *socket;
    bool exit_requested;
} OnyrionServer;

[[nodiscard]]
bool onyrion_server_init(
    OnyrionServer *server,
    struct onyrion_core_config *config,
    const char *config_path,
    const char *socket_name
);

[[nodiscard]]
int onyrion_server_run(OnyrionServer *server);

[[nodiscard]]
bool onyrion_server_request_exit(OnyrionServer *server);

void onyrion_server_finish(OnyrionServer *server);
