#pragma once

#include "action.h"
#include "compiler.h"
#include "config.h"

#include <stdbool.h>
#include <stddef.h>

struct onyrion_server;

typedef enum onyrion_control_request_kind {
    ONYRION_CONTROL_REQUEST_ACTION,
    ONYRION_CONTROL_REQUEST_QUERY,
    ONYRION_CONTROL_REQUEST_KIND_COUNT,
} OnyrionControlRequestKind;

typedef enum onyrion_query_kind {
    ONYRION_QUERY_CONFIG_STATUS,
    ONYRION_QUERY_KIND_COUNT,
} OnyrionQueryKind;

typedef struct onyrion_config_status {
    unsigned version;
    OnyrionConfigSource source;
    bool explicit_config;

    size_t binding_count;
    int repeat_rate;
    int repeat_delay;

    double initial_split_ratio;
    double resize_step;
    double resize_min;
    double resize_max;
} OnyrionConfigStatus;

typedef struct onyrion_control_request {
    OnyrionControlRequestKind kind;

    union {
        OnyrionActionRequest action;
        OnyrionQueryKind query;
    };
} OnyrionControlRequest;

typedef struct onyrion_control_result {
    bool success;

    union {
        OnyrionConfigStatus config_status;
    };
} OnyrionControlResult;

[[nodiscard]]
bool onyrion_control_execute(
    struct onyrion_server *server,
    OnyrionControlRequest request,
    OnyrionControlResult *result
);
