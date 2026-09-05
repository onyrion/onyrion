#pragma once

#include "compiler.h"

#include <stdbool.h>

struct onyrion_core_config;
struct onyrion_server;

[[nodiscard]]
bool onyrion_config_runtime_init(
    struct onyrion_server *server,
    struct onyrion_core_config *config,
    const char *path
);

void onyrion_config_runtime_finish(
    struct onyrion_server *server
);
