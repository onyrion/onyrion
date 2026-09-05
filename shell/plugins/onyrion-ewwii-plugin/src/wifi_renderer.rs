use std::{
    cell::RefCell,
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

        let active_label = gtk4::Label::new(Some("Wi-Fi · loading"));
        active_label.set_halign(gtk4::Align::Start);
        active_label.add_css_class("onyrion-wifi-spaced");

        let scan = gtk4::Button::with_label("Scan");
        scan.add_css_class("onyrion-shell-control");
        scan.add_css_class("onyrion-wifi-spaced");
        scan.connect_clicked(move |_| {
            spawn_control(
                host.clone(),
                vec![
                    "wifi".to_string(),
                    "networks".to_string(),
                    "scan".to_string(),
                ],
            );
        });

        let networks_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        root.append(&active_label);
        root.append(&scan);
        root.append(&networks_box);

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
    if net.active || net.saved || !net.secured {
        WifiAction::Connect(net.bssid.clone())
    } else {
        WifiAction::Prompt(net.bssid.clone())
    }
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
    let wanted: HashSet<&str> = networks.iter().map(|net| net.bssid.as_str()).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|bssid| !wanted.contains(bssid.as_str()))
        .cloned()
        .collect();

    for bssid in stale {
        if let Some(row) = rows.remove(&bssid) {
            container.remove(&row.button);
        }
    }

    for net in networks {
        if let Some(row) = rows.remove(&net.bssid) {
            container.remove(&row.button);
        }

        let row = create_network_row(host.clone(), net);
        container.append(&row.button);
        rows.insert(net.bssid.clone(), row);
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
            WifiAction::Connect("AA:BB:CC:DD:EE:FF".to_string())
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
}
