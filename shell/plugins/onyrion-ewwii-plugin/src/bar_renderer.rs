use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    process::Command,
    rc::Rc,
    sync::{Arc, Mutex, OnceLock},
    thread,
    time::Duration,
};

use ewwii_plugin_api::{
    gtk4,
    EwwiiAPI,
    ListenHandleFn,
    ListenHandleFnExt,
    SignalUpdateFn,
    SignalUpdateFnExt,
};
use gtk4::gdk;
use gtk4::prelude::*;
use serde::Deserialize;

const BAR_OUTPUTS: usize = 8;
const SYNC_WIDGETS: usize = 1;
const BAR_SIGNAL: &str = "onyrion_state";
const TOPBAR_DRAG_HOVER_ACTIVATE_MS: u64 = 3_000;

#[derive(Clone, Debug, Default, Deserialize)]
struct BarState {
    #[serde(default)]
    generation: u64,
    #[serde(default)]
    block: String,
    #[serde(default)]
    outputs: Vec<BarOutput>,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct BarOutput {
    #[serde(default)]
    index: usize,
    #[serde(default)]
    name: String,
    #[serde(default)]
    bar_visible: bool,
    #[serde(default)]
    workspaces: Vec<WorkspaceItem>,
    #[serde(default)]
    groups: Vec<GroupItem>,
}

#[derive(Clone, Debug, Deserialize)]
struct WorkspaceItem {
    id: String,
    label: String,
    #[serde(default)]
    active: bool,
}

#[derive(Clone, Debug, Deserialize)]
struct GroupItem {
    id: String,
    #[serde(default)]
    active: bool,
    #[serde(default)]
    placement: String,
    #[serde(default)]
    pinned: bool,
    #[serde(default)]
    pinned_output_name: String,
    #[serde(default)]
    placement_seen: bool,
    #[serde(default)]
    windows: Vec<WindowItem>,
}

#[derive(Clone, Debug, Deserialize)]
struct WindowItem {
    id: String,
    group_id: String,
    label: String,
    #[serde(default)]
    app_id: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    placement: String,
    #[serde(default)]
    active: bool,
}

struct WorkspaceRow {
    button: gtk4::Button,
}

#[derive(Clone, Debug)]
struct TabAction {
    window_id: String,
    group_id: String,
    placement: String,
    group_size: usize,
}

struct TabRow {
    button: gtk4::Button,
    image: gtk4::Image,
    label: gtk4::Label,
    action: Rc<RefCell<TabAction>>,
}

struct GroupRow {
    root: gtk4::Box,
    tabs: Rc<RefCell<HashMap<String, TabRow>>>,
}

struct BarUi {
    workspaces_root: gtk4::Box,
    groups_root: gtk4::Box,
    workspace_rows: HashMap<String, WorkspaceRow>,
    group_rows: HashMap<String, GroupRow>,
}

#[derive(Default)]
struct PendingBarState {
    state: Option<BarState>,
    generation: u64,
    applied_generation: u64,
}

static BAR_PENDING: OnceLock<Mutex<PendingBarState>> = OnceLock::new();

fn bar_pending() -> &'static Mutex<PendingBarState> {
    BAR_PENDING.get_or_init(|| Mutex::new(PendingBarState::default()))
}

thread_local! {
    static BAR_UIS: RefCell<Option<Vec<Rc<RefCell<BarUi>>>>> = const { RefCell::new(None) };
}

impl BarUi {
    fn new() -> Self {
        let workspaces_root = gtk4::Box::new(gtk4::Orientation::Horizontal, 4);
        workspaces_root.add_css_class("onyrion-workspaces");

        let groups_root = gtk4::Box::new(gtk4::Orientation::Horizontal, 4);
        groups_root.add_css_class("onyrion-tabs");
        groups_root.set_hexpand(true);
        groups_root.set_halign(gtk4::Align::Fill);

        Self {
            workspaces_root,
            groups_root,
            workspace_rows: HashMap::new(),
            group_rows: HashMap::new(),
        }
    }

    fn apply(&mut self, host: &Arc<dyn EwwiiAPI>, output: Option<&BarOutput>) {
        match output {
            Some(output) => {
                reconcile_workspaces(
                    host,
                    &self.workspaces_root,
                    &mut self.workspace_rows,
                    &output.workspaces,
                );
                reconcile_groups(
                    host,
                    &self.groups_root,
                    &mut self.group_rows,
                    &output.groups,
                );
            }
            None => {
                clear_workspaces(&self.workspaces_root, &mut self.workspace_rows);
                clear_groups(&self.groups_root, &mut self.group_rows);
            }
        }
    }
}

fn shell_bin() -> String {
    std::env::var("ONYRION_SHELL_BIN")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| "onyrion-shell".to_string())
}

fn control_bin() -> String {
    std::env::var("ONYRION_CONTROL_BIN")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| "onyrionctl".to_string())
}

fn spawn_shell(host: Arc<dyn EwwiiAPI>, args: Vec<String>, tag: String) {
    thread::spawn(move || {
        let command = shell_bin();
        match Command::new(&command).args(&args).status() {
            Ok(status) if status.success() => {
                eprintln!(
                    "[onyrion-ewwii-plugin] BAR_ACTION status=PASS tag={} command={} args={}",
                    tag,
                    command,
                    args.join(" ")
                );
            }
            Ok(status) => host.error(&format!(
                "BAR_ACTION status=FAIL tag={} command={} rc={:?} args={}",
                tag,
                command,
                status.code(),
                args.join(" ")
            )),
            Err(error) => host.error(&format!(
                "BAR_ACTION status=FAIL tag={} command={} error={} args={}",
                tag,
                command,
                error,
                args.join(" ")
            )),
        }
    });
}

fn spawn_control(host: Arc<dyn EwwiiAPI>, args: Vec<String>, tag: String) {
    thread::spawn(move || {
        let command = control_bin();
        match Command::new(&command).args(&args).status() {
            Ok(status) if status.success() => {
                eprintln!(
                    "[onyrion-ewwii-plugin] BAR_CONTROL status=PASS tag={} command={} args={}",
                    tag,
                    command,
                    args.join(" ")
                );
            }
            Ok(status) => host.error(&format!(
                "BAR_CONTROL status=FAIL tag={} command={} rc={:?} args={}",
                tag,
                command,
                status.code(),
                args.join(" ")
            )),
            Err(error) => host.error(&format!(
                "BAR_CONTROL status=FAIL tag={} command={} error={} args={}",
                tag,
                command,
                error,
                args.join(" ")
            )),
        }
    });
}

fn icon_name(app_id: &str) -> &str {
    let lower = app_id.to_ascii_lowercase();
    if lower.contains("kitty") {
        "kitty"
    } else if lower.contains("telegram") || lower.contains("ayugram") {
        "org.telegram.desktop"
    } else if lower.contains("zen") {
        "zen-browser"
    } else if app_id.trim().is_empty() {
        "application-x-executable-symbolic"
    } else {
        app_id
    }
}

fn workspace_row(host: Arc<dyn EwwiiAPI>, item: &WorkspaceItem) -> WorkspaceRow {
    let button = gtk4::Button::with_label(&item.label);
    button.add_css_class("onyrion-workspace");
    button.set_focusable(false);
    if item.active {
        button.add_css_class("onyrion-workspace-active");
    }

    let id = item.id.clone();
    button.connect_clicked(move |_| {
        spawn_shell(
            host.clone(),
            vec!["workspace".to_string(), id.clone()],
            format!("workspace:{id}"),
        );
    });

    WorkspaceRow { button }
}

fn attach_drag_hover_activation(
    host: Arc<dyn EwwiiAPI>,
    button: &gtk4::Button,
    action: Rc<RefCell<TabAction>>,
) {
    // DropControllerMotion observes an ongoing DnD without becoming a drop
    // destination.  Hovering the same topbar application for three seconds
    // activates that exact Window; leave/drag-end/target-change invalidates
    // the pending timer.  This does not accept or complete the drop.
    let motion = gtk4::DropControllerMotion::new();
    let generation = Rc::new(Cell::new(0u64));

    let enter_generation = generation.clone();
    let enter_host = host;
    let enter_action = action;
    let enter_button = button.clone();
    motion.connect_enter(move |controller, _, _| {
        let token = enter_generation.get().wrapping_add(1);
        enter_generation.set(token);
        enter_button.add_css_class("onyrion-tab-drag-hover");

        let timer_generation = enter_generation.clone();
        let timer_controller = controller.clone();
        let timer_host = enter_host.clone();
        let timer_action = enter_action.clone();
        let timer_button = enter_button.clone();
        gtk4::glib::timeout_add_local_once(
            Duration::from_millis(TOPBAR_DRAG_HOVER_ACTIVATE_MS),
            move || {
                if timer_generation.get() != token
                    || !timer_controller.contains_pointer()
                    || timer_controller.drop().is_none()
                {
                    return;
                }

                let id = timer_action.borrow().window_id.clone();
                spawn_control(
                    timer_host,
                    vec!["window".to_string(), "focus".to_string(), id.clone()],
                    format!("topbar-drag-hover-focus:{id}"),
                );
                timer_button.remove_css_class("onyrion-tab-drag-hover");

                // Fire once for this hover.  A leave + re-enter creates a new
                // generation and may arm another activation.
                timer_generation.set(token.wrapping_add(1));
            },
        );
    });

    let leave_generation = generation.clone();
    let leave_button = button.clone();
    motion.connect_leave(move |_| {
        leave_generation.set(leave_generation.get().wrapping_add(1));
        leave_button.remove_css_class("onyrion-tab-drag-hover");
    });

    let drop_generation = generation;
    let drop_button = button.clone();
    motion.connect_drop_notify(move |controller| {
        if controller.drop().is_none() {
            drop_generation.set(drop_generation.get().wrapping_add(1));
            drop_button.remove_css_class("onyrion-tab-drag-hover");
        }
    });

    button.add_controller(motion);
}

fn tab_row(host: Arc<dyn EwwiiAPI>, item: &WindowItem, group_size: usize) -> TabRow {
    let button = gtk4::Button::new();
    button.add_css_class("onyrion-tab");
    button.set_focusable(false);
    button.set_tooltip_text(Some(if item.title.trim().is_empty() {
        &item.label
    } else {
        &item.title
    }));
    if item.active {
        button.add_css_class("onyrion-tab-active");
    }

    let content = gtk4::Box::new(gtk4::Orientation::Horizontal, 4);
    let image = gtk4::Image::from_icon_name(icon_name(&item.app_id));
    image.set_pixel_size(16);
    let label = gtk4::Label::new(Some(&item.label));
    label.set_ellipsize(gtk4::pango::EllipsizeMode::End);
    label.set_max_width_chars(14);
    content.append(&image);
    content.append(&label);
    button.set_child(Some(&content));

    let action = Rc::new(RefCell::new(TabAction {
        window_id: item.id.clone(),
        group_id: item.group_id.clone(),
        placement: item.placement.clone(),
        group_size,
    }));

    let left_action = action.clone();
    let left_host = host.clone();
    button.connect_clicked(move |_| {
        let id = left_action.borrow().window_id.clone();
        spawn_control(
            left_host.clone(),
            vec!["window".to_string(), "focus".to_string(), id.clone()],
            format!("window-focus:{id}"),
        );
    });

    attach_drag_hover_activation(host.clone(), &button, action.clone());

    // Topbar tabs intentionally have no RMB menu.  Window/Group RMB belongs
    // to the Shell-owned Group-attached TabGroup surface after v13 migration.
    let drag_source = gtk4::DragSource::new();
    drag_source.set_actions(gdk::DragAction::MOVE);

    let drag_action = action.clone();
    drag_source.connect_prepare(move |_, _, _| {
        let action = drag_action.borrow();
        let payload = format!(
            "onyrion-window:{}:{}",
            action.window_id, action.group_id
        );
        Some(gdk::ContentProvider::for_value(&gtk4::glib::Value::from(&payload)))
    });

    let drag_button = button.clone();
    drag_source.connect_drag_begin(move |_, _| {
        drag_button.add_css_class("onyrion-tab-dragging");
    });

    let cancel_action = action.clone();
    let cancel_host = host.clone();
    drag_source.connect_drag_cancel(move |_, _, reason| {
        if reason != gdk::DragCancelReason::NoTarget {
            return false;
        }

        let action = cancel_action.borrow().clone();
        if action.group_size <= 1 {
            return false;
        }

        /* Releasing an actual drag outside every group means detach one tab.
         * UserCancelled/Error are deliberately not mapped to a mutation. */
        spawn_control(
            cancel_host.clone(),
            vec![
                "window".to_string(),
                "split".to_string(),
                action.window_id.clone(),
                "horizontal".to_string(),
            ],
            format!("tab-detach:{}", action.window_id),
        );
        true
    });

    let drag_button = button.clone();
    drag_source.connect_drag_end(move |_, _, _| {
        drag_button.remove_css_class("onyrion-tab-dragging");
    });
    button.add_controller(drag_source);

    TabRow {
        button,
        image,
        label,
        action,
    }
}

fn group_row(host: Arc<dyn EwwiiAPI>, item: &GroupItem) -> GroupRow {
    let root = gtk4::Box::new(gtk4::Orientation::Horizontal, 2);
    root.add_css_class("onyrion-group");
    if item.active {
        root.add_css_class("onyrion-group-active");
    }

    let tabs: Rc<RefCell<HashMap<String, TabRow>>> =
        Rc::new(RefCell::new(HashMap::new()));

    let drop_target = gtk4::DropTarget::new(String::static_type(), gdk::DragAction::MOVE);
    let drop_host = host.clone();
    let drop_group_id = item.id.clone();
    drop_target.connect_drop(move |_, value, _, _| {
        let Ok(payload) = value.get::<String>() else {
            return false;
        };

        let Some(rest) = payload.strip_prefix("onyrion-window:") else {
            return false;
        };
        let mut parts = rest.splitn(2, ':');
        let Some(window_id) = parts.next() else {
            return false;
        };
        let Some(source_group_id) = parts.next() else {
            return false;
        };

        if window_id.is_empty() || source_group_id.is_empty() {
            return false;
        }

        /* Dropping back onto the source group is a handled no-op, not a failed
         * drag; this prevents the NoTarget detach path from firing. */
        if source_group_id == drop_group_id {
            return true;
        }

        spawn_control(
            drop_host.clone(),
            vec![
                "window".to_string(),
                "move-to-group".to_string(),
                window_id.to_string(),
                drop_group_id.clone(),
            ],
            format!("tab-join:{window_id}->{drop_group_id}"),
        );
        true
    });
    root.add_controller(drop_target);

    GroupRow { root, tabs }
}

fn clear_workspaces(container: &gtk4::Box, rows: &mut HashMap<String, WorkspaceRow>) {
    for (_, row) in rows.drain() {
        container.remove(&row.button);
    }
}

fn clear_groups(container: &gtk4::Box, rows: &mut HashMap<String, GroupRow>) {
    for (_, row) in rows.drain() {
        container.remove(&row.root);
    }
}

fn reconcile_workspaces(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<String, WorkspaceRow>,
    items: &[WorkspaceItem],
) {
    let wanted: HashSet<&str> = items.iter().map(|item| item.id.as_str()).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|id| !wanted.contains(id.as_str()))
        .cloned()
        .collect();

    for id in stale {
        if let Some(row) = rows.remove(&id) {
            container.remove(&row.button);
        }
    }

    for item in items {
        if let Some(row) = rows.get(&item.id) {
            row.button.set_label(&item.label);
            if item.active {
                row.button.add_css_class("onyrion-workspace-active");
            } else {
                row.button.remove_css_class("onyrion-workspace-active");
            }
        } else {
            let row = workspace_row(host.clone(), item);
            container.append(&row.button);
            rows.insert(item.id.clone(), row);
        }
    }

    let mut previous: Option<gtk4::Widget> = None;
    for item in items {
        if let Some(row) = rows.get(&item.id) {
            container.reorder_child_after(&row.button, previous.as_ref());
            previous = Some(row.button.clone().upcast());
        }
    }
}

fn reconcile_tabs(
    host: &Arc<dyn EwwiiAPI>,
    group: &gtk4::Box,
    rows: &mut HashMap<String, TabRow>,
    items: &[WindowItem],
) {
    let wanted: HashSet<&str> = items.iter().map(|item| item.id.as_str()).collect();
    let stale: Vec<String> = rows
        .keys()
        .filter(|id| !wanted.contains(id.as_str()))
        .cloned()
        .collect();

    for id in stale {
        if let Some(row) = rows.remove(&id) {
            group.remove(&row.button);
        }
    }

    for item in items {
        if let Some(row) = rows.get(&item.id) {
            row.label.set_text(&item.label);
            row.image.set_icon_name(Some(icon_name(&item.app_id)));
            row.button.set_tooltip_text(Some(if item.title.trim().is_empty() {
                &item.label
            } else {
                &item.title
            }));
            if item.active {
                row.button.add_css_class("onyrion-tab-active");
            } else {
                row.button.remove_css_class("onyrion-tab-active");
            }
            *row.action.borrow_mut() = TabAction {
                window_id: item.id.clone(),
                group_id: item.group_id.clone(),
                placement: item.placement.clone(),
                group_size: items.len(),
            };
        } else {
            let row = tab_row(host.clone(), item, items.len());
            group.append(&row.button);
            rows.insert(item.id.clone(), row);
        }
    }

    let mut previous: Option<gtk4::Widget> = None;
    for item in items {
        if let Some(row) = rows.get(&item.id) {
            group.reorder_child_after(&row.button, previous.as_ref());
            previous = Some(row.button.clone().upcast());
        }
    }
}

fn reconcile_groups(
    host: &Arc<dyn EwwiiAPI>,
    container: &gtk4::Box,
    rows: &mut HashMap<String, GroupRow>,
    items: &[GroupItem],
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
        if !rows.contains_key(&item.id) {
            let row = group_row(host.clone(), item);
            container.append(&row.root);
            rows.insert(item.id.clone(), row);
        }

        let row = rows.get_mut(&item.id).expect("group row");
        if item.active {
            row.root.add_css_class("onyrion-group-active");
        } else {
            row.root.remove_css_class("onyrion-group-active");
        }
        {
            let mut tabs = row.tabs.borrow_mut();
            reconcile_tabs(
                host,
                &row.root,
                &mut tabs,
                &item.windows,
            );
        }
    }

    let mut previous: Option<gtk4::Widget> = None;
    for item in items {
        if let Some(row) = rows.get(&item.id) {
            container.reorder_child_after(&row.root, previous.as_ref());
            previous = Some(row.root.clone().upcast());
        }
    }
}

fn validate_bar_state(state: &BarState) -> Result<(), String> {
    if state.block != "bar" {
        return Err(format!("unexpected block {}", state.block));
    }
    if state.outputs.len() > BAR_OUTPUTS {
        return Err(format!("too many outputs {}", state.outputs.len()));
    }

    let mut seen = HashSet::new();
    for output in &state.outputs {
        if output.index >= BAR_OUTPUTS {
            return Err(format!("output index out of range {}", output.index));
        }
        if !seen.insert(output.index) {
            return Err(format!("duplicate output index {}", output.index));
        }
        if output.name.trim().is_empty() {
            return Err(format!("output {} missing name", output.index));
        }

        for group in &output.groups {
            if group.id.trim().is_empty() || group.windows.is_empty() {
                return Err("group missing id/windows".to_string());
            }
            if group.placement_seen &&
                    group.placement != "tiled" &&
                    group.placement != "floating" {
                return Err("group invalid placement".to_string());
            }
            if group.pinned &&
                    (group.placement != "floating" || group.pinned_output_name.trim().is_empty()) {
                return Err("group invalid pin state".to_string());
            }
            for window in &group.windows {
                if window.id.trim().is_empty() ||
                        window.group_id != group.id {
                    return Err("window/group projection mismatch".to_string());
                }
            }
        }

        let _ = output.bar_visible;
    }

    Ok(())
}

fn apply_bar_state(host: &Arc<dyn EwwiiAPI>, state: BarState) {
    let generation = state.generation;
    let workspace_count: usize = state.outputs.iter().map(|output| output.workspaces.len()).sum();
    let group_count: usize = state.outputs.iter().map(|output| output.groups.len()).sum();
    let window_count: usize = state
        .outputs
        .iter()
        .flat_map(|output| &output.groups)
        .map(|group| group.windows.len())
        .sum();

    let applied = BAR_UIS.with(|slot| {
        let slot = slot.borrow();
        let Some(uis) = slot.as_ref() else {
            return false;
        };

        for output_index in 0..BAR_OUTPUTS {
            let output = state.outputs.iter().find(|output| output.index == output_index);
            uis[output_index].borrow_mut().apply(host, output);
        }
        true
    });

    if !applied {
        host.error("BAR_RENDERER_STATE status=FAIL reason=ui_registry_unavailable");
        return;
    }

    host.log(&format!(
        "BAR_RENDERER_STATE generation={} outputs={} workspaces={} groups={} windows={}",
        generation,
        state.outputs.len(),
        workspace_count,
        group_count,
        window_count
    ));
}

pub fn ingest(host: Arc<dyn EwwiiAPI>, raw: &str) -> String {
    if raw.trim().is_empty() {
        return "onyrion-bar-ingest-hidden".to_string();
    }

    let state = match serde_json::from_str::<BarState>(raw) {
        Ok(state) => state,
        Err(error) => {
            host.error(&format!(
                "BAR_RENDERER_PARSE status=FAIL bytes={} error={}",
                raw.len(),
                error
            ));
            return "onyrion-bar-ingest-hidden".to_string();
        }
    };

    if let Err(reason) = validate_bar_state(&state) {
        host.error(&format!(
            "BAR_RENDERER_PARSE status=FAIL block={} outputs={} reason={}",
            state.block,
            state.outputs.len(),
            reason
        ));
        return "onyrion-bar-ingest-hidden".to_string();
    }

    // The same authoritative Ewwii Listen stream feeds both the topbar and
    // Group-attached TabGroup renderer.  TabGroup ingest is parse/store-only;
    // GTK mutation remains on its own main-loop reconciler.
    crate::tabgroup_renderer::ingest_raw(raw);

    let mut pending = bar_pending()
        .lock()
        .expect("bar renderer state mutex poisoned");

    /* signal_value() seeds after subscription so a live callback cannot be
     * lost between snapshot and subscribe.  If that asynchronous seed races
     * a newer watch-state update, never let the older Shell generation win. */
    if pending
        .state
        .as_ref()
        .is_some_and(|current| state.generation <= current.generation)
    {
        return "onyrion-bar-ingest-hidden".to_string();
    }

    pending.state = Some(state);
    pending.generation = pending.generation.wrapping_add(1).max(1);

    "onyrion-bar-ingest-hidden".to_string()
}

fn attach_bar_signal(host: Arc<dyn EwwiiAPI>) {
    let update_host = host.clone();
    host.on_signal_update(
        BAR_SIGNAL,
        SignalUpdateFn::new(move |raw| {
            let _ = ingest(update_host.clone(), raw);
        }),
    );

    let seed_host = host.clone();
    host.signal_value(BAR_SIGNAL).resolve_async(move |result| match result {
        Ok(raw) => {
            let _ = ingest(seed_host.clone(), &raw);
            seed_host.log(&format!(
                "BAR_RENDERER_SIGNAL status=SEEDED signal={} bytes={}",
                BAR_SIGNAL,
                raw.len()
            ));
        }
        Err(error) => seed_host.error(&format!(
            "BAR_RENDERER_SIGNAL status=FAIL phase=seed signal={} error={}",
            BAR_SIGNAL, error
        )),
    });

    host.log(&format!(
        "BAR_RENDERER_SIGNAL status=SUBSCRIBED signal={}",
        BAR_SIGNAL
    ));
}

pub fn init(host: Arc<dyn EwwiiAPI>) {
    let mut ui_vec = Vec::with_capacity(BAR_OUTPUTS);
    let mut deferred_widgets = Vec::with_capacity(BAR_OUTPUTS * 2 - SYNC_WIDGETS);

    for output in 0..BAR_OUTPUTS {
        let ui = Rc::new(RefCell::new(BarUi::new()));
        let workspaces = ui.borrow().workspaces_root.clone().upcast::<gtk4::Widget>();
        let groups = ui.borrow().groups_root.clone().upcast::<gtk4::Widget>();
        let workspace_name = format!("onyrion-bar-workspaces-{output}");
        let group_name = format!("onyrion-bar-groups-{output}");

        if output == 0 {
            // Preserve the proven v31 startup invariant: exactly one static
            // widget registration occurs synchronously during plugin init.
            // The second output-0 widget and all remaining widgets are
            // registered from the GTK idle loop after daemon startup.
            host.register_static_widget(&workspace_name, workspaces);
            deferred_widgets.push((group_name, groups));
        } else {
            deferred_widgets.push((workspace_name, workspaces));
            deferred_widgets.push((group_name, groups));
        }

        ui_vec.push(ui);
    }

    BAR_UIS.with(|slot| {
        *slot.borrow_mut() = Some(ui_vec);
    });

    let timer_host = host.clone();
    gtk4::glib::timeout_add_local(Duration::from_millis(50), move || {
        let snapshot = {
            let mut pending = bar_pending()
                .lock()
                .expect("bar renderer state mutex poisoned");
            if pending.generation == pending.applied_generation {
                None
            } else {
                pending.applied_generation = pending.generation;
                pending.state.clone()
            }
        };

        if let Some(state) = snapshot {
            apply_bar_state(&timer_host, state);
        }

        gtk4::glib::ControlFlow::Continue
    });

    /*
     * Ewwii 0.10 does not drain plugin_rx until every plugin init() returns.
     * Its bounded plugin->host channel has capacity 32.  The rest of the
     * Onyrion plugin already consumes exactly 31 synchronous requests and the
     * one bar static-widget registration below makes 32.  Registering the bar
     * signal listener synchronously here would be request #33 and deadlock
     * HostProxy::call_host() in blocking_send().
     *
     * Bind the signal only after the GTK main loop starts, when plugin_rx is
     * actively drained.  On reload, ewwii-started-signals is delivered through
     * plugin_callback_handler() while its CALLBACKS mutex is held, so the
     * rebind itself must also be deferred out of that callback.
     */
    let deferred_host = host;
    gtk4::glib::idle_add_local_once(move || {
        let reload_host = deferred_host.clone();
        deferred_host.listen(
            "ewwii-started-signals",
            ListenHandleFn::new(move |_event| {
                let rebind_host = reload_host.clone();
                gtk4::glib::idle_add_once(move || {
                    attach_bar_signal(rebind_host);
                });
            }),
        );

        /* Initial bind: the startup ewwii-started-signals event may already
         * have happened before this idle callback, so subscribe explicitly to
         * the current VarWatcher and seed its current value. */
        attach_bar_signal(deferred_host.clone());

        for (name, widget) in deferred_widgets {
            deferred_host.register_static_widget(&name, widget);
        }
        eprintln!(
            "[onyrion-ewwii-plugin] BAR_RENDERER_STATIC_WIDGETS status=REGISTERED sync=1 deferred=15 total=16"
        );
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_widget_block_bar_projection() {
        let state: BarState = serde_json::from_str(
            r#"{
                "generation":484,
                "block":"bar",
                "outputs":[{
                    "index":0,
                    "name":"eDP-1",
                    "bar_visible":true,
                    "workspaces":[{"id":"1","label":"●","active":false},{"id":"2","label":"◆","active":true}],
                    "groups":[{
                        "id":"14",
                        "active":true,
                        "placement":"floating",
                        "pinned":true,
                        "pinned_output_name":"eDP-1",
                        "placement_seen":true,
                        "windows":[
                            {"id":"11","group_id":"14","label":"Zen","app_id":"org.mozilla.zen","title":"A","placement":"floating","active":true},
                            {"id":"12","group_id":"14","label":"Kitty","app_id":"kitty","title":"B","placement":"floating","active":false}
                        ]
                    }]
                }]
            }"#,
        )
        .expect("bar projection");

        assert_eq!(state.block, "bar");
        assert_eq!(state.outputs.len(), 1);
        assert_eq!(state.outputs[0].workspaces.len(), 2);
        assert_eq!(state.outputs[0].groups[0].windows.len(), 2);
        assert_eq!(state.outputs[0].groups[0].windows[0].id, "11");
    }

    #[test]
    fn rejects_duplicate_output_indexes() {
        let state: BarState = serde_json::from_str(
            r#"{
                "generation":1,
                "block":"bar",
                "outputs":[
                    {"index":0,"name":"eDP-1","bar_visible":true,"workspaces":[],"groups":[]},
                    {"index":0,"name":"DP-1","bar_visible":true,"workspaces":[],"groups":[]}
                ]
            }"#,
        )
        .expect("bar projection");
        assert!(validate_bar_state(&state).is_err());
    }
}
