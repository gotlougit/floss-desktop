# Isolated BlueDevil smoke check

Supply already-built package outputs from the same pinned Nixpkgs version:

```sh
python3 tools/smoke-kde-isolated.py \
  --bluedevil /path/to/built-bluedevil \
  --plasma /path/to/plasma-workspace \
  --desktop /path/to/plasma-desktop \
  --breeze /path/to/qqc2-breeze-style \
  --output /tmp/floss-kde-smoke-report
```

The runner queries existing Nix store closures; it does not download packages.
It loads the Floss QML backend, the Bluetooth KCM in `kcmshell6`, and the actual
Bluetooth applet in `plasmawindowed`. Each case uses a private D-Bus with no
activation directories. Both bus addresses point to that private bus. Qt uses
offscreen/software rendering, and home/config/cache/runtime/state are temporary.
No host display, host D-Bus or Bluetooth hardware is used.

The backend must report unavailable/no devices and successfully attach/detach a
pairing view. KCM/applet processes must stay alive for the observation window
without QML loading errors. They are then terminated along with their private
bus. Logs and a machine-readable result remain at the output path.

Without the optional mock below, this verifies initialization and the disconnected
UI only. It does not verify actual pairing, active-seat policy, visual appearance,
or integration inside the user's Plasma shell. Hardware checks and a real login
after system activation remain necessary.

On 2026-09-17 all three checks passed against the corrected BlueDevil output
`/nix/store/s38vbs1q5p5f4hvazgingckr1d4m47hm-bluedevil-floss-6.7.90-c15cdd83448c`.
The first actual applet-host run found a QML property unsupported by pinned
Plasma 6.7.4 (`PlasmaCore.Action.alwaysShowAsMenu`); that optional menu hint was
removed, BlueDevil rebuilt, and all checks repeated. This failure was invisible
to the successful C++ build and the simpler backend/KCM checks.

Offscreen warnings about platform plugins, window shadows and the omitted icon
theme remain expected in this harness. They do not validate visual appearance.

## Synthetic device and callback scenario

Build the small test double with a cached C++ compiler and matching Qt base output
(the arguments are executable/output paths already present locally; no fetching):

```sh
bash tools/kde-mock/build.sh /path/to/c++ /path/to/qtbase /tmp/floss-kde-mock
```

Add `--mock-helper /tmp/floss-kde-mock` to the runner command. This adds a separate
private-bus case using the **real built BlueDevil QML backend** and a **mock Floss
manager/adapter**, with a fake device named `Mock Floss headset`. The case checks:

- Adapter initialization and manager Start/Stop action signatures.
- Discovery callbacks and device address/name/bond/connection properties.
- SSP confirmation prompt, user answer dispatch, bond completion and automatic
  connect request.
- Connected/disconnected callbacks, forget, and discovery cancellation.
- Rejection of a callback from a different D-Bus sender.
- Clearing availability and the device model when the daemon loses its name.

These are protocol/UI-state tests. The fake implements only the methods needed
by this scenario; its successful replies do not establish that actual Floss
accepts a request, a headset pairs, profile negotiation succeeds, or audio works.
The helper refuses to run unless both bus addresses reference the temporary
private bus created by this harness.

## Extended desktop and reconnect cases

With `--mock-helper`, the desktop scenario also exercises confirmed alias
changes and clearing, rejected alias updates, battery snapshots and callbacks,
zero/unknown/invalid percentages, multiple battery components, and rejection of
battery callbacks from another bus sender. Battery data uses Floss's native
`Option<BatterySet>` property map, including nested `aa{sv}` components.

The separate `mock-reconnect` case starts with an already bonded device, the
production `bluedevil-floss-policy` background executable, and two real QML
backend processes sharing one private session bus. It rejects the first
connection request, then requires a delayed retry, recovery from a dropped link,
and rediscovery/reconnection after daemon name loss. Exactly four connection
requests must occur, checking that the three backend instances do not issue
competing attempts; automatic device failures must not become user-facing errors.
After an explicit user disconnect, the mock deliberately delivers a late
connection callback. The policy must disconnect that late connection and issue no
new connect request during the ten-second observation window. Forgetting the
device must prevent reconnection for a further six seconds. This checks a bounded inhibition
window, not an exhaustive long-duration persistence or radio test. Each scenario
has its own temporary configuration directory. The reconnect case may take over
a minute because it uses production retry timers.

The `mock-headless` case closes its QML backend before the initial connection
attempt. The production policy executable alone must complete the retry, dropped
link recovery, and daemon restart recovery. This explicitly checks that automatic
reconnect does not depend on leaving the Bluetooth UI open. Use repeatable
`--case NAME` to run individual scenarios while diagnosing a failure; the default
runs all available cases.

## Standalone pairing and radio controls

The mock suite also runs the shipped `bluedevil-floss-pairing` executable with a
second process hosting the actual `PairingWindow` QML component. In
`mock-pairing-owner`, the UI acquires the pairing name first; the background agent
must not steal it, and two immediate acceptance attempts must produce exactly one
Floss confirmation. In `mock-pairing-handoff`, the agent owns the name first and
receives an authenticated SSP callback without automatically answering. After the
agent exits, the observer may show the cached request only as uncertain, with
acceptance disabled and cancellation available. The mock requires zero acceptance
calls and exactly one cancellation. This deliberately does not promise safe
resubmission after an owner disappears: Floss has no pairing transaction ID.

The harness checks the real QML window is hidden while idle, visible for its
owned request, and hidden again when the request finishes. The actual agent is
launched offscreen and its name ownership and continued operation are observed;
its pixels and its internal window visibility are not inspected. SIGTERM ends the
agent and its private-bus ownership; no host session is involved.

Five `mock-rfkill-*` cases exercise power-off followed by power-on with a hard
block, a removable software block, a failed unblock operation, an accepted unblock that never changes state, and unavailable
rfkill metadata. They check actual manager method counts: no Start after a hard
block, failed unblock, or unchanged state after bounded retries; one targeted unblock for software-blocked cases, and
normal Stop/Start when unblocked or metadata is absent. These tests simulate
manager replies; kernel rfkill permissions and physical switches require separate
validation.

The expanded suite passed all 13 cases against
`/nix/store/2y655l0mjv6aipmms1m174k1lxzqva1z-bluedevil-floss-6.7.90-c15cdd83448c`.
The seven new cases were repeated after two fixture corrections: waiting for
queued authentication probes before destroying a callback receiver, and using
`Qt.exit(0)` so the generic QML test runner exits despite the production window's
intentional close rejection. No product fix was needed for these failures.
`runtime-results/kde/summary.json` records the combined-run provenance; the
original six cases passed against the same binary in the preceding full run.
