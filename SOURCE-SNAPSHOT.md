# Source snapshot

Generated: `2026-09-10T14:33:22Z`

## Core

- HEAD: `413f046dd2ea11af4e155a96ab18109f02df1a19`
- Branch: `main`
- Untracked non-ignored files included: `3`

```text
 M include/action.h
 M include/group.h
 M include/input.h
 M include/server.h
 M include/shell_protocol.h
 M include/window.h
 M meson.build
 M protocol/onyrion-shell-unstable-v1.xml
 M src/action.c
 M src/config.c
 M src/group.c
 M src/input.c
 M src/layout.c
 M src/server.c
 M src/shell_protocol.c
 M src/window.c
 M src/workspace.c
 M tools/test-shell.c
?? include/group_surface.h
?? src/group_surface.c
?? tools/test-group-surface-configure.c
```

## Shell

- HEAD: `cec69f6fc853a0b57cfd4766982caaec1adaa12c`
- Branch: `main`
- Untracked non-ignored files included: `12`

```text
 M config/shell.kdl
 M docs/shell-architecture.md
 M include/shell_state.h
 M include/ui_actions_v11.h
 M meson.build
 M plugins/onyrion-ewwii-plugin/Cargo.toml
 M plugins/onyrion-ewwii-plugin/build.sh
 M plugins/onyrion-ewwii-plugin/src/audio_renderer.rs
 M plugins/onyrion-ewwii-plugin/src/bluetooth_renderer.rs
 M plugins/onyrion-ewwii-plugin/src/lib.rs
 M plugins/onyrion-ewwii-plugin/src/scalar_projection.rs
 M plugins/onyrion-ewwii-plugin/src/wifi_renderer.rs
 M protocol/onyrion-shell-unstable-v1.xml
 M src/bluetooth_extended.c
 M src/launcher.c
 M src/main.c
 M src/session.c
 M src/state.c
 M src/ui_actions_v11.c
 M src/ui_daily_v11.c
 M src/ui_runtime.c
 M src/wifi_extended.c
 M ui/ewwii/ewwii.nbcl
 M ui/ewwii/ewwii.scss
 M ui/ewwii/start-ewwii.sh
?? docs/tabgroup-v13-integration.md
?? include/ewwii_adapter.h
?? include/widget_model.h
?? plugins/onyrion-ewwii-plugin/build.rs
?? plugins/onyrion-ewwii-plugin/src/bar_renderer.rs
?? plugins/onyrion-ewwii-plugin/src/group_surface_bridge.c
?? plugins/onyrion-ewwii-plugin/src/group_surface_role.rs
?? plugins/onyrion-ewwii-plugin/src/tabgroup_model.rs
?? plugins/onyrion-ewwii-plugin/src/tabgroup_renderer.rs
?? src/ewwii_adapter.c
?? src/widget_model.c
?? tests/
```

## Public release boundary

- Project version from Meson: `0.1.0`
- Explicit alpha/beta field in source: `NONE`
- Publication stage: `pre-beta-development-snapshot`

The public snapshot contains current working-tree bytes of tracked files plus
untracked non-ignored source files from the canonical Core and Shell
repositories. Ignored build/cache output and nested Git metadata are excluded.
The canonical owner repositories are not modified by this publication.
