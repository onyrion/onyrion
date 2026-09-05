#pragma once

#include "compiler.h"

#include <stdbool.h>

struct onyrion_server;

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
