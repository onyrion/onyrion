#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum provider_restart_policy {
    PROVIDER_RESTART_NEVER,
    PROVIDER_RESTART_ON_FAILURE,
    PROVIDER_RESTART_ALWAYS,
} ProviderRestartPolicy;

typedef enum provider_health_mode {
    PROVIDER_HEALTH_PROCESS,
    PROVIDER_HEALTH_COMMAND,
} ProviderHealthMode;

typedef struct provider_action_config {
    char *name;
    char **argv;
    size_t argc;
} ProviderActionConfig;

typedef struct provider_capability_config {
    char *id;
    ProviderActionConfig *actions;
    size_t action_count;
} ProviderCapabilityConfig;

typedef struct provider_config {
    char *id;
    char *type;

    char **exec_argv;
    size_t exec_argc;

    ProviderCapabilityConfig *capabilities;
    size_t capability_count;

    int priority;
    bool autostart;
    bool required;

    ProviderRestartPolicy restart_policy;
    unsigned restart_delay_ms;
    unsigned restart_max_delay_ms;

    ProviderHealthMode health_mode;
    char **health_argv;
    size_t health_argc;
    unsigned health_interval_ms;
    unsigned health_timeout_ms;
    unsigned health_startup_timeout_ms;
} ProviderConfig;

void provider_config_finish(ProviderConfig *provider);

const ProviderCapabilityConfig *provider_config_find_capability(
    const ProviderConfig *provider,
    const char *capability
);

const ProviderActionConfig *provider_capability_find_action(
    const ProviderCapabilityConfig *capability,
    const char *action
);

const ProviderActionConfig *provider_config_find_action(
    const ProviderConfig *provider,
    const char *capability,
    const char *action
);

const char *provider_restart_policy_name(ProviderRestartPolicy policy);
const char *provider_health_mode_name(ProviderHealthMode mode);
