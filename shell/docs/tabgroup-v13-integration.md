# TabGroup UI — Core v13 Shell integration status

## Authoritative UX

```text
TILED  [◆] [‹] [APP][APP]… [›] [⛶] [×]
FLOAT  [◆] [‹] [APP][APP]… [›] [📌] [×]
```

Tabs are always application-icon-only.  Shell owns presentation, viewport,
RMB, controls and exact tab hit geometry.  Core owns compositor state and final
drag/drop mutation.

## Source foundation in this slice

- Shell protocol XML synchronized exactly to current Core v13 XML.
- Pure `TabViewport` model implements discrete full-tab view movement, active
  reveal and normalization.
- Pure `HoldRepeat` model implements immediate step + delayed accelerating
  repeat with a hard maximum and no catch-up burst.
- Unit tests cover viewport and hold invariants.
- Old RMB menus attached to Ewwii **topbar** tabs/groups are removed.  Final
  Window/Group RMB belongs to the Group-attached TabGroup surface.
- Topbar application rows gain a non-accepting DnD hover controller: staying
  over the same application for 3 seconds focuses that exact Window once;
  leave/drag-end/target change cancels the pending activation.

## Not implemented yet: final Group surface renderer

This is blocked by transport, not by missing UI policy.

### Blocker A — Group geometry is not observable

`onyrion_group_surface_v1.set_rect()` accepts a requested rectangle, but Core
v13 emits no Group outer width/height/configure event.  The current state stream
also has no Group geometry.  Therefore a Shell renderer cannot correctly know
how many complete icon tabs fit.  Requesting an arbitrarily large surface and
relying on compositor clipping would make GTK allocation disagree with visible
geometry and would break overflow/arrows.

Required Core follow-up: expose generic Group-surface geometry (for example a
`configure(width,height)` event) initially and after every outer-size change.
This must remain geometry-only; no TabBar semantics belong in Core.

### Ewwii same-client role path — source implemented, runtime not yet V

The v13 role request consumes a `wl_surface` owned by the requesting Wayland
client.  `onyrion-shell` and Ewwii are separate Wayland clients, so the daemon
cannot pass or role Ewwii's GTK `wl_surface` over its own connection.

The Shell plugin now has source for a same-process Ewwii Wayland bridge. It
binds `onyrion_shell_unstable_v1` version 13 on Ewwii's own GDK Wayland
connection before realization and registers a custom-role candidate through the
generic pre-realize/gtk4-layer-shell seam. When GTK attempts
`xdg_wm_base.get_xdg_surface`, the create callback sends
`get_group_surface()` for that exact Ewwii `wl_surface`, sets a 1x1 bootstrap
rect and explicit above-client relation, then returns TRUE so the real XDG
request is suppressed.

The 1x1 rectangle is deliberately only a role/bootstrap probe. It reserves no
Group content and is not the final TabGroup geometry. Once Core supplies
authoritative Group dimensions, the same bridge exposes a configure path which
updates both `onyrion_group_surface_v1.set_rect()` and GTK's client-only fake
XDG facade.

This source path is **A/build-testable, not runtime V** until Core v13 is
actually running and a valid Group probe proves `CreateFunc(TRUE)` end to end.
It does not justify putting tabs or layout policy back in Core.

## Next implementation boundary

After generic Group geometry is available and the same-client bridge has a
runtime TRUE-role proof, wire:

1. one Group-attached surface per visible Group;
2. Shell-selected rect/layer + independent content insets;
3. icon-only TabGroup widget and local viewport capacity from configured width;
4. Group/tab click + RMB + TILED/FLOAT controls;
5. Shell-origin `begin_window_drag` / `begin_group_drag` with real input serial;
6. `drag_surface_motion` -> exact tab before/after/body hit test ->
   `set_window_drag_target`;
7. owner disconnect/fallback and runtime/physical regression gates.


## v14 geometry consumer / final Shell ownership

Core v14 closes the previous geometry blocker with:

```text
onyrion_group_surface_v1.configure(width,height)
```

where width/height are authoritative Group `outer_box` dimensions before
Shell content insets.

The Shell bridge now treats the old 1x1 rectangle as pre-configure bootstrap
only.  For `onyrion.tabgroup`, the first v14 configure selects the concrete
Shell policy:

```text
surface rect = (0, 0, group_width, 32)
layer        = above_client
content inset top = 32
```

This placement is Shell policy, not Core policy.  The protocol still allows a
different rect/layer/inset choice for another Group-attached namespace.

The final native TabGroup renderer is created directly during Ewwii's
pre-realize hook, replacing the placeholder root for each dynamic
`onyrion-tabgroup:<group-id>` instance.  It consumes the same authoritative
`onyrion_state` Listen stream as the topbar and stores/reconciles state without
HostProxy calls from the pre-realize callback.

Implemented source behavior:

```text
TAB = app icon only

TILED:
[◆] [‹] [APP]... [›] [⛶] [×]

FLOATING:
[◆] [‹] [APP]... [›] [📌] [×]
```

Overflow capacity is derived from the actual v14 Group width using complete
32px slots.  `TabViewport` remains Shell-local.  Manual arrow movement never
changes Core active Window; active Window changes reveal the active icon.
`HoldRepeat` remains the tested no-catch-up implementation.

RMB is Shell-owned and invokes the existing Shell daemon context-action
provider directly over the Shell control socket:

```text
tab RMB   -> ui:context-actions window:<id>
group RMB -> ui:context-actions group:<id>
```

The Core native chrome hit-test is not used for these gestures.

The Ewwii adapter owns lifetime of dynamic TabGroup windows:

```text
projected visible Group appears
 -> ewwii open onyrion-tabgroup --id onyrion-tabgroup:<group-id>

Group disappears from projected visible state
 -> ewwii close onyrion-tabgroup:<group-id>
```

## Shell-origin drag bridge source

The Group-attached renderer now has a same-client pointer serial path without
reaching into GDK private event internals.

The bridge binds its own `wl_seat` / `wl_pointer` resource on Ewwii's existing
Wayland client connection.  A primary-button press serial is accepted only
while that press originated on the exact `wl_surface` owned by the target
TabGroup role.  The serial is consumed once per physical press.

The GTK renderer arms a local 6px movement threshold on the Group control and
each visible app-icon tab.  Crossing the threshold sends exactly one:

```text
tab   -> begin_window_drag(window_id, BTN_LEFT, real_pointer_serial)
Group -> begin_group_drag(group_id, BTN_LEFT, real_pointer_serial)
```

No synthetic serial is generated.  If the same-client pointer listener has not
observed a valid press yet, the renderer stays armed and retries on a later
motion event instead of inventing/falling back to another serial.

During a Core-owned Window drag:

```text
Core drag_surface_motion(group, "onyrion.tabgroup", local_x, local_y)
-> C bridge
-> Rust renderer current geometry
-> Group / tab-before / tab-after
-> set_window_drag_target(...)
```

Hit geometry uses the renderer's current `visible_ids`, arrow visibility and
32px complete tab slots.  Non-tab controls or blank strip area resolve to the
semantic Group target.  Layout-edge classification remains Core-owned.

Still pending after this source slice:

```text
real pointer/drag runtime + physical V
same-Group reorder physical V
cross-Group before/after physical V
Group-body merge physical V
Tile-edge split/move regression V
Group drag physical V
production deployment of patched Ewwii + gtk4-layer-shell + Shell plugin
coordinated Core/Shell/Ewwii restart and full physical gate
```

## Group-attached surface popup boundary

Fresh physical runtime evidence established an additional constraint on the
custom Group role integration:

```text
Group-attached GtkWindow
    -> client-only fake xdg_surface inside gtk4-layer-shell shim
    -> real compositor role = onyrion_group_surface_v1
```

A native GTK tooltip/GdkPopup child cannot use that fake xdg_surface as a
compositor-visible `xdg_popup` parent. The Wayland layer-shell protocol solves
the analogous non-XDG-parent case with its own popup-adoption request; the
current Onyrion Group-surface contract does not expose such a request.

Therefore the TabGroup renderer must not create native GTK tooltip/GdkPopup
children from the Group-attached window. In particular, do not add
`set_tooltip_text()` back to Group controls or tabs.

Auxiliary UI must currently use one of:

```text
in-surface widgets inside the Group-attached surface
separate Shell-owned surface/window with its own valid role
```

The existing Shell `ui:context-actions` transport remains separate from this
native-child-popup restriction.

Physical evidence preceding this rule:

```text
Group surfaces claimed successfully
-> fresh wl_surface
-> compositor observes new xdg_surface
-> error in client communication
-> Gdk Error 71 (Protocol error)
```

No `POINTER_PRIMARY_PRESS`, `DRAG_BEGIN`, `TABGROUP_DRAG`, new SIGSEGV, or new
Ewwii coredump preceded that failure. Thus the failure happened before the
Onyrion Core-drag bridge actually began a drag.

