#include "config.h"
#include "server.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum onyrion_command {
    ONYRION_COMMAND_RUN,
    ONYRION_COMMAND_CONFIG_CHECK,
} OnyrionCommand;

typedef struct onyrion_cli {
    OnyrionCommand command;
    const char *config_path;
    const char *socket_name;
    bool explicit_config;
} OnyrionCli;

static void print_usage(
        const char *argv0) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s [--config path] [--socket name]\n"
        "  %s config-check [--config path]\n",
        argv0,
        argv0
    );
}

static bool parse_cli(
        int argc,
        char **argv,
        OnyrionCli *cli) {
    *cli = (OnyrionCli){
        .command = ONYRION_COMMAND_RUN,
    };

    int index = 1;

    if (index < argc &&
            strcmp(
                argv[index],
                "config-check") == 0) {
        cli->command =
            ONYRION_COMMAND_CONFIG_CHECK;

        index++;
    }

    while (index < argc) {
        const char *option =
            argv[index++];

        if (strcmp(
                option,
                "--config") == 0) {
            if (cli->explicit_config ||
                    index >= argc) {
                return false;
            }

            cli->config_path =
                argv[index++];

            cli->explicit_config = true;
            continue;
        }

        if (strcmp(
                option,
                "--socket") == 0) {
            if (cli->command !=
                    ONYRION_COMMAND_RUN ||
                    cli->socket_name ||
                    index >= argc) {
                return false;
            }

            cli->socket_name =
                argv[index++];

            continue;
        }

        return false;
    }

    return true;
}

static void print_policy(
        const OnyrionCoreConfig *config,
        const char *path) {
    printf(
        "CONFIG_PATH=%s\n",
        path
    );

    printf(
        "CONFIG_SOURCE=%s\n",
        onyrion_core_config_source_name(
            config->source)
    );

    printf(
        "CONFIG_ACTIVE_PATH=%s\n",
        config->source_path
            ? config->source_path
            : "<compiled-defaults>"
    );

    printf(
        "REPEAT_RATE=%d\n",
        config->policy.input.keyboard.repeat_rate
    );

    printf(
        "REPEAT_DELAY=%d\n",
        config->policy.input.keyboard.repeat_delay
    );

    printf(
        "BINDINGS=%zu\n",
        config->policy.input.binding_count
    );

    printf(
        "WORKSPACES=%zu\n",
        config->policy.workspaces.count
    );

    printf(
        "INITIAL_SPLIT_RATIO=%.6f\n",
        config->policy.layout.initial_split_ratio
    );

    printf(
        "RESIZE_STEP=%.6f\n",
        config->policy.layout.resize_step
    );

    printf(
        "RESIZE_MIN=%.6f\n",
        config->policy.layout.resize_min
    );

    printf(
        "RESIZE_MAX=%.6f\n",
        config->policy.layout.resize_max
    );

    printf(
        "OUTER_GAP=%d\n",
        config->policy.layout.outer_gap
    );

    printf(
        "INNER_GAP=%d\n",
        config->policy.layout.inner_gap
    );

    printf(
        "WINDOW_RULES=%zu\n",
        config->policy.window_rules.count
    );
}

int main(
        int argc,
        char **argv) {
    OnyrionCli cli;

    if (!parse_cli(
            argc,
            argv,
            &cli)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    char error[256] = {0};

    char *config_path = NULL;

    OnyrionCoreConfig config = {0};

    if (!onyrion_core_config_load_startup(
            &config,
            cli.config_path,
            cli.explicit_config,
            cli.command == ONYRION_COMMAND_RUN,
            &config_path,
            error,
            sizeof(error))) {
        fprintf(
            stderr,
            "FAIL: config: %s\n",
            error
        );

        return EXIT_FAILURE;
    }

    if (cli.command ==
            ONYRION_COMMAND_CONFIG_CHECK) {
        printf(
            "PASS: config valid\n"
        );

        print_policy(
            &config,
            config_path
        );

        onyrion_core_config_finish(
            &config
        );

        free(config_path);

        return EXIT_SUCCESS;
    }

    printf(
        "PASS: core policy ready\n"
    );

    print_policy(
        &config,
        config_path
    );

    OnyrionServer server;

    if (!onyrion_server_init(
            &server,
            &config,
            config_path,
            cli.socket_name)) {
        onyrion_server_finish(
            &server
        );

        onyrion_core_config_finish(
            &config
        );

        free(config_path);

        return EXIT_FAILURE;
    }

    char persist_error[256] = {0};

    if (!onyrion_core_config_persist_lkg(
            &config,
            persist_error,
            sizeof(persist_error))) {
        fprintf(
            stderr,
            "WARN: persistent LKG update failed: %s\n",
            persist_error
        );
    }

    const int result =
        onyrion_server_run(
            &server
        );

    onyrion_server_finish(
        &server
    );

    onyrion_core_config_finish(
        &config
    );

    free(config_path);

    return result;
}
