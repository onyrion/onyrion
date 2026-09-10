use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    ffi::{CStr, CString},
    process::Command,
    rc::Rc,
    sync::{Mutex, OnceLock},
    thread,
    time::{Duration, Instant},
};

use ewwii_plugin_api::gtk4;
use gtk4::{
    glib::translate::from_glib_none,
    prelude::*,
};
use serde::Deserialize;

use crate::tabgroup_model::{
    HoldRepeat,
    TabViewport,
    ViewDirection,
};

pub const TABGROUP_UI_HEIGHT_PX: i32 = 32;

const CONTROL_SLOT_PX: i32 = 32;
const TAB_SLOT_PX: i32 = 32;
const TAB_ICON_PX: i32 = 18;
const TICK_MS: u64 = 20;
const DRAG_START_THRESHOLD_PX: f64 = 6.0;

// A Group-attached TabGroup window has an Onyrion custom wl_surface role, not
// a compositor-visible xdg_toplevel. GTK tooltips/GdkPopup children would try
// to create an xdg_popup whose parent is the shim's client-only fake
// xdg_surface, which cannot be referenced by the compositor. Keep this
// renderer native-popup-free; use in-surface UI or a separate Shell-owned
// surface for auxiliary UI.

#[derive(Clone, Debug, Default, Deserialize)]
struct ShellState {
    #[serde(default)]
    generation: u64,
    #[serde(default)]
    block: String,
    #[serde(default)]
    outputs: Vec<OutputState>,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct OutputState {
    #[serde(default)]
    groups: Vec<GroupState>,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct GroupState {
    id: String,
    #[serde(default)]
    placement: String,
    #[serde(default)]
    placement_seen: bool,
    #[serde(default)]
    pinned: bool,
    #[serde(default)]
    windows: Vec<WindowState>,
}

#[derive(Clone, Debug, Default, Deserialize)]
struct WindowState {
    id: String,
    #[serde(default)]
    app_id: String,
    #[serde(default)]
    label: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    active: bool,
}

#[derive(Clone, Debug, Default)]
struct PendingState {
    generation: u64,
    applied_generation: u64,
    state: Option<ShellState>,
}

#[derive(Clone, Debug, Default)]
struct GroupActions {
    placement: String,
    pinned: bool,
    active_window_id: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum DragHit {
    None,
    Group,
    Before(String),
    After(String),
}

#[derive(Clone, Debug)]
enum DragOrigin {
    Group(String),
    Window(String),
}

extern "C" {
    fn onyrion_group_surface_bridge_begin_window_drag(
        gtk_window_ptr: usize,
        window_id: *const std::ffi::c_char,
    ) -> i32;

    fn onyrion_group_surface_bridge_begin_group_drag(
        gtk_window_ptr: usize,
        group_id: *const std::ffi::c_char,
    ) -> i32;

    fn onyrion_group_surface_bridge_set_group_drag_target(
        group_id: *const std::ffi::c_char,
    ) -> i32;

    fn onyrion_group_surface_bridge_set_tab_drag_target(
        group_id: *const std::ffi::c_char,
        reference_window_id: *const std::ffi::c_char,
        after: i32,
    ) -> i32;
}

struct TabGroupUi {
    window_ptr: usize,
    group_id: String,
    root: gtk4::Box,
    group_button: gtk4::Button,
    previous_button: gtk4::Button,
    tabs_root: gtk4::Box,
    next_button: gtk4::Button,
    mode_button: gtk4::Button,
    close_button: gtk4::Button,
    actions: Rc<RefCell<GroupActions>>,
    viewport: TabViewport,
    hold: HoldRepeat,
    group_width: i32,
    group_height: i32,
    arrows_visible: bool,
    visible_ids: Vec<String>,
    group: Option<GroupState>,
}

static PENDING: OnceLock<Mutex<PendingState>> = OnceLock::new();
static EPOCH: OnceLock<Instant> = OnceLock::new();

thread_local! {
    static UIS: RefCell<HashMap<usize, Rc<RefCell<TabGroupUi>>>> =
        RefCell::new(HashMap::new());
}

fn pending() -> &'static Mutex<PendingState> {
    PENDING.get_or_init(|| Mutex::new(PendingState::default()))
}

fn now_ms() -> u64 {
    EPOCH
        .get_or_init(Instant::now)
        .elapsed()
        .as_millis()
        .min(u128::from(u64::MAX)) as u64
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

fn spawn_command(command: String, args: Vec<String>, tag: String) {
    thread::spawn(move || match Command::new(&command).args(&args).status() {
        Ok(status) if status.success() => {
            eprintln!(
                "[onyrion-ewwii-plugin] TABGROUP_ACTION status=PASS tag={} command={} args={}",
                tag,
                command,
                args.join(" ")
            );
        }
        Ok(status) => {
            eprintln!(
                "[onyrion-ewwii-plugin] TABGROUP_ACTION status=FAIL tag={} command={} rc={:?} args={}",
                tag,
                command,
                status.code(),
                args.join(" ")
            );
        }
        Err(error) => {
            eprintln!(
                "[onyrion-ewwii-plugin] TABGROUP_ACTION status=FAIL tag={} command={} error={} args={}",
                tag,
                command,
                error,
                args.join(" ")
            );
        }
    });
}

fn spawn_shell(args: Vec<String>, tag: String) {
    spawn_command(shell_bin(), args, tag);
}

fn spawn_control(args: Vec<String>, tag: String) {
    spawn_command(control_bin(), args, tag);
}

fn spawn_active_window_action(window_id: String, verb: &'static str) {
    thread::spawn(move || {
        let command = control_bin();

        let focus = Command::new(&command)
            .args(["window", "focus", &window_id])
            .status();

        match focus {
            Ok(status) if status.success() => {}
            Ok(status) => {
                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_ACTION status=FAIL tag={}:focus window={} rc={:?}",
                    verb,
                    window_id,
                    status.code()
                );
                return;
            }
            Err(error) => {
                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_ACTION status=FAIL tag={}:focus window={} error={}",
                    verb,
                    window_id,
                    error
                );
                return;
            }
        }

        match Command::new(&command)
            .args(["window", verb])
            .status()
        {
            Ok(status) if status.success() => {
                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_ACTION status=PASS tag={} window={}",
                    verb,
                    window_id
                );
            }
            Ok(status) => {
                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_ACTION status=FAIL tag={} window={} rc={:?}",
                    verb,
                    window_id,
                    status.code()
                );
            }
            Err(error) => {
                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_ACTION status=FAIL tag={} window={} error={}",
                    verb,
                    window_id,
                    error
                );
            }
        }
    });
}

fn context_action(kind: &str, id: &str) {
    spawn_shell(
        vec![
            "invoke".to_string(),
            "ui:context-actions".to_string(),
            format!("{kind}:{id}"),
        ],
        format!("context:{kind}:{id}"),
    );
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

fn fixed_button(label: &str, css: &str) -> gtk4::Button {
    let button = gtk4::Button::with_label(label);
    button.set_focusable(false);
    button.set_size_request(CONTROL_SLOT_PX, TABGROUP_UI_HEIGHT_PX);
    button.add_css_class("onyrion-tabgroup-button");
    button.add_css_class(css);
    button
}

fn clear_box(root: &gtk4::Box) {
    while let Some(child) = root.first_child() {
        root.remove(&child);
    }
}

fn group_for_id(state: &ShellState, group_id: &str) -> Option<GroupState> {
    state
        .outputs
        .iter()
        .flat_map(|output| output.groups.iter())
        .find(|group| group.id == group_id)
        .cloned()
}

fn validate_state(state: &ShellState) -> Result<(), String> {
    if state.block != "bar" {
        return Err(format!("unexpected block {}", state.block));
    }

    let mut windows = HashSet::new();

    for output in &state.outputs {
        for group in &output.groups {
            if group.id.is_empty()
                || group.id.starts_with('0')
                || !group.id.bytes().all(|byte| byte.is_ascii_digit())
            {
                return Err(format!("invalid group id {}", group.id));
            }

            if group.placement_seen
                && group.placement != "tiled"
                && group.placement != "floating"
            {
                return Err(format!(
                    "group {} invalid placement {}",
                    group.id, group.placement
                ));
            }

            if group.placement_seen
                && group.pinned
                && group.placement != "floating"
            {
                return Err(format!(
                    "group {} invalid pinned placement {}",
                    group.id, group.placement
                ));
            }

            if group.windows.is_empty() {
                return Err(format!("group {} has no windows", group.id));
            }

            for window in &group.windows {
                if window.id.is_empty()
                    || window.id.starts_with('0')
                    || !window.id.bytes().all(|byte| byte.is_ascii_digit())
                {
                    return Err(format!("invalid window id {}", window.id));
                }

                if !windows.insert(window.id.clone()) {
                    return Err(format!("duplicate window id {}", window.id));
                }
            }
        }
    }

    Ok(())
}

pub fn ingest_raw(raw: &str) {
    if raw.trim().is_empty() {
        return;
    }

    let state = match serde_json::from_str::<ShellState>(raw) {
        Ok(state) => state,
        Err(error) => {
            eprintln!(
                "[onyrion-ewwii-plugin] TABGROUP_STATE status=FAIL phase=parse bytes={} error={}",
                raw.len(),
                error
            );
            return;
        }
    };

    if let Err(reason) = validate_state(&state) {
        eprintln!(
            "[onyrion-ewwii-plugin] TABGROUP_STATE status=FAIL phase=validate generation={} reason={}",
            state.generation,
            reason
        );
        return;
    }

    let mut pending = pending()
        .lock()
        .expect("tabgroup pending state mutex poisoned");

    if pending
        .state
        .as_ref()
        .is_some_and(|current| state.generation <= current.generation)
    {
        return;
    }

    pending.generation = state.generation.max(1);
    pending.state = Some(state);
}

fn begin_core_drag(
    gtk_window_ptr: usize,
    origin: &DragOrigin,
) -> i32 {
    match origin {
        DragOrigin::Group(group_id) => {
            let Ok(group_id) = CString::new(group_id.as_str()) else {
                return -1;
            };

            unsafe {
                onyrion_group_surface_bridge_begin_group_drag(
                    gtk_window_ptr,
                    group_id.as_ptr(),
                )
            }
        }
        DragOrigin::Window(window_id) => {
            let Ok(window_id) = CString::new(window_id.as_str()) else {
                return -1;
            };

            unsafe {
                onyrion_group_surface_bridge_begin_window_drag(
                    gtk_window_ptr,
                    window_id.as_ptr(),
                )
            }
        }
    }
}

fn install_core_drag_candidate(
    button: &gtk4::Button,
    gtk_window_ptr: usize,
    origin: DragOrigin,
) -> Rc<Cell<bool>> {
    let pressed = Rc::new(Cell::new(false));
    let started = Rc::new(Cell::new(false));
    let start_x = Rc::new(Cell::new(0.0f64));
    let start_y = Rc::new(Cell::new(0.0f64));

    let gesture = gtk4::GestureClick::new();
    gesture.set_button(1);

    let press_state = pressed.clone();
    let drag_state = started.clone();
    let press_x = start_x.clone();
    let press_y = start_y.clone();

    gesture.connect_pressed(move |_, _, x, y| {
        press_state.set(true);
        drag_state.set(false);
        press_x.set(x);
        press_y.set(y);
    });

    let release_state = pressed.clone();
    gesture.connect_released(move |_, _, _, _| {
        release_state.set(false);
    });

    button.add_controller(gesture);

    let motion = gtk4::EventControllerMotion::new();
    let motion_pressed = pressed.clone();
    let motion_started = started.clone();
    let motion_x = start_x.clone();
    let motion_y = start_y.clone();
    let motion_origin = origin.clone();

    motion.connect_motion(move |_, x, y| {
        if !motion_pressed.get() || motion_started.get() {
            return;
        }

        let dx = x - motion_x.get();
        let dy = y - motion_y.get();

        if dx * dx + dy * dy
            < DRAG_START_THRESHOLD_PX * DRAG_START_THRESHOLD_PX
        {
            return;
        }

        match begin_core_drag(
            gtk_window_ptr,
            &motion_origin,
        ) {
            1 => {
                motion_started.set(true);
                motion_pressed.set(false);

                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_DRAG status=BEGIN origin={:?}",
                    motion_origin
                );
            }
            0 => {
                // The bridge has not observed a valid same-client primary
                // press serial yet. Keep the candidate armed so the next
                // motion can retry without inventing a serial.
            }
            rc => {
                motion_pressed.set(false);

                eprintln!(
                    "[onyrion-ewwii-plugin] TABGROUP_DRAG status=FAIL phase=begin origin={:?} rc={}",
                    motion_origin,
                    rc
                );
            }
        }
    });

    button.add_controller(motion);
    started
}

fn drag_hit_for_geometry(
    x: f64,
    y: f64,
    group_width: i32,
    group_height: i32,
    arrows_visible: bool,
    visible_ids: &[String],
) -> DragHit {
    let effective_height =
        TABGROUP_UI_HEIGHT_PX.min(group_height.max(0));

    if x < 0.0
        || y < 0.0
        || x >= f64::from(group_width.max(0))
        || y >= f64::from(effective_height)
    {
        return DragHit::None;
    }

    let tabs_start = CONTROL_SLOT_PX
        + if arrows_visible {
            CONTROL_SLOT_PX
        } else {
            0
        };

    let tabs_end = group_width.saturating_sub(
        CONTROL_SLOT_PX * 2
            + if arrows_visible {
                CONTROL_SLOT_PX
            } else {
                0
            },
    );

    if x < f64::from(tabs_start)
        || x >= f64::from(tabs_end.max(tabs_start))
    {
        return DragHit::Group;
    }

    let local = x - f64::from(tabs_start);
    let slot = (local / f64::from(TAB_SLOT_PX)).floor() as usize;

    let Some(window_id) = visible_ids.get(slot) else {
        return DragHit::Group;
    };

    let within_slot =
        local - (slot as f64 * f64::from(TAB_SLOT_PX));

    if within_slot < f64::from(TAB_SLOT_PX) / 2.0 {
        DragHit::Before(window_id.clone())
    } else {
        DragHit::After(window_id.clone())
    }
}

fn send_drag_hit(
    group_id: &str,
    hit: DragHit,
) {
    let group_id_text = group_id;
    let Ok(group_id) = CString::new(group_id_text) else {
        return;
    };

    let rc = match hit {
        DragHit::None => return,
        DragHit::Group => unsafe {
            onyrion_group_surface_bridge_set_group_drag_target(
                group_id.as_ptr(),
            )
        },
        DragHit::Before(window_id) => {
            let Ok(window_id) = CString::new(window_id) else {
                return;
            };

            unsafe {
                onyrion_group_surface_bridge_set_tab_drag_target(
                    group_id.as_ptr(),
                    window_id.as_ptr(),
                    0,
                )
            }
        }
        DragHit::After(window_id) => {
            let Ok(window_id) = CString::new(window_id) else {
                return;
            };

            unsafe {
                onyrion_group_surface_bridge_set_tab_drag_target(
                    group_id.as_ptr(),
                    window_id.as_ptr(),
                    1,
                )
            }
        }
    };

    if rc < 0 {
        eprintln!(
            "[onyrion-ewwii-plugin] TABGROUP_DRAG status=FAIL phase=target group={} rc={}",
            group_id_text,
            rc
        );
    }
}

fn install_context_gesture(button: &gtk4::Button, kind: &'static str, id: String) {
    let gesture = gtk4::GestureClick::new();
    gesture.set_button(3);
    gesture.connect_pressed(move |_, _, _, _| {
        context_action(kind, &id);
    });
    button.add_controller(gesture);
}

fn viewport_capacity_for_width(
    width: i32,
    tab_count: usize,
) -> (bool, usize) {
    let width = width.max(0);
    let fixed_without_arrows = CONTROL_SLOT_PX * 3;
    let available_without_arrows =
        width.saturating_sub(fixed_without_arrows);
    let capacity_without_arrows =
        (available_without_arrows / TAB_SLOT_PX).max(0) as usize;

    let arrows_visible =
        tab_count > capacity_without_arrows;

    let fixed = fixed_without_arrows
        + if arrows_visible {
            CONTROL_SLOT_PX * 2
        } else {
            0
        };

    let available = width.saturating_sub(fixed);
    let capacity =
        (available / TAB_SLOT_PX).max(0) as usize;

    (arrows_visible, capacity)
}

impl TabGroupUi {
    fn new(window_ptr: usize, group_id: String) -> Rc<RefCell<Self>> {
        let root = gtk4::Box::new(gtk4::Orientation::Horizontal, 0);
        root.add_css_class("onyrion-tabgroup");
        root.set_size_request(1, TABGROUP_UI_HEIGHT_PX);
        root.set_hexpand(true);
        root.set_vexpand(false);

        let group_button = fixed_button("◆", "onyrion-tabgroup-group");

        let previous_button = fixed_button("‹", "onyrion-tabgroup-arrow");

        let tabs_root = gtk4::Box::new(gtk4::Orientation::Horizontal, 0);
        tabs_root.add_css_class("onyrion-tabgroup-tabs");
        tabs_root.set_hexpand(true);
        tabs_root.set_halign(gtk4::Align::Fill);

        let next_button = fixed_button("›", "onyrion-tabgroup-arrow");

        let mode_button = fixed_button("⛶", "onyrion-tabgroup-mode");
        let close_button = fixed_button("×", "onyrion-tabgroup-close");

        root.append(&group_button);
        root.append(&previous_button);
        root.append(&tabs_root);
        root.append(&next_button);
        root.append(&mode_button);
        root.append(&close_button);

        let actions = Rc::new(RefCell::new(GroupActions::default()));

        let ui = Rc::new(RefCell::new(Self {
            window_ptr,
            group_id: group_id.clone(),
            root,
            group_button,
            previous_button,
            tabs_root,
            next_button,
            mode_button,
            close_button,
            actions,
            viewport: TabViewport::new(0),
            hold: HoldRepeat::default(),
            group_width: 1,
            group_height: 1,
            arrows_visible: false,
            visible_ids: Vec::new(),
            group: None,
        }));

        Self::install_handlers(&ui);
        ui
    }

    fn install_handlers(ui: &Rc<RefCell<Self>>) {
        let weak = Rc::downgrade(ui);
        let previous = ui.borrow().previous_button.clone();
        let gesture = gtk4::GestureClick::new();
        gesture.set_button(1);

        let pressed = weak.clone();
        gesture.connect_pressed(move |_, _, _, _| {
            if let Some(ui) = pressed.upgrade() {
                let mut ui = ui.borrow_mut();
                if let Some(step) = ui.hold.press(ViewDirection::Previous, now_ms()) {
                    ui.step_view(step.direction);
                }
            }
        });

        let released = weak.clone();
        gesture.connect_released(move |_, _, _, _| {
            if let Some(ui) = released.upgrade() {
                ui.borrow_mut().hold.release();
            }
        });
        previous.add_controller(gesture);

        let weak = Rc::downgrade(ui);
        let next = ui.borrow().next_button.clone();
        let gesture = gtk4::GestureClick::new();
        gesture.set_button(1);

        let pressed = weak.clone();
        gesture.connect_pressed(move |_, _, _, _| {
            if let Some(ui) = pressed.upgrade() {
                let mut ui = ui.borrow_mut();
                if let Some(step) = ui.hold.press(ViewDirection::Next, now_ms()) {
                    ui.step_view(step.direction);
                }
            }
        });

        let released = weak.clone();
        gesture.connect_released(move |_, _, _, _| {
            if let Some(ui) = released.upgrade() {
                ui.borrow_mut().hold.release();
            }
        });
        next.add_controller(gesture);

        let group_id = ui.borrow().group_id.clone();
        let group_button = ui.borrow().group_button.clone();
        let window_ptr = ui.borrow().window_ptr;
        install_core_drag_candidate(
            &group_button,
            window_ptr,
            DragOrigin::Group(group_id.clone()),
        );
        install_context_gesture(
            &group_button,
            "group",
            group_id,
        );

        let mode_actions = ui.borrow().actions.clone();
        let mode_group_id = ui.borrow().group_id.clone();
        ui.borrow().mode_button.connect_clicked(move |_| {
            let actions = mode_actions.borrow().clone();

            if actions.placement == "floating" {
                let verb = if actions.pinned { "unpin" } else { "pin" };
                spawn_control(
                    vec![
                        "group".to_string(),
                        verb.to_string(),
                        mode_group_id.clone(),
                    ],
                    format!("group-{verb}:{}", mode_group_id),
                );
            } else if let Some(window_id) = actions.active_window_id {
                spawn_active_window_action(window_id, "fullscreen");
            }
        });

        let close_actions = ui.borrow().actions.clone();
        ui.borrow().close_button.connect_clicked(move |_| {
            if let Some(window_id) = close_actions.borrow().active_window_id.clone() {
                spawn_active_window_action(window_id, "close");
            }
        });
    }

    fn set_geometry(&mut self, width: i32, height: i32) {
        self.group_width = width.max(1);
        self.group_height = height.max(1);
        self.root.set_size_request(
            self.group_width,
            TABGROUP_UI_HEIGHT_PX.min(self.group_height),
        );
        self.rebuild();
    }

    fn reconcile_group(&mut self, group: Option<GroupState>) {
        self.group = group;

        let ids = self
            .group
            .as_ref()
            .map(|group| {
                group
                    .windows
                    .iter()
                    .map(|window| window.id.clone())
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();

        // Own the active id before mutating `self`: borrowing `&str` from
        // self.group across update_capacity() would keep an immutable borrow
        // alive and correctly trips Rust E0502.
        let active = self
            .group
            .as_ref()
            .and_then(|group| {
                group
                    .windows
                    .iter()
                    .find(|window| window.active)
                    .map(|window| window.id.clone())
            });

        self.update_capacity(ids.len());
        self.viewport.reconcile(&ids, active.as_deref());
        self.rebuild();
    }

    fn update_capacity(&mut self, tab_count: usize) {
        let (arrows_visible, capacity) =
            viewport_capacity_for_width(
                self.group_width,
                tab_count,
            );

        self.arrows_visible = arrows_visible;
        self.viewport.set_capacity(
            capacity,
            tab_count,
        );
    }

    fn step_view(&mut self, direction: ViewDirection) {
        let tab_count = self
            .group
            .as_ref()
            .map(|group| group.windows.len())
            .unwrap_or(0);

        if self.viewport.step(direction, tab_count) {
            self.rebuild();
        }
    }

    fn rebuild(&mut self) {
        let Some(group) = self.group.clone() else {
            clear_box(&self.tabs_root);
            self.previous_button.set_visible(false);
            self.next_button.set_visible(false);
            self.mode_button.set_sensitive(false);
            self.close_button.set_sensitive(false);
            self.visible_ids.clear();
            return;
        };

        self.update_capacity(group.windows.len());

        let ids = group
            .windows
            .iter()
            .map(|window| window.id.clone())
            .collect::<Vec<_>>();

        let active = group
            .windows
            .iter()
            .find(|window| window.active)
            .map(|window| window.id.as_str());

        self.viewport.reconcile(&ids, active);

        self.previous_button.set_visible(self.arrows_visible);
        self.next_button.set_visible(self.arrows_visible);
        self.previous_button
            .set_sensitive(self.viewport.can_previous());
        self.next_button
            .set_sensitive(self.viewport.can_next(group.windows.len()));

        let active_window = group.windows.iter().find(|window| window.active);
        *self.actions.borrow_mut() = GroupActions {
            placement: group.placement.clone(),
            pinned: group.pinned,
            active_window_id: active_window.map(|window| window.id.clone()),
        };

        if group.placement == "floating" {
            self.mode_button.set_label("📌");
            if group.pinned {
                self.mode_button.add_css_class("onyrion-tabgroup-mode-active");
            } else {
                self.mode_button.remove_css_class("onyrion-tabgroup-mode-active");
            }
        } else {
            self.mode_button.set_label("⛶");
            self.mode_button
                .remove_css_class("onyrion-tabgroup-mode-active");
        }

        self.mode_button.set_visible(group.placement_seen);
        self.mode_button.set_sensitive(
            group.placement_seen && active_window.is_some()
        );
        self.close_button.set_sensitive(active_window.is_some());

        clear_box(&self.tabs_root);
        self.visible_ids.clear();

        for index in self.viewport.visible_range(group.windows.len()) {
            let window = &group.windows[index];

            let button = gtk4::Button::new();
            button.set_focusable(false);
            button.set_size_request(TAB_SLOT_PX, TABGROUP_UI_HEIGHT_PX);
            button.add_css_class("onyrion-tabgroup-tab");

            if window.active {
                button.add_css_class("onyrion-tabgroup-tab-active");
            }

            let image = gtk4::Image::from_icon_name(icon_name(&window.app_id));
            image.set_pixel_size(TAB_ICON_PX);
            image.set_can_target(false);
            button.set_child(Some(&image));

            let drag_started = install_core_drag_candidate(
                &button,
                self.window_ptr,
                DragOrigin::Window(window.id.clone()),
            );

            let focus_id = window.id.clone();
            button.connect_clicked(move |_| {
                if drag_started.replace(false) {
                    return;
                }

                spawn_control(
                    vec![
                        "window".to_string(),
                        "focus".to_string(),
                        focus_id.clone(),
                    ],
                    format!("window-focus:{}", focus_id),
                );
            });

            install_context_gesture(
                &button,
                "window",
                window.id.clone(),
            );

            self.tabs_root.append(&button);
            self.visible_ids.push(window.id.clone());
        }
    }

    fn tick_hold(&mut self, now: u64) {
        if let Some(step) = self.hold.due(now) {
            self.step_view(step.direction);
        }
    }
}

pub fn prepare_window(
    gtk_window_ptr: usize,
    group_id: &str,
) -> Result<i32, String> {
    if gtk_window_ptr == 0 {
        return Err("null GtkWindow pointer".to_string());
    }

    let group_id = group_id.to_string();

    let window: gtk4::Window = unsafe {
        from_glib_none(
            gtk_window_ptr as *mut gtk4::ffi::GtkWindow
        )
    };

    if window.is_realized() {
        return Err(format!(
            "TabGroup {} prepare called after realize",
            group_id
        ));
    }

    let ui = TabGroupUi::new(gtk_window_ptr, group_id.clone());
    window.set_child(Some(&ui.borrow().root));

    let state = pending()
        .lock()
        .expect("tabgroup pending state mutex poisoned")
        .state
        .clone();

    if let Some(state) = state {
        ui.borrow_mut()
            .reconcile_group(group_for_id(&state, &group_id));
    }

    UIS.with(|slot| {
        slot.borrow_mut().insert(gtk_window_ptr, ui);
    });

    eprintln!(
        "[onyrion-ewwii-plugin] TABGROUP_UI status=PREPARED group={} window_ptr={}",
        group_id,
        gtk_window_ptr
    );

    Ok(TABGROUP_UI_HEIGHT_PX)
}

pub fn forget_window(gtk_window_ptr: usize) {
    UIS.with(|slot| {
        if slot.borrow_mut().remove(&gtk_window_ptr).is_some() {
            eprintln!(
                "[onyrion-ewwii-plugin] TABGROUP_UI status=RELEASED window_ptr={}",
                gtk_window_ptr
            );
        }
    });
}

#[no_mangle]
pub extern "C" fn onyrion_tabgroup_group_configure(
    gtk_window_ptr: usize,
    group_width: i32,
    group_height: i32,
) {
    if group_width <= 0 || group_height <= 0 {
        return;
    }

    UIS.with(|slot| {
        let ui = slot.borrow().get(&gtk_window_ptr).cloned();
        if let Some(ui) = ui {
            ui.borrow_mut()
                .set_geometry(group_width, group_height);
        }
    });
}

#[no_mangle]
pub unsafe extern "C" fn onyrion_tabgroup_drag_surface_motion(
    group_id_ptr: *const std::ffi::c_char,
    x: f64,
    y: f64,
) {
    if group_id_ptr.is_null() {
        return;
    }

    let Ok(group_id) =
        CStr::from_ptr(group_id_ptr).to_str()
    else {
        return;
    };

    let ui = UIS.with(|slot| {
        slot.borrow()
            .values()
            .find(|ui| ui.borrow().group_id == group_id)
            .cloned()
    });

    let Some(ui) = ui else {
        return;
    };

    let hit = {
        let ui = ui.borrow();

        drag_hit_for_geometry(
            x,
            y,
            ui.group_width,
            ui.group_height,
            ui.arrows_visible,
            &ui.visible_ids,
        )
    };

    send_drag_hit(group_id, hit);
}

#[no_mangle]
pub extern "C" fn onyrion_tabgroup_role_released(
    gtk_window_ptr: usize,
) {
    forget_window(gtk_window_ptr);
}

pub fn init() {
    gtk4::glib::timeout_add_local(
        Duration::from_millis(TICK_MS),
        move || {
            let snapshot = {
                let mut pending = pending()
                    .lock()
                    .expect("tabgroup pending state mutex poisoned");

                if pending.generation == pending.applied_generation {
                    None
                } else {
                    pending.applied_generation = pending.generation;
                    pending.state.clone()
                }
            };

            let uis = UIS.with(|slot| {
                slot.borrow().values().cloned().collect::<Vec<_>>()
            });

            if let Some(state) = snapshot {
                for ui in &uis {
                    let group_id = ui.borrow().group_id.clone();
                    let group = group_for_id(&state, &group_id);
                    ui.borrow_mut().reconcile_group(group);
                }
            }

            let now = now_ms();
            for ui in &uis {
                ui.borrow_mut().tick_hold(now);
            }

            gtk4::glib::ControlFlow::Continue
        },
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(raw: &str) -> ShellState {
        serde_json::from_str(raw).expect("state")
    }

    #[test]
    fn custom_group_role_renderer_has_no_native_tooltips() {
        let source = include_str!("tabgroup_renderer.rs");
        assert!(!source.contains(concat!("set_tooltip_", "text")));
    }

    #[test]
    fn validates_icon_only_group_projection() {
        let state = state(
            r#"{
                "generation":9,
                "block":"bar",
                "outputs":[{
                    "groups":[{
                        "id":"7",
                        "placement":"tiled",
                        "placement_seen":true,
                        "pinned":false,
                        "windows":[
                            {"id":"11","app_id":"kitty","label":"Kitty","title":"A","active":true},
                            {"id":"12","app_id":"org.mozilla.zen","label":"Zen","title":"B","active":false}
                        ]
                    }]
                }]
            }"#,
        );

        assert!(validate_state(&state).is_ok());
        let group = group_for_id(&state, "7").expect("group 7");
        assert_eq!(group.windows.len(), 2);
        assert_eq!(group.windows[0].app_id, "kitty");
    }

    #[test]
    fn rejects_duplicate_window_ids() {
        let duplicate_window = state(
            r#"{
                "generation":1,
                "block":"bar",
                "outputs":[{
                    "groups":[
                        {"id":"1","placement":"tiled","placement_seen":true,"windows":[{"id":"2"}]},
                        {"id":"3","placement":"tiled","placement_seen":true,"windows":[{"id":"2"}]}
                    ]
                }]
            }"#,
        );
        assert!(validate_state(&duplicate_window).is_err());
    }

    #[test]
    fn width_capacity_is_whole_slot_and_arrow_aware() {
        // 32px Group + 2 right controls = 96px fixed.
        assert_eq!(
            viewport_capacity_for_width(224, 4),
            (false, 4)
        );

        // Five tabs no longer fit without arrows. Adding both 32px arrows
        // leaves exactly two complete 32px tab slots.
        assert_eq!(
            viewport_capacity_for_width(224, 5),
            (true, 2)
        );

        // No partial tab is ever reported.
        assert_eq!(
            viewport_capacity_for_width(207, 4),
            (true, 1)
        );
    }

    #[test]
    fn drag_hit_maps_complete_icon_halves_to_before_after() {
        let ids = vec![
            "11".to_string(),
            "12".to_string(),
        ];

        // With arrows: Group [0..32], prev [32..64], tabs start at 64.
        assert_eq!(
            drag_hit_for_geometry(
                65.0,
                10.0,
                224,
                720,
                true,
                &ids,
            ),
            DragHit::Before("11".to_string())
        );

        assert_eq!(
            drag_hit_for_geometry(
                81.0,
                10.0,
                224,
                720,
                true,
                &ids,
            ),
            DragHit::After("11".to_string())
        );

        assert_eq!(
            drag_hit_for_geometry(
                97.0,
                10.0,
                224,
                720,
                true,
                &ids,
            ),
            DragHit::Before("12".to_string())
        );
    }

    #[test]
    fn drag_hit_uses_group_target_for_controls_and_blank_strip() {
        let ids = vec!["11".to_string()];

        assert_eq!(
            drag_hit_for_geometry(
                12.0,
                10.0,
                224,
                720,
                true,
                &ids,
            ),
            DragHit::Group
        );

        // Second logical slot is blank because only one icon is visible.
        assert_eq!(
            drag_hit_for_geometry(
                100.0,
                10.0,
                224,
                720,
                true,
                &ids,
            ),
            DragHit::Group
        );

        // Right arrow/right-side controls are not tab hit geometry.
        assert_eq!(
            drag_hit_for_geometry(
                170.0,
                10.0,
                224,
                720,
                true,
                &ids,
            ),
            DragHit::Group
        );
    }

    #[test]
    fn drag_hit_without_arrows_starts_immediately_after_group_control() {
        let ids = vec![
            "21".to_string(),
            "22".to_string(),
        ];

        assert_eq!(
            drag_hit_for_geometry(
                33.0,
                8.0,
                224,
                720,
                false,
                &ids,
            ),
            DragHit::Before("21".to_string())
        );

        assert_eq!(
            drag_hit_for_geometry(
                49.0,
                8.0,
                224,
                720,
                false,
                &ids,
            ),
            DragHit::After("21".to_string())
        );
    }

    #[test]
    fn drag_hit_rejects_coordinates_outside_attached_surface() {
        let ids = vec!["1".to_string()];

        assert_eq!(
            drag_hit_for_geometry(
                -1.0,
                10.0,
                224,
                720,
                false,
                &ids,
            ),
            DragHit::None
        );

        assert_eq!(
            drag_hit_for_geometry(
                40.0,
                32.0,
                224,
                720,
                false,
                &ids,
            ),
            DragHit::None
        );
    }

    #[test]
    fn icon_lookup_has_stable_fallback() {
        assert_eq!(icon_name("kitty"), "kitty");
        assert_eq!(
            icon_name(""),
            "application-x-executable-symbolic"
        );
    }
}
