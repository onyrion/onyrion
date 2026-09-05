use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    process::Command,
    rc::Rc,
    sync::{Arc, Mutex},
    thread,
    time::Duration,
};

use ewwii_plugin_api::{gtk4, EwwiiAPI, NativeFn, NativeFnExt, NbclType, PluginValue};
use gtk4::prelude::*;
use serde::Deserialize;

const BLUETOOTH_WIDGET: &str = "onyrion-bluetooth-dynamic";
const BLUETOOTH_STATUS_SIGNAL: &str = "onyrion_bluetooth_renderer_status";

#[derive(Clone, Debug, Default, Deserialize)]
struct ControlState {
    #[serde(default)]
    bluetooth: BluetoothState,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct BluetoothState {
    #[serde(default)]
    available: bool,
    #[serde(default)]
    powered: bool,
    #[serde(default)]
    alias: Option<String>,
    #[serde(default)]
    display: Option<String>,
    #[serde(default)]
    devices: Vec<BluetoothDevice>,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct BluetoothDevice {
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    address: Option<String>,
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    connected: bool,
    #[serde(default)]
    paired: bool,
    #[serde(default)]
    trusted: bool,
}

#[derive(Clone, Debug, Default)]
struct PendingBluetoothState {
    state: Option<BluetoothState>,
    generation: u64,
    applied_generation: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum BluetoothAction {
    Connect(String),
    Disconnect(String),
    Pair(String),
}

struct DeviceRow {
    button: gtk4::Button,
    action: Rc<RefCell<Option<BluetoothAction>>>,
}

struct BluetoothUi {
    root: gtk4::Box,
    state_label: gtk4::Label,
    devices_box: gtk4::Box,
    rows: HashMap<String, DeviceRow>,
}

impl BluetoothUi {
    fn new(host: Arc<dyn EwwiiAPI>) -> Self {
        let root = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        let state_label = gtk4::Label::new(Some("Bluetooth · loading"));
        state_label.set_halign(gtk4::Align::Start);
        state_label.add_css_class("onyrion-wifi-spaced");

        let scan = gtk4::Button::with_label("Scan 6s");
        scan.add_css_class("onyrion-shell-control");
        scan.add_css_class("onyrion-wifi-spaced");
        scan.connect_clicked(move |_| {
            spawn_control(
                host.clone(),
                vec![
                    "bluetooth".to_string(),
                    "scan".to_string(),
                    "6".to_string(),
                ],
            );
        });

        let devices_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        root.append(&state_label);
        root.append(&scan);
        root.append(&devices_box);

        Self {
            root,
            state_label,
            devices_box,
            rows: HashMap::new(),
        }
    }

    fn apply(&mut self, host: &Arc<dyn EwwiiAPI>, state: &BluetoothState) {
        self.state_label.set_text(&state_label(state));
        reconcile_devices(host, &self.devices_box, &mut self.rows, &state.devices);
    }
}

fn nonempty(value: Option<&str>) -> Option<&str> {
    value.filter(|value| !value.trim().is_empty())
}

fn state_label(state: &BluetoothState) -> String {
    if let Some(display) = nonempty(state.display.as_deref()) {
        return display.to_string();
    }
    if !state.available {
        "Bluetooth · unavailable".to_string()
    } else if !state.powered {
        "Bluetooth · off".to_string()
    } else {
        "Bluetooth · on".to_string()
    }
}

fn device_key(device: &BluetoothDevice) -> Option<String> {
    nonempty(device.address.as_deref())
        .or_else(|| nonempty(device.path.as_deref()))
        .map(str::to_string)
}

fn device_address(device: &BluetoothDevice) -> Option<String> {
    nonempty(device.address.as_deref()).map(str::to_string)
}

fn device_name(device: &BluetoothDevice) -> String {
    nonempty(device.name.as_deref())
        .or_else(|| nonempty(device.address.as_deref()))
        .or_else(|| nonempty(device.path.as_deref()))
        .unwrap_or("Bluetooth device")
        .to_string()
}

fn device_label(device: &BluetoothDevice) -> String {
    let name = device_name(device);
    let address = nonempty(device.address.as_deref()).unwrap_or("unknown");

    if device.connected {
        format!("{name} · connected · {address}")
    } else if device.paired {
        format!("{name} · paired · {address}")
    } else {
        format!("{name} · available · {address}")
    }
}

fn device_action(device: &BluetoothDevice) -> Option<BluetoothAction> {
    let address = device_address(device)?;
    if device.connected {
        Some(BluetoothAction::Disconnect(address))
    } else if device.paired {
        Some(BluetoothAction::Connect(address))
    } else {
        Some(BluetoothAction::Pair(address))
    }
}

fn control_bin() -> String {
    std::env::var("ONYRION_CONTROL_BIN")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| "onyrion-control".to_string())
}

fn spawn_control(host: Arc<dyn EwwiiAPI>, args: Vec<String>) {
    thread::spawn(move || {
        let command = control_bin();
        match Command::new(&command).args(&args).status() {
            Ok(status) if status.success() => {
                host.log(&format!(
                    "BLUETOOTH_RENDERER_ACTION status=PASS command={} args={}",
                    command,
                    args.join(" ")
                ));
            }
            Ok(status) => {
                host.error(&format!(
                    "BLUETOOTH_RENDERER_ACTION status=FAIL command={} rc={:?} args={}",
                    command,
                    status.code(),
                    args.join(" ")
                ));
            }
            Err(error) => {
                host.error(&format!(
                    "BLUETOOTH_RENDERER_ACTION status=FAIL command={} error={} args={}",
                    command,
                    error,
                    args.join(" ")
                ));
            }
        }
    });
}

fn run_action(host: Arc<dyn EwwiiAPI>, action: BluetoothAction) {
    match action {
        BluetoothAction::Connect(address) => spawn_control(
            host,
            vec![
                "bluetooth".to_string(),
                "device".to_string(),
                "connect".to_string(),
                address,
            ],
        ),
        BluetoothAction::Disconnect(address) => spawn_control(
            host,
            vec![
                "bluetooth".to_string(),
                "device".to_string(),
                "disconnect".to_string(),
                address,
            ],
        ),
        BluetoothAction::Pair(address) => spawn_control(
            host,
            vec![
                "bluetooth".to_string(),
                "device".to_string(),
                "pair".to_string(),
                address,
            ],
        ),
    }
}

fn create_device_row(host: Arc<dyn EwwiiAPI>, device: &BluetoothDevice) -> DeviceRow {
    let button = gtk4::Button::with_label(&device_label(device));
    button.add_css_class("onyrion-wifi-slot-live");
    button.add_css_class("onyrion-wifi-spaced");

    let action = Rc::new(RefCell::new(device_action(device)));
    button.set_sensitive(action.borrow().is_some());

    let click_action = action.clone();
    button.connect_clicked(move |_| {
        let action = click_action.borrow().clone();
        if let Some(action) = action {
            run_action(host.clone(), action);
        }
    });

    DeviceRow { button, action }
}

fn update_device_row(row: &DeviceRow, device: &BluetoothDevice) {
    row.button.set_label(&device_label(device));
    let action = device_action(device);
    row.button.set_sensitive(action.is_some());
    row.action.replace(action);
}

fn reconcile_devices(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<String, DeviceRow>,
    devices: &[BluetoothDevice],
) {
    let wanted: HashSet<String> = devices.iter().filter_map(device_key).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|key| !wanted.contains(*key))
        .cloned()
        .collect();

    for key in stale {
        if let Some(row) = rows.remove(&key) {
            container.remove(&row.button);
        }
    }

    for device in devices {
        let Some(key) = device_key(device) else {
            continue;
        };

        if let Some(row) = rows.get(&key) {
            update_device_row(row, device);
            continue;
        }

        let row = create_device_row(host.clone(), device);
        container.append(&row.button);
        rows.insert(key, row);
    }
}

fn publish_bluetooth(
    host: &Arc<dyn EwwiiAPI>,
    pending: &Arc<Mutex<PendingBluetoothState>>,
    raw: &str,
) {
    if raw.trim().is_empty() {
        return;
    }

    match serde_json::from_str::<ControlState>(raw) {
        Ok(state) => {
            let mut pending = pending
                .lock()
                .expect("bluetooth renderer state mutex poisoned");
            pending.state = Some(state.bluetooth);
            pending.generation = pending.generation.wrapping_add(1).max(1);
        }
        Err(error) => {
            host.error(&format!(
                "BLUETOOTH_RENDERER_PARSE status=FAIL bytes={} error={}",
                raw.len(),
                error
            ));
        }
    }
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    let pending = Arc::new(Mutex::new(PendingBluetoothState::default()));
    let ui = Rc::new(RefCell::new(BluetoothUi::new(host.clone())));

    let widget = ui.borrow().root.clone().upcast::<gtk4::Widget>();
    host.register_static_widget(BLUETOOTH_WIDGET, widget);
    host.register_signal(
        BLUETOOTH_STATUS_SIGNAL,
        "{\"ready\":false,\"generation\":0,\"devices\":0,\"available\":false,\"powered\":false}"
            .to_string(),
    );

    let timer_host = host.clone();
    let timer_pending = pending.clone();
    let timer_ui = ui;
    gtk4::glib::timeout_add_local(Duration::from_millis(50), move || {
        let snapshot = {
            let mut pending = timer_pending
                .lock()
                .expect("bluetooth renderer state mutex poisoned");

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
                BLUETOOTH_STATUS_SIGNAL,
                serde_json::json!({
                    "ready": true,
                    "generation": generation,
                    "devices": actual_rows,
                    "input_devices": state.devices.len(),
                    "available": state.available,
                    "powered": state.powered,
                    "alias": state.alias,
                })
                .to_string(),
            );

            timer_host.log(&format!(
                "BLUETOOTH_RENDERER_STATE generation={} available={} powered={} input_devices={} rows={}",
                generation,
                state.available,
                state.powered,
                state.devices.len(),
                actual_rows
            ));
        }

        gtk4::glib::ControlFlow::Continue
    });

    let ingest_host = host.clone();
    let ingest_pending = pending;
    host.register_function(
        "onyrion_bluetooth_ingest",
        vec![NbclType::String],
        NbclType::String,
        NativeFn::new(move |args| {
            let PluginValue::String(ref raw) = args[0] else {
                unreachable!("onyrion_bluetooth_ingest requires String")
            };
            publish_bluetooth(&ingest_host, &ingest_pending, raw);
            Ok(PluginValue::String(
                "onyrion-wifi-slot-hidden".to_string(),
            ))
        }),
    );

    host.log("BLUETOOTH_RENDERER_INGEST status=REGISTERED mode=nbcl-native");
    host.log("BLUETOOTH_RENDERER_STATIC_WIDGET status=REGISTERED");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn device(connected: bool, paired: bool) -> BluetoothDevice {
        BluetoothDevice {
            path: Some("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF".to_string()),
            address: Some("AA:BB:CC:DD:EE:FF".to_string()),
            name: Some("Test Device".to_string()),
            connected,
            paired,
            trusted: false,
        }
    }

    #[test]
    fn parses_current_control_watch_schema() {
        let state: ControlState = serde_json::from_str(
            r#"{
                "wifi":{"available":false},
                "bluetooth":{
                    "available":true,
                    "powered":true,
                    "alias":"oniilap",
                    "display":"Bluetooth · on",
                    "devices":[{
                        "path":"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF",
                        "address":"AA:BB:CC:DD:EE:FF",
                        "name":"Test Device",
                        "connected":false,
                        "paired":true,
                        "trusted":true
                    }]
                }
            }"#,
        )
        .expect("control state");

        assert!(state.bluetooth.available);
        assert!(state.bluetooth.powered);
        assert_eq!(state.bluetooth.devices.len(), 1);
        assert_eq!(
            state.bluetooth.devices[0].address.as_deref(),
            Some("AA:BB:CC:DD:EE:FF"),
        );
    }

    #[test]
    fn labels_preserve_legacy_semantics() {
        assert_eq!(
            device_label(&device(true, true)),
            "Test Device · connected · AA:BB:CC:DD:EE:FF"
        );
        assert_eq!(
            device_label(&device(false, true)),
            "Test Device · paired · AA:BB:CC:DD:EE:FF"
        );
        assert_eq!(
            device_label(&device(false, false)),
            "Test Device · available · AA:BB:CC:DD:EE:FF"
        );
    }

    #[test]
    fn actions_preserve_legacy_semantics() {
        assert_eq!(
            device_action(&device(true, true)),
            Some(BluetoothAction::Disconnect(
                "AA:BB:CC:DD:EE:FF".to_string()
            ))
        );
        assert_eq!(
            device_action(&device(false, true)),
            Some(BluetoothAction::Connect(
                "AA:BB:CC:DD:EE:FF".to_string()
            ))
        );
        assert_eq!(
            device_action(&device(false, false)),
            Some(BluetoothAction::Pair(
                "AA:BB:CC:DD:EE:FF".to_string()
            ))
        );
    }

    #[test]
    fn state_label_prefers_authoritative_display() {
        let state = BluetoothState {
            available: true,
            powered: false,
            display: Some("Bluetooth · exact".to_string()),
            ..BluetoothState::default()
        };
        assert_eq!(state_label(&state), "Bluetooth · exact");
    }
}
