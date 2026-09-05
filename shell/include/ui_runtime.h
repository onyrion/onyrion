#pragma once

#include <stdbool.h>

bool daily_ui_sync_apply(
    const char *config_dir,
    const char *json
);

bool watch_status_stream(void);
