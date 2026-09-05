#include "fallback.h"

#include "server.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

typedef struct onyrion_fallback_controller {
    OnyrionServer *server;
    struct wlr_scene_tree *tree;
    struct onyrion_window *recovery_window;
    bool recovery_window_pending;
    struct wl_listener layout_change;
} OnyrionFallbackController;

typedef struct fallback_glyph {
    char character;
    const char *pixels;
} FallbackGlyph;

enum {
    PANEL_WIDTH = 520,
    PANEL_HEIGHT = 164,
    FONT_WIDTH = 3,
    FONT_HEIGHT = 5,
    FONT_SCALE = 4,
    FONT_ADVANCE = 4,
};

static const float panel_color[4] = {
    0.06f, 0.07f, 0.09f, 0.96f,
};

static const float title_color[4] = {
    0.95f, 0.96f, 0.98f, 1.0f,
};

static const float text_color[4] = {
    0.78f, 0.82f, 0.88f, 1.0f,
};

static const FallbackGlyph glyphs[] = {
    {' ', "000000000000000"},
    {'+', "000010111010000"},
    {'3', "110001010001110"},
    {'A', "010101111101101"},
    {'B', "110101110101110"},
    {'C', "011100100100011"},
    {'D', "110101101101110"},
    {'E', "111100110100111"},
    {'F', "111100110100100"},
    {'H', "101101111101101"},
    {'I', "111010010010111"},
    {'K', "101101110101101"},
    {'L', "100100100100111"},
    {'M', "101111111101101"},
    {'N', "101111111111101"},
    {'O', "010101101101010"},
    {'P', "110101110100100"},
    {'Q', "010101101111011"},
    {'R', "110101110101101"},
    {'S', "011100010001110"},
    {'T', "111010010010010"},
    {'U', "101101101101111"},
    {'V', "101101101101010"},
    {'W', "101101111111101"},
    {'Y', "101101010010010"},
};

static const char *glyph_pixels(char character) {
    const char upper =
        (char)toupper((unsigned char)character);

    for (size_t i = 0;
            i < sizeof(glyphs) /
                sizeof(glyphs[0]);
            i++) {
        if (glyphs[i].character == upper) {
            return glyphs[i].pixels;
        }
    }

    return glyphs[0].pixels;
}

static bool draw_text(
        struct wlr_scene_tree *tree,
        int x,
        int y,
        const char *text,
        const float color[static 4]) {
    if (!tree ||
            !text) {
        return false;
    }

    for (size_t character = 0;
            text[character] != '\0';
            character++) {
        const char *pixels =
            glyph_pixels(text[character]);

        for (int row = 0;
                row < FONT_HEIGHT;
                row++) {
            for (int column = 0;
                    column < FONT_WIDTH;
                    column++) {
                if (pixels[
                        row * FONT_WIDTH +
                        column] != '1') {
                    continue;
                }

                struct wlr_scene_rect *pixel =
                    wlr_scene_rect_create(
                        tree,
                        FONT_SCALE,
                        FONT_SCALE,
                        color
                    );

                if (!pixel) {
                    return false;
                }

                wlr_scene_node_set_position(
                    &pixel->node,
                    x +
                        (int)character *
                            FONT_ADVANCE *
                            FONT_SCALE +
                        column *
                            FONT_SCALE,
                    y +
                        row *
                            FONT_SCALE
                );
            }
        }
    }

    return true;
}

static void fallback_overlay_set_visible(
        OnyrionFallbackController *fallback,
        bool visible) {
    if (!fallback ||
            !fallback->server ||
            !fallback->tree) {
        return;
    }

    wlr_scene_node_set_enabled(
        &fallback->tree->node,
        fallback->server->fallback_active &&
            visible
    );
}

static void fallback_reflow(
        OnyrionFallbackController *fallback) {
    if (!fallback ||
            !fallback->server ||
            !fallback->server->output_layout ||
            !fallback->tree) {
        return;
    }

    struct wlr_box layout_box = {0};

    wlr_output_layout_get_box(
        fallback->server->output_layout,
        NULL,
        &layout_box
    );

    if (layout_box.width <= 0 ||
            layout_box.height <= 0) {
        return;
    }

    int x =
        layout_box.x +
        (layout_box.width -
            PANEL_WIDTH) / 2;

    int y =
        layout_box.y +
        (layout_box.height -
            PANEL_HEIGHT) / 2;

    if (x < layout_box.x) {
        x = layout_box.x;
    }

    if (y < layout_box.y) {
        y = layout_box.y;
    }

    wlr_scene_node_set_position(
        &fallback->tree->node,
        x,
        y
    );
}

static void handle_layout_change(
        struct wl_listener *listener,
        void *data) {
    (void)data;

    OnyrionFallbackController *fallback =
        wl_container_of(
            listener,
            fallback,
            layout_change
        );

    fallback_reflow(fallback);
}

bool onyrion_fallback_init(
        struct onyrion_server *server) {
    if (!server ||
            !server->scene_overlay ||
            !server->output_layout) {
        return false;
    }

    OnyrionFallbackController *fallback =
        calloc(
            1,
            sizeof(*fallback)
        );

    if (!fallback) {
        return false;
    }

    fallback->server = server;

    fallback->tree =
        wlr_scene_tree_create(
            server->scene_overlay
        );

    if (!fallback->tree) {
        free(fallback);
        return false;
    }

    struct wlr_scene_rect *panel =
        wlr_scene_rect_create(
            fallback->tree,
            PANEL_WIDTH,
            PANEL_HEIGHT,
            panel_color
        );

    if (!panel ||
            !draw_text(
                fallback->tree,
                32,
                22,
                "ONYRION RECOVERY",
                title_color) ||
            !draw_text(
                fallback->tree,
                32,
                50,
                "SHELL CONTROL UNAVAILABLE",
                text_color) ||
            !draw_text(
                fallback->tree,
                32,
                82,
                "SUPER+RETURN TERMINAL",
                text_color) ||
            !draw_text(
                fallback->tree,
                32,
                110,
                "SUPER+Q CLOSE WINDOW",
                text_color) ||
            !draw_text(
                fallback->tree,
                32,
                138,
                "CTRL+ALT+F3 SWITCH VT",
                text_color)) {
        wlr_scene_node_destroy(
            &fallback->tree->node
        );

        free(fallback);
        return false;
    }

    fallback->layout_change.notify =
        handle_layout_change;

    wl_signal_add(
        &server->output_layout->events.change,
        &fallback->layout_change
    );

    server->fallback = fallback;
    server->fallback_active = true;

    wlr_scene_node_raise_to_top(
        &fallback->tree->node
    );

    wlr_scene_node_set_enabled(
        &fallback->tree->node,
        true
    );

    fallback_reflow(fallback);

    wlr_log(
        WLR_INFO,
        "Fallback recovery active:"
        " terminal=Super+Return"
        " close=Super+Q"
        " vt=Ctrl+Alt+F3"
    );

    return true;
}

void onyrion_fallback_set_active(
        struct onyrion_server *server,
        bool active) {
    if (!server ||
            server->fallback_active ==
                active) {
        return;
    }

    server->fallback_active = active;

    if (server->fallback) {
        server->fallback->recovery_window_pending =
            false;

        server->fallback->recovery_window = NULL;

        if (active) {
            fallback_reflow(
                server->fallback
            );

            if (server->fallback->tree) {
                wlr_scene_node_raise_to_top(
                    &server->fallback->tree->node
                );
            }
        }

        fallback_overlay_set_visible(
            server->fallback,
            active
        );
    }

    wlr_log(
        WLR_INFO,
        "Fallback recovery %s",
        active
            ? "active"
            : "inactive"
    );
}

void onyrion_fallback_expect_recovery_window(
        struct onyrion_server *server) {
    if (!server ||
            !server->fallback_active ||
            !server->fallback) {
        return;
    }

    server->fallback->recovery_window_pending =
        true;

    server->fallback->recovery_window = NULL;
}

void onyrion_fallback_cancel_recovery_window(
        struct onyrion_server *server) {
    if (!server ||
            !server->fallback) {
        return;
    }

    server->fallback->recovery_window_pending =
        false;

    server->fallback->recovery_window = NULL;
}

void onyrion_fallback_window_mapped(
        struct onyrion_server *server,
        struct onyrion_window *window) {
    if (!server ||
            !window ||
            !server->fallback_active ||
            !server->fallback ||
            !server->fallback->recovery_window_pending) {
        return;
    }

    server->fallback->recovery_window_pending =
        false;

    server->fallback->recovery_window = window;

    fallback_overlay_set_visible(
        server->fallback,
        false
    );

    wlr_log(
        WLR_INFO,
        "Fallback overlay hidden:"
        " recovery window mapped"
    );
}

void onyrion_fallback_window_unavailable(
        struct onyrion_server *server,
        struct onyrion_window *window) {
    if (!server ||
            !window ||
            !server->fallback ||
            server->fallback->recovery_window !=
                window) {
        return;
    }

    server->fallback->recovery_window = NULL;

    fallback_overlay_set_visible(
        server->fallback,
        true
    );

    if (server->fallback_active) {
        wlr_log(
            WLR_INFO,
            "Fallback overlay restored:"
            " recovery window unavailable"
        );
    }
}

void onyrion_fallback_finish(
        struct onyrion_server *server) {
    if (!server ||
            !server->fallback) {
        return;
    }

    OnyrionFallbackController *fallback =
        server->fallback;

    server->fallback = NULL;
    server->fallback_active = false;

    if (fallback->layout_change.notify) {
        wl_list_remove(
            &fallback->layout_change.link
        );

        fallback->layout_change.notify =
            NULL;
    }

    if (fallback->tree) {
        wlr_scene_node_destroy(
            &fallback->tree->node
        );

        fallback->tree = NULL;
    }

    free(fallback);
}
