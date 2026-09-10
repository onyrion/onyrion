#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>

struct onyrion_server;
struct onyrion_window;
struct wl_client;
struct wl_resource;
struct wlr_surface;

struct onyrion_input_policy;
struct onyrion_keyboard_policy;

typedef enum onyrion_window_interaction_kind {
    ONYRION_WINDOW_INTERACTION_NONE,
    ONYRION_WINDOW_INTERACTION_MOVE,
    ONYRION_WINDOW_INTERACTION_RESIZE,
} OnyrionWindowInteractionKind;

typedef enum onyrion_window_interaction_source {
    ONYRION_WINDOW_INTERACTION_SOURCE_BINDING,
    ONYRION_WINDOW_INTERACTION_SOURCE_XDG,
} OnyrionWindowInteractionSource;

typedef struct onyrion_window_interaction {
    OnyrionWindowInteractionKind kind;
    OnyrionWindowInteractionSource source;
    uint32_t button;
    uint32_t resize_edges;
    uint64_t window_id;
    double pointer_origin_x;
    double pointer_origin_y;
    int window_origin_x;
    int window_origin_y;
    int window_origin_width;
    int window_origin_height;
} OnyrionWindowInteraction;

[[nodiscard]]
bool onyrion_input_apply_keyboard_policy(
    struct onyrion_server *server,
    const struct onyrion_keyboard_policy *policy
);

[[nodiscard]]
bool onyrion_input_apply_pointer_policy(
    struct onyrion_server *server,
    const struct onyrion_input_policy *policy
);


[[nodiscard]]
bool onyrion_input_begin_window_move(
    struct onyrion_server *server,
    struct onyrion_window *window,
    OnyrionWindowInteractionSource source,
    uint32_t button
);

[[nodiscard]]
bool onyrion_input_begin_window_resize(
    struct onyrion_server *server,
    struct onyrion_window *window,
    OnyrionWindowInteractionSource source,
    uint32_t button,
    uint32_t edges
);

void onyrion_input_cancel_window_interaction(
    struct onyrion_server *server,
    struct onyrion_window *window
);

void onyrion_input_cancel_chrome_drag_window(
    struct onyrion_server *server,
    uint64_t window_id
);

void onyrion_input_cancel_chrome_drag_group(
    struct onyrion_server *server,
    uint64_t group_id
);

[[nodiscard]]
bool onyrion_input_init(struct onyrion_server *server);

void onyrion_input_finish(struct onyrion_server *server);

void onyrion_input_clear_pointer_focus(
    struct onyrion_server *server
);

void onyrion_input_focus_surface(
    struct onyrion_server *server,
    struct wlr_surface *surface
);

void onyrion_input_focus_window(
    struct onyrion_server *server,
    struct onyrion_window *window
);


[[nodiscard]]
bool onyrion_input_begin_shell_drag(
    struct onyrion_server *server,
    struct wl_client *client,
    struct wl_resource *owner_resource,
    uint32_t serial,
    uint32_t button,
    bool group_drag,
    uint64_t object_id
);

[[nodiscard]]
bool onyrion_input_set_shell_drag_target(
    struct onyrion_server *server,
    struct wl_client *client,
    uint64_t group_id,
    uint64_t reference_window_id,
    int hint_kind
);
