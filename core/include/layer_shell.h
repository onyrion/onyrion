#pragma once

#include "compiler.h"

#include <stdbool.h>

struct onyrion_server;
struct wlr_box;
struct wlr_output;

[[nodiscard]]
bool onyrion_layer_shell_init(
    struct onyrion_server *server
);

void onyrion_layer_shell_finish(
    struct onyrion_server *server
);

bool onyrion_layer_shell_arrange(
    struct onyrion_server *server
);

void onyrion_layer_shell_get_usable_box(
    struct onyrion_server *server,
    struct wlr_box *box
);

void onyrion_layer_shell_get_output_usable_box(
    struct onyrion_server *server,
    struct wlr_output *output,
    struct wlr_box *box
);

void onyrion_layer_shell_output_destroy(
    struct onyrion_server *server,
    struct wlr_output *output
);

[[nodiscard]]
bool onyrion_layer_shell_focus_at_cursor(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_layer_shell_has_keyboard_focus(
    struct onyrion_server *server
);

void onyrion_layer_shell_refresh_keyboard_focus(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_layer_shell_has_exclusive_keyboard(
    struct onyrion_server *server
);
