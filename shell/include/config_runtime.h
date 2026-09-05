#pragma once

#include <stdbool.h>

#include "provider_runtime.h"
#include "shell_config.h"

typedef struct shell_config_runtime ShellConfigRuntime;

bool shell_config_runtime_bootstrap(
    ShellConfig *config,
    const char *path,
    char **lkg_path_out
);

bool shell_config_runtime_publish_environment(
    const ShellConfig *config
);

ShellConfigRuntime *shell_config_runtime_start(
    const char *path,
    const char *lkg_path,
    ShellConfig *config,
    ProviderManager *providers
);

void shell_config_runtime_finish(
    ShellConfigRuntime *runtime
);
