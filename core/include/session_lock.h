#pragma once

#include "compiler.h"

#include <stdbool.h>
#include <stdint.h>

struct onyrion_server;
struct wlr_output;
struct wlr_surface;

[[nodiscard]] bool onyrion_session_lock_init(struct onyrion_server *server);
void onyrion_session_lock_finish(struct onyrion_server *server);
[[nodiscard]] bool onyrion_session_lock_active(const struct onyrion_server *server);
[[nodiscard]] bool onyrion_session_lock_surface_allowed(const struct onyrion_server *server, struct wlr_surface *surface);
[[nodiscard]] bool onyrion_session_lock_focus_at_cursor(struct onyrion_server *server);
void onyrion_session_lock_output_add(struct onyrion_server *server, struct wlr_output *output);
void onyrion_session_lock_output_remove(struct onyrion_server *server, struct wlr_output *output);
void onyrion_session_lock_output_commit_pending(struct onyrion_server *server, struct wlr_output *output, uint32_t commit_seq);
void onyrion_session_lock_output_commit_failed(struct onyrion_server *server, struct wlr_output *output, uint32_t commit_seq);
void onyrion_session_lock_output_presented(struct onyrion_server *server, struct wlr_output *output, uint32_t commit_seq, bool presented);
