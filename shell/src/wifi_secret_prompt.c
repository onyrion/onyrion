#include <gio/gio.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <gtk/gtk.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct prompt_ctx {
    GMainLoop *loop;
    GtkWidget *window;
    GtkWidget *entry;
    GtkWidget *status;
    const char *bssid;
} PromptCtx;

static bool valid_bssid(const char *value) {
    if (!value || strlen(value) != 17) return false;
    for (size_t i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':') return false;
        } else if (!g_ascii_isxdigit(value[i])) {
            return false;
        }
    }
    return true;
}

static bool run_control(
        const char *bssid,
        const char *secret,
        char **stdout_out,
        char **stderr_out,
        GError **error) {
    const char *control = g_getenv("ONYRION_CONTROL_BIN");
    if (!control || !*control) control = "onyrion-control";

    g_autoptr(GSubprocess) child = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error,
        control,
        "wifi",
        "connect",
        bssid,
        "--password-stdin",
        NULL
    );
    if (!child) return false;

    g_autofree char *input = g_strdup_printf("%s\n", secret);
    if (!g_subprocess_communicate_utf8(
                child,
                input,
                NULL,
                stdout_out,
                stderr_out,
                error)) {
        if (input) memset(input, 0, strlen(input));
        return false;
    }
    if (input) memset(input, 0, strlen(input));
    return g_subprocess_get_successful(child);
}

static void connect_clicked(GtkButton *button, gpointer data) {
    (void)button;
    PromptCtx *ctx = data;
    const char *secret = gtk_editable_get_text(GTK_EDITABLE(ctx->entry));
    if (!secret || !*secret) {
        gtk_label_set_text(GTK_LABEL(ctx->status), "Password is empty");
        return;
    }

    gtk_widget_set_sensitive(ctx->entry, FALSE);
    gtk_label_set_text(GTK_LABEL(ctx->status), "Connecting…");

    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    g_autoptr(GError) error = NULL;
    const bool ok = run_control(
        ctx->bssid,
        secret,
        &stdout_text,
        &stderr_text,
        &error
    );
    gtk_editable_set_text(GTK_EDITABLE(ctx->entry), "");

    if (ok) {
        gtk_window_destroy(GTK_WINDOW(ctx->window));
        g_main_loop_quit(ctx->loop);
        return;
    }

    gtk_widget_set_sensitive(ctx->entry, TRUE);
    if (error) {
        gtk_label_set_text(GTK_LABEL(ctx->status), error->message);
    } else if (stderr_text && *stderr_text) {
        gtk_label_set_text(GTK_LABEL(ctx->status), stderr_text);
    } else {
        gtk_label_set_text(GTK_LABEL(ctx->status), "Wi-Fi connection failed");
    }
}

static gboolean close_request(GtkWindow *window, gpointer data) {
    (void)window;
    g_main_loop_quit(((PromptCtx *)data)->loop);
    return FALSE;
}

#ifdef ONYRION_WIFI_PROMPT_TEST
static gboolean test_close(gpointer data) {
    PromptCtx *ctx = data;
    gtk_window_destroy(GTK_WINDOW(ctx->window));
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}

static int test_stdin_pipeline(const char *bssid) {
    char *line = NULL;
    size_t cap = 0;
    const ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        g_free(line);
        return 20;
    }
    line[strcspn(line, "\r\n")] = '\0';
    g_autofree char *stdout_text = NULL;
    g_autofree char *stderr_text = NULL;
    g_autoptr(GError) error = NULL;
    const bool ok = run_control(bssid, line, &stdout_text, &stderr_text, &error);
    memset(line, 0, strlen(line));
    g_free(line);
    if (!ok) {
        if (error) g_printerr("TEST_FAIL=%s\n", error->message);
        else if (stderr_text) g_printerr("TEST_FAIL=%s\n", stderr_text);
        return 21;
    }
    g_print("TEST_SECRET_STDIN_PIPELINE=V bssid=%s\n", bssid);
    return 0;
}
#endif

int main(int argc, char **argv) {
#ifdef ONYRION_WIFI_PROMPT_TEST
    if (argc == 3 && g_strcmp0(argv[1], "--test-secret-stdin") == 0) {
        if (!valid_bssid(argv[2])) return 2;
        return test_stdin_pipeline(argv[2]);
    }
#endif
    if (argc != 2 || !valid_bssid(argv[1])) {
        g_printerr("usage: onyrion-wifi-prompt BSSID\n");
        return 2;
    }

    gtk_init();
    PromptCtx ctx = {0};
    ctx.loop = g_main_loop_new(NULL, FALSE);
    ctx.bssid = argv[1];
    ctx.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ctx.window), "Wi-Fi password");
    gtk_window_set_default_size(GTK_WINDOW(ctx.window), 420, 150);
    gtk_window_set_resizable(GTK_WINDOW(ctx.window), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(ctx.window), box);

    g_autofree char *title = g_strdup_printf("Password for Wi-Fi AP %s", ctx.bssid);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_append(GTK_BOX(box), label);

    ctx.entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(ctx.entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx.entry), "Wi-Fi password");
    gtk_box_append(GTK_BOX(box), ctx.entry);

    GtkWidget *button = gtk_button_new_with_label("Connect");
    gtk_box_append(GTK_BOX(box), button);
    ctx.status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ctx.status), 0.0f);
    gtk_box_append(GTK_BOX(box), ctx.status);

    g_signal_connect(button, "clicked", G_CALLBACK(connect_clicked), &ctx);
    g_signal_connect(ctx.window, "close-request", G_CALLBACK(close_request), &ctx);

    gtk_window_present(GTK_WINDOW(ctx.window));
#ifdef ONYRION_WIFI_PROMPT_TEST
    if (g_getenv("ONYRION_WIFI_PROMPT_TEST_AUTOCLOSE")) {
        g_timeout_add(700, test_close, &ctx);
    }
#endif
    g_main_loop_run(ctx.loop);
    g_main_loop_unref(ctx.loop);
    return 0;
}
