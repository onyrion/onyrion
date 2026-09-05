#pragma once

#include "compiler.h"

#include <stdbool.h>

struct onyrion_server;
struct onyrion_window;
struct wlr_surface;

struct onyrion_input_policy;
struct onyrion_keyboard_policy;

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
