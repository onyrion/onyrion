use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    io::{BufRead, BufReader},
    process::{Child, ChildStdin, Command, Stdio},
    rc::Rc,
    sync::{
        atomic::{AtomicU8, Ordering},
        Arc, Mutex,
    },
    thread,
    time::Duration,
};

use ewwii_plugin_api::{gtk4, EwwiiAPI, NativeFn, NativeFnExt, NbclType, PluginValue};
use gtk4::prelude::*;
use serde::Deserialize;

const BLUETOOTH_WIDGET: &str = "onyrion-bluetooth-dynamic";
const BLUETOOTH_STATUS_SIGNAL: &str = "onyrion_bluetooth_renderer_status";
const BLUETOOTH_SCAN_TICK_MS: u64 = 100;
const BLUETOOTH_SCAN_PERIOD_TICKS: u32 = 60;
const BT_SCAN_STARTING: u8 = 0;
const BT_SCAN_READY: u8 = 1;
const BT_SCAN_FAILED: u8 = 2;

struct BluetoothScanSession {
    child: Child,
    stdin: Option<ChildStdin>,
    state: Arc<AtomicU8>,
}

fn start_scan_session(host: Arc<dyn EwwiiAPI>) -> Result<BluetoothScanSession, String> {
    let command = control_bin();
    let mut child = Command::new(&command)
        .args(["bluetooth", "scan", "hold"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("command={} error={}", command, error))?;

    let stdin = child.stdin.take().ok_or_else(|| "missing child stdin".to_string())?;
    let stdout = child.stdout.take().ok_or_else(|| "missing child stdout".to_string())?;
    let stderr = child.stderr.take().ok_or_else(|| "missing child stderr".to_string())?;
    let state = Arc::new(AtomicU8::new(BT_SCAN_STARTING));

    let stdout_state = state.clone();
    let stdout_host = host.clone();
    thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            stdout_host.log(&format!("BLUETOOTH_SCAN_CHILD stdout={}", line));
            if line.contains("CONTROL_OK bluetooth scan state=started mode=hold") {
                stdout_state.store(BT_SCAN_READY, Ordering::Release);
            }
        }
    });

    let stderr_state = state.clone();
    let stderr_host = host;
    thread::spawn(move || {
        for line in BufReader::new(stderr).lines().map_while(Result::ok) {
            stderr_host.error(&format!("BLUETOOTH_SCAN_CHILD stderr={}", line));
            if line.contains("CONTROL_FAIL bluetooth scan") {
                stderr_state.store(BT_SCAN_FAILED, Ordering::Release);
            }
        }
    });

    Ok(BluetoothScanSession {
        child,
        stdin: Some(stdin),
        state,
    })
}

fn stop_scan_session(host: Arc<dyn EwwiiAPI>, mut session: BluetoothScanSession, reason: &str) {
    session.stdin.take();
    let reason = reason.to_string();
    thread::spawn(move || match session.child.wait() {
        Ok(status) if status.success() => host.log(&format!(
            "BLUETOOTH_SCAN_SESSION status=STOPPED reason={} rc=0",
            reason
        )),
        Ok(status) => host.error(&format!(
            "BLUETOOTH_SCAN_SESSION status=FAIL reason={} rc={:?}",
            reason,
            status.code()
        )),
        Err(error) => host.error(&format!(
            "BLUETOOTH_SCAN_SESSION status=FAIL reason={} wait={}",
            reason, error
        )),
    });
}

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
    PairConnect(String),
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
        root.set_hexpand(true);
        root.set_halign(gtk4::Align::Fill);
        root.set_vexpand(false);

        let state_label = gtk4::Label::new(Some("Bluetooth · loading"));
        state_label.set_halign(gtk4::Align::Start);
        state_label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
        state_label.set_max_width_chars(36);
        state_label.add_css_class("onyrion-wifi-spaced");

        let scan = gtk4::Button::with_label("Scan");
        scan.add_css_class("onyrion-shell-control");
        scan.add_css_class("onyrion-wifi-spaced");

        let progress = gtk4::ProgressBar::new();
        progress.add_css_class("onyrion-scan-progress");
        progress.set_fraction(0.0);

        let scan_tick = Rc::new(Cell::new(0u32));
        let scan_timer: Rc<RefCell<Option<gtk4::glib::SourceId>>> =
            Rc::new(RefCell::new(None));
        let scan_session: Rc<RefCell<Option<BluetoothScanSession>>> =
            Rc::new(RefCell::new(None));

        {
            let click_host = host.clone();
            let click_tick = scan_tick.clone();
            let click_timer = scan_timer.clone();
            let click_session = scan_session.clone();
            let click_progress = progress.clone();
            scan.connect_clicked(move |button| {
                if let Some(session) = click_session.borrow_mut().take() {
                    click_tick.set(0);
                    if let Some(id) = click_timer.borrow_mut().take() {
                        id.remove();
                    }
                    button.set_label("Scan");
                    click_progress.set_fraction(0.0);
                    stop_scan_session(click_host.clone(), session, "toggle-off");
                    return;
                }

                let session = match start_scan_session(click_host.clone()) {
                    Ok(session) => session,
                    Err(error) => {
                        click_host.error(&format!(
                            "BLUETOOTH_SCAN_SESSION status=FAIL phase=spawn {}",
                            error
                        ));
                        button.set_label("Scan · failed");
                        click_progress.set_fraction(0.0);
                        return;
                    }
                };

                click_tick.set(0);
                button.set_label("Scan · starting");
                click_progress.set_fraction(0.0);
                *click_session.borrow_mut() = Some(session);

                let timer_host = click_host.clone();
                let timer_tick = click_tick.clone();
                let timer_button = button.clone();
                let timer_progress = click_progress.clone();
                let timer_session = click_session.clone();
                let timer_slot = click_timer.clone();
                let timer_id = gtk4::glib::timeout_add_local(
                    Duration::from_millis(BLUETOOTH_SCAN_TICK_MS),
                    move || {
                        let (state, exited) = {
                            let mut slot = timer_session.borrow_mut();
                            let Some(session) = slot.as_mut() else {
                                return gtk4::glib::ControlFlow::Break;
                            };
                            let state = session.state.load(Ordering::Acquire);
                            let exited = match session.child.try_wait() {
                                Ok(status) => status,
                                Err(error) => {
                                    timer_host.error(&format!(
                                        "BLUETOOTH_SCAN_SESSION status=FAIL phase=try-wait error={}",
                                        error
                                    ));
                                    None
                                }
                            };
                            (state, exited)
                        };

                        if let Some(status) = exited {
                            timer_session.borrow_mut().take();
                            timer_tick.set(0);
                            timer_button.set_label("Scan");
                            timer_progress.set_fraction(0.0);
                            timer_slot.borrow_mut().take();
                            if !status.success() {
                                timer_host.error(&format!(
                                    "BLUETOOTH_SCAN_SESSION status=FAIL phase=running rc={:?}",
                                    status.code()
                                ));
                            }
                            return gtk4::glib::ControlFlow::Break;
                        }

                        if state == BT_SCAN_FAILED {
                            if let Some(session) = timer_session.borrow_mut().take() {
                                stop_scan_session(timer_host.clone(), session, "start-failed");
                            }
                            timer_tick.set(0);
                            timer_button.set_label("Scan · failed");
                            timer_progress.set_fraction(0.0);
                            timer_slot.borrow_mut().take();
                            return gtk4::glib::ControlFlow::Break;
                        }

                        if state != BT_SCAN_READY {
                            timer_button.set_label("Scan · starting");
                            return gtk4::glib::ControlFlow::Continue;
                        }

                        let next = (timer_tick.get() + 1).min(BLUETOOTH_SCAN_PERIOD_TICKS);
                        timer_tick.set(next);
                        timer_progress
                            .set_fraction(next as f64 / BLUETOOTH_SCAN_PERIOD_TICKS as f64);
                        let remaining =
                            ((BLUETOOTH_SCAN_PERIOD_TICKS - next) * BLUETOOTH_SCAN_TICK_MS as u32 + 999)
                                / 1000;
                        if next >= BLUETOOTH_SCAN_PERIOD_TICKS {
                            timer_button.set_label("Scan · on");
                        } else {
                            timer_button.set_label(&format!("Scan · {}s", remaining.max(1)));
                        }
                        gtk4::glib::ControlFlow::Continue
                    },
                );
                *click_timer.borrow_mut() = Some(timer_id);
            });
        }

        {
            let unmap_host = host.clone();
            let unmap_tick = scan_tick.clone();
            let unmap_timer = scan_timer.clone();
            let unmap_session = scan_session.clone();
            let unmap_button = scan.clone();
            let unmap_progress = progress.clone();
            root.connect_unmap(move |_| {
                unmap_tick.set(0);
                if let Some(id) = unmap_timer.borrow_mut().take() {
                    id.remove();
                }
                unmap_button.set_label("Scan");
                unmap_progress.set_fraction(0.0);
                if let Some(session) = unmap_session.borrow_mut().take() {
                    stop_scan_session(unmap_host.clone(), session, "panel-unmap");
                }
            });
        }

        let devices_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
        devices_box.set_hexpand(true);

        let devices_scroll = gtk4::ScrolledWindow::new();
        devices_scroll.set_hexpand(true);
        devices_scroll.set_halign(gtk4::Align::Fill);
        devices_scroll.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::Automatic);
        devices_scroll.set_propagate_natural_height(true);
        devices_scroll.set_max_content_height(280);
        devices_scroll.set_vexpand(false);
        devices_scroll.set_child(Some(&devices_box));

        root.append(&state_label);
        root.append(&scan);
        root.append(&progress);
        root.append(&devices_scroll);

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
        Some(BluetoothAction::PairConnect(address))
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
        BluetoothAction::PairConnect(address) => {
            thread::spawn(move || {
                let command = control_bin();
                let pair = Command::new(&command)
                    .args(["bluetooth", "device", "pair", address.as_str()])
                    .status();
                match pair {
                    Ok(status) if status.success() => {}
                    Ok(status) => {
                        host.error(&format!(
                            "BLUETOOTH_RENDERER_ACTION status=FAIL command={} action=pair rc={:?} address={}",
                            command, status.code(), address
                        ));
                        return;
                    }
                    Err(error) => {
                        host.error(&format!(
                            "BLUETOOTH_RENDERER_ACTION status=FAIL command={} action=pair error={} address={}",
                            command, error, address
                        ));
                        return;
                    }
                }

                match Command::new(&command)
                    .args(["bluetooth", "device", "connect", address.as_str()])
                    .status()
                {
                    Ok(status) if status.success() => host.log(&format!(
                        "BLUETOOTH_RENDERER_ACTION status=PASS command={} action=pair-connect address={}",
                        command, address
                    )),
                    Ok(status) => host.error(&format!(
                        "BLUETOOTH_RENDERER_ACTION status=FAIL command={} action=connect rc={:?} address={}",
                        command, status.code(), address
                    )),
                    Err(error) => host.error(&format!(
                        "BLUETOOTH_RENDERER_ACTION status=FAIL command={} action=connect error={} address={}",
                        command, error, address
                    )),
                }
            });
        }
    }
}

fn create_device_row(host: Arc<dyn EwwiiAPI>, device: &BluetoothDevice) -> DeviceRow {
    let button = gtk4::Button::with_label(&device_label(device));
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
    let label = device_label(device);
    row.button.set_label(&label);
    row.button.set_tooltip_text(Some(&label));
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
    let mut ordered: Vec<&BluetoothDevice> = devices.iter().collect();
    ordered.sort_by(|left, right| {
        right
            .connected
            .cmp(&left.connected)
            .then_with(|| right.paired.cmp(&left.paired))
            .then_with(|| device_name(left).to_lowercase().cmp(&device_name(right).to_lowercase()))
            .then_with(|| device_key(left).cmp(&device_key(right)))
    });

    let wanted: HashSet<String> = ordered.iter().filter_map(|device| device_key(device)).collect();
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

    for device in &ordered {
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

    let mut previous: Option<gtk4::Widget> = None;
    for device in ordered {
        let Some(key) = device_key(device) else {
            continue;
        };
        if let Some(row) = rows.get(&key) {
            container.reorder_child_after(&row.button, previous.as_ref());
            previous = Some(row.button.clone().upcast());
        }
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
            Some(BluetoothAction::PairConnect(
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
