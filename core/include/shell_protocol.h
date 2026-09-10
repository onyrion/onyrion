#pragma once

#include "compiler.h"

#include <stdbool.h>

struct onyrion_server;
struct wl_resource;

[[nodiscard]]
bool onyrion_shell_protocol_init(
    struct onyrion_server *server
);

void onyrion_shell_protocol_finish(
    struct onyrion_server *server
);

void onyrion_shell_protocol_mark_changed(
    struct onyrion_server *server
);

[[nodiscard]]
bool onyrion_shell_protocol_invoke(
    struct onyrion_server *server,
    const char *capability,
    const char *action
);


void onyrion_shell_protocol_send_drag_surface_motion(
    struct wl_resource *resource,
    uint64_t group_id,
    const char *name_space,
    double x,
    double y
);
