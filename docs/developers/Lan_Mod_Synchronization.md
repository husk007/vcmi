# LAN Mod Synchronization Design

## Purpose

Allow guests to join a LAN game with the host's gameplay mod set without manually installing or toggling each mod. On leaving the multiplayer session, the guest's previous active preset must be restored exactly. Downloaded host packages remain in a verified local cache for future sessions.

## Existing integration points

- `server/NetPacksLobbyServer.cpp`: `LobbyQueryState` produces `LobbyModsCheck`, including the host version and `ModCompatibilityInfo`.
- `client/CServerHandler.cpp`: `onLobbyPreviewResponse` opens the room preview before joining.
- `client/globalLobby/GlobalLobbyRoomWindow.cpp`: the preview compares the host's mod list against local mods, shows the action for each mod and offers Join or Prepare host mods.
- `lib/modding/ModVerificationInfo.cpp`: `verifyListAgainstLocalMods` already classifies missing, disabled, excessive, version-mismatched, and matching entries.
- `lib/modding/ModManager.cpp`: preset export/import and activation, with the per-user `modSettings.json` configuration.
- `launcher/modManager/modstatecontroller.cpp`: archive installation and existing mod validation logic.

## Implementation status

The opt-in path is implemented from discovery to rejoin and covered by the `LanModSyncTest.*` suite in `test/modding/`. The evidence gates are in `ROADMAP.md`; the feature is not release-ready.

- Linux: `utils/lan_gui_acceptance.py` passes with two graphical clients and licensed Heroes III data on one machine: rejoin after the relaunch, game start, host disconnect, preset restore.
- Android, two phones on one Wi-Fi network: the host packed its mods, the guest downloaded 596 MB, verified, cached and unpacked them, relaunched and rejoined, and the game started with the same mods on both phones. After the start, the guest stopped receiving packets and its packets stopped reaching the host, without a disconnection on either side; the cause is open (see Diagnostics).
- iOS and macOS on Intel do not build, because the LAN code uses `std::filesystem` and those deployment targets predate it. The rest of VCMI uses `boost::filesystem` for this reason.

| Step | Code |
| --- | --- |
| Capability negotiation | UDP probe `VCMI_DISCOVERY;lanModSync=preview` in `lib/network/NetworkDiscoveryProtocol.h`; legacy hosts and clients keep the unchanged probe and reply |
| Preview with manifest | `LobbyQueryModSync` and `LobbyModSyncPreview` in `lib/networkPacks/PacksForLobby.h` |
| Archive transfer | `LobbyQueryLanModArchiveChunk` and `LobbyLanModArchiveChunk`, served by `LanHostArchiveTransfer` to registered guests only; requested by `CServerHandler::beginLanArchiveDownload` |
| Verified cache and staging | `ModArchiveDigest`, `LanModArchiveCache`, `LanModPackageStage` |
| Temporary preset and recovery | `ModsPresetState`, `AtomicModSettings`, `LanModRecoveryJournal` |
| Relaunch and rejoin | `--lan-relaunch-token`, `LanRelaunchProcess`, `LanRejoinTarget`; on Android `NativeMethods.relaunchGame` and `ActivityLanRelaunch` |
| Host publishing | `LanHostModPublisher` packs active mods into `userData/lan-mod-shares/` when the server starts listening; `utils/lan_mod_share.py` publishes by hand |

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

## Session lifecycle

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

An archive is never reused because its name or declared version matches: its SHA-256 is computed again before each use.

On a second match with unchanged host content, the expected transfer size is zero. On a changed mod release, transfer only affected packages, not the entire mod library. The user can later explicitly clear cached packages; leaving multiplayer does not delete them.

`ModSyncPlan::buildSyncPlan` consumes two maps: expected archive SHA-256 values from the host and *already verified* cached SHA-256 values.

A cache entry is stored as `userData/lan-mod-archives/<SHA-256 of root mod ID>/<archive SHA-256>.zip` and is rehashed before each use. `LanModPackageStage` extracts a verified archive into `userData/lan-mod-staged/`. A staged root is mounted only on a controlled relaunch, and only when that root mod is not installed locally. A partial download is written to `userCachePath()/lan-mod-incoming/` and removed after verification or failure.

The host hashes each shared archive once per change of size or modification time. Every lobby preview lists all shared archives, so rehashing them for each request would stall the server network thread.

### Package limits

`LanModPackageStage` and `LanHostModPublisher` share the limits: 2 GiB unpacked, 256 MiB per file and 65535 entries, the maximum of a ZIP without the ZIP64 extension. `LanModArchiveCache::maxArchiveBytes` limits one archive to 2 GiB. `utils/lan_mod_share.py` uses the same values.

### Storage without hard links

The recovery journal and cache entries are published with `AtomicModSettings::moveWithoutReplacing`, which must never replace an existing file. It uses a hard link, which fails atomically for an existing destination. Android app storage refuses hard links, so it falls back to `renameat2` with `RENAME_NOREPLACE` on Linux and Android, and only then to an existence check followed by rename.

## Host publishing

A server that listens for LAN connections starts `LanHostModPublisher` on a background thread. For every active root mod installed in `userData/Mods` it writes `lan-mod-shares/<root>.zip` and `<root>.source`, the list of files with sizes and modification times. The archive is rebuilt only when this list changes, so a returning guest finds the same SHA-256 in its cache. Mods shipped with the game, such as `vcmi`, are not packed.

- A mod with a link, a special file, original game data (`.lod`, `.snd`, `.vid`, `.exe`, `.iso`, `.cab`) or a size over the `LanModPackageStage` limits is not shared, and an earlier automatic archive of it is removed.
- An archive without `.source` was published by hand and is never replaced.
- `settings.server.lanModSharing` disables the publisher. Until an archive is ready, guests see the mod as `manual setup required`.

## Guest download

When the preview offers every missing root as a host archive, the window shows Prepare host mods below the mod list. The player confirms a consent dialog that lists the roots, enabled submods, versions and digests; an approved offer is stored under `lan-mod-archives/approved-offers/` and is not asked again for identical content.

`CServerHandler::beginLanArchiveDownload` opens a separate connection that registers with no player names, so it does not take a player slot. It requests 64 KiB chunks one after another and shows progress per archive. When an archive is complete, it releases the interface mutex while `publishVerifiedArchive` hashes and copies it and `stageVerifiedArchive` unpacks it, and the window shows "Verifying and unpacking host mods...". This takes minutes for a large mod on a phone. If the player cancels meanwhile, the result is discarded. Every reason why an archive is not cached or not staged is logged.

After all archives are cached, Join prepares the temporary preset and relaunches; the relaunched client mounts the staged roots and rejoins.

## Preset isolation and exact rollback

`LanModSessionState::begin` clones the complete current preset into a reserved `__vcmi_lan_session` preset and stores `lanSession.originalPresetName` and `lanSession.originalPreset`. This retains disabled submods as well as enabled mods, and preserves the user's other presets. `restore` reinstates the original preset and its settings and removes the temporary preset.

The helper is available through `ModsPresetState` / `ModManager`. Joining with a changed mod set uses a **one-shot controlled relaunch marker**:

- `CModHandler::requestLanRelaunch` flags the relaunch while the temporary preset is active. On shutdown, after the game released its resources, `EntryPoint.cpp` calls `CModHandler::prepareControlledLanRelaunch`. It generates the token with `LanRelaunchToken::generate` and writes the marker atomically through `ModManager::prepareControlledRelaunch`. Only then `LanRelaunchProcess::spawnWithRollback` starts the replacement process with `--lan-relaunch-token`. If either step fails, the original preset is restored.
- The token is forwarded through `GameLibrary::initializeFilesystem`, `CModHandler`, and the `ModManager` constructor. An exact match consumes the marker on disk while keeping the temporary preset and the original recovery snapshot, and the client opens the lobby of the stored `LanRejoinTarget`.
- A missing or mismatched token restores the original preset. If the client crashes after consuming a valid token, the next startup also restores automatically. A corrupt session snapshot raises an error instead of discarding recovery data.
- Under AppImage, the relaunch starts the original image instead of the temporary mount.
- Leaving the session restores the original preset. On desktop, `requestLanRestoreRelaunch` then starts a client without a token so the personal mods load again.
- On Android, `LanRelaunchProcess` calls `NativeMethods.relaunchGame`. It starts `ActivityLanRelaunch` in a separate process, which ends the game process and opens `VcmiSDLActivity` with the token as program argument. iOS has no relaunch path (`VCMI_LAN_RELAUNCH_SUPPORTED` is not defined), so it does not offer host-mod preparation and asks the player to restart manually.
- The default constructor arguments supply no token, so clients without the feature keep interrupted-session restoration.

The Android relaunch worked on two phones. A privately installed root is never overwritten: the staging tree is separate from `userData/Mods`, and the mount is refused for an installed root.

Open items:

- Play a complete match on two devices, including host disconnect and crash recovery.
- Find why the guest connection stopped carrying data after the game start on Android.
- Verify Windows.
- Port the LAN code to `boost::filesystem` for iOS and macOS on Intel.

## Diagnostics

- The client keeps the log of the previous run as `VCMI_Client_log_previous.txt`; the launcher log export includes it, so a crash is not lost when the game is started again.
- Each received heartbeat, sent every 10 seconds by both sides, is traced as "Received heartbeat". A connection that stops carrying data shows as a gap.
- The host logs "LAN mod sharing: packed mod ..." for every archive it builds; the guest logs the download, verification and unpacking of each archive.

## Security and legal constraints

- Synchronize only redistributable mod archives; never send base-game commercial assets. Each guest supplies their own Heroes III game data.
- A host is not a trusted package publisher. Show one-time approval of host-supplied mods (including scripts); allow remembering approval for an identified host.
- Authenticate the transfer endpoint with a session-scoped capability token and bind it to the expected LAN host. Avoid accepting arbitrary URLs from a peer.
- Enforce download size and expansion limits, reject `../`, absolute paths, links, and decompression bombs.
- Validate canonical IDs, versions, dependencies, checksums, and archive contents before enabling anything.
- Do not permit silent game-state changes before every client confirms that its local mod set matches the manifest.
- Keep the feature optional; unmodified clients continue using the existing compatibility preview.

## Tests

- `LanModSyncTest.*` in `test/modding/` runs headless; the tests that need a running host read its port from `VCMI_LAN_TEST_SERVER_PORT`. `.github/workflows/lan-mod-sync-validation.yml` starts that host and runs them.
- `utils/lan_gui_acceptance.py --bin-dir <bin> --assets-dir <dir with Data/>` runs the graphical branch with the owner's Heroes III data. It needs `xvfb-run` and `xdotool`. The host in these tests publishes archives by hand and sets `lanModSharing` to false, because the tests compare archive digests between previews.

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
