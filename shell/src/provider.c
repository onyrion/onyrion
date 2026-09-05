#include "provider.h"

#include <string.h>

#include <glib.h>

static void provider_action_finish(ProviderActionConfig *action) {
    g_free(action->name);
    g_strfreev(action->argv);
    *action = (ProviderActionConfig){0};
}

static void provider_capability_finish(ProviderCapabilityConfig *capability) {
    g_free(capability->id);

    for (size_t i = 0; i < capability->action_count; i++) {
        provider_action_finish(&capability->actions[i]);
    }

    g_free(capability->actions);
    *capability = (ProviderCapabilityConfig){0};
}

void provider_config_finish(ProviderConfig *provider) {
    if (!provider) {
        return;
    }

    g_free(provider->id);
    g_free(provider->type);
    g_strfreev(provider->exec_argv);
    g_strfreev(provider->health_argv);

    for (size_t i = 0; i < provider->capability_count; i++) {
        provider_capability_finish(&provider->capabilities[i]);
    }

    g_free(provider->capabilities);
    *provider = (ProviderConfig){0};
}

const ProviderCapabilityConfig *provider_config_find_capability(
        const ProviderConfig *provider,
        const char *capability) {
    if (!provider || !capability) {
        return NULL;
    }

    for (size_t i = 0; i < provider->capability_count; i++) {
        if (strcmp(provider->capabilities[i].id, capability) == 0) {
            return &provider->capabilities[i];
        }
    }

    return NULL;
}

const ProviderActionConfig *provider_capability_find_action(
        const ProviderCapabilityConfig *capability,
        const char *action) {
    if (!capability || !action) {
        return NULL;
    }

    for (size_t i = 0; i < capability->action_count; i++) {
        if (strcmp(capability->actions[i].name, action) == 0) {
            return &capability->actions[i];
        }
    }

    return NULL;
}

const ProviderActionConfig *provider_config_find_action(
        const ProviderConfig *provider,
        const char *capability,
        const char *action) {
    return provider_capability_find_action(
        provider_config_find_capability(provider, capability),
        action
    );
}

const char *provider_restart_policy_name(ProviderRestartPolicy policy) {
    switch (policy) {
    case PROVIDER_RESTART_NEVER:
        return "never";
    case PROVIDER_RESTART_ON_FAILURE:
        return "on-failure";
    case PROVIDER_RESTART_ALWAYS:
        return "always";
    }

    return "unknown";
}

const char *provider_health_mode_name(ProviderHealthMode mode) {
    switch (mode) {
    case PROVIDER_HEALTH_PROCESS:
        return "process";
    case PROVIDER_HEALTH_COMMAND:
        return "command";
    }

    return "unknown";
}
