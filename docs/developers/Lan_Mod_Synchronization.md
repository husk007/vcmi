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

## Implementation status

The opt-in path is implemented from discovery to rejoin and covered by the headless `LanModSyncTest.*` suite in `test/modding/`. No LAN match with graphical clients and licensed Heroes III data has been run, so the feature is not release-ready. The evidence gates are in `ROADMAP.md`.

| Step | Code |
| --- | --- |
| Capability negotiation | UDP probe `VCMI_DISCOVERY;lanModSync=preview` in `lib/network/NetworkDiscoveryProtocol.h`; legacy hosts and clients keep the unchanged probe and reply |
| Preview with manifest | `LobbyQueryModSync` and `LobbyModSyncPreview` in `lib/networkPacks/PacksForLobby.h` |
| Archive transfer | `LobbyQueryLanModArchiveChunk` and `LobbyLanModArchiveChunk`, served by `LanHostArchiveTransfer` to registered guests only |
| Verified cache and staging | `ModArchiveDigest`, `LanModArchiveCache`, `LanModPackageStage` |
| Temporary preset and recovery | `ModsPresetState`, `AtomicModSettings`, `LanModRecoveryJournal` |
| Relaunch and rejoin | `--lan-relaunch-token`, `LanRelaunchProcess`, `LanRejoinTarget` |
| Host publishing | `utils/lan_mod_share.py` writes archives to `userData/lan-mod-shares/` |

## Preview packet fields

The LAN preview reuses the legacy query and reply. The opt-in packets derive from them and serialize the legacy fields first.

`LobbyQueryState` has no fields. Any connection may send it; the server answers on the same connection and does not register a player.

`LobbyModsCheck`, the legacy reply, filled in `ApplyOnServerNetPackVisitor::visitLobbyQueryState`:

- `vcmiVersion` - `VCMI_VERSION_STRING` of the host; shown as the room game version.
- `mods` - every active host mod as `ModVerificationInfo`: `name`, `version`, CRC-32 `checksum`, `parent` (empty for a root mod) and `impactsGameplay`. The guest compares it with `ModVerificationInfo::verifyListAgainstLocalMods`.
- `hostAccountDisplayName` - name of the player on the host connection, empty before the host registers.
- `participantNames` - names of all registered players.

`LobbyQueryModSync` has no fields of its own. The client sends it instead of `LobbyQueryState` only when the UDP reply carried `;lanModSync=preview`.

`LobbyModSyncPreview` adds:

- `archives` - `ModArchiveManifest` of active root mods with a ZIP in `userData/lan-mod-shares/`: SHA-256 and size in bytes. A mod without an entry can only be enabled from a local installation.
- `acceptingPlayers` - false outside the lobby state or when every human-playable slot is taken; the preview shows the room as busy.
- `humanPlayerSlots` - number of slots of the selected map that are not `compOnly`; 0 before a map is selected.

`CServerHandler::onLobbyPreviewResponse` maps the reply to a `GlobalLobbyRoom`, so `GlobalLobbyRoomWindow` shows LAN and global lobby rooms with the same code. The legacy reply leaves `playerLimit` at 0 and the room state at `open`, as before this feature.

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

`ModSyncPlan::buildSyncPlan` consumes two maps: expected archive SHA-256 values from the host and *already verified* cached SHA-256 values.

A cache entry is stored as `userData/lan-mod-archives/<SHA-256 of root mod ID>/<archive SHA-256>.zip` and is rehashed before each use. `LanModPackageStage` extracts a verified archive into `userData/lan-mod-staged/`. A staged root is mounted only on a controlled relaunch, and only when that root mod is not installed locally. A partial download is written to `userCachePath()/lan-mod-incoming/` and removed after verification or failure.

The host hashes each shared archive once per change of size or modification time. Every lobby preview lists all shared archives, so rehashing them for each request would stall the server network thread.

## Preset isolation and exact rollback

`LanModSessionState::begin` clones the complete current preset into a reserved `__vcmi_lan_session` preset and stores `lanSession.originalPresetName` and `lanSession.originalPreset`. This retains disabled submods as well as enabled mods, and preserves the user's other presets. `restore` reinstates the original preset and its settings and removes the temporary preset.

The helper is available through `ModsPresetState` / `ModManager`. Joining with a changed mod set uses a **one-shot controlled relaunch marker**:

- `CModHandler::requestLanRelaunch` flags the relaunch while the temporary preset is active. On shutdown, after the game released its resources, `EntryPoint.cpp` calls `CModHandler::prepareControlledLanRelaunch`. It generates the token with `LanRelaunchToken::generate` and writes the marker atomically through `ModManager::prepareControlledRelaunch`. Only then `LanRelaunchProcess::spawnWithRollback` starts the replacement process with `--lan-relaunch-token`. If either step fails, the original preset is restored.
- The token is forwarded through `GameLibrary::initializeFilesystem`, `CModHandler`, and the `ModManager` constructor. An exact match consumes the marker on disk while keeping the temporary preset and the original recovery snapshot, and the client opens the lobby of the stored `LanRejoinTarget`.
- A missing or mismatched token restores the original preset. If the client crashes after consuming a valid token, the next startup also restores automatically. A corrupt session snapshot raises an error instead of discarding recovery data.
- Under AppImage, the relaunch starts the original image instead of the temporary mount.
- Leaving the session restores the original preset. On desktop, `requestLanRestoreRelaunch` then starts a client without a token so the personal mods load again.
- `LanRelaunchProcess` is compiled out under `VCMI_MOBILE`. Mobile builds do not offer host-mod preparation in the preview, and a restored preset asks the player to restart manually.
- The default constructor arguments supply no token, so clients without the feature keep interrupted-session restoration.

Open items:

- Run a real two-client match with licensed game data, including host disconnect and crash recovery.
- Verify Windows. Android has no relaunch path.
- Keep the original personal version side by side when personal and host mod versions differ; never overwrite it. The staging tree is separate from `userData/Mods`, and the mount is refused for an installed root.

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
