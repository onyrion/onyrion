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

const TRAY_WIDGETS: usize = 8;
const SYNC_TRAY_WIDGETS: usize = 5;
const TRAY_STATUS_SIGNAL: &str = "onyrion_tray_renderer_status";

#[derive(Clone, Debug, Default, Deserialize)]
struct TrayState {
    #[serde(default)]
    items: Vec<TrayItem>,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct TrayItem {
    id: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    status: String,
    #[serde(default)]
    icon: String,
}

#[derive(Clone, Debug, Default)]
struct PendingTrayState {
    state: Option<TrayState>,
    generation: u64,
    applied_generation: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TrayAction {
    Activate,
    Context,
}

struct TrayRow {
    root: gtk4::Box,
    image: gtk4::Image,
}

struct TrayUi {
    root: gtk4::Box,
    rows: HashMap<String, TrayRow>,
}

impl TrayUi {
    fn new() -> Self {
        let root = gtk4::Box::new(gtk4::Orientation::Horizontal, 4);
        Self {
            root,
            rows: HashMap::new(),
        }
    }

    fn apply(&mut self, host: &Arc<dyn EwwiiAPI>, state: &TrayState) {
        reconcile_items(host, &self.root, &mut self.rows, &state.items);
    }
}

fn shell_bin() -> String {
    std::env::var("ONYRION_SHELL_BIN")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| "onyrion-shell".to_string())
}

fn spawn_action(host: Arc<dyn EwwiiAPI>, action: TrayAction, id: String) {
    thread::spawn(move || {
        let command = shell_bin();
        let verb = match action {
            TrayAction::Activate => "activate",
            TrayAction::Context => "context",
        };
        let args = ["tray", verb, id.as_str()];

        match Command::new(&command).args(args).status() {
            Ok(status) if status.success() => {
                host.log(&format!(
                    "TRAY_RENDERER_ACTION status=PASS action={} id={}",
                    verb, id
                ));
            }
            Ok(status) => {
                host.error(&format!(
                    "TRAY_RENDERER_ACTION status=FAIL action={} id={} rc={:?}",
                    verb,
                    id,
                    status.code()
                ));
            }
            Err(error) => {
                host.error(&format!(
                    "TRAY_RENDERER_ACTION status=FAIL action={} id={} error={}",
                    verb, id, error
                ));
            }
        }
    });
}

fn icon_name(item: &TrayItem) -> &str {
    if item.icon.trim().is_empty() {
        "application-x-executable-symbolic"
    } else {
        item.icon.as_str()
    }
}

fn tooltip(item: &TrayItem) -> String {
    if item.title.trim().is_empty() {
        item.id.clone()
    } else if item.status.trim().is_empty() {
        item.title.clone()
    } else {
        format!("{} · {}", item.title, item.status)
    }
}

fn create_row(host: Arc<dyn EwwiiAPI>, item: &TrayItem) -> TrayRow {
    let root = gtk4::Box::new(gtk4::Orientation::Horizontal, 0);
    root.add_css_class("onyrion-tray-slot-live");
    root.set_tooltip_text(Some(&tooltip(item)));

    let image = gtk4::Image::from_icon_name(icon_name(item));
    image.set_pixel_size(16);
    root.append(&image);

    let left = gtk4::GestureClick::new();
    left.set_button(1);
    let activate_host = host.clone();
    let activate_id = item.id.clone();
    left.connect_released(move |_, _, _, _| {
        spawn_action(
            activate_host.clone(),
            TrayAction::Activate,
            activate_id.clone(),
        );
    });
    root.add_controller(left);

    let right = gtk4::GestureClick::new();
    right.set_button(3);
    let context_host = host;
    let context_id = item.id.clone();
    right.connect_released(move |_, _, _, _| {
        spawn_action(
            context_host.clone(),
            TrayAction::Context,
            context_id.clone(),
        );
    });
    root.add_controller(right);

    TrayRow { root, image }
}

fn update_row(row: &TrayRow, item: &TrayItem) {
    row.image.set_icon_name(Some(icon_name(item)));
    row.root.set_tooltip_text(Some(&tooltip(item)));
}

fn reconcile_items(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<String, TrayRow>,
    items: &[TrayItem],
) {
    let wanted: HashSet<&str> = items.iter().map(|item| item.id.as_str()).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|id| !wanted.contains(id.as_str()))
        .cloned()
        .collect();

    for id in stale {
        if let Some(row) = rows.remove(&id) {
            container.remove(&row.root);
        }
    }

    for item in items {
        if item.id.trim().is_empty() {
            continue;
        }

        if let Some(row) = rows.get(&item.id) {
            update_row(row, item);
            continue;
        }

        let row = create_row(host.clone(), item);
        container.append(&row.root);
        rows.insert(item.id.clone(), row);
    }
}

fn publish_tray(
    host: &Arc<dyn EwwiiAPI>,
    pending: &Arc<Mutex<PendingTrayState>>,
    raw: &str,
) {
    if raw.trim().is_empty() {
        return;
    }

    match serde_json::from_str::<TrayState>(raw) {
        Ok(state) => {
            let mut pending = pending
                .lock()
                .expect("tray renderer state mutex poisoned");
            pending.state = Some(state);
            pending.generation = pending.generation.wrapping_add(1).max(1);
        }
        Err(error) => {
            host.error(&format!(
                "TRAY_RENDERER_PARSE status=FAIL bytes={} error={}",
                raw.len(),
                error
            ));
        }
    }
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    let pending = Arc::new(Mutex::new(PendingTrayState::default()));

    let mut ui_vec = Vec::with_capacity(TRAY_WIDGETS);
    let mut deferred_widgets = Vec::with_capacity(TRAY_WIDGETS - SYNC_TRAY_WIDGETS);

    for output in 0..TRAY_WIDGETS {
        let ui = Rc::new(RefCell::new(TrayUi::new()));
        let widget = ui.borrow().root.clone().upcast::<gtk4::Widget>();
        let name = format!("onyrion-tray-dynamic-{output}");

        if output < SYNC_TRAY_WIDGETS {
            host.register_static_widget(&name, widget);
        } else {
            deferred_widgets.push((name, widget));
        }

        ui_vec.push(ui);
    }
    let uis = Rc::new(ui_vec);

    host.register_signal(
        TRAY_STATUS_SIGNAL,
        serde_json::json!({
            "ready": false,
            "generation": 0,
            "items": 0,
            "bars": TRAY_WIDGETS,
        })
        .to_string(),
    );

    let timer_host = host.clone();
    let timer_pending = pending.clone();
    let timer_uis = uis;
    gtk4::glib::timeout_add_local(Duration::from_millis(50), move || {
        let snapshot = {
            let mut pending = timer_pending
                .lock()
                .expect("tray renderer state mutex poisoned");

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
            for ui in timer_uis.iter() {
                ui.borrow_mut().apply(&timer_host, &state);
            }

            timer_host.update_signal(
                TRAY_STATUS_SIGNAL,
                serde_json::json!({
                    "ready": true,
                    "generation": generation,
                    "items": state.items.len(),
                    "bars": TRAY_WIDGETS,
                })
                .to_string(),
            );

            timer_host.log(&format!(
                "TRAY_RENDERER_STATE generation={} items={} bars={}",
                generation,
                state.items.len(),
                TRAY_WIDGETS
            ));
        }

        gtk4::glib::ControlFlow::Continue
    });

    let ingest_host = host.clone();
    let ingest_pending = pending;
    host.register_function(
        "onyrion_tray_ingest",
        vec![NbclType::String],
        NbclType::String,
        NativeFn::new(move |args| {
            let PluginValue::String(ref raw) = args[0] else {
                unreachable!("onyrion_tray_ingest requires String")
            };
            publish_tray(&ingest_host, &ingest_pending, raw);
            Ok(PluginValue::String(
                "onyrion-tray-slot-hidden".to_string(),
            ))
        }),
    );

    host.log("TRAY_RENDERER_INGEST status=REGISTERED mode=nbcl-native");
    host.log(
        "TRAY_RENDERER_STATIC_WIDGETS status=SYNC_REGISTERED sync=5 deferred=3 total=8",
    );

    let deferred_host = host.clone();
    gtk4::glib::idle_add_local_once(move || {
        for (name, widget) in deferred_widgets {
            deferred_host.register_static_widget(&name, widget);
        }
        deferred_host.log(
            "TRAY_RENDERER_STATIC_WIDGETS status=REGISTERED sync=5 deferred=3 total=8",
        );
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state() -> TrayState {
        serde_json::from_str(
            r#"{
                "items":[
                    {
                        "id":"stable-a",
                        "title":"Nextcloud",
                        "status":"Active",
                        "icon":"state-offline"
                    },
                    {
                        "id":"stable-b",
                        "title":"AyuGramDesktop",
                        "status":"Active",
                        "icon":"com.ayugram.desktop-symbolic"
                    }
                ]
            }"#,
        )
        .expect("tray state")
    }

    #[test]
    fn parses_current_shell_tray_state_schema() {
        let state = state();
        assert_eq!(state.items.len(), 2);
        assert_eq!(state.items[0].id, "stable-a");
        assert_eq!(state.items[1].title, "AyuGramDesktop");
    }

    #[test]
    fn preserves_current_icon_names() {
        let state = state();
        assert_eq!(icon_name(&state.items[0]), "state-offline");
        assert_eq!(
            icon_name(&state.items[1]),
            "com.ayugram.desktop-symbolic"
        );
    }

    #[test]
    fn empty_icon_has_standard_fallback() {
        let item = TrayItem {
            id: "x".to_string(),
            title: "X".to_string(),
            status: "Active".to_string(),
            icon: String::new(),
        };
        assert_eq!(icon_name(&item), "application-x-executable-symbolic");
    }

    #[test]
    fn tooltip_uses_title_and_status() {
        let state = state();
        assert_eq!(tooltip(&state.items[0]), "Nextcloud · Active");
    }
}
