# Onyrion Shell architecture

## Authority

`onyrion-shell` is the persistent C23 desktop authority between Onyrion core and
replaceable generic UI providers.

Generic UI failure must not terminate the shell or make application launching,
window management, workspace actions, shortcut handling, or provider recovery
unavailable.

## UI provider model

Provider selection is tag-based.

```text
tag      -> who can handle a capability
action   -> what Onyrion wants done
adapter  -> how that provider is invoked
```

Reserved tag namespaces:

```text
ui:*
system:*
user:*
```

Initial UI capabilities include:

```text
ui:bar
ui:launcher
ui:tray
ui:clipboard
ui:powermenu
ui:quicksettings
ui:osd
ui:notifications
```

A provider may expose many tags, and many providers may expose the same tag.
The highest-priority healthy provider is selected. Health and supervision are
part of the provider runtime rather than UI-provider-specific code.

Ewwii is the default generic UI provider, not an architectural dependency.
Custom and multiple providers are allowed.

## Trusted/native UI

These remain outside generic UI providers:

- compositor-coupled TabGroupUI/window chrome;
- lock/security surface;
- emergency/degraded-mode overlay.

## Failure model

If the default UI provider dies, the shell stays alive, marks its capabilities
unavailable, keeps direct application launching functional, presents the native
fallback overlay, and later supervises/restarts the provider.

The fallback overlay content and emergency application shortcuts come from
Onyrion configuration rather than hard-coded application names.

## Configuration

Initial source of truth:

```text
~/.config/onyrion/shell/config.json
```

The future ConfEditor edits the Onyrion semantic model. Provider-specific files
are generated/adapted from it rather than becoming the architecture itself.

## Foundation slice

The first implemented slice contains:

1. persistent shell process;
2. atomic `state_begin` -> `workspace*` -> `state_end` snapshot commits;
3. `changed` -> fresh authoritative snapshot resync;
4. JSON configuration loading;
5. provider/tag model and deterministic priority resolution;
6. no provider spawning or supervision yet.

Reconnect after compositor loss, UI provider execution, shortcuts, GTK UI,
fallback rendering, and provider health checks are later slices.


## Provider supervision slice

Autostart providers are now supervised by the persistent shell process.

Health mode `process` keeps the original semantics: successful spawn makes the
provider READY and child exit makes it unavailable. Health mode `command` adds
a real readiness/health probe. The provider starts in STARTING, becomes READY
only after `health_exec` exits successfully, and is removed from runtime tag
resolution when the health command fails. Health probes are serialized and
time-bounded by `health_timeout_ms`; initial readiness is bounded by
`health_startup_timeout_ms`.

Restart policy remains configurable as `never`, `on-failure`, or `always`, with
exponential delay capped by `restart_max_delay_ms`. A command-health startup
timeout terminates the unhealthy provider process so normal restart policy can
apply.

Tag resolution at runtime considers only providers in the ready state. This
means a higher-priority provider can fail and the same tag can immediately
resolve to a lower-priority healthy provider without making the shell fail.

Provider absence is not fatal to the desktop session. `required` currently
changes diagnostics/startup status only; it does not panic or terminate the
shell. Native degraded-mode UI is implemented in a later slice.

## Internal provider invocation

The persistent shell owns provider runtime state. Shortcut/front-end clients do
not execute provider commands directly. They send a semantic request to the
shell control socket:

```text
tag      -> who can handle the capability
action   -> what to do
adapter  -> how to execute it
```

Initial control transport is a per-user Unix socket at
`$XDG_RUNTIME_DIR/onyrion-shell/control.sock`. The CLI form is:

```text
onyrion-shell invoke ui:launcher open
```

For `type=command`, actions are explicit argv arrays and are spawned without an
implicit shell. Runtime selection considers READY state, tag membership, action
support, and priority. If action spawn fails for the highest-priority eligible
provider, lower-priority eligible providers are attempted.

Ewwii is initially integrated through its public CLI (`daemon`, `ping`,
`open`, `close`) using the same argv adapter and command-health mechanism. The
shell does not bind to Ewwii's private implementation IPC. This does not change
the `tag/action` contract.

## Provider-facing state data path

The shell daemon owns the committed snapshot received from
`onyrion_shell_unstable_v1`.

External UI providers must not connect to the private core protocol merely to
duplicate shell state tracking. The shell exposes the currently committed
snapshot through its per-user control socket.

The first public query is:

```text
onyrion-shell state
```

It returns one JSON object representing one atomically committed generation:

```json
{
  "generation": 4,
  "workspaces": [
    {
      "id": "1",
      "active": true,
      "groups": 0,
      "tiles": 0,
      "windows": 0
    }
  ]
}
```

This is a data contract, not a provider-specific Ewwii API. A streaming/update
mechanism can be layered on the same canonical snapshot later without changing
the authoritative state model.

### Event-driven state consumer

For long-lived UI consumers the shell exposes:

```text
onyrion-shell watch-state
```

The command writes one compact JSON object per line. The first line is the
current committed snapshot. Further lines are emitted only after a newer
snapshot has been atomically committed following a core `changed`/resync cycle.

The CLI is event-driven and does not poll on a timer. Internally it uses a
generation-aware long-poll request on the same per-user control socket. After
each delivered generation it immediately waits for a generation newer than the
one already seen. If state changes between requests, the next request is
answered immediately with the latest committed generation.

The daemon retains only pending one-shot waiters. A waiter is answered once and
its socket is closed. Dead waiters are discarded on delivery, and all pending
waiters are closed during daemon shutdown. The data path is provider-neutral;
Ewwii is only a consumer of the JSON-lines stream.
