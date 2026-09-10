#![allow(dead_code)]

// Pure Shell-local TabGroup state.  This module deliberately contains no Core
// authority and no GTK code so viewport/hold policy can be verified before the
// Group-surface renderer is wired to the v13 Wayland bridge.

pub const HOLD_INITIAL_DELAY_MS: u64 = 420;
pub const HOLD_MIN_INTERVAL_MS: u64 = 55;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ViewDirection {
    Previous,
    Next,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct TabViewport {
    first_visible: usize,
    capacity: usize,
    last_active: Option<String>,
}

impl TabViewport {
    pub fn new(capacity: usize) -> Self {
        Self {
            first_visible: 0,
            capacity,
            last_active: None,
        }
    }

    pub fn first_visible(&self) -> usize {
        self.first_visible
    }

    pub fn capacity(&self) -> usize {
        self.capacity
    }

    pub fn visible_range(&self, tab_count: usize) -> std::ops::Range<usize> {
        if self.capacity == 0 || tab_count == 0 {
            return 0..0;
        }

        let start = self.first_visible.min(tab_count);
        let end = start.saturating_add(self.capacity).min(tab_count);
        start..end
    }

    pub fn can_previous(&self) -> bool {
        self.capacity > 0 && self.first_visible > 0
    }

    pub fn can_next(&self, tab_count: usize) -> bool {
        self.capacity > 0
            && tab_count > self.capacity
            && self.first_visible < tab_count - self.capacity
    }

    pub fn set_capacity(&mut self, capacity: usize, tab_count: usize) {
        self.capacity = capacity;
        self.normalize(tab_count);
    }

    /// Reconcile the Shell-local viewport with the authoritative ordered tab
    /// list.  Manual viewport movement is preserved while the same Window
    /// remains active.  A changed active Window is revealed.
    pub fn reconcile(&mut self, tabs: &[String], active_id: Option<&str>) {
        self.normalize(tabs.len());

        let active_changed = self.last_active.as_deref() != active_id;
        if active_changed {
            if let Some(active_id) = active_id {
                if let Some(index) = tabs.iter().position(|id| id == active_id) {
                    self.reveal(index, tabs.len());
                }
            }
            self.last_active = active_id.map(ToOwned::to_owned);
        }

        self.normalize(tabs.len());
    }

    /// Move the viewport by exactly one complete logical tab.  This never
    /// changes the active Window or compositor focus.
    pub fn step(&mut self, direction: ViewDirection, tab_count: usize) -> bool {
        if self.capacity == 0 || tab_count <= self.capacity {
            self.first_visible = 0;
            return false;
        }

        let old = self.first_visible;
        match direction {
            ViewDirection::Previous => {
                self.first_visible = self.first_visible.saturating_sub(1);
            }
            ViewDirection::Next => {
                let max_first = tab_count - self.capacity;
                self.first_visible = self.first_visible.saturating_add(1).min(max_first);
            }
        }
        self.first_visible != old
    }

    fn reveal(&mut self, index: usize, tab_count: usize) {
        if self.capacity == 0 || tab_count == 0 {
            self.first_visible = 0;
            return;
        }

        if index < self.first_visible {
            self.first_visible = index;
        } else if index >= self.first_visible.saturating_add(self.capacity) {
            self.first_visible = index + 1 - self.capacity;
        }
    }

    fn normalize(&mut self, tab_count: usize) {
        if self.capacity == 0 || tab_count <= self.capacity {
            self.first_visible = 0;
            return;
        }

        let max_first = tab_count - self.capacity;
        self.first_visible = self.first_visible.min(max_first);
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HoldStep {
    pub direction: ViewDirection,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct HoldRepeat {
    direction: Option<ViewDirection>,
    pressed_at_ms: u64,
    next_due_ms: u64,
}

impl HoldRepeat {
    pub fn press(&mut self, direction: ViewDirection, now_ms: u64) -> Option<HoldStep> {
        if self.direction == Some(direction) {
            return None;
        }

        self.direction = Some(direction);
        self.pressed_at_ms = now_ms;
        self.next_due_ms = now_ms.saturating_add(HOLD_INITIAL_DELAY_MS);
        Some(HoldStep { direction })
    }

    pub fn release(&mut self) {
        self.direction = None;
        self.pressed_at_ms = 0;
        self.next_due_ms = 0;
    }

    pub fn is_active(&self) -> bool {
        self.direction.is_some()
    }

    pub fn next_due_ms(&self) -> Option<u64> {
        self.direction.map(|_| self.next_due_ms)
    }

    /// Return at most one due step.  The next deadline is based on `now_ms`,
    /// not the old deadline, so an event-loop stall cannot create a catch-up
    /// burst of missed repeats.
    pub fn due(&mut self, now_ms: u64) -> Option<HoldStep> {
        let direction = self.direction?;
        if now_ms < self.next_due_ms {
            return None;
        }

        let held_ms = now_ms.saturating_sub(self.pressed_at_ms);
        self.next_due_ms = now_ms.saturating_add(repeat_interval_ms(held_ms));
        Some(HoldStep { direction })
    }
}

fn repeat_interval_ms(held_ms: u64) -> u64 {
    match held_ms {
        0..=1199 => 180,
        1200..=2499 => 120,
        2500..=4499 => 80,
        _ => HOLD_MIN_INTERVAL_MS,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ids(items: &[&str]) -> Vec<String> {
        items.iter().map(|item| (*item).to_string()).collect()
    }

    #[test]
    fn viewport_moves_one_complete_tab_per_step() {
        let tabs = ids(&["A", "B", "C", "D", "E"]);
        let mut view = TabViewport::new(3);
        view.reconcile(&tabs, Some("A"));
        assert_eq!(view.visible_range(tabs.len()), 0..3);
        assert!(view.step(ViewDirection::Next, tabs.len()));
        assert_eq!(view.visible_range(tabs.len()), 1..4);
        assert!(view.step(ViewDirection::Next, tabs.len()));
        assert_eq!(view.visible_range(tabs.len()), 2..5);
        assert!(!view.step(ViewDirection::Next, tabs.len()));
    }

    #[test]
    fn manual_view_may_hide_same_active_window() {
        let tabs = ids(&["A", "B", "C", "D", "E"]);
        let mut view = TabViewport::new(3);
        view.reconcile(&tabs, Some("A"));
        assert!(view.step(ViewDirection::Next, tabs.len()));
        assert_eq!(view.visible_range(tabs.len()), 1..4);

        // Same authoritative active id must not undo manual viewport movement.
        view.reconcile(&tabs, Some("A"));
        assert_eq!(view.visible_range(tabs.len()), 1..4);
    }

    #[test]
    fn changed_active_window_is_revealed() {
        let tabs = ids(&["A", "B", "C", "D", "E"]);
        let mut view = TabViewport::new(3);
        view.reconcile(&tabs, Some("A"));
        view.step(ViewDirection::Next, tabs.len());
        view.reconcile(&tabs, Some("E"));
        assert_eq!(view.visible_range(tabs.len()), 2..5);
    }

    #[test]
    fn removal_normalizes_trailing_edge_without_empty_slots() {
        let mut view = TabViewport::new(3);
        let before = ids(&["A", "B", "C", "D", "E", "F"]);
        view.reconcile(&before, Some("A"));
        view.step(ViewDirection::Next, before.len());
        view.step(ViewDirection::Next, before.len());
        view.step(ViewDirection::Next, before.len());
        assert_eq!(view.first_visible(), 3);

        let after = ids(&["A", "B", "C", "D"]);
        view.reconcile(&after, Some("A"));
        assert_eq!(view.first_visible(), 1);
        assert_eq!(view.visible_range(after.len()), 1..4);
    }

    #[test]
    fn viewport_boundaries_disable_invalid_steps() {
        let tabs = ids(&["A", "B", "C", "D"]);
        let mut view = TabViewport::new(2);
        view.reconcile(&tabs, Some("A"));
        assert!(!view.can_previous());
        assert!(view.can_next(tabs.len()));
        assert!(!view.step(ViewDirection::Previous, tabs.len()));
        assert!(view.step(ViewDirection::Next, tabs.len()));
        assert!(view.step(ViewDirection::Next, tabs.len()));
        assert!(!view.can_next(tabs.len()));
        assert!(!view.step(ViewDirection::Next, tabs.len()));
    }

    #[test]
    fn zero_capacity_has_no_partial_or_invalid_view() {
        let tabs = ids(&["A", "B"]);
        let mut view = TabViewport::new(0);
        view.reconcile(&tabs, Some("B"));
        assert_eq!(view.visible_range(tabs.len()), 0..0);
        assert!(!view.step(ViewDirection::Next, tabs.len()));
        assert!(!view.can_previous());
        assert!(!view.can_next(tabs.len()));
    }

    #[test]
    fn hold_has_immediate_step_and_accelerates_to_hard_max() {
        let mut hold = HoldRepeat::default();
        assert_eq!(
            hold.press(ViewDirection::Next, 1000),
            Some(HoldStep {
                direction: ViewDirection::Next
            })
        );
        assert_eq!(hold.next_due_ms(), Some(1420));
        assert_eq!(hold.due(1419), None);
        assert!(hold.due(1420).is_some());
        assert_eq!(hold.next_due_ms(), Some(1600));

        assert!(hold.due(5500).is_some());
        assert_eq!(hold.next_due_ms(), Some(5500 + HOLD_MIN_INTERVAL_MS));
    }

    #[test]
    fn stalled_loop_never_emits_catch_up_burst() {
        let mut hold = HoldRepeat::default();
        hold.press(ViewDirection::Next, 0);
        assert!(hold.due(10_000).is_some());
        assert_eq!(hold.due(10_000), None);
        assert_eq!(hold.next_due_ms(), Some(10_000 + HOLD_MIN_INTERVAL_MS));
    }

    #[test]
    fn release_and_direction_change_reset_hold_state() {
        let mut hold = HoldRepeat::default();
        hold.press(ViewDirection::Next, 0);
        assert!(hold.due(5_000).is_some());
        hold.release();
        assert!(!hold.is_active());
        assert_eq!(hold.due(9_000), None);

        assert_eq!(
            hold.press(ViewDirection::Previous, 9_000),
            Some(HoldStep {
                direction: ViewDirection::Previous
            })
        );
        assert_eq!(hold.next_due_ms(), Some(9_000 + HOLD_INITIAL_DELAY_MS));

        // Direction change while held also starts a new acceleration epoch and
        // emits one immediate step in the new direction.
        assert_eq!(
            hold.press(ViewDirection::Next, 9_100),
            Some(HoldStep {
                direction: ViewDirection::Next
            })
        );
        assert_eq!(hold.next_due_ms(), Some(9_100 + HOLD_INITIAL_DELAY_MS));
    }
}
