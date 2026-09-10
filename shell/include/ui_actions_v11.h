#pragma once

#include <stdbool.h>

bool ui_actions_v11_sync_apply(
    const char *config_dir,
    const char *json
);

bool ui_actions_v11_context_open(
    const char *config_dir,
    const char *json,
    const char *subject_kind,
    const char *subject_id
);

bool ui_actions_v11_dismiss_transients(
    const char *config_dir
);
