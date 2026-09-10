mod notifications;
mod audio_renderer;
mod wifi_renderer;
mod bluetooth_renderer;
mod tray_renderer;
mod scalar_projection;
mod bar_renderer;
mod tabgroup_model;
mod tabgroup_renderer;
mod group_surface_role;

use std::sync::Arc;

use ewwii_plugin_api::{
    auto_plugin,
    init_gtk,
    gtk4,
    EmitInfo,
    EwwiiAPI,
    ListenHandleFn,
    ListenHandleFnExt,
    PluginInfo,
};
use gtk4::gdk::prelude::SurfaceExt;
use gtk4::prelude::*;

const PLUGIN_ID: &str = "io.onyrion.shell.ewwii";
const PLUGIN_VERSION: &str = "0.2.0";
const WALLPAPER_SIGNAL: &str = "onyrion_wallpaper";
const CURSOR_STATUS_SIGNAL: &str = "onyrion_cursor_policy_status";
const WALLPAPER_WINDOW_TITLE: &str = "Ewwii - onyrion-background";
const DEFAULT_WALLPAPER: &str = "/usr/share/onyrion/ui/ewwii/default-wallpaper.svg";

fn observe_cursor_policy(host: &Arc<dyn EwwiiAPI>, reason: &str) {
    let Some(settings) = gtk4::Settings::default() else {
        host.warn("CURSOR_POLICY status=SKIP reason=no_gtk_settings");
        return;
    };

    /*
     * Do not force XCURSOR_THEME/XCURSOR_SIZE back into GtkSettings here.
     * In the current session those environment defaults are only a fallback,
     * while GTK/portal already resolves the effective desktop cursor policy.
     * Overwriting GtkSettings made the pointer visibly change when entering
     * Ewwii surfaces.  Observe the effective GTK values instead and leave the
     * policy locally overridable through the normal GTK/desktop mechanisms.
     */
    let theme = settings
        .gtk_cursor_theme_name()
        .map(|value| value.to_string())
        .unwrap_or_default();
    let size = settings.gtk_cursor_theme_size();

    host.register_signal(
        CURSOR_STATUS_SIGNAL,
        serde_json::json!({
            "ready": true,
            "theme": theme,
            "size": size,
            "source": "gtk-effective",
        })
        .to_string(),
    );
    host.log(&format!(
        "CURSOR_POLICY status=OBSERVED reason={} theme={} size={}",
        reason, theme, size
    ));
}

fn initial_wallpaper() -> String {
    std::env::var("ONYRION_WALLPAPER")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| DEFAULT_WALLPAPER.to_string())
}

fn apply_wallpaper_input_region(host: &Arc<dyn EwwiiAPI>) {
    let empty_region = gtk4::cairo::Region::create();
    let mut matched = 0usize;
    let mut applied = 0usize;

    for widget in gtk4::Window::list_toplevels() {
        let Ok(window) = widget.downcast::<gtk4::Window>() else {
            continue;
        };

        if window.title().as_deref() != Some(WALLPAPER_WINDOW_TITLE) {
            continue;
        }

        matched += 1;

        let Some(surface) = window.surface() else {
            host.error("WALLPAPER_INPUT_REGION=FAIL reason=no_surface");
            eprintln!("[onyrion-ewwii-plugin] WALLPAPER_INPUT_REGION=FAIL reason=no_surface");
            continue;
        };

        // Empty GDK input region makes the layer surface pointer-transparent:
        // input is passed to the surface below instead of being consumed here.
        surface.set_input_region(&empty_region);
        applied += 1;
    }

    if applied > 0 {
        let msg = format!(
            "WALLPAPER_INPUT_REGION=EMPTY matched={} applied={}",
            matched, applied
        );
        host.log(&msg);
        eprintln!("[onyrion-ewwii-plugin] {msg}");
    } else if matched == 0 {
        host.warn("WALLPAPER_INPUT_REGION=SKIP reason=background_not_present");
    }
}

auto_plugin!(
    OnyrionEwwiiPlugin,
    PluginInfo::new(PLUGIN_ID, PLUGIN_VERSION),
    host,
    {
        init_gtk();

        let cursor_host = host.clone();
        gtk4::glib::idle_add_local_once(move || {
            observe_cursor_policy(&cursor_host, "plugin-idle");
        });

        host.register_signal(WALLPAPER_SIGNAL, initial_wallpaper());
        audio_renderer::init(host.clone());
        wifi_renderer::init(host.clone());
        bluetooth_renderer::init(host.clone());
        scalar_projection::init(host.clone());
        tray_renderer::init(host.clone());
        tabgroup_renderer::init();
        bar_renderer::init(host.clone());
        notifications::init(host.clone());

        let input_host = host.clone();
        host.listen(
            "ewwii-init-window",
            ListenHandleFn::new(move |_event: EmitInfo| {
                /*
                 * Ewwii invokes this callback through plugin_callback_handler.
                 * HostProxy/EwwiiAPI calls are not re-entrant through
                 * ffi_gateway while that callback is active. Defer all
                 * host-facing work to the GTK main-context idle queue.
                 */
                let deferred_host = input_host.clone();
                gtk4::glib::idle_add_local_once(move || {
                    observe_cursor_policy(&deferred_host, "ewwii-init-window");
                    apply_wallpaper_input_region(&deferred_host);
                });
            }),
        );

        eprintln!("[onyrion-ewwii-plugin] PLUGIN_READY role=wallpaper+notifications+audio-renderer+wifi-renderer+bluetooth-renderer+scalar-projection+tray-renderer+bar-renderer+tabgroup-renderer");
    }
);
