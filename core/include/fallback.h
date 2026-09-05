#pragma once

#include "compiler.h"

#include <stdbool.h>

struct onyrion_server;
struct onyrion_window;

[[nodiscard]]
bool onyrion_fallback_init(
    struct onyrion_server *server
);

void onyrion_fallback_set_active(
    struct onyrion_server *server,
    bool active
);

void onyrion_fallback_expect_recovery_window(
    struct onyrion_server *server
);

void onyrion_fallback_cancel_recovery_window(
    struct onyrion_server *server
);

void onyrion_fallback_window_mapped(
    struct onyrion_server *server,
    struct onyrion_window *window
);

void onyrion_fallback_window_unavailable(
    struct onyrion_server *server,
    struct onyrion_window *window
);

void onyrion_fallback_finish(
    struct onyrion_server *server
);
