#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <glib.h>

#include "shell_config.h"

typedef enum provider_runtime_state {
    PROVIDER_RUNTIME_STOPPED,
    PROVIDER_RUNTIME_STARTING,
    PROVIDER_RUNTIME_READY,
    PROVIDER_RUNTIME_BACKOFF,
    PROVIDER_RUNTIME_UNAVAILABLE,
} ProviderRuntimeState;

typedef struct provider_runtime ProviderRuntime;

typedef struct provider_manager {
    const ShellConfig *config;
    ProviderRuntime *providers;
    size_t provider_count;
    bool stopping;
} ProviderManager;

void provider_manager_init(ProviderManager *manager, const ShellConfig *config);
void provider_manager_finish(ProviderManager *manager);
bool provider_manager_start(ProviderManager *manager);
void provider_manager_stop(ProviderManager *manager);

const ProviderConfig *provider_manager_resolve_capability_action(
    const ProviderManager *manager,
    const char *capability,
    const char *action
);

bool provider_manager_invoke(
    ProviderManager *manager,
    const char *capability,
    const char *action,
    const ProviderConfig **used_provider,
    GError **error
);
