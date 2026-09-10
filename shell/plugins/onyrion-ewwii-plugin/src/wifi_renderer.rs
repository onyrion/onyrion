use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    process::{Command, Stdio},
    rc::Rc,
    sync::{Arc, Mutex},
    thread,
    time::Duration,
};

use ewwii_plugin_api::{gtk4, EwwiiAPI, NativeFn, NativeFnExt, NbclType, PluginValue};
use gtk4::prelude::*;
use serde::Deserialize;

const WIFI_WIDGET: &str = "onyrion-wifi-dynamic";
const WIFI_STATUS_SIGNAL: &str = "onyrion_wifi_renderer_status";
const WIFI_SCAN_TICK_MS: u64 = 100;
const WIFI_SCAN_PERIOD_TICKS: u32 = 50;

#[derive(Clone, Debug, Default, Deserialize)]
struct WifiState {
    #[serde(default)]
    available: bool,
    #[serde(default)]
    enabled: bool,
    #[serde(default)]
    hardware_enabled: bool,
    #[serde(default)]
    active_bssid: Option<String>,
    #[serde(default)]
    active_ssid: Option<String>,
    #[serde(default)]
    networks: Vec<WifiNetwork>,
}

#[derive(Clone, Debug, Deserialize)]
struct WifiNetwork {
    bssid: String,
    ssid: String,
    #[serde(default)]
    active: bool,
    #[serde(default)]
    saved: bool,
    #[serde(default)]
    secured: bool,
    #[serde(default)]
    strength: u64,
    #[serde(default)]
    frequency: u64,
    #[serde(default)]
    security: String,
}

#[derive(Clone, Debug, Default)]
struct PendingWifiState {
    state: Option<WifiState>,
    generation: u64,
    applied_generation: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum WifiAction {
    Connect(String),
    Disconnect,
    Prompt(String),
}

struct NetworkRow {
    button: gtk4::Button,
}

struct WifiUi {
    root: gtk4::Box,
    active_label: gtk4::Label,
    networks_box: gtk4::Box,
    rows: HashMap<String, NetworkRow>,
}

impl WifiUi {
    fn new(host: Arc<dyn EwwiiAPI>) -> Self {
        let root = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
        root.set_hexpand(true);
        root.set_halign(gtk4::Align::Fill);
        root.set_vexpand(false);

        let active_label = gtk4::Label::new(Some("Wi-Fi · loading"));
        active_label.set_halign(gtk4::Align::Start);
        active_label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
        active_label.set_max_width_chars(36);
        active_label.add_css_class("onyrion-wifi-spaced");

        let scan = gtk4::Button::with_label("Scan");
        scan.add_css_class("onyrion-shell-control");
        scan.add_css_class("onyrion-wifi-spaced");

        let progress = gtk4::ProgressBar::new();
        progress.add_css_class("onyrion-scan-progress");
        progress.set_fraction(0.0);

        let scan_active = Rc::new(Cell::new(false));
        let scan_tick = Rc::new(Cell::new(0u32));
        let scan_timer: Rc<RefCell<Option<gtk4::glib::SourceId>>> =
            Rc::new(RefCell::new(None));

        {
            let click_host = host.clone();
            let click_active = scan_active.clone();
            let click_tick = scan_tick.clone();
            let click_timer = scan_timer.clone();
            let click_progress = progress.clone();
            scan.connect_clicked(move |button| {
                if click_active.get() {
                    click_active.set(false);
                    click_tick.set(0);
                    if let Some(id) = click_timer.borrow_mut().take() {
                        id.remove();
                    }
                    button.set_label("Scan");
                    click_progress.set_fraction(0.0);
                    return;
                }

                click_active.set(true);
                click_tick.set(0);
                button.set_label("Scan · 5s");
                click_progress.set_fraction(0.0);
                spawn_control(
                    click_host.clone(),
                    vec![
                        "wifi".to_string(),
                        "networks".to_string(),
                        "scan".to_string(),
                    ],
                );

                let timer_host = click_host.clone();
                let timer_active = click_active.clone();
                let timer_tick = click_tick.clone();
                let timer_button = button.clone();
                let timer_progress = click_progress.clone();
                let timer_id = gtk4::glib::timeout_add_local(
                    Duration::from_millis(WIFI_SCAN_TICK_MS),
                    move || {
                        if !timer_active.get() {
                            return gtk4::glib::ControlFlow::Break;
                        }

                        let next = timer_tick.get() + 1;
                        if next >= WIFI_SCAN_PERIOD_TICKS {
                            timer_tick.set(0);
                            timer_progress.set_fraction(0.0);
                            timer_button.set_label("Scan · 5s");
                            spawn_control(
                                timer_host.clone(),
                                vec![
                                    "wifi".to_string(),
                                    "networks".to_string(),
                                    "scan".to_string(),
                                ],
                            );
                        } else {
                            timer_tick.set(next);
                            let fraction = next as f64 / WIFI_SCAN_PERIOD_TICKS as f64;
                            timer_progress.set_fraction(fraction);
                            let remaining =
                                ((WIFI_SCAN_PERIOD_TICKS - next) * WIFI_SCAN_TICK_MS as u32 + 999)
                                    / 1000;
                            timer_button.set_label(&format!("Scan · {}s", remaining.max(1)));
                        }

                        gtk4::glib::ControlFlow::Continue
                    },
                );
                *click_timer.borrow_mut() = Some(timer_id);
            });
        }

        {
            let unmap_active = scan_active.clone();
            let unmap_tick = scan_tick.clone();
            let unmap_timer = scan_timer.clone();
            let unmap_button = scan.clone();
            let unmap_progress = progress.clone();
            root.connect_unmap(move |_| {
                unmap_active.set(false);
                unmap_tick.set(0);
                if let Some(id) = unmap_timer.borrow_mut().take() {
                    id.remove();
                }
                unmap_button.set_label("Scan");
                unmap_progress.set_fraction(0.0);
            });
        }

        let networks_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
        networks_box.set_hexpand(true);

        let networks_scroll = gtk4::ScrolledWindow::new();
        networks_scroll.set_hexpand(true);
        networks_scroll.set_halign(gtk4::Align::Fill);
        networks_scroll.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::Automatic);
        networks_scroll.set_propagate_natural_height(true);
        networks_scroll.set_max_content_height(280);
        networks_scroll.set_vexpand(false);
        networks_scroll.set_child(Some(&networks_box));

        root.append(&active_label);
        root.append(&scan);
        root.append(&progress);
        root.append(&networks_scroll);

        Self {
            root,
            active_label,
            networks_box,
            rows: HashMap::new(),
        }
    }

    fn apply(&mut self, host: &Arc<dyn EwwiiAPI>, state: &WifiState) {
        self.active_label.set_text(&active_label(state));
        reconcile_networks(host, &self.networks_box, &mut self.rows, &state.networks);
    }
}

fn active_label(state: &WifiState) -> String {
    if !state.available {
        return "Wi-Fi · unavailable".to_string();
    }
    if !state.enabled {
        return "Wi-Fi · off".to_string();
    }
    match state
        .active_ssid
        .as_deref()
        .filter(|ssid| !ssid.trim().is_empty())
    {
        Some(ssid) => format!("Wi-Fi · {ssid}"),
        None => "Wi-Fi · disconnected".to_string(),
    }
}

fn network_label(net: &WifiNetwork) -> String {
    if net.active {
        format!(
            "{} · active · {}% · {}MHz · {}",
            net.ssid, net.strength, net.frequency, net.security
        )
    } else if net.saved {
        format!(
            "{} · saved · {}% · {}MHz · {}",
            net.ssid, net.strength, net.frequency, net.security
        )
    } else if net.secured {
        format!(
            "{} · password required · {}% · {}MHz · {}",
            net.ssid, net.strength, net.frequency, net.security
        )
    } else {
        format!(
            "{} · open · {}% · {}MHz",
            net.ssid, net.strength, net.frequency
        )
    }
}

fn network_action(net: &WifiNetwork) -> WifiAction {
    if net.active {
        WifiAction::Disconnect
    } else if net.saved || !net.secured {
        WifiAction::Connect(net.bssid.clone())
    } else {
        WifiAction::Prompt(net.bssid.clone())
    }
}

fn network_better(candidate: &WifiNetwork, current: &WifiNetwork) -> bool {
    (candidate.active, candidate.saved, candidate.strength, &candidate.bssid)
        > (current.active, current.saved, current.strength, &current.bssid)
}

fn network_key(net: &WifiNetwork) -> &str {
    if net.ssid.trim().is_empty() {
        net.bssid.as_str()
    } else {
        net.ssid.as_str()
    }
}

fn canonical_networks(networks: &[WifiNetwork]) -> Vec<&WifiNetwork> {
    let mut by_ssid: HashMap<&str, &WifiNetwork> = HashMap::new();

    for net in networks {
        match by_ssid.get(network_key(net)).copied() {
            Some(current) if !network_better(net, current) => {}
            _ => {
                by_ssid.insert(network_key(net), net);
            }
        }
    }

    let mut result: Vec<&WifiNetwork> = by_ssid.into_values().collect();
    result.sort_by(|left, right| {
        right
            .active
            .cmp(&left.active)
            .then_with(|| right.saved.cmp(&left.saved))
            .then_with(|| right.strength.cmp(&left.strength))
            .then_with(|| left.ssid.to_lowercase().cmp(&right.ssid.to_lowercase()))
            .then_with(|| left.bssid.cmp(&right.bssid))
    });
    result
}

fn control_bin() -> String {
    std::env::var("ONYRION_CONTROL_BIN")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| "onyrion-control".to_string())
}

fn wifi_prompt_bin() -> String {
    std::env::var("ONYRION_WIFI_PROMPT_BIN")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| "/usr/libexec/onyrion/onyrion-wifi-prompt".to_string())
}

fn spawn_control(host: Arc<dyn EwwiiAPI>, args: Vec<String>) {
    thread::spawn(move || {
        let command = control_bin();
        match Command::new(&command).args(&args).status() {
            Ok(status) if status.success() => {
                host.log(&format!(
                    "WIFI_RENDERER_ACTION status=PASS command={} args={}",
                    command,
                    args.join(" ")
                ));
            }
            Ok(status) => {
                host.error(&format!(
                    "WIFI_RENDERER_ACTION status=FAIL command={} rc={:?} args={}",
                    command,
                    status.code(),
                    args.join(" ")
                ));
            }
            Err(error) => {
                host.error(&format!(
                    "WIFI_RENDERER_ACTION status=FAIL command={} error={} args={}",
                    command,
                    error,
                    args.join(" ")
                ));
            }
        }
    });
}

fn spawn_prompt(host: Arc<dyn EwwiiAPI>, bssid: String) {
    thread::spawn(move || {
        let command = wifi_prompt_bin();
        match Command::new(&command)
            .arg(&bssid)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(_) => {
                host.log(&format!(
                    "WIFI_RENDERER_ACTION status=STARTED command={} bssid={}",
                    command, bssid
                ));
            }
            Err(error) => {
                host.error(&format!(
                    "WIFI_RENDERER_ACTION status=FAIL command={} error={} bssid={}",
                    command, error, bssid
                ));
            }
        }
    });
}

fn create_network_row(host: Arc<dyn EwwiiAPI>, net: &WifiNetwork) -> NetworkRow {
    let button = gtk4::Button::with_label(&network_label(net));
    button.add_css_class("onyrion-list-row");
    button.set_tooltip_text(button.label().as_deref());
    if let Some(label) = button.child().and_then(|child| child.downcast::<gtk4::Label>().ok()) {
        label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
        label.set_max_width_chars(36);
        label.set_xalign(0.0);
    }
    button.set_focusable(false);
    button.add_css_class("onyrion-wifi-slot-live");
    button.add_css_class("onyrion-wifi-spaced");

    let action = network_action(net);
    button.connect_clicked(move |_| match &action {
        WifiAction::Connect(bssid) => {
            spawn_control(
                host.clone(),
                vec!["wifi".to_string(), "connect".to_string(), bssid.clone()],
            );
        }
        WifiAction::Disconnect => {
            spawn_control(
                host.clone(),
                vec!["wifi".to_string(), "disconnect".to_string()],
            );
        }
        WifiAction::Prompt(bssid) => {
            spawn_prompt(host.clone(), bssid.clone());
        }
    });

    NetworkRow { button }
}

fn reconcile_networks(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<String, NetworkRow>,
    networks: &[WifiNetwork],
) {
    let canonical = canonical_networks(networks);
    let wanted: HashSet<&str> = canonical.iter().map(|net| network_key(net)).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|ssid| !wanted.contains(ssid.as_str()))
        .cloned()
        .collect();

    for ssid in stale {
        if let Some(row) = rows.remove(&ssid) {
            container.remove(&row.button);
        }
    }

    /* Recreate rows because the click action contains the representative BSSID.
     * The representative can change when roaming or when a stronger AP appears. */
    for net in &canonical {
        if let Some(row) = rows.remove(network_key(net)) {
            container.remove(&row.button);
        }
        let row = create_network_row(host.clone(), net);
        container.append(&row.button);
        rows.insert(network_key(net).to_string(), row);
    }

    let mut previous: Option<gtk4::Widget> = None;
    for net in canonical {
        if let Some(row) = rows.get(network_key(net)) {
            container.reorder_child_after(&row.button, previous.as_ref());
            previous = Some(row.button.clone().upcast());
        }
    }
}

fn publish_wifi(host: &Arc<dyn EwwiiAPI>, pending: &Arc<Mutex<PendingWifiState>>, raw: &str) {
    if raw.trim().is_empty() {
        return;
    }

    match serde_json::from_str::<WifiState>(raw) {
        Ok(state) => {
            let mut pending = pending.lock().expect("wifi renderer state mutex poisoned");
            pending.state = Some(state);
            pending.generation = pending.generation.wrapping_add(1).max(1);
        }
        Err(error) => {
            host.error(&format!(
                "WIFI_RENDERER_PARSE status=FAIL bytes={} error={}",
                raw.len(),
                error
            ));
        }
    }
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    let pending = Arc::new(Mutex::new(PendingWifiState::default()));
    let ui = Rc::new(RefCell::new(WifiUi::new(host.clone())));

    let widget = ui.borrow().root.clone().upcast::<gtk4::Widget>();
    host.register_static_widget(WIFI_WIDGET, widget);
    host.register_signal(
        WIFI_STATUS_SIGNAL,
        "{\"ready\":false,\"generation\":0,\"networks\":0,\"available\":false,\"enabled\":false}"
            .to_string(),
    );

    let timer_host = host.clone();
    let timer_pending = pending.clone();
    let timer_ui = ui;
    gtk4::glib::timeout_add_local(Duration::from_millis(50), move || {
        let snapshot = {
            let mut pending = timer_pending
                .lock()
                .expect("wifi renderer state mutex poisoned");

            if pending.generation == pending.applied_generation {
                None
            } else {
                pending.applied_generation = pending.generation;
                pending
                    .state
                    .clone()
                    .map(|state| (state, pending.generation))
            }
        };

        if let Some((state, generation)) = snapshot {
            let actual_rows = {
                let mut ui = timer_ui.borrow_mut();
                ui.apply(&timer_host, &state);
                ui.rows.len()
            };

            timer_host.update_signal(
                WIFI_STATUS_SIGNAL,
                serde_json::json!({
                    "ready": true,
                    "generation": generation,
                    "networks": actual_rows,
                    "input_networks": state.networks.len(),
                    "available": state.available,
                    "enabled": state.enabled,
                    "hardware_enabled": state.hardware_enabled,
                    "active_bssid": state.active_bssid,
                    "active_ssid": state.active_ssid,
                })
                .to_string(),
            );

            timer_host.log(&format!(
                "WIFI_RENDERER_STATE generation={} available={} enabled={} input_networks={} rows={}",
                generation,
                state.available,
                state.enabled,
                state.networks.len(),
                actual_rows
            ));
        }

        gtk4::glib::ControlFlow::Continue
    });

    let ingest_host = host.clone();
    let ingest_pending = pending;
    host.register_function(
        "onyrion_wifi_networks_ingest",
        vec![NbclType::String],
        NbclType::String,
        NativeFn::new(move |args| {
            let PluginValue::String(ref raw) = args[0] else {
                unreachable!("onyrion_wifi_networks_ingest requires String")
            };
            publish_wifi(&ingest_host, &ingest_pending, raw);
            Ok(PluginValue::String("onyrion-wifi-slot-hidden".to_string()))
        }),
    );

    host.log("WIFI_RENDERER_INGEST status=REGISTERED mode=nbcl-native");
    host.log("WIFI_RENDERER_STATIC_WIDGET status=REGISTERED");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn network(active: bool, saved: bool, secured: bool, security: &str) -> WifiNetwork {
        WifiNetwork {
            bssid: "AA:BB:CC:DD:EE:FF".to_string(),
            ssid: "Test".to_string(),
            active,
            saved,
            secured,
            strength: 73,
            frequency: 5180,
            security: security.to_string(),
        }
    }

    #[test]
    fn state_parses_native_network_schema() {
        let state: WifiState = serde_json::from_str(
            r#"{
                "available":true,
                "enabled":true,
                "hardware_enabled":true,
                "device":"wlan0",
                "device_path":"/org/freedesktop/NetworkManager/Devices/2",
                "active_bssid":"AA:BB:CC:DD:EE:FF",
                "active_ssid":"Test",
                "last_scan":123,
                "networks":[{
                    "bssid":"AA:BB:CC:DD:EE:FF",
                    "ssid":"Test",
                    "active":true,
                    "saved":true,
                    "secured":true,
                    "strength":73,
                    "frequency":5180,
                    "security":"WPA2"
                }]
            }"#,
        )
        .expect("wifi state");

        assert_eq!(state.networks.len(), 1);
        assert_eq!(state.networks[0].bssid, "AA:BB:CC:DD:EE:FF");
        assert_eq!(active_label(&state), "Wi-Fi · Test");
    }

    #[test]
    fn labels_preserve_existing_semantics() {
        assert_eq!(
            network_label(&network(true, true, true, "WPA2")),
            "Test · active · 73% · 5180MHz · WPA2"
        );
        assert_eq!(
            network_label(&network(false, true, true, "WPA2")),
            "Test · saved · 73% · 5180MHz · WPA2"
        );
        assert_eq!(
            network_label(&network(false, false, true, "WPA2")),
            "Test · password required · 73% · 5180MHz · WPA2"
        );
        assert_eq!(
            network_label(&network(false, false, false, "")),
            "Test · open · 73% · 5180MHz"
        );
    }

    #[test]
    fn actions_preserve_connect_vs_prompt_policy() {
        assert_eq!(
            network_action(&network(true, false, true, "WPA2")),
            WifiAction::Disconnect
        );
        assert_eq!(
            network_action(&network(false, true, true, "WPA2")),
            WifiAction::Connect("AA:BB:CC:DD:EE:FF".to_string())
        );
        assert_eq!(
            network_action(&network(false, false, false, "")),
            WifiAction::Connect("AA:BB:CC:DD:EE:FF".to_string())
        );
        assert_eq!(
            network_action(&network(false, false, true, "WPA2")),
            WifiAction::Prompt("AA:BB:CC:DD:EE:FF".to_string())
        );
    }
    #[test]
    fn canonical_networks_dedup_and_rank() {
        let mut weak = network(false, false, true, "WPA2");
        weak.bssid = "00:00:00:00:00:01".to_string();
        weak.strength = 20;

        let mut strong = network(false, true, true, "WPA2");
        strong.bssid = "00:00:00:00:00:02".to_string();
        strong.strength = 80;

        let mut other = network(false, false, false, "");
        other.ssid = "Other".to_string();
        other.bssid = "00:00:00:00:00:03".to_string();
        other.strength = 90;

        let items = vec![weak, other, strong];
        let canonical = canonical_networks(&items);
        assert_eq!(canonical.len(), 2);
        assert_eq!(canonical[0].ssid, "Test");
        assert_eq!(canonical[0].bssid, "00:00:00:00:00:02");
        assert_eq!(canonical[1].ssid, "Other");
    }

}
