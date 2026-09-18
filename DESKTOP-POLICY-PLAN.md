# Per-device Trust and Block implementation plan

This document proposes daemon-enforced desktop policy. **Per-device Trust and Block described here are not implemented.** Existing reconnect inhibition is a connection preference, not a security boundary. UI flags alone must not be presented as protection against a remote device.

## Existing mechanisms and their limits

The current Floss administrative policy is a global service UUID allowlist:

- [bluetooth_admin.rs](Bluetooth/system/gd/rust/linux/stack/src/bluetooth_admin.rs): `BluetoothAdminPolicyHelper::is_service_allowed` permits everything when the allowlist is empty. `PolicyEffect` describes how that global list affects a device; it does not define a device-specific policy. Configuration currently contains `allowed_services`.
- [bluetooth.rs](Bluetooth/system/gd/rust/linux/stack/src/bluetooth.rs): `handle_admin_policy_changed` activates/deactivates HID and HOGP globally.
- [bluetooth_media.rs](Bluetooth/system/gd/rust/linux/stack/src/bluetooth_media.rs): `handle_admin_policy_changed` enables/disables audio profiles globally.
- [socket_manager.rs](Bluetooth/system/gd/rust/linux/stack/src/socket_manager.rs): `handle_admin_policy_changed` closes disallowed UUID-based RFCOMM listeners and explicitly excludes raw L2CAP sockets. It is not a per-peer admission boundary.
- [bluetooth_gatt.rs](Bluetooth/system/gd/rust/linux/stack/src/bluetooth_gatt.rs): GATT client/server connection paths require separate enforcement; the existing service allowlist does not cover them as a per-device policy.
- [service main.rs](Bluetooth/system/gd/rust/linux/service/src/main.rs): existing administrative settings live at `/var/lib/bluetooth/admin_policy.json`. This is not a persistent identity-indexed Trust/Block database.

Relevant native entry points already exist, but need integration:

- [classic_impl.h](Bluetooth/system/gd/hci/acl_manager/classic_impl.h): `on_incoming_connection` consults `should_accept_connection_`, currently initialized to always accept. Outgoing creation must also be gated; changing only incoming acceptance is insufficient.
- [le_impl.h](Bluetooth/system/gd/hci/acl_manager/le_impl.h): `create_le_connection`, accept-list management, pending/retry paths and `on_le_connection_complete` cover distinct admission paths. Queue registration and callbacks currently expose successful connections to the upper stack.
- [acl_manager.cc](Bluetooth/system/gd/hci/acl_manager.cc): public Classic/LE creation methods dispatch onto the ACL handlers; policy publication must respect that threading model.
- [btm_ble_addr.cc](Bluetooth/system/stack/btm/btm_ble_addr.cc): existing identity/pseudo-address/RPA resolution should be reused, with its threading requirements established before calling it from new code.
- [topshim btif.rs](Bluetooth/system/gd/rust/topshim/src/btif.rs): exposes `AddressConsolidate` and `LeAddressAssociate`; the Linux Rust stack currently has no implementation handling those identity association callbacks. Policy migration must not assume that a displayed address is a permanent identity.

## Required semantics

**Blocked** means the daemon denies new connections and pairing to the identified peer, cancels pending attempts, and stops delivery on existing connections. The rule applies to both directions, Classic and LE, profiles, GATT and raw sockets. Blocking must not merely hide the device, suppress desktop reconnects, or remove its bond.

An LE peripheral connection may already exist at the controller before the host receives its completion event. The defensible guarantee is no upper-layer application traffic from a blocked identity, followed by immediate disconnect. Do not promise that a radio-level link can never briefly exist.

**Trusted** means a bonded identity can use explicitly supported services without an additional desktop authorization prompt. It must not automatically accept new pairing or weaken encryption/authentication requirements. An untrusted peer needs bounded service authorization, with denial when no authorized agent is available or the request times out. Untrusting must not implicitly unblock a peer.

The native block decision takes precedence over trust and one-shot authorization. Trust should not be exposed until every service advertised as covered has an enforced authorization path. Pairing-agent support alone does not supply this coverage.

A bonded identity can be recognized across resolvable address changes using its retained identity material. An unbonded peer that changes an unresolvable private address cannot be reliably linked to its previous address. The UI and API must disclose that boundary rather than claim physical-device tracking. Policy also cannot distinguish an unauthenticated address impersonator from the corresponding address solely from its address string.

## Phase 1: identity, persistence and API contract

1. Define a canonical adapter-scoped identity key with address type, and explicit mappings for Classic addresses, LE identity addresses, pseudo-addresses and RPAs. Reuse upstream identity resolution rather than duplicating cryptography.
2. Define bond removal, identity consolidation and re-pairing behavior. Removing a bond must not silently erase a block. Decide how retained blocked identities and any necessary IRK association survive bond removal; without that material, the guarantee for future RPAs changes and must be represented accurately.
3. Store versioned policy in daemon-owned storage with restrictive permissions, bounded parsing and entry count, atomic replacement, and durable acknowledgement. A missing initial database can use the documented default; an unreadable or corrupt existing database must not silently discard blocks. Fail adapter startup closed or expose a recovery state.
4. Specify `GetDevicePolicy`, `SetDevicePolicy` and change notifications. Return applied status only after persistence and native enforcement succeed. A queued request is not proof that blocking is active. Define rollback/error behavior if either stage fails.
5. Restrict mutation to the intended authorized system-bus clients. Define agent ownership, disconnect cleanup and which callers may approve service access. Avoid relying on a desktop process's private preferences as the daemon's policy source.
6. Decide default trust and migration explicitly. Existing bonds must not silently acquire stronger privileges because the new UI appeared.

Review this contract before adding UI controls.

## Phase 2: native Block enforcement

1. Publish immutable policy snapshots onto the relevant native handlers. Establish ordering between policy changes, connection creation, completion, identity resolution and ACL data delivery; avoid synchronous Rust/native calls that deadlock handlers.
2. Gate Classic incoming acceptance before `AcceptConnectionRequest`, and outgoing creation before issuing controller commands. Reject blocked pairing/security requests as well.
3. Gate LE direct and background creation and accept-list additions. Remove blocked entries from connection scheduling, cancel pending attempts and prevent retry paths from restoring them. Do not indiscriminately erase resolving-list identity information needed to recognize a blocked peer.
4. Check resolved LE identities before exposing queues or connection-success callbacks. Disconnect blocked incoming links and dispose of pending state without delivering application traffic. Cover peripheral connections awaiting advertising termination as well as central-role connections.
5. On a policy update, quarantine existing ACL delivery, cancel pending operations and disconnect matching links. Acknowledge effective block only after the defined enforcement barrier; account for packets queued before the update.
6. Reconcile late connection-complete, authentication, pairing and disconnect callbacks without resurrecting state or retrying a blocked peer. Identity consolidation must merge restrictive policy rather than accidentally clearing it.

Rust checks remain useful for immediate API errors, but native enforcement must cover internal profile reconnects and peers that initiate connections without any Rust API request.

## Phase 3: Rust admission and desktop behavior

Add consistent checks to bond creation, profile connection, media operations, GATT client/server connects and socket connection/acceptance paths. Propagate typed policy-denied results rather than generic transport failures. Cancel pending desktop reconnection when policy changes, but preserve the distinction between a reconnect preference and a block.

Expose policy to the existing BlueDevil device model only after native enforcement and persistence pass review. Refresh authoritative daemon state after mutations; do not optimistically present a denied or incomplete change as applied. Show actionable errors for persistence failures and unavailable policy support.

## Phase 4: service authorization and Trust

Inventory authorization entry points for each supported profile, RFCOMM/L2CAP and GATT. Define whether authorization applies to the whole device, a service, or an operation; avoid ambiguously reusing global UUID policy.

Add asynchronous requests with explicit agent ownership, identity, requested service, timeout and cancellation. Requests must hold admission before service data reaches an application. Bonding, authentication and authorization remain separate decisions. No agent, agent loss, timeout, a concurrent block or identity mismatch must deny access.

Only mark Trust complete after this inventory has no uncovered enabled service. If implementation is phased by service, capability-report its supported scope and avoid a universal Trust toggle until that scope is complete.

## Verification and review gates

Use isolated fixtures and mocked controller events first; never access host controllers or activate host services during these tests.

- **Identity:** public/random address collisions, changed RPAs for a bonded identity, identity association/consolidation, dual-mode peers, adapter separation, bond removal and re-pairing. A restrictive merged policy must survive aliases and daemon restart.
- **Admission matrix:** incoming/outgoing Classic and LE, direct/background attempts, existing and newly created profile connections, GATT client/server and raw RFCOMM/L2CAP. Include paths initiated by the native stack without a desktop client.
- **Races:** block during controller creation, completion, authentication, pending advertising termination, pairing prompt and active data delivery; late success after cancellation; unblock with stale retries; agent loss during approval. Assert no post-barrier application traffic and no blocked reconnect resurrection.
- **Persistence:** short writes, permission denial, interrupted replacement, truncated/oversized data, unsupported versions, restart before/after acknowledgement and retained identity policy after bond removal. Verify reported state matches the defined durable guarantee.
- **Authorization:** trusted/untrusted/blocked precedence, explicit approval/denial, timeout, wrong agent/caller, service mismatch and stale responses. Pairing must never be silently approved by Trust.
- **Resource bounds:** repeated denied requests, callback floods, unresolved peers and identity mappings must not grow memory or queues without bound.
- **Regression:** ordinary paired audio/HID/GATT operation, explicit manual connection and automatic reconnect remain functional for allowed peers. Existing global service restrictions still apply.

Independent review should check native/Rust/API/UI agreement, policy publication ordering, identity handling and every enabled service entry point. Physical controller tests must subsequently cover privacy/address resolution, real reconnect behavior and long-running blocked-device attempts. Passing mocks alone does not establish those hardware behaviors.
