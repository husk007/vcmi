# LAN Mod Synchronization Design

## Purpose

Allow guests to join a LAN game with the host's gameplay mod set without manually installing or toggling each mod. On leaving the multiplayer session, the guest's previous active preset must be restored exactly. Downloaded host packages remain in a verified local cache for future sessions.

## Existing integration points

- `server/NetPacksLobbyServer.cpp`: `LobbyQueryState` produces `LobbyModsCheck`, including the host version and `ModCompatibilityInfo`.
- `client/CServerHandler.cpp`: `onLobbyPreviewResponse` opens the room preview before joining.
- `client/globalLobby/GlobalLobbyRoomWindow.cpp`: the preview compares the host's mod list against local mods and currently blocks joining on major incompatibilities.
- `lib/modding/ModVerificationInfo.cpp`: `verifyListAgainstLocalMods` already classifies missing, disabled, excessive, version-mismatched, and matching entries.
- `lib/modding/ModManager.cpp`: preset export/import and activation, with the per-user `modSettings.json` configuration.
- `launcher/modManager/modstatecontroller.cpp`: archive installation and existing mod validation logic.

The first code milestone adds a reusable `ModSyncPlan` and `LanModSessionState`. They are not yet connected to the lobby or file-transfer implementation.

## Proposed session lifecycle

1. Host advertises support for synchronization and supplies a manifest for active gameplay mods.
2. Guest compares the manifest to installed mods and a **verified** persistent cache of complete archives.
3. Guest previews any unfamiliar host content, required changes, and estimated download size. Approving a trusted host can be remembered.
4. Guest persists a recovery journal, including original active preset name and full preset JSON, before applying changes.
5. Cache hit: select the local package. Cache miss: transfer from the host and verify bytes before making them available.
6. Load host-required mods into an isolated temporary mod preset, with required submod selections; deactivate extra gameplay mods in this preset only.
7. Reload the game resources or restart the client in a controlled way; rejoin the original LAN room. Host starts once guests acknowledge compatibility.
8. On disconnection or after gameplay, restore the exact prior preset and record the session as completed. Keep cached packages inactive and available.
9. An interrupted session is recovered on the next launch, unless the journal explicitly indicates a controlled restart to continue that session.

```text
Host          Guest                                            Persistent storage
 |              |                                                       |
 |--manifest--->|                                                       |
 |              |--compare installed + verified local cache----------->|
 |              |<----------------------cache hits----------------------|
 |              |--snapshot preset / atomic journal------------------->|
 |<--request----|   (only missing or changed archives)                  |
 |--chunks----->|--verify hash / stage package------------------------->|
 |              |--temporary preset / controlled reload----------------|
 |<---ready-----|                                                       |
 |<== gameplay ========================================================>|
 |              |--restore original preset / retain cached packages--->|
```

## Persistent cache and repeat visits

Store downloaded archives in a content-addressed directory under `VCMIDirs::userDataPath()` (not `VCMIDirs::userCachePath()`, which the OS may evict).

Cache identity is `(canonical root mod ID, archive SHA-256)`. The manifest also includes version, dependencies, size, and the original host source. An installed mod of the exact expected version and verified content requires no download and may need only temporary activation.

Do not reuse an archive merely because its name or declared version matches. Verify the archive's SHA-256 and preserve a stable index with a per-entry verification timestamp. Verify again before use if the archive could have been modified on disk.

On a second match with unchanged host content, the expected transfer size is zero. On a changed mod release, transfer only affected packages, not the entire mod library. The user can later explicitly clear cached packages; leaving multiplayer does not delete them.

The current `ModSyncPlan::buildSyncPlan` consumes two maps: expected archive SHA-256 values from the host and *already verified* cached SHA-256 values. The integrity verifier, persistent storage, and network transfer are separate upcoming implementations.

## Preset isolation and exact rollback

`LanModSessionState::begin` clones the complete current preset into a reserved `__vcmi_lan_session` preset and stores `lanSession.originalPresetName` and `lanSession.originalPreset`. This retains disabled submods as well as enabled mods, and preserves the user's other presets. `restore` reinstates the original preset and its settings and removes the temporary preset.

The helper is currently available through `ModsPresetState` / `ModManager`. The implementation now supports a **one-shot controlled relaunch marker**:

- When a LAN session is active, the future launcher will generate an unpredictable local token, call `ModManager::prepareControlledRelaunch(token)` and only restart after the settings file has been written atomically.
- On the restarted process, the same token must be supplied to the `ModManager` constructor (last argument). An exact match consumes the marker on disk while keeping the temporary preset and original recovery snapshot.
- A missing or mismatched token restores the original preset. If the client crashes after consuming a valid token, the next startup also restores automatically. Corrupt session snapshots raise an error rather than discarding recovery data.
- The default constructor arguments supply no token, so existing clients retain safe interrupted-session restoration behavior.
- Token transport through `CModHandler` / launcher process arguments and controlled restart itself are **not yet implemented**, so the new API alone does not enable session resume.

Before enabling it in production:

- Make the journal write crash-safe (write temporary file, fsync when supported, atomically replace).
- Handle normal disconnect, host dropout, user cancellation, client crash, and restart recovery separately.
- Wire the existing one-shot marker to an actual controlled process relaunch, including a fresh locally generated token and startup propagation through `CModHandler`.
- If personal and host mod versions differ, keep the original personal version side by side; never overwrite it.
- Restore the original preset **only after** the session has ended, then reload engine resources if necessary.
- Treat an invalid journal as recoverable user data. Preserve a backup and present an actionable error instead of deleting it.

## Security and legal constraints

- Synchronize only redistributable mod archives; never send base-game commercial assets. Each guest supplies their own Heroes III game data.
- A host is not a trusted package publisher. Show one-time approval of host-supplied mods (including scripts); allow remembering approval for an identified host.
- Authenticate the transfer endpoint with a session-scoped capability token and bind it to the expected LAN host. Avoid accepting arbitrary URLs from a peer.
- Enforce download size and expansion limits, reject `../`, absolute paths, links, and decompression bombs.
- Validate canonical IDs, versions, dependencies, checksums, and archive contents before enabling anything.
- Do not permit silent game-state changes before every client confirms that its local mod set matches the manifest.
- Keep the feature optional; unmodified clients continue using the existing compatibility preview.

## Test cases

| Scenario | Expected behavior |
| --- | --- |
| First join, missing mod | Host sends only the missing redistributable package; guest hashes and caches it |
| Second join, unchanged mod | No transfer; guest activates cached or already installed content |
| Same mod name, new archive hash | Guest downloads changed archive; older personal version stays untouched |
| Extra active guest mod | Disable temporarily; return to its prior setting after the session |
| Disabled guest submod | Enable only when required by the host; restore its original boolean afterward |
| Transfer interrupted | Part remains untrusted; retry or resume, never enable incomplete archive |
| Guest closed without disconnect | Restore from journal or resume an explicitly marked controlled restart |
| Host departs unexpectedly | Restore guest preset; retain verified cached archives |
| No mod-sync support on one side | Preserve the existing compatibility warning and normal connection behavior |
| Windows, Linux, Android | Same logical manifest and restore behavior with platform-specific data paths |
