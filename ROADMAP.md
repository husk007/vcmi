# Fork Roadmap: LAN Mod Sync and Browser Client

This roadmap describes opt-in features developed on `husk007/vcmi`. It does not change upstream VCMI behavior unless the feature is explicitly enabled. Work in small, reviewable changes and verify each milestone before marking it complete.

## Execution strategy — deliver a usable LAN vertical slice

The hourly task follows [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md), and [RUNNER_STATUS.md](RUNNER_STATUS.md). Work only on `feature/lan-mod-sync-and-web-roadmap`. The original L0–L4/W0–W4 sections below define **feature scope**, not a rule to keep patching L0 indefinitely. Follow the concrete milestones M0–M5 below in order; leave any unverified test gate open but continue implementing independently testable functionality.

**Audit 2026-10-29:** 182 commits existed on the feature branch relative to `develop`, with zero GitHub Actions runs and no verified full VCMI compile, `vcmitest`, two-client LAN exchange, or automatic mod transfer. Many changes improve cache/backup integrity, but the host manifest and network integration are still missing. Source-only checks and isolated reproductions are **not** proof that VCMI builds.

**Current focus:** get the real VCMI compile/test result, then implement the **host manifest + capability negotiation and client preview**. Do not add another cache-hardening edge case unless the next vertical slice demonstrably fails without it.

### M0 — Real build and regression evidence (highest-priority parallel gate)

- [x] Add a narrowly triggered GitHub Actions workflow for a real Linux VCMI build and committed LAN tests: `.github/workflows/lan-mod-sync-validation.yml`.
- [x] Obtain a real `vcmitest` build and execute `LanModSyncTest.*`: [CI run #40796885539](https://github.com/husk007/vcmi/actions/runs/40796885539) passed **71/71 tests** on commit `a1e2bf2e70eb9ac2636091c8baf6d1926087a374` after correcting the runtime data setup. The wider client build is a separate gate. Latest completed run: [CI #41568986427](https://github.com/husk007/vcmi/actions/runs/41568986427) passed **104/104** on `7d67416` with the full Linux client/server/`vcmitest` build.
- [ ] If GitHub Actions is disabled or quota-blocked on this fork, record the blocker once and ask for the **specific** enablement action; do not replay the same failed DNS probe every hour. Continue M1–M3 with source-level validation and explicitly keep M0 open.
- [ ] Before a feature release, run the broader `ctest --preset linux-gcc-test` and verify at least one actual game startup. Broader ctest: 1361/1361 passed on `30eb687` (local Ubuntu 24.04, GCC 13, 2026-11-04); game startup with licensed data still open.

GitHub Actions is intentionally **not** triggered by every hourly code commit. It runs when its workflow file or `.github/lan-mod-sync-validation-trigger.txt` changes, or when launched via `workflow_dispatch`. Change the trigger file only after a meaningful code milestone or a fix for a proven CI failure; do not generate empty reruns. GitHub Actions availability is unverified until a run appears.

### M1 — Host compatibility preview and opt-in negotiation (first functional slice)

- [x] Review `LobbyQueryState` → `LobbyModsCheck` → `CServerHandler::onLobbyPreviewResponse` and document exact fields that already work. Reuse them; do not create a parallel protocol. Documented under "Preview packet fields" in `docs/developers/Lan_Mod_Synchronization.md`; the opt-in packets derive from the legacy ones.
- [x] Advertise `lanModSync` as an optional capability using a backward-compatible/version-gated protocol path. A client lacking the capability must retain the existing room preview without disconnecting. Headless TEST_PASS: CI #41574744788 (legacy probe and join unchanged).
- [x] Display a read-only host/guest comparison in the LAN join preview: mod ID, version, required/disabled/excessive, and action plan. Do not yet change disk files. BUILD_PASS: CI #41574744788; not yet viewed with licensed data.
- [x] Add a focused integration or protocol fixture test proving that the host advertises the correct mod list and the guest produces a matching diff. TEST_PASS: `LanModSyncTest.*`, CI #41574744788.

### M2 — Connect with mods already installed (first executable happy path)

- [x] On joining a host requiring only locally installed mods, snapshot the full original preset durably; activate required mods/deactivate excessive mods in a temporary LAN preset. Headless TEST_PASS against the independent `vcmiserver` (CI #41580945912, local run 2026-11-04).
- [ ] Safely reload game resources or restart/rejoin the same LAN room; perform the connection with the correct active mods, then restore the original preset on clean disconnect, rejected join, and crash recovery.
- [x] Test one host + one guest where the guest initially has the right mod **installed but disabled**. Prove a successful match without downloading an archive, then verify exact prior preset and submod restoration. Headless TEST_PASS on an independent server (see RUNNER_STATUS); graphical session still open.

### M3 — Reuse cached mod packages between sessions

- [x] Bind the existing persistent SHA-256 cache to the actual host manifest and guest join flow. A valid local archive is a cache hit; an already installed matching mod only needs activation. Headless TEST_PASS: CI #41344249608.
- [x] Verify two successive joins to the same host with identical manifest: the **second transfer must be 0 bytes** and cached content still activates correctly. Headless TEST_PASS: CI #41344249608 (transfer is 0 bytes on the second join); GUI reuse untested.
- [x] Verify changed hash/version triggers only affected archive transfer; other cached mods are reused. Headless TEST_PASS: CI #41344249608.

### M4 — Transfer missing, redistributable mods from host

- [x] Add bounded, authenticated, session-scoped host→guest archive transfer and a first-use trust prompt. Reject missing/invalid hashes, malicious archive paths, and nonredistributable content. TEST_PASS: CI #41290727321, trust dialog CI #41323542905.
- [x] Stage versions side by side so the player's privately installed mod version is not overwritten. TEST_PASS: staging tree is separate from `userData/Mods`; a root installed locally is never mounted.
- [ ] Complete an end-to-end join where the guest initially lacks a mod, downloads it from host, plays, restores the original preset, and keeps the downloaded package for another match.

### M5 — Release validation and cross-platform support

- [ ] Demonstrate a two-client Linux LAN match through join → play → disconnect with complete mod restoration and no repeat download.
- [ ] Test Windows and Android compatibility, including old clients without `lanModSync`, reconnect, interrupted transfer, and crash recovery.
- [ ] Only after LAN passes the above gates prioritize the browser W0–W4 milestones.

**Anti-stall rule:** Do not repeatedly implement unrelated edge-case hardening in L0/L2 while M1 is unimplemented. A missing CI environment is a verification blocker, not a prohibition on reviewing or implementing the next small functional slice. After two consecutive runs with the same blocker, skip repeating the probe and work on M1/M2. Every run must identify the milestone and prove a change in functional coverage, a fixed real compiler error, or a new test result; do not treat documentation/status growth as product progress.

### Release gates (must all be satisfied before claiming working LAN mod sync)

- [ ] Repeat-session gate: a previously verified matching host mod requires no network transfer, only temporary activation if inactive.
- [ ] Restoration gate: original preset, root mods, and enabled/disabled submods are restored after clean exit, disconnect, cancelled join, crashed client, and a controlled restart.
- [ ] Integrity gate: SHA-256 verified for staged content; no overwrite of privately installed versions, no proprietary game assets transmitted.
- [ ] Compatibility gate: normal VCMI clients without the new capability still work; all guests confirm compatible gameplay content before start.
- [ ] End-to-end gate: two real clients successfully finish a LAN match and return to their own original mod configurations.


## A. LAN multiplayer: host-managed mod set (first priority)

**Goal:** Joining a game should automatically prepare the host's compatible gameplay mods, use previously downloaded packages without retransmitting them, and restore the guest's exact prior active preset after the match.

### L0 — Compatibility, cache decisions, and reversible preset foundation — PARTIAL

The helpers and their tests compile and pass in the real `vcmitest` build (see M0). The open items below need a real client session. Do not repeat hardening without a failing validation or a dependency on M1–M4.

- [x] Reuse `ModVerificationInfo::verifyListAgainstLocalMods` and existing `LobbyModsCheck` metadata.
- [x] Add a pure `ModSyncPlan` for missing, disabled, excessive, and version-mismatched mods.
- [x] Plan reuse of a previously verified cached archive when its SHA-256 matches the host manifest.
- [x] Add a LAN-only preset snapshot helper retaining the original active preset and submod settings.
- [x] Add targeted unit tests for both behaviors.
- [x] Verify compilation and run `vcmitest` on a compatible Linux build: see M0, CI #41568986427.
- [x] Persist LAN session journal with crash-safe atomic writes and recovery semantics. TEST_PASS: `RealDesktopClientProcessesResumeInstalledModThenRecoverAfterCrash` kills a real `vcmiclient` and recovers the preset (CI #41580945912).
- [ ] Complete guest lifecycle integration. Restore/cancellation hooks already exist in source, but actual host-selected mod activation and automatic LAN rejoin are not wired or validated in a real client.

### L1 — Capability negotiation and manifest

- [x] Negotiate an optional `lanModSync` capability before modifying any guest files; retain interoperability with unmodified VCMI clients. Same evidence as M1.
- [ ] Extend the existing `LobbyQueryState` / `LobbyModsCheck` preview flow with host game version and an authenticated mod manifest: canonical mod ID, root ID, version, enabled submods, archive SHA-256, size, dependencies, and redistributability.
- [ ] Compare manifest against installed mods *and* a verified persistent cache; version string alone is not an integrity check.
- [ ] Reject unsupported game versions and incompatible dependencies, and block game start until everyone confirms matching gameplay content.
- [ ] Do not send original proprietary Heroes III assets through the mod-transfer mechanism.

### L2 — Persistent and secure transfer

- [ ] Keep transferred packages between matches, keyed by content digest and mod identity. Use `VCMIDirs::userDataPath()` rather than a temporary download folder.
- [ ] Revalidate local package hashes before use. Cache hit: skip network transfer; if already installed at the right version: only activate.
- [ ] On miss, stream only missing or changed archives from the actual LAN host, using a session-bound token and bounded, resumable chunks. Authenticate metadata and transfers.
- [ ] Limit archive size and extraction expansion; reject path traversal, absolute paths, symlinks, tampered archives, and unexpected mod IDs.
- [ ] Stage each version side by side. Never overwrite a guest's privately installed version or original installation.
- [ ] Keep cache on disconnect; optional explicit cleanup can be added later. No retransmission on repeated games with the same hash.
- [ ] Review mod licenses and require first-time user trust for content from a host. Reconnecting to a previously trusted host may work unattended.

### L3 — Apply, play, and restore

- [ ] Verify the already implemented on-disk recovery journal in a full VCMI build and real clean/crash recovery; fix only demonstrated failures before marking it done.
- [ ] Verify the implemented `__vcmi_lan_session` temporary preset preserves the full original active preset and submod choices across a real launch/disconnect/restart sequence.
- [ ] Activate all required gameplay mods and disable excessive ones **only in the temporary session**; preserve unrelated personal presets.
- [ ] Reinitialize game resources safely or use a controlled relaunch; game assets currently load during initialization.
- [ ] Automatically rejoin the selected room after the reload, and keep the host from starting until all clients report ready.
- [ ] Restore exactly the previous preset after normal exit, host disconnect, guest disconnect, failure, cancellation, and game closure; keep cached packages installed but inactive unless previously active.
- [ ] Detect an incomplete journal on next start and offer recovery; distinguish intentional relaunch-to-join from a crash. Never silently destroy the only recovery snapshot.
- [ ] Support returning to the same host immediately without redownloading identical content.

### L4 — UX, compatibility, and acceptance tests

- [ ] Show differences and transfer size before accepting a new host's mod content, followed by progress, cancellation, and retry.
- [ ] For previously trusted hosts, automatically reuse already verified packages with no user interaction.
- [ ] Test Windows ↔ Linux Mint (AppImage) and Android LAN; include submods, dependency conflicts, oversized packages, malformed ZIP, interrupted transfer, crash restoration, and multiple consecutive sessions.
- [ ] Test clients without sync capability and ensure the existing join/preview flow still works.
- [ ] Mark this feature complete only after a real two-client LAN match succeeds, including exact preset restoration.

## B. WebAssembly / browser VCMI (second workstream)

**Goal:** Play in modern desktop browsers in a local network without a native installation, while each player supplies legally obtained Heroes III assets.

### W0 — Feasibility spike

- [ ] Inventory CMake dependencies and incompatible targets for Emscripten (Boost networking, LuaJIT, FFmpeg, threading, filesystem, SDL and dynamic libraries).
- [ ] Determine whether SDL2 rendering and a non-threaded event loop can run in a canvas.
- [ ] Produce a minimal compiled WASM executable and automated browser smoke test without distributing proprietary game assets.
- [ ] Choose a fallback where the gameplay server remains native and only the client runs in the browser.

### W1 — Buildable browser runtime

- [ ] Add a separate Emscripten CMake preset/toolchain; preserve desktop builds.
- [ ] Replace or conditionally disable unsupported dependencies (notably LuaJIT where necessary).
- [ ] Implement browser-compatible asset loading into a virtual filesystem and persist user-supplied assets in IndexedDB / OPFS when available.
- [ ] Support mouse/keyboard controls, screen resize, audio unlock gestures, and deterministic update loop.

### W2 — Browser-to-server multiplayer

- [ ] Add a WebSocket protocol adapter/gateway: browsers cannot directly use VCMI's native TCP protocol.
- [ ] Retain the authoritative native game server for early browser builds; test reconnect, save/load, and synchronization.
- [ ] Provide local hostname/IP instructions; design TLS/WSS requirements for remotely hosted or secure-context use.
- [ ] Reuse the LAN mod manifest and content cache when legally redistributable, and require explicit access for guest-supplied original game assets.

### W3 — Browser launch and packaging

- [ ] Create a lightweight web launcher and file-import workflow (local user assets; no commercial assets bundled).
- [ ] Add caching/PWA support with versioned content and a clear storage quota UI.
- [ ] Add multi-browser functional and performance tests, including a representative large map.
- [ ] Document GPL obligations, upstream attribution, and licenses for additional art/sound assets.

### W4 — LAN release candidate

- [ ] Complete a browser-vs-Linux and browser-vs-browser LAN game using the native host and WebSocket bridge.
- [ ] Verify compatible mod lists, persistent cache, and post-session restoration across supported clients.
- [ ] Publish only source and distributable engine assets; require users to import their own original game content.

## C. PlayStation 4 homebrew port (third workstream, gated)

**Gate:** do not start before M0–M5 are complete. Neither the hourly runner nor a contributor may take P-items ahead of LAN. Browser W0–W4 and PS4 P0–P4 are independent of each other.

**Goal:** a homebrew build of the client for PS4 consoles that already run user-installed homebrew. The fork supplies only the application port.

**Scope limits:**

- No jailbreak, exploit, or firmware-modification code or instructions in this repository.
- Toolchain is the community OpenOrbis SDK. No file from the official Sony SDK is added to the repository or to CI.
- No Heroes III data is distributed. The player copies data from their own copy of the game.
- Distributed binaries ship with the matching source (GPL).
- Testing is done by volunteers who have homebrew. The maintainer's console stays on stock firmware.
- Every P-item stays unverified until a log or test result from a real build or run is recorded. An emulator result is not proof of behavior on hardware.

### P0 — Compilation spike (CI only)

- [ ] Document which `lib/` dependencies (Boost, minizip, TBB, Lua/LuaJIT, FFmpeg, SDL2, ICU/Qt-free paths) have OpenOrbis-compatible builds or must be disabled.
- [ ] Add a separate toolchain file and CMake preset for OpenOrbis; desktop presets stay unchanged.
- [ ] Compile `vcmiMain` and the server library in a dedicated workflow, without GUI. The result is recorded as `BUILD_PASS` only from a real CI run.

### P1 — Client backend and input

- [ ] Choose the rendering path (SDL2 port available for OpenOrbis, or a custom backend behind `client/render/`) and record the decision with its limits.
- [ ] Map the gamepad to the existing mouse/keyboard events, including a cursor emulation mode.
- [ ] Resolution, memory and audio budgets are measured, not assumed.

### P2 — Emulator run

- [ ] Run the build in the shadPS4 emulator on PC through the main menu and a small map, and record the emulator version. Missing emulator support is recorded as `BLOCKED`, not worked around in engine code.

### P3 — Packaging and tester kit

- [ ] Package the application and write short instructions for testers: where to copy game data, how to start, how to collect `vcmi.log`.
- [ ] Add the GPL notice and the attribution list to the package.

### P4 — Hardware tests

- [ ] At least two testers on different consoles complete start → map → save/load, with logs attached.
- [ ] LAN mod sync from section A works between a PS4 client and a Linux host, using the same manifest and cache rules. Mod sync on PS4 requires the mobile-style path (no controlled relaunch) until a relaunch mechanism exists.

## Delivery rules

- Follow repository `AGENTS.md` and `CONTRIBUTING.md`. No automated upstream issues or pull requests.
- Keep LAN mod-sync and WebAssembly features separately gated and reviewable.
- Add tests for each significant behavioral change. Never mark a roadmap box complete based on code review alone when runtime integration is required.
- A host-provided mod may contain executable scripts. Treat it as untrusted: validate hashes and obtain trust before first use.
- Do not claim a working LAN transfer or web game until it has been tested end-to-end.
