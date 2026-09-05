#pragma once

#include "compiler.h"

#include <stdbool.h>

[[nodiscard]]
bool onyrion_recovery_spawn_terminal(
    const char *wayland_display
);
