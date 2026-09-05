#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

#define ONYRION_LAUNCHER_MAX_RESULTS 12

typedef struct {
    GAppInfo *info;
    char *id;
    char *name;
    char *search_key;
} LauncherApp;

typedef struct {
    GtkWindow *window;
    GtkSearchEntry *search;
    GtkListBox *results;
    GPtrArray *apps;
    GMainLoop *loop;
    GSocketService *service;
    char *socket_path;
} Launcher;

static void launcher_app_free(gpointer data)
{
    LauncherApp *app = data;

    if (!app) {
        return;
    }

    g_clear_object(&app->info);
    g_free(app->id);
    g_free(app->name);
    g_free(app->search_key);
    g_free(app);
}

static gint launcher_app_compare(gconstpointer a, gconstpointer b)
{
    const LauncherApp *left = *(LauncherApp * const *)a;
    const LauncherApp *right = *(LauncherApp * const *)b;
    int cmp = g_utf8_collate(left->name, right->name);

    if (cmp != 0) {
        return cmp;
    }

    return g_strcmp0(left->id, right->id);
}

static GPtrArray *launcher_load_apps(void)
{
    GPtrArray *apps =
        g_ptr_array_new_with_free_func(launcher_app_free);
    GList *all = g_app_info_get_all();

    for (GList *node = all; node; node = node->next) {
        GAppInfo *info = G_APP_INFO(node->data);
        const char *id;
        const char *name;
        LauncherApp *app;

        if (!G_IS_DESKTOP_APP_INFO(info) ||
            !g_app_info_should_show(info)) {
            continue;
        }

        id = g_app_info_get_id(info);
        name = g_app_info_get_display_name(info);

        if (!id || !*id || !name || !*name) {
            continue;
        }

        app = g_new0(LauncherApp, 1);
        app->info = g_object_ref(info);
        app->id = g_strdup(id);
        app->name = g_strdup(name);

        char *joined = g_strdup_printf("%s\n%s", name, id);
        app->search_key = g_utf8_casefold(joined, -1);
        g_free(joined);

        g_ptr_array_add(apps, app);
    }

    g_list_free_full(all, g_object_unref);
    g_ptr_array_sort(apps, launcher_app_compare);

    return apps;
}

static void launcher_clear_results(Launcher *launcher)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(
                GTK_WIDGET(launcher->results)))) {
        gtk_list_box_remove(
            launcher->results,
            child
        );
    }
}

static LauncherApp *launcher_row_app(GtkListBoxRow *row)
{
    if (!row) {
        return NULL;
    }

    return g_object_get_data(
        G_OBJECT(row),
        "onyrion-launcher-app"
    );
}

static void launcher_select_index(
        Launcher *launcher,
        int index)
{
    GtkListBoxRow *row =
        gtk_list_box_get_row_at_index(
            launcher->results,
            index
        );

    if (!row) {
        return;
    }

    gtk_list_box_select_row(
        launcher->results,
        row
    );

    gtk_widget_grab_focus(
        GTK_WIDGET(launcher->search)
    );
}

static guint launcher_rebuild_results(
        Launcher *launcher,
        const char *query)
{
    g_autofree char *folded =
        g_utf8_casefold(query ? query : "", -1);
    guint count = 0;

    launcher_clear_results(launcher);

    for (guint i = 0;
         i < launcher->apps->len &&
         count < ONYRION_LAUNCHER_MAX_RESULTS;
         i++) {
        LauncherApp *app =
            g_ptr_array_index(launcher->apps, i);

        if (*folded &&
            !strstr(app->search_key, folded)) {
            continue;
        }

        GtkWidget *row =
            gtk_list_box_row_new();
        GtkWidget *content =
            gtk_box_new(
                GTK_ORIENTATION_HORIZONTAL,
                10
            );
        GtkWidget *label =
            gtk_label_new(app->name);
        GtkIconTheme *icon_theme =
            gtk_icon_theme_get_for_display(
                gtk_widget_get_display(
                    GTK_WIDGET(launcher->window)
                )
            );
        GIcon *app_icon =
            g_app_info_get_icon(app->info);
        GtkWidget *icon =
            app_icon &&
            gtk_icon_theme_has_gicon(
                icon_theme,
                app_icon
            )
                ? gtk_image_new_from_gicon(
                    app_icon
                )
                : gtk_image_new_from_icon_name(
                    "application-x-executable-symbolic"
                );

        gtk_image_set_pixel_size(
            GTK_IMAGE(icon),
            32
        );
        gtk_widget_set_valign(
            icon,
            GTK_ALIGN_CENTER
        );

        gtk_label_set_xalign(
            GTK_LABEL(label),
            0.0f
        );
        gtk_widget_set_hexpand(label, TRUE);

        gtk_widget_set_margin_start(content, 12);
        gtk_widget_set_margin_end(content, 12);
        gtk_widget_set_margin_top(content, 8);
        gtk_widget_set_margin_bottom(content, 8);

        gtk_box_append(
            GTK_BOX(content),
            icon
        );
        gtk_box_append(
            GTK_BOX(content),
            label
        );

        gtk_list_box_row_set_child(
            GTK_LIST_BOX_ROW(row),
            content
        );

        g_object_set_data(
            G_OBJECT(row),
            "onyrion-launcher-app",
            app
        );

        gtk_list_box_append(
            launcher->results,
            row
        );

        count++;
    }

    if (count > 0) {
        launcher_select_index(launcher, 0);
    }

    printf(
        "LAUNCHER_QUERY query=%s results=%u\n",
        query ? query : "",
        count
    );

    return count;
}

static void launcher_hide(
        Launcher *launcher,
        const char *reason)
{
    if (!gtk_widget_get_visible(
            GTK_WIDGET(launcher->window))) {
        return;
    }

    gtk_widget_set_visible(
        GTK_WIDGET(launcher->window),
        FALSE
    );

    printf(
        "LAUNCHER_CLOSE reason=%s\n",
        reason
    );
}

static gboolean launcher_focus_search(gpointer data)
{
    Launcher *launcher = data;

    gtk_widget_grab_focus(
        GTK_WIDGET(launcher->search)
    );

    return G_SOURCE_REMOVE;
}

static void launcher_show(Launcher *launcher)
{
    gtk_editable_set_text(
        GTK_EDITABLE(launcher->search),
        ""
    );

    launcher_rebuild_results(
        launcher,
        ""
    );

    gtk_window_present(launcher->window);
    g_idle_add(
        launcher_focus_search,
        launcher
    );

    printf(
        "LAUNCHER_OPEN apps=%u\n",
        launcher->apps->len
    );
}

static gboolean launcher_launch_app(
        Launcher *launcher,
        LauncherApp *app)
{
    GError *error = NULL;
    GdkAppLaunchContext *context;
    gboolean ok;

    if (!app) {
        return FALSE;
    }

    context = gdk_display_get_app_launch_context(
        gtk_widget_get_display(
            GTK_WIDGET(launcher->window)
        )
    );

    ok = g_app_info_launch(
        app->info,
        NULL,
        G_APP_LAUNCH_CONTEXT(context),
        &error
    );

    g_clear_object(&context);

    if (!ok) {
        fprintf(
            stderr,
            "LAUNCHER_APP_FAIL id=%s error=%s\n",
            app->id,
            error ? error->message : "unknown"
        );
        g_clear_error(&error);
        return FALSE;
    }

    printf(
        "LAUNCHER_APP id=%s name=%s\n",
        app->id,
        app->name
    );

    launcher_hide(
        launcher,
        "launch"
    );

    return TRUE;
}

static void launcher_row_activated(
        GtkListBox *box,
        GtkListBoxRow *row,
        gpointer data)
{
    Launcher *launcher = data;

    (void)box;

    launcher_launch_app(
        launcher,
        launcher_row_app(row)
    );
}

static void launcher_search_changed(
        GtkSearchEntry *entry,
        gpointer data)
{
    Launcher *launcher = data;

    launcher_rebuild_results(
        launcher,
        gtk_editable_get_text(
            GTK_EDITABLE(entry)
        )
    );
}

static void launcher_move_selection(
        Launcher *launcher,
        int delta)
{
    GtkListBoxRow *selected =
        gtk_list_box_get_selected_row(
            launcher->results
        );
    int index = selected
        ? gtk_list_box_row_get_index(selected)
        : 0;
    int next = index + delta;

    if (next < 0) {
        next = 0;
    }

    if (!gtk_list_box_get_row_at_index(
            launcher->results,
            next)) {
        return;
    }

    launcher_select_index(
        launcher,
        next
    );

    LauncherApp *app =
        launcher_row_app(
            gtk_list_box_get_selected_row(
                launcher->results
            )
        );

    if (app) {
        printf(
            "LAUNCHER_KEY move=%s selected=%s\n",
            delta > 0 ? "down" : "up",
            app->id
        );
    }
}

static gboolean launcher_key_pressed(
        GtkEventControllerKey *controller,
        guint keyval,
        guint keycode,
        GdkModifierType state,
        gpointer data)
{
    Launcher *launcher = data;

    (void)controller;
    (void)keycode;
    (void)state;

    switch (keyval) {
    case GDK_KEY_Escape:
        printf("LAUNCHER_KEY key=Escape\n");
        launcher_hide(
            launcher,
            "escape"
        );
        return GDK_EVENT_STOP;

    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
        launcher_move_selection(
            launcher,
            1
        );
        return GDK_EVENT_STOP;

    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
        launcher_move_selection(
            launcher,
            -1
        );
        return GDK_EVENT_STOP;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter: {
        GtkListBoxRow *row =
            gtk_list_box_get_selected_row(
                launcher->results
            );
        LauncherApp *app =
            launcher_row_app(row);

        printf("LAUNCHER_KEY key=Enter\n");

        if (app) {
            launcher_launch_app(
                launcher,
                app
            );
        }

        return GDK_EVENT_STOP;
    }

    default:
        return GDK_EVENT_PROPAGATE;
    }
}

static gboolean launcher_close_request(
        GtkWindow *window,
        gpointer data)
{
    Launcher *launcher = data;

    (void)window;

    launcher_hide(
        launcher,
        "close-request"
    );

    return TRUE;
}

static void launcher_apply_css(void)
{
    static const char css[] =
        "window#onyrion-launcher {"
        "  background: rgba(18,18,20,0.97);"
        "  border-radius: 14px;"
        "}"
        "#onyrion-launcher-root {"
        "  padding: 18px;"
        "}"
        "entry {"
        "  min-height: 42px;"
        "  font-size: 16px;"
        "}"
        "list {"
        "  background: transparent;"
        "}"
        "row {"
        "  border-radius: 8px;"
        "}"
        "row:selected {"
        "  background: rgba(242,242,243,0.18);"
        "}";

    GtkCssProvider *provider =
        gtk_css_provider_new();

    gtk_css_provider_load_from_string(
        provider,
        css
    );

    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    g_object_unref(provider);
}

static gboolean launcher_build_window(
        Launcher *launcher)
{
    GtkWidget *root;
    GtkWidget *scroll;
    GtkEventController *keys;

    if (!gtk_layer_is_supported()) {
        fprintf(
            stderr,
            "LAUNCHER_FAIL layer-shell unsupported\n"
        );
        return FALSE;
    }

    launcher->window =
        GTK_WINDOW(gtk_window_new());

    gtk_widget_set_name(
        GTK_WIDGET(launcher->window),
        "onyrion-launcher"
    );

    gtk_window_set_title(
        launcher->window,
        "Onyrion Launcher"
    );

    gtk_window_set_default_size(
        launcher->window,
        640,
        460
    );

    gtk_window_set_decorated(
        launcher->window,
        FALSE
    );

    gtk_layer_init_for_window(
        launcher->window
    );

    gtk_layer_set_namespace(
        launcher->window,
        "onyrion-launcher"
    );

    gtk_layer_set_layer(
        launcher->window,
        GTK_LAYER_SHELL_LAYER_OVERLAY
    );

    gtk_layer_set_keyboard_mode(
        launcher->window,
        GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
    );

    root = gtk_box_new(
        GTK_ORIENTATION_VERTICAL,
        12
    );

    gtk_widget_set_name(
        root,
        "onyrion-launcher-root"
    );

    launcher->search =
        GTK_SEARCH_ENTRY(gtk_search_entry_new());

    gtk_search_entry_set_placeholder_text(
        launcher->search,
        "Search applications"
    );

    gtk_box_append(
        GTK_BOX(root),
        GTK_WIDGET(launcher->search)
    );

    launcher->results =
        GTK_LIST_BOX(gtk_list_box_new());

    gtk_list_box_set_selection_mode(
        launcher->results,
        GTK_SELECTION_SINGLE
    );

    gtk_list_box_set_activate_on_single_click(
        launcher->results,
        TRUE
    );

    scroll = gtk_scrolled_window_new();

    gtk_widget_set_vexpand(
        scroll,
        TRUE
    );

    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );

    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(scroll),
        GTK_WIDGET(launcher->results)
    );

    gtk_box_append(
        GTK_BOX(root),
        scroll
    );

    gtk_window_set_child(
        launcher->window,
        root
    );

    keys = gtk_event_controller_key_new();

    gtk_event_controller_set_propagation_phase(
        keys,
        GTK_PHASE_CAPTURE
    );

    g_signal_connect(
        keys,
        "key-pressed",
        G_CALLBACK(launcher_key_pressed),
        launcher
    );

    gtk_widget_add_controller(
        GTK_WIDGET(launcher->window),
        keys
    );

    g_signal_connect(
        launcher->search,
        "search-changed",
        G_CALLBACK(launcher_search_changed),
        launcher
    );

    g_signal_connect(
        launcher->results,
        "row-activated",
        G_CALLBACK(launcher_row_activated),
        launcher
    );

    g_signal_connect(
        launcher->window,
        "close-request",
        G_CALLBACK(launcher_close_request),
        launcher
    );

    launcher_apply_css();
    launcher_rebuild_results(
        launcher,
        ""
    );

    return TRUE;
}

static char *launcher_socket_path(void)
{
    const char *runtime =
        g_get_user_runtime_dir();

    if (!runtime || !*runtime) {
        return NULL;
    }

    return g_build_filename(
        runtime,
        "onyrion-launcher.sock",
        NULL
    );
}

static gboolean launcher_send_command(
        const char *command,
        gboolean quiet)
{
    g_autofree char *path =
        launcher_socket_path();
    GSocketClient *client;
    GSocketAddress *address;
    GSocketConnection *connection;
    GOutputStream *output;
    GInputStream *input;
    GError *error = NULL;
    char response[32] = {0};
    gsize written = 0;
    gssize received;
    gboolean ok = FALSE;

    if (!path) {
        if (!quiet) {
            fprintf(
                stderr,
                "launcher: XDG_RUNTIME_DIR unavailable\n"
            );
        }
        return FALSE;
    }

    client = g_socket_client_new();
    address =
        g_unix_socket_address_new(path);

    connection =
        g_socket_client_connect(
            client,
            G_SOCKET_CONNECTABLE(address),
            NULL,
            &error
        );

    g_object_unref(address);
    g_object_unref(client);

    if (!connection) {
        if (!quiet) {
            fprintf(
                stderr,
                "launcher: daemon unavailable: %s\n",
                error ? error->message : "unknown"
            );
        }
        g_clear_error(&error);
        return FALSE;
    }

    output =
        g_io_stream_get_output_stream(
            G_IO_STREAM(connection)
        );

    if (!g_output_stream_write_all(
            output,
            command,
            strlen(command),
            &written,
            NULL,
            &error) ||
        written != strlen(command)) {
        if (!quiet) {
            fprintf(
                stderr,
                "launcher: command write failed: %s\n",
                error ? error->message : "unknown"
            );
        }
        g_clear_error(&error);
        g_object_unref(connection);
        return FALSE;
    }

    g_output_stream_flush(
        output,
        NULL,
        NULL
    );

    input =
        g_io_stream_get_input_stream(
            G_IO_STREAM(connection)
        );

    received =
        g_input_stream_read(
            input,
            response,
            sizeof(response) - 1,
            NULL,
            &error
        );

    if (received > 0) {
        response[received] = '\0';
        ok = g_str_has_prefix(
            response,
            "OK"
        );
    } else if (!quiet) {
        fprintf(
            stderr,
            "launcher: command response failed: %s\n",
            error ? error->message : "EOF"
        );
    }

    g_clear_error(&error);
    g_object_unref(connection);

    return ok;
}

static gboolean launcher_socket_incoming(
        GSocketService *service,
        GSocketConnection *connection,
        GObject *source_object,
        gpointer data)
{
    Launcher *launcher = data;
    GInputStream *input =
        g_io_stream_get_input_stream(
            G_IO_STREAM(connection)
        );
    GOutputStream *output =
        g_io_stream_get_output_stream(
            G_IO_STREAM(connection)
        );
    char command[32] = {0};
    GError *error = NULL;
    gssize received;
    const char *response = "OK\n";

    (void)service;
    (void)source_object;

    received =
        g_input_stream_read(
            input,
            command,
            sizeof(command) - 1,
            NULL,
            &error
        );

    if (received <= 0) {
        g_clear_error(&error);
        return TRUE;
    }

    command[received] = '\0';
    g_strstrip(command);

    if (g_strcmp0(command, "ping") == 0) {
        /* no-op */
    } else if (g_strcmp0(command, "open") == 0) {
        launcher_show(launcher);
    } else if (g_strcmp0(command, "close") == 0) {
        launcher_hide(
            launcher,
            "control"
        );
    } else if (g_strcmp0(command, "toggle") == 0) {
        if (gtk_widget_get_visible(
                GTK_WIDGET(launcher->window))) {
            launcher_hide(
                launcher,
                "toggle"
            );
        } else {
            launcher_show(launcher);
        }
    } else {
        response = "ERR unknown-command\n";
    }

    g_output_stream_write_all(
        output,
        response,
        strlen(response),
        NULL,
        NULL,
        NULL
    );

    g_output_stream_flush(
        output,
        NULL,
        NULL
    );

    return TRUE;
}

static gboolean launcher_stop_signal(gpointer data)
{
    Launcher *launcher = data;

    g_main_loop_quit(
        launcher->loop
    );

    return G_SOURCE_REMOVE;
}

static int launcher_daemon(void)
{
    Launcher launcher = {0};
    GSocketAddress *address;
    GError *error = NULL;
    gboolean bound;

    if (launcher_send_command(
            "ping\n",
            TRUE)) {
        fprintf(
            stderr,
            "launcher: daemon already running\n"
        );
        return 17;
    }

    launcher.socket_path =
        launcher_socket_path();

    if (!launcher.socket_path) {
        fprintf(
            stderr,
            "launcher: XDG_RUNTIME_DIR unavailable\n"
        );
        return 18;
    }

    unlink(launcher.socket_path);

    gtk_init();

    launcher.apps =
        launcher_load_apps();

    launcher.loop =
        g_main_loop_new(
            NULL,
            FALSE
        );

    if (!launcher_build_window(
            &launcher)) {
        g_ptr_array_unref(launcher.apps);
        g_main_loop_unref(launcher.loop);
        g_free(launcher.socket_path);
        return 19;
    }

    launcher.service =
        g_socket_service_new();

    address =
        g_unix_socket_address_new(
            launcher.socket_path
        );

    bound =
        g_socket_listener_add_address(
            G_SOCKET_LISTENER(launcher.service),
            address,
            G_SOCKET_TYPE_STREAM,
            G_SOCKET_PROTOCOL_DEFAULT,
            NULL,
            NULL,
            &error
        );

    g_object_unref(address);

    if (!bound) {
        fprintf(
            stderr,
            "launcher: socket bind failed: %s\n",
            error ? error->message : "unknown"
        );
        g_clear_error(&error);
        g_clear_object(&launcher.service);
        g_ptr_array_unref(launcher.apps);
        g_main_loop_unref(launcher.loop);
        g_free(launcher.socket_path);
        return 20;
    }

    chmod(
        launcher.socket_path,
        S_IRUSR | S_IWUSR
    );

    g_signal_connect(
        launcher.service,
        "incoming",
        G_CALLBACK(launcher_socket_incoming),
        &launcher
    );

    g_socket_service_start(
        launcher.service
    );

    g_unix_signal_add(
        SIGTERM,
        launcher_stop_signal,
        &launcher
    );

    g_unix_signal_add(
        SIGINT,
        launcher_stop_signal,
        &launcher
    );

    printf(
        "LAUNCHER_DAEMON_READY socket=%s apps=%u\n",
        launcher.socket_path,
        launcher.apps->len
    );

    g_main_loop_run(
        launcher.loop
    );

    g_socket_service_stop(
        launcher.service
    );

    g_clear_object(&launcher.service);
    gtk_window_destroy(
        launcher.window
    );
    launcher.window = NULL;
    g_ptr_array_unref(launcher.apps);
    g_main_loop_unref(launcher.loop);
    unlink(launcher.socket_path);
    g_free(launcher.socket_path);

    printf("LAUNCHER_DAEMON_STOP\n");

    return 0;
}

static void usage(FILE *stream)
{
    fprintf(
        stream,
        "usage: onyrion-launcher daemon|ping|open|close|toggle\n"
    );
}

int main(int argc, char **argv)
{
    const char *command;

    setvbuf(
        stdout,
        NULL,
        _IONBF,
        0
    );

    setvbuf(
        stderr,
        NULL,
        _IONBF,
        0
    );

    if (argc != 2) {
        usage(stderr);
        return 64;
    }

    command = argv[1];

    if (g_strcmp0(command, "daemon") == 0) {
        return launcher_daemon();
    }

    if (g_strcmp0(command, "ping") == 0 ||
        g_strcmp0(command, "open") == 0 ||
        g_strcmp0(command, "close") == 0 ||
        g_strcmp0(command, "toggle") == 0) {
        g_autofree char *wire =
            g_strdup_printf(
                "%s\n",
                command
            );

        return launcher_send_command(
            wire,
            FALSE
        )
            ? 0
            : 1;
    }

    usage(stderr);
    return 64;
}
