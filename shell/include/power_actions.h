#pragma once

#include <stdbool.h>

#include <glib.h>

bool onyrion_power_action(
    const char *action,
    bool interactive,
    GError **error
);
