use std::{
    collections::{BTreeMap, HashMap},
    process::{Command, Output},
    sync::{Arc, Mutex, OnceLock},
    thread,
    time::Duration,
};

use ewwii_plugin_api::{EwwiiAPI, NativeFn, NativeFnExt, NbclType, PluginValue};
use serde::Serialize;
use zbus::{
    blocking::{connection::Builder as BlockingConnectionBuilder, Connection as BlockingConnection},
    interface,
    object_server::SignalEmitter,
    zvariant::OwnedValue,
};

const BUS_NAME: &str = "org.freedesktop.Notifications";
const OBJECT_PATH: &str = "/org/freedesktop/Notifications";
const INTERFACE_NAME: &str = "org.freedesktop.Notifications";

const STATE_SIGNAL: &str = "onyrion_notifications";
const WINDOW_NAME: &str = "onyrion-notifications";
const PARENT_WIDGET: &str = "onyrion-notification-list";
const DEFAULT_UI_DIR: &str = "/usr/share/onyrion/ui/ewwii";
const DEFAULT_TIMEOUT_MS: u64 = 5_000;

const CLOSED_EXPIRED: u32 = 1;
const CLOSED_DISMISSED: u32 = 2;
const CLOSED_BY_CALL: u32 = 3;

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
struct ActionView {
    index: usize,
    label: String,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
struct NotificationView {
    id: u32,
    app_name: String,
    summary: String,
    body: String,
    actions: Vec<ActionView>,
}

#[derive(Clone, Debug)]
struct StoredNotification {
    view: NotificationView,
    action_keys: Vec<String>,
    generation: u64,
}

#[derive(Debug)]
struct NotificationStore {
    next_id: u32,
    next_generation: u64,
    active: BTreeMap<u32, StoredNotification>,
}

impl Default for NotificationStore {
    fn default() -> Self {
        Self {
            next_id: 1,
            next_generation: 1,
            active: BTreeMap::new(),
        }
    }
}

impl NotificationStore {
    fn allocate_id(&mut self) -> u32 {
        loop {
            let id = self.next_id.max(1);
            self.next_id = if id == u32::MAX { 1 } else { id + 1 };

            if !self.active.contains_key(&id) {
                return id;
            }
        }
    }

    fn note_explicit_id(&mut self, id: u32) {
        if id != u32::MAX && id >= self.next_id {
            self.next_id = id + 1;
        }
    }

    fn next_generation(&mut self) -> u64 {
        let generation = self.next_generation;
        self.next_generation = self.next_generation.wrapping_add(1).max(1);
        generation
    }

    fn upsert(
        &mut self,
        app_name: String,
        replaces_id: u32,
        summary: String,
        body: String,
        actions: Vec<String>,
    ) -> (u32, u64, bool) {
        let id = if replaces_id == 0 {
            self.allocate_id()
        } else {
            self.note_explicit_id(replaces_id);
            replaces_id
        };

        let replaced = self.active.contains_key(&id);
        let generation = self.next_generation();

        let mut action_views = Vec::new();
        let mut action_keys = Vec::new();

        for (index, pair) in actions.chunks_exact(2).enumerate() {
            action_keys.push(pair[0].clone());
            action_views.push(ActionView {
                index,
                label: pair[1].clone(),
            });
        }

        self.active.insert(
            id,
            StoredNotification {
                view: NotificationView {
                    id,
                    app_name,
                    summary,
                    body,
                    actions: action_views,
                },
                action_keys,
                generation,
            },
        );

        (id, generation, replaced)
    }

    fn remove(&mut self, id: u32) -> bool {
        self.active.remove(&id).is_some()
    }

    fn remove_if_generation(&mut self, id: u32, generation: u64) -> bool {
        let matches =
            self.active.get(&id).map(|item| item.generation == generation).unwrap_or(false);

        if matches {
            self.active.remove(&id);
            true
        } else {
            false
        }
    }

    fn action_key(&self, id: u32, index: usize) -> Option<String> {
        self.active.get(&id)?.action_keys.get(index).cloned()
    }

    fn view(&self, id: u32) -> Option<NotificationView> {
        self.active.get(&id).map(|item| item.view.clone())
    }

    fn views(&self) -> Vec<NotificationView> {
        let mut notifications: Vec<_> = self.active.values().cloned().collect();
        notifications.sort_by_key(|item| item.generation);
        notifications.into_iter().map(|item| item.view).collect()
    }

    fn is_empty(&self) -> bool {
        self.active.is_empty()
    }

    #[cfg(test)]
    fn len(&self) -> usize {
        self.active.len()
    }
}

fn effective_timeout(expire_timeout: i32) -> Option<Duration> {
    match expire_timeout {
        0 => None,
        value if value > 0 => Some(Duration::from_millis(value as u64)),
        _ => Some(Duration::from_millis(DEFAULT_TIMEOUT_MS)),
    }
}

fn card_widget_name(id: u32) -> String {
    format!("onyrion-notification-card-{id}")
}

fn field_signal(id: u32, field: &str) -> String {
    format!("onyrion_notification_{id}_{field}")
}

fn action_signal(id: u32, index: usize) -> String {
    format!("onyrion_notification_{id}_action_{index}")
}

fn card_nbcl(view: &NotificationView) -> String {
    let id = view.id;
    let card = card_widget_name(id);
    let app = field_signal(id, "app");
    let summary = field_signal(id, "summary");
    let body = field_signal(id, "body");

    let mut actions = String::new();
    for action in &view.actions {
        let signal = action_signal(id, action.index);
        actions.push_str(&format!(
            r#"
            Button {{
                widget_name = "onyrion-notification-card-{id}-action-{index}"
                label = global("{signal}")
                class = "onyrion-notification-action"
                onclick = "ewwii --config /usr/share/onyrion/ui/ewwii nbcl-run 'onyrion_notification_action({id},{index})'"
            }}
"#,
            id = id,
            index = action.index,
            signal = signal,
        ));
    }

    format!(
        r#"Box {{
    widget_name = "{card}"
    orientation = "v"
    spacing = 6
    class = "onyrion-notification"

    Box {{
        orientation = "h"
        spacing = 6
        class = "onyrion-notification-header"

        Label {{
            widget_name = "{card}-app"
            text = global("{app}")
            halign = "start"
            hexpand = true
            class = "onyrion-notification-app"
        }}

        Button {{
            widget_name = "{card}-dismiss"
            label = "×"
            class = "onyrion-notification-dismiss"
            onclick = "ewwii --config /usr/share/onyrion/ui/ewwii nbcl-run 'onyrion_notification_dismiss({id})'"
        }}
    }}

    Label {{
        widget_name = "{card}-summary"
        text = global("{summary}")
        halign = "start"
        wrap = true
        class = "onyrion-notification-summary"
    }}

    Label {{
        widget_name = "{card}-body"
        text = global("{body}")
        halign = "start"
        wrap = true
        class = "onyrion-notification-body"
    }}

    Box {{
        widget_name = "{card}-actions"
        orientation = "h"
        spacing = 6
        class = "onyrion-notification-actions"
{actions}
    }}
}}"#,
        card = card,
        app = app,
        summary = summary,
        body = body,
        id = id,
        actions = actions,
    )
}

#[derive(Clone)]
struct NotificationBridge {
    host: Arc<dyn EwwiiAPI>,
    store: Arc<Mutex<NotificationStore>>,
    connection: Arc<OnceLock<BlockingConnection>>,
    ui_dir: Arc<String>,
    window_open: Arc<Mutex<bool>>,
    ui_sync: Arc<Mutex<()>>,
}

impl NotificationBridge {
    fn new(host: Arc<dyn EwwiiAPI>) -> Self {
        let ui_dir =
            std::env::var("ONYRION_UI_DIR").unwrap_or_else(|_| DEFAULT_UI_DIR.to_string());

        Self {
            host,
            store: Arc::new(Mutex::new(NotificationStore::default())),
            connection: Arc::new(OnceLock::new()),
            ui_dir: Arc::new(ui_dir),
            window_open: Arc::new(Mutex::new(false)),
            ui_sync: Arc::new(Mutex::new(())),
        }
    }

    fn attach_connection(&self, connection: BlockingConnection) -> Result<(), &'static str> {
        self.connection.set(connection).map_err(|_| "connection already attached")
    }

    fn publish_state(&self) {
        let (json, count) = {
            let store = self.store.lock().expect("notification store mutex poisoned");
            let views = store.views();
            let json = serde_json::to_string(&views).unwrap_or_else(|_| "[]".to_string());
            (json, views.len())
        };

        self.host.update_signal(STATE_SIGNAL, json);
        self.host.log(&format!("NOTIFICATIONS_STATE count={count}"));
    }

    fn run_ewwii(&self, args: &[&str]) -> std::io::Result<Output> {
        Command::new("ewwii")
            .arg("--config")
            .arg(self.ui_dir.as_str())
            .args(args)
            .output()
    }

    fn sync_window_locked(&self, should_be_open: bool) -> bool {
        let mut known_open = self.window_open.lock().expect("window mutex poisoned");

        if *known_open == should_be_open {
            return true;
        }

        let action = if should_be_open { "open" } else { "close" };
        let output = self.run_ewwii(&[action, WINDOW_NAME]);

        match output {
            Ok(output) if output.status.success() => {
                *known_open = should_be_open;
                self.host.log(&format!(
                    "NOTIFICATION_WINDOW={} status=PASS",
                    if should_be_open { "OPEN" } else { "CLOSED" }
                ));
                true
            }
            Ok(output) => {
                self.host.error(&format!(
                    "NOTIFICATION_WINDOW={} status=FAIL rc={:?} stderr={}",
                    if should_be_open { "OPEN" } else { "CLOSE" },
                    output.status.code(),
                    String::from_utf8_lossy(&output.stderr).trim()
                ));
                false
            }
            Err(error) => {
                self.host.error(&format!(
                    "NOTIFICATION_WINDOW={} status=FAIL error={error}",
                    if should_be_open { "OPEN" } else { "CLOSE" }
                ));
                false
            }
        }
    }

    fn register_card_signals(&self, view: &NotificationView) {
        self.host
            .register_signal(&field_signal(view.id, "app"), view.app_name.clone());
        self.host
            .register_signal(&field_signal(view.id, "summary"), view.summary.clone());
        self.host
            .register_signal(&field_signal(view.id, "body"), view.body.clone());

        for action in &view.actions {
            self.host
                .register_signal(&action_signal(view.id, action.index), action.label.clone());
        }
    }

    fn remove_card_locked(&self, id: u32, required: bool) -> bool {
        let name = card_widget_name(id);
        let output = self.run_ewwii(&["widget-control", "remove", &name]);

        match output {
            Ok(output) if output.status.success() => {
                self.host
                    .log(&format!("NOTIFICATION_WIDGET_REMOVE id={id} status=PASS"));
                true
            }
            Ok(output) if !required => {
                self.host.warn(&format!(
                    "NOTIFICATION_WIDGET_REMOVE id={id} status=SKIP rc={:?} stderr={}",
                    output.status.code(),
                    String::from_utf8_lossy(&output.stderr).trim()
                ));
                false
            }
            Ok(output) => {
                self.host.error(&format!(
                    "NOTIFICATION_WIDGET_REMOVE id={id} status=FAIL rc={:?} stderr={}",
                    output.status.code(),
                    String::from_utf8_lossy(&output.stderr).trim()
                ));
                false
            }
            Err(error) if !required => {
                self.host.warn(&format!(
                    "NOTIFICATION_WIDGET_REMOVE id={id} status=SKIP error={error}"
                ));
                false
            }
            Err(error) => {
                self.host.error(&format!(
                    "NOTIFICATION_WIDGET_REMOVE id={id} status=FAIL error={error}"
                ));
                false
            }
        }
    }

    fn create_card_locked(&self, view: &NotificationView) -> bool {
        self.register_card_signals(view);

        let code = card_nbcl(view);
        let output = Command::new("ewwii")
            .arg("--config")
            .arg(self.ui_dir.as_str())
            .arg("widget-control")
            .arg("create")
            .arg(&code)
            .arg("--parent")
            .arg(PARENT_WIDGET)
            .output();

        match output {
            Ok(output) if output.status.success() => {
                self.host.log(&format!(
                    "NOTIFICATION_WIDGET_CREATE id={} actions={} status=PASS",
                    view.id,
                    view.actions.len()
                ));
                true
            }
            Ok(output) => {
                self.host.error(&format!(
                    "NOTIFICATION_WIDGET_CREATE id={} status=FAIL rc={:?} stderr={}",
                    view.id,
                    output.status.code(),
                    String::from_utf8_lossy(&output.stderr).trim()
                ));
                false
            }
            Err(error) => {
                self.host.error(&format!(
                    "NOTIFICATION_WIDGET_CREATE id={} status=FAIL error={error}",
                    view.id
                ));
                false
            }
        }
    }

    fn sync_upsert_ui(&self, id: u32, replaced: bool) {
        let view = {
            let store = self.store.lock().expect("notification store mutex poisoned");
            store.view(id)
        };

        let Some(view) = view else {
            self.host
                .error(&format!("NOTIFICATION_WIDGET_CREATE id={id} status=FAIL reason=no_state"));
            return;
        };

        let _guard = self.ui_sync.lock().expect("notification ui mutex poisoned");

        if !self.sync_window_locked(true) {
            return;
        }

        if replaced && !self.remove_card_locked(id, true) {
            return;
        }

        self.create_card_locked(&view);
    }

    fn sync_remove_ui(&self, id: u32) {
        let should_close = {
            let store = self.store.lock().expect("notification store mutex poisoned");
            store.is_empty()
        };

        let _guard = self.ui_sync.lock().expect("notification ui mutex poisoned");

        self.remove_card_locked(id, false);

        if should_close {
            self.sync_window_locked(false);
        }
    }

    fn emit_signal<B>(&self, member: &str, body: &B) -> bool
    where
        B: serde::Serialize + zbus::zvariant::DynamicType,
    {
        let Some(connection) = self.connection.get() else {
            self.host.error(&format!(
                "NOTIFICATIONS_SIGNAL member={member} status=FAIL reason=no_connection"
            ));
            return false;
        };

        match connection.emit_signal(
            None::<&str>,
            OBJECT_PATH,
            INTERFACE_NAME,
            member,
            body,
        ) {
            Ok(()) => true,
            Err(error) => {
                self.host.error(&format!(
                    "NOTIFICATIONS_SIGNAL member={member} status=FAIL error={error}"
                ));
                false
            }
        }
    }

    fn emit_closed(&self, id: u32, reason: u32) {
        if self.emit_signal("NotificationClosed", &(id, reason)) {
            self.host
                .log(&format!("NOTIFICATION_CLOSED id={id} reason={reason}"));
        }
    }

    fn schedule_expiry(&self, id: u32, generation: u64, timeout: Duration) {
        let bridge = self.clone();

        thread::spawn(move || {
            thread::sleep(timeout);

            let removed = {
                let mut store =
                    bridge.store.lock().expect("notification store mutex poisoned");
                store.remove_if_generation(id, generation)
            };

            if removed {
                bridge.publish_state();
                bridge.sync_remove_ui(id);
                bridge.emit_closed(id, CLOSED_EXPIRED);
            }
        });
    }

    fn dismiss_from_ui(&self, id: u32) {
        let removed = {
            let mut store = self.store.lock().expect("notification store mutex poisoned");
            store.remove(id)
        };

        if removed {
            self.publish_state();
            self.sync_remove_ui(id);
            self.emit_closed(id, CLOSED_DISMISSED);
        }
    }

    fn invoke_action_from_ui(&self, id: u32, index: usize) {
        let key = {
            let store = self.store.lock().expect("notification store mutex poisoned");
            store.action_key(id, index)
        };

        let Some(key) = key else {
            self.host.warn(&format!(
                "NOTIFICATION_ACTION status=IGNORE id={id} index={index} reason=not_found"
            ));
            return;
        };

        if self.emit_signal("ActionInvoked", &(id, key.as_str())) {
            self.host
                .log(&format!("NOTIFICATION_ACTION id={id} index={index} status=EMITTED"));
        }

        self.dismiss_from_ui(id);
    }
}

#[derive(Clone)]
struct FreedesktopNotifications {
    bridge: NotificationBridge,
}

#[interface(name = "org.freedesktop.Notifications")]
impl FreedesktopNotifications {
    #[zbus(name = "GetCapabilities")]
    fn get_capabilities(&self) -> Vec<String> {
        vec!["actions".to_string(), "body".to_string()]
    }

    #[zbus(name = "Notify")]
    fn notify(
        &self,
        app_name: String,
        replaces_id: u32,
        _app_icon: String,
        summary: String,
        body: String,
        actions: Vec<String>,
        _hints: HashMap<String, OwnedValue>,
        expire_timeout: i32,
    ) -> u32 {
        let (id, generation, replaced) = {
            let mut store = self
                .bridge
                .store
                .lock()
                .expect("notification store mutex poisoned");

            store.upsert(app_name, replaces_id, summary, body, actions)
        };

        self.bridge.publish_state();
        self.bridge.sync_upsert_ui(id, replaced);
        self.bridge.host.log(&format!(
            "NOTIFICATION_NOTIFY id={id} replaced={} timeout_ms={expire_timeout}",
            if replaced { "YES" } else { "NO" }
        ));

        if let Some(timeout) = effective_timeout(expire_timeout) {
            self.bridge.schedule_expiry(id, generation, timeout);
        }

        id
    }

    #[zbus(name = "CloseNotification")]
    async fn close_notification(
        &self,
        id: u32,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        let removed = {
            let mut store = self
                .bridge
                .store
                .lock()
                .expect("notification store mutex poisoned");

            store.remove(id)
        };

        if !removed {
            return Err(zbus::fdo::Error::Failed(String::new()));
        }

        self.bridge.publish_state();
        self.bridge.sync_remove_ui(id);

        Self::notification_closed(&emitter, id, CLOSED_BY_CALL)
            .await
            .map_err(zbus::fdo::Error::ZBus)?;

        self.bridge
            .host
            .log(&format!("NOTIFICATION_CLOSED id={id} reason={CLOSED_BY_CALL}"));

        Ok(())
    }

    #[zbus(name = "GetServerInformation")]
    #[zbus(out_args("name", "vendor", "version", "spec_version"))]
    fn get_server_information(&self) -> (String, String, String, String) {
        (
            "Onyrion".to_string(),
            "Onyrion".to_string(),
            "0.2.0".to_string(),
            "1.3".to_string(),
        )
    }

    #[zbus(signal, name = "NotificationClosed")]
    async fn notification_closed(
        emitter: &SignalEmitter<'_>,
        id: u32,
        reason: u32,
    ) -> zbus::Result<()>;

    #[zbus(signal, name = "ActionInvoked")]
    async fn action_invoked(
        emitter: &SignalEmitter<'_>,
        id: u32,
        action_key: &str,
    ) -> zbus::Result<()>;
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    host.register_signal(STATE_SIGNAL, "[]".to_string());

    let bridge = NotificationBridge::new(host.clone());

    let dismiss_bridge = bridge.clone();
    host.register_function(
        "onyrion_notification_dismiss",
        vec![NbclType::Int],
        NbclType::Null,
        NativeFn::new(move |args| {
            if let [PluginValue::Int(id)] = args.as_slice() {
                if let Ok(id) = u32::try_from(*id) {
                    let bridge = dismiss_bridge.clone();
                    thread::spawn(move || bridge.dismiss_from_ui(id));
                }
            }

            Ok(PluginValue::Null)
        }),
    );

    let action_bridge = bridge.clone();
    host.register_function(
        "onyrion_notification_action",
        vec![NbclType::Int, NbclType::Int],
        NbclType::Null,
        NativeFn::new(move |args| {
            if let [PluginValue::Int(id), PluginValue::Int(index)] = args.as_slice() {
                if let (Ok(id), Ok(index)) = (u32::try_from(*id), usize::try_from(*index)) {
                    let bridge = action_bridge.clone();
                    thread::spawn(move || bridge.invoke_action_from_ui(id, index));
                }
            }

            Ok(PluginValue::Null)
        }),
    );

    thread::spawn(move || {
        let service = FreedesktopNotifications {
            bridge: bridge.clone(),
        };

        let connection = BlockingConnectionBuilder::session()
            .and_then(|builder| builder.serve_at(OBJECT_PATH, service))
            .and_then(|builder| builder.name(BUS_NAME))
            .map(|builder| {
                builder
                    .allow_name_replacements(false)
                    .replace_existing_names(false)
            })
            .and_then(|builder| builder.build());

        let connection = match connection {
            Ok(connection) => connection,
            Err(error) => {
                host.error(&format!(
                    "NOTIFICATIONS_DBUS status=FAIL bus={BUS_NAME} error={error}"
                ));
                return;
            }
        };

        if bridge.attach_connection(connection.clone()).is_err() {
            host.error("NOTIFICATIONS_DBUS status=FAIL reason=connection_already_attached");
            return;
        }

        host.log(&format!(
            "NOTIFICATIONS_DBUS status=READY bus={BUS_NAME} path={OBJECT_PATH}"
        ));

        loop {
            thread::park();
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn upsert(
        store: &mut NotificationStore,
        replaces_id: u32,
        summary: &str,
        actions: &[&str],
    ) -> (u32, u64, bool) {
        store.upsert(
            "test-app".to_string(),
            replaces_id,
            summary.to_string(),
            "body".to_string(),
            actions.iter().map(|value| (*value).to_string()).collect(),
        )
    }

    #[test]
    fn generated_ids_are_nonzero_and_distinct() {
        let mut store = NotificationStore::default();

        let (a, _, _) = upsert(&mut store, 0, "a", &[]);
        let (b, _, _) = upsert(&mut store, 0, "b", &[]);

        assert_ne!(a, 0);
        assert_ne!(b, 0);
        assert_ne!(a, b);
        assert_eq!(store.len(), 2);
    }

    #[test]
    fn replacement_keeps_id_and_replaces_payload_atomically() {
        let mut store = NotificationStore::default();

        let (id, old_generation, _) = upsert(&mut store, 0, "old", &[]);
        let (replacement_id, new_generation, replaced) =
            upsert(&mut store, id, "new", &[]);

        assert_eq!(replacement_id, id);
        assert!(replaced);
        assert_ne!(old_generation, new_generation);
        assert_eq!(store.len(), 1);
        assert_eq!(store.views()[0].summary, "new");
    }

    #[test]
    fn stale_expiry_generation_does_not_remove_replacement() {
        let mut store = NotificationStore::default();

        let (id, old_generation, _) = upsert(&mut store, 0, "old", &[]);
        let (_, new_generation, _) = upsert(&mut store, id, "new", &[]);

        assert!(!store.remove_if_generation(id, old_generation));
        assert_eq!(store.len(), 1);
        assert!(store.remove_if_generation(id, new_generation));
        assert_eq!(store.len(), 0);
    }

    #[test]
    fn action_pairs_map_index_to_original_key() {
        let mut store = NotificationStore::default();

        let (id, _, _) = upsert(
            &mut store,
            0,
            "actions",
            &["default", "Open", "reply", "Reply"],
        );

        assert_eq!(store.action_key(id, 0).as_deref(), Some("default"));
        assert_eq!(store.action_key(id, 1).as_deref(), Some("reply"));
        assert_eq!(store.views()[0].actions[0].label, "Open");
        assert_eq!(store.views()[0].actions[1].label, "Reply");
    }

    #[test]
    fn dangling_action_element_is_not_exposed() {
        let mut store = NotificationStore::default();

        let (id, _, _) = upsert(
            &mut store,
            0,
            "actions",
            &["default", "Open", "dangling"],
        );

        assert_eq!(store.action_key(id, 0).as_deref(), Some("default"));
        assert_eq!(store.action_key(id, 1), None);
        assert_eq!(store.views()[0].actions.len(), 1);
    }

    #[test]
    fn timeout_semantics_cover_default_never_and_explicit() {
        assert_eq!(effective_timeout(0), None);
        assert_eq!(
            effective_timeout(-1),
            Some(Duration::from_millis(DEFAULT_TIMEOUT_MS))
        );
        assert_eq!(
            effective_timeout(250),
            Some(Duration::from_millis(250))
        );
    }

    #[test]
    fn generated_nbcl_contains_no_application_text() {
        let mut store = NotificationStore::default();

        let (id, _, _) = store.upsert(
            "app \"quoted\"".to_string(),
            0,
            "summary ${unsafe}".to_string(),
            "body 'unsafe'".to_string(),
            vec![
                "action-key; rm -rf /".to_string(),
                "label \"quoted\"".to_string(),
            ],
        );

        let view = store.view(id).expect("notification view");
        let code = card_nbcl(&view);

        assert!(!code.contains("app \\\"quoted\\\""));
        assert!(!code.contains("summary ${unsafe}"));
        assert!(!code.contains("body 'unsafe'"));
        assert!(!code.contains("action-key; rm -rf /"));
        assert!(!code.contains("label \\\"quoted\\\""));
        assert!(code.contains(&format!("onyrion_notification_{id}_summary")));
        assert!(code.contains(&format!("onyrion_notification_action({id},0)")));
    }
}
