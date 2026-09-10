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

const AUDIO_WIDGET: &str = "onyrion-audio-dynamic";
const DEVICES_SIGNAL: &str = "onyrion_audio_devices";
const STREAMS_SIGNAL: &str = "onyrion_audio_streams";
const STATUS_SIGNAL: &str = "onyrion_audio_renderer_status";

#[derive(Clone, Debug, Default, Deserialize)]
struct AudioDevicesState {
    #[serde(default)]
    outputs: Vec<AudioDevice>,
    #[serde(default)]
    inputs: Vec<AudioDevice>,
}

#[derive(Clone, Debug, Deserialize)]
struct AudioDevice {
    name: String,
    description: String,
    #[serde(default)]
    default: bool,
    #[serde(default)]
    configured: bool,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct AudioStreamsState {
    #[serde(default)]
    playback: Vec<AudioStream>,
    #[serde(default)]
    capture: Vec<AudioStream>,
}

#[derive(Clone, Debug, Deserialize)]
struct AudioStream {
    serial: u64,
    name: String,
    #[serde(default)]
    volume: f64,
    #[serde(default)]
    muted: bool,
}

#[derive(Clone, Debug, Default)]
struct PendingAudioState {
    devices: Option<AudioDevicesState>,
    streams: Option<AudioStreamsState>,
    generation: u64,
    applied_generation: u64,
}

#[derive(Clone, Copy)]
enum DeviceKind {
    Sink,
    Source,
}

impl DeviceKind {
    fn token(self) -> &'static str {
        match self {
            Self::Sink => "sink",
            Self::Source => "source",
        }
    }
}

#[derive(Clone, Copy)]
enum StreamKind {
    Playback,
    Capture,
}

impl StreamKind {
    fn token(self) -> &'static str {
        match self {
            Self::Playback => "playback",
            Self::Capture => "capture",
        }
    }
}

struct DeviceRow {
    button: gtk4::Button,
}

struct StreamRow {
    root: gtk4::Box,
    label: gtk4::Label,
}

struct AudioUi {
    root: gtk4::ScrolledWindow,
    outputs_box: gtk4::Box,
    inputs_box: gtk4::Box,
    playback_box: gtk4::Box,
    capture_box: gtk4::Box,
    outputs: HashMap<String, DeviceRow>,
    inputs: HashMap<String, DeviceRow>,
    playback: HashMap<u64, StreamRow>,
    capture: HashMap<u64, StreamRow>,
}

impl AudioUi {
    fn new() -> Self {
        let content = gtk4::Box::new(gtk4::Orientation::Vertical, 0);
        content.set_hexpand(true);

        let outputs_title = section_label("Outputs");
        let outputs_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        let inputs_title = section_label("Inputs");
        let inputs_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        let playback_title = section_label("Playback streams");
        let playback_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        let capture_title = section_label("Capture streams");
        let capture_box = gtk4::Box::new(gtk4::Orientation::Vertical, 0);

        content.append(&outputs_title);
        content.append(&outputs_box);
        content.append(&inputs_title);
        content.append(&inputs_box);
        content.append(&playback_title);
        content.append(&playback_box);
        content.append(&capture_title);
        content.append(&capture_box);

        /* Keep the mixer compact for ordinary device/stream counts, but never
         * let dynamic PipeWire rows push the layer-shell overlay below the
         * usable screen.  The title and Close button remain outside this
         * widget, so they stay reachable while the dynamic rows scroll. */
        let root = gtk4::ScrolledWindow::new();
        root.set_hexpand(true);
        root.set_halign(gtk4::Align::Fill);
        root.set_policy(gtk4::PolicyType::Never, gtk4::PolicyType::Automatic);
        root.set_propagate_natural_height(true);
        root.set_max_content_height(320);
        root.set_vexpand(false);
        root.set_child(Some(&content));

        Self {
            root,
            outputs_box,
            inputs_box,
            playback_box,
            capture_box,
            outputs: HashMap::new(),
            inputs: HashMap::new(),
            playback: HashMap::new(),
            capture: HashMap::new(),
        }
    }

    fn apply(
        &mut self,
        host: &Arc<dyn EwwiiAPI>,
        devices: Option<&AudioDevicesState>,
        streams: Option<&AudioStreamsState>,
    ) {
        if let Some(devices) = devices {
            reconcile_devices(
                host,
                &self.outputs_box,
                &mut self.outputs,
                &devices.outputs,
                DeviceKind::Sink,
            );
            reconcile_devices(
                host,
                &self.inputs_box,
                &mut self.inputs,
                &devices.inputs,
                DeviceKind::Source,
            );
        }

        if let Some(streams) = streams {
            reconcile_streams(
                host,
                &self.playback_box,
                &mut self.playback,
                &streams.playback,
                StreamKind::Playback,
            );
            reconcile_streams(
                host,
                &self.capture_box,
                &mut self.capture,
                &streams.capture,
                StreamKind::Capture,
            );
        }
    }
}

fn section_label(text: &str) -> gtk4::Label {
    let label = gtk4::Label::new(Some(text));
    label.set_halign(gtk4::Align::Start);
    label.add_css_class("onyrion-audio-mixer-spaced");
    label
}

fn device_label(item: &AudioDevice) -> String {
    let base = if item.description.trim().is_empty() {
        item.name.as_str()
    } else {
        item.description.as_str()
    };

    if item.default {
        format!("{base} · active")
    } else if item.configured {
        format!("{base} · preferred")
    } else {
        base.to_string()
    }
}

fn stream_label(item: &AudioStream) -> String {
    /*
     * The PipeWire-facing model stores normalized linear volume in [0, 1].
     * Formatting f64 directly leaks conversion noise such as
     * 0.9999999999999998 into the UI.  Display a stable rounded percentage;
     * control actions continue to operate on the normalized value.
     */
    let percent = (item.volume.clamp(0.0, 1.0) * 100.0).round() as u32;
    if item.muted {
        format!("{} · {}% · muted", item.name, percent)
    } else {
        format!("{} · {}%", item.name, percent)
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
                    "AUDIO_RENDERER_ACTION status=PASS command={} args={}",
                    command,
                    args.join(" ")
                ));
            }
            Ok(status) => {
                host.error(&format!(
                    "AUDIO_RENDERER_ACTION status=FAIL command={} rc={:?} args={}",
                    command,
                    status.code(),
                    args.join(" ")
                ));
            }
            Err(error) => {
                host.error(&format!(
                    "AUDIO_RENDERER_ACTION status=FAIL command={} error={} args={}",
                    command,
                    error,
                    args.join(" ")
                ));
            }
        }
    });
}

fn create_device_row(host: Arc<dyn EwwiiAPI>, item: &AudioDevice, kind: DeviceKind) -> DeviceRow {
    let button = gtk4::Button::with_label(&device_label(item));
    button.add_css_class("onyrion-audio-mixer-spaced");
    button.add_css_class("onyrion-list-row");
    button.set_tooltip_text(Some(&device_label(item)));
    if let Some(label) = button.child().and_then(|child| child.downcast::<gtk4::Label>().ok()) {
        label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
        label.set_max_width_chars(36);
        label.set_xalign(0.0);
    }

    let name = item.name.clone();
    button.connect_clicked(move |_| {
        spawn_control(
            host.clone(),
            vec![
                "audio".to_string(),
                "device".to_string(),
                kind.token().to_string(),
                "select".to_string(),
                name.clone(),
            ],
        );
    });

    DeviceRow { button }
}

fn reconcile_devices(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<String, DeviceRow>,
    items: &[AudioDevice],
    kind: DeviceKind,
) {
    let mut ordered: Vec<&AudioDevice> = items.iter().collect();
    ordered.sort_by(|left, right| {
        right
            .default
            .cmp(&left.default)
            .then_with(|| right.configured.cmp(&left.configured))
            .then_with(|| device_label(left).to_lowercase().cmp(&device_label(right).to_lowercase()))
            .then_with(|| left.name.cmp(&right.name))
    });

    let wanted: HashSet<&str> = ordered.iter().map(|item| item.name.as_str()).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|key| !wanted.contains(key.as_str()))
        .cloned()
        .collect();

    for key in stale {
        if let Some(row) = rows.remove(&key) {
            container.remove(&row.button);
        }
    }

    for item in &ordered {
        if let Some(row) = rows.get(&item.name) {
            let label = device_label(item);
            row.button.set_label(&label);
            row.button.set_tooltip_text(Some(&label));
            continue;
        }

        let row = create_device_row(host.clone(), item, kind);
        container.append(&row.button);
        rows.insert(item.name.clone(), row);
    }

    let mut previous: Option<gtk4::Widget> = None;
    for item in ordered {
        if let Some(row) = rows.get(&item.name) {
            container.reorder_child_after(&row.button, previous.as_ref());
            previous = Some(row.button.clone().upcast());
        }
    }
}

fn create_stream_row(host: Arc<dyn EwwiiAPI>, item: &AudioStream, kind: StreamKind) -> StreamRow {
    let root = gtk4::Box::new(gtk4::Orientation::Horizontal, 8);
    root.add_css_class("onyrion-audio-mixer-spaced");
    root.add_css_class("onyrion-list-row");

    let label = gtk4::Label::new(Some(&stream_label(item)));
    label.set_hexpand(true);
    label.set_halign(gtk4::Align::Start);
    label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
    label.set_max_width_chars(24);

    let down = gtk4::Button::with_label("-");
    let mute = gtk4::Button::with_label("Mute");
    let up = gtk4::Button::with_label("+");
    down.add_css_class("onyrion-control-compact");
    mute.add_css_class("onyrion-control-compact");
    up.add_css_class("onyrion-control-compact");

    let serial = item.serial;
    let down_host = host.clone();
    down.connect_clicked(move |_| {
        spawn_control(
            down_host.clone(),
            vec![
                "audio".to_string(),
                "stream".to_string(),
                kind.token().to_string(),
                serial.to_string(),
                "volume".to_string(),
                "change".to_string(),
                "-0.05".to_string(),
            ],
        );
    });

    let mute_host = host.clone();
    mute.connect_clicked(move |_| {
        spawn_control(
            mute_host.clone(),
            vec![
                "audio".to_string(),
                "stream".to_string(),
                kind.token().to_string(),
                serial.to_string(),
                "mute".to_string(),
                "toggle".to_string(),
            ],
        );
    });

    up.connect_clicked(move |_| {
        spawn_control(
            host.clone(),
            vec![
                "audio".to_string(),
                "stream".to_string(),
                kind.token().to_string(),
                serial.to_string(),
                "volume".to_string(),
                "change".to_string(),
                "0.05".to_string(),
            ],
        );
    });

    root.append(&label);
    root.append(&down);
    root.append(&mute);
    root.append(&up);

    StreamRow { root, label }
}

fn reconcile_streams(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<u64, StreamRow>,
    items: &[AudioStream],
    kind: StreamKind,
) {
    let mut ordered: Vec<&AudioStream> = items.iter().collect();
    ordered.sort_by(|left, right| {
        left
            .name
            .to_lowercase()
            .cmp(&right.name.to_lowercase())
            .then_with(|| left.serial.cmp(&right.serial))
    });

    let wanted: HashSet<u64> = ordered.iter().map(|item| item.serial).collect();
    let stale: Vec<u64> = rows
        .keys()
        .filter(|serial| !wanted.contains(serial))
        .copied()
        .collect();

    for serial in stale {
        if let Some(row) = rows.remove(&serial) {
            container.remove(&row.root);
        }
    }

    for item in &ordered {
        if let Some(row) = rows.get(&item.serial) {
            row.label.set_text(&stream_label(item));
            continue;
        }

        let row = create_stream_row(host.clone(), item, kind);
        container.append(&row.root);
        rows.insert(item.serial, row);
    }

    let mut previous: Option<gtk4::Widget> = None;
    for item in ordered {
        if let Some(row) = rows.get(&item.serial) {
            container.reorder_child_after(&row.root, previous.as_ref());
            previous = Some(row.root.clone().upcast());
        }
    }
}

fn publish_devices(host: &Arc<dyn EwwiiAPI>, pending: &Arc<Mutex<PendingAudioState>>, raw: &str) {
    match serde_json::from_str::<AudioDevicesState>(raw) {
        Ok(state) => {
            let mut pending = pending.lock().expect("audio renderer state mutex poisoned");
            pending.devices = Some(state);
            pending.generation = pending.generation.wrapping_add(1).max(1);
        }
        Err(error) => {
            host.error(&format!(
                "AUDIO_RENDERER_DEVICES_PARSE status=FAIL bytes={} error={}",
                raw.len(),
                error
            ));
        }
    }
}

fn publish_streams(host: &Arc<dyn EwwiiAPI>, pending: &Arc<Mutex<PendingAudioState>>, raw: &str) {
    match serde_json::from_str::<AudioStreamsState>(raw) {
        Ok(state) => {
            let mut pending = pending.lock().expect("audio renderer state mutex poisoned");
            pending.streams = Some(state);
            pending.generation = pending.generation.wrapping_add(1).max(1);
        }
        Err(error) => {
            host.error(&format!(
                "AUDIO_RENDERER_STREAMS_PARSE status=FAIL bytes={} error={}",
                raw.len(),
                error
            ));
        }
    }
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    let pending = Arc::new(Mutex::new(PendingAudioState::default()));
    let ui = Rc::new(RefCell::new(AudioUi::new()));

    let widget = ui.borrow().root.clone().upcast::<gtk4::Widget>();
    host.register_static_widget(AUDIO_WIDGET, widget);
    host.register_signal(
        STATUS_SIGNAL,
        "{\"ready\":false,\"outputs\":0,\"inputs\":0,\"playback\":0,\"capture\":0}".to_string(),
    );

    let timer_host = host.clone();
    let timer_pending = pending.clone();
    let timer_ui = ui;
    gtk4::glib::timeout_add_local(Duration::from_millis(50), move || {
        let snapshot = {
            let mut pending = timer_pending
                .lock()
                .expect("audio renderer state mutex poisoned");

            if pending.generation == pending.applied_generation {
                None
            } else {
                pending.applied_generation = pending.generation;
                Some((
                    pending.devices.clone(),
                    pending.streams.clone(),
                    pending.generation,
                ))
            }
        };

        if let Some((devices, streams, generation)) = snapshot {
            let (outputs, inputs, playback, capture) = {
                let mut ui = timer_ui.borrow_mut();
                ui.apply(&timer_host, devices.as_ref(), streams.as_ref());
                (
                    ui.outputs.len(),
                    ui.inputs.len(),
                    ui.playback.len(),
                    ui.capture.len(),
                )
            };

            timer_host.update_signal(
                STATUS_SIGNAL,
                serde_json::json!({
                    "ready": devices.is_some() && streams.is_some(),
                    "generation": generation,
                    "outputs": outputs,
                    "inputs": inputs,
                    "playback": playback,
                    "capture": capture,
                })
                .to_string(),
            );

            timer_host.log(&format!(
                "AUDIO_RENDERER_STATE generation={} outputs={} inputs={} playback={} capture={}",
                generation, outputs, inputs, playback, capture
            ));
        }

        gtk4::glib::ControlFlow::Continue
    });

    let devices_host = host.clone();
    let devices_pending = pending.clone();
    host.register_function(
        "onyrion_audio_devices_ingest",
        vec![NbclType::String],
        NbclType::String,
        NativeFn::new(move |args| {
            let PluginValue::String(ref raw) = args[0] else {
                unreachable!("onyrion_audio_devices_ingest requires String")
            };
            publish_devices(&devices_host, &devices_pending, raw);
            Ok(PluginValue::String("onyrion-audio-slot-hidden".to_string()))
        }),
    );

    let streams_host = host.clone();
    let streams_pending = pending;
    host.register_function(
        "onyrion_audio_streams_ingest",
        vec![NbclType::String],
        NbclType::String,
        NativeFn::new(move |args| {
            let PluginValue::String(ref raw) = args[0] else {
                unreachable!("onyrion_audio_streams_ingest requires String")
            };
            publish_streams(&streams_host, &streams_pending, raw);
            Ok(PluginValue::String("onyrion-audio-slot-hidden".to_string()))
        }),
    );

    host.log("AUDIO_RENDERER_INGEST status=REGISTERED mode=nbcl-native");

    host.log("AUDIO_RENDERER_STATIC_WIDGET status=REGISTERED");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn devices_parse_and_use_name_as_stable_identity() {
        let state: AudioDevicesState = serde_json::from_str(
            r#"{
                "default_sink":"sink-a",
                "outputs":[
                    {
                        "id":52,
                        "serial":52,
                        "name":"sink-a",
                        "description":"Speaker",
                        "default":true,
                        "configured":false
                    }
                ],
                "inputs":[
                    {
                        "id":53,
                        "serial":53,
                        "name":"source-a",
                        "description":"Microphone",
                        "default":false,
                        "configured":true
                    }
                ]
            }"#,
        )
        .expect("device state");

        assert_eq!(state.outputs.len(), 1);
        assert_eq!(state.outputs[0].name, "sink-a");
        assert_eq!(device_label(&state.outputs[0]), "Speaker · active");
        assert_eq!(state.inputs[0].name, "source-a");
        assert_eq!(device_label(&state.inputs[0]), "Microphone · preferred");
    }

    #[test]
    fn streams_parse_and_use_serial_as_stable_identity() {
        let state: AudioStreamsState = serde_json::from_str(
            r#"{
                "playback":[
                    {
                        "id":74,
                        "serial":148,
                        "application":"Zen",
                        "name":"Zen",
                        "description":null,
                        "available":true,
                        "volume":0.75,
                        "muted":true
                    }
                ],
                "capture":[]
            }"#,
        )
        .expect("stream state");

        assert_eq!(state.playback.len(), 1);
        assert_eq!(state.playback[0].serial, 148);
        assert_eq!(stream_label(&state.playback[0]), "Zen · 75% · muted");
    }
}
