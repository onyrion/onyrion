#include "control.h"

#include "server.h"

#include <string.h>

static bool query_config_status(
        const OnyrionServer *server,
        OnyrionControlResult *result) {
    if (!server ||
            !server->config ||
            !result) {
        return false;
    }

    const OnyrionCoreConfig *config =
        server->config;

    result->config_status =
        (OnyrionConfigStatus){
            .version =
                config->version,
            .source =
                config->source,
            .explicit_config =
                config->explicit_config,
            .binding_count =
                config->policy.input.binding_count,
            .repeat_rate =
                config->policy.input.keyboard.repeat_rate,
            .repeat_delay =
                config->policy.input.keyboard.repeat_delay,
            .initial_split_ratio =
                config->policy.layout.initial_split_ratio,
            .resize_step =
                config->policy.layout.resize_step,
            .resize_min =
                config->policy.layout.resize_min,
            .resize_max =
                config->policy.layout.resize_max,
        };

    return true;
}

bool onyrion_control_execute(
        struct onyrion_server *server,
        OnyrionControlRequest request,
        OnyrionControlResult *result) {
    if (!server ||
            !result) {
        return false;
    }

    memset(
        result,
        0,
        sizeof(*result)
    );

    bool success = false;

    switch (request.kind) {
    case ONYRION_CONTROL_REQUEST_ACTION:
        success =
            onyrion_action_execute(
                server,
                request.action
            );
        break;

    case ONYRION_CONTROL_REQUEST_QUERY:
        switch (request.query) {
        case ONYRION_QUERY_CONFIG_STATUS:
            success =
                query_config_status(
                    server,
                    result
                );
            break;

        case ONYRION_QUERY_KIND_COUNT:
            success = false;
            break;
        }
        break;

    case ONYRION_CONTROL_REQUEST_KIND_COUNT:
        success = false;
        break;
    }

    result->success = success;

    return success;
}
