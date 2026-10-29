# Fork Roadmap: LAN Mod Sync and Browser Client

This roadmap describes opt-in features developed on `husk007/vcmi`. It does not change upstream VCMI behavior unless the feature is explicitly enabled. Work in small, reviewable changes and verify each milestone before marking it complete.

## Execution strategy — deliver a usable LAN vertical slice

The hourly task follows [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md), and [RUNNER_STATUS.md](RUNNER_STATUS.md). Work only on `feature/lan-mod-sync-and-web-roadmap`. The original L0–L4/W0–W4 sections below define **feature scope**, not a rule to keep patching L0 indefinitely. Follow the concrete milestones M0–M5 below in order; leave any unverified test gate open but continue implementing independently testable functionality.

**Audit 2026-10-29:** 182 commits existed on the feature branch relative to `develop`, with zero GitHub Actions runs and no verified full VCMI compile, `vcmitest`, two-client LAN exchange, or automatic mod transfer. Many changes improve cache/backup integrity, but the host manifest and network integration are still missing. Source-only checks and isolated reproductions are **not** proof that VCMI builds.

**Current focus:** get the real VCMI compile/test result, then implement the **host manifest + capability negotiation and client preview**. Do not add another cache-hardening edge case unless the next vertical slice demonstrably fails without it.

### M0 — Real build and regression evidence (highest-priority parallel gate)

- [x] Add a narrowly triggered GitHub Actions workflow for a real Linux VCMI build and committed LAN tests: `.github/workflows/lan-mod-sync-validation.yml`.
- [x] Obtain a real `vcmitest` build and execute `LanModSyncTest.*`: [CI run #40796885539](https://github.com/husk007/vcmi/actions/runs/40796885539) passed **71/71 tests** on commit `a1e2bf2e70eb9ac2636091c8baf6d1926087a374` after correcting the runtime data setup. The wider client build is a separate gate.
- [ ] If GitHub Actions is disabled or quota-blocked on this fork, record the blocker once and ask for the **specific** enablement action; do not replay the same failed DNS probe every hour. Continue M1–M3 with source-level validation and explicitly keep M0 open.
- [ ] Before a feature release, run the broader `ctest --preset linux-gcc-test` and verify at least one actual game startup.

GitHub Actions is intentionally **not** triggered by every hourly code commit. It runs when its workflow file or `.github/lan-mod-sync-validation-trigger.txt` changes, or when launched via `workflow_dispatch`. Change the trigger file only after a meaningful code milestone or a fix for a proven CI failure; do not generate empty reruns. GitHub Actions availability is unverified until a run appears.

### M1 — Host compatibility preview and opt-in negotiation (first functional slice)

- [ ] Review `LobbyQueryState` → `LobbyModsCheck` → `CServerHandler::onLobbyPreviewResponse` and document exact fields that already work. Reuse them; do not create a parallel protocol.
- [ ] Advertise `lanModSync` as an optional capability using a backward-compatible/version-gated protocol path. A client lacking the capability must retain the existing room preview without disconnecting.
- [ ] Display a read-only host/guest comparison in the LAN join preview: mod ID, version, required/disabled/excessive, and action plan. Do not yet change disk files.
- [ ] Add a focused integration or protocol fixture test proving that the host advertises the correct mod list and the guest produces a matching diff.

### M2 — Connect with mods already installed (first executable happy path)

- [ ] On joining a host requiring only locally installed mods, snapshot the full original preset durably; activate required mods/deactivate excessive mods in a temporary LAN preset.
- [ ] Safely reload game resources or restart/rejoin the same LAN room; perform the connection with the correct active mods, then restore the original preset on clean disconnect, rejected join, and crash recovery.
- [ ] Test one host + one guest where the guest initially has the right mod **installed but disabled**. Prove a successful match without downloading an archive, then verify exact prior preset and submod restoration.

### M3 — Reuse cached mod packages between sessions

- [ ] Bind the existing persistent SHA-256 cache to the actual host manifest and guest join flow. A valid local archive is a cache hit; an already installed matching mod only needs activation.
- [ ] Verify two successive joins to the same host with identical manifest: the **second transfer must be 0 bytes** and cached content still activates correctly.
- [ ] Verify changed hash/version triggers only affected archive transfer; other cached mods are reused.

### M4 — Transfer missing, redistributable mods from host

- [ ] Add bounded, authenticated, session-scoped host→guest archive transfer and a first-use trust prompt. Reject missing/invalid hashes, malicious archive paths, and nonredistributable content.
- [ ] Stage versions side by side so the player's privately installed mod version is not overwritten.
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

Source helpers and tests have been added, but this milestone remains unverified until the full build and real test suite succeed. Several crash-restore and cache helpers are now implemented beyond the initial draft. Do not repeat hardening without a failing validation or a dependency on M1–M4.

- [x] Reuse `ModVerificationInfo::verifyListAgainstLocalMods` and existing `LobbyModsCheck` metadata.
- [x] Add a pure `ModSyncPlan` for missing, disabled, excessive, and version-mismatched mods.
- [x] Plan reuse of a previously verified cached archive when its SHA-256 matches the host manifest.
- [x] Add a LAN-only preset snapshot helper retaining the original active preset and submod settings.
- [x] Add targeted unit tests for both behaviors.
- [ ] Verify compilation and run `vcmitest` on a compatible Linux build.
- [ ] Persist LAN session journal with crash-safe atomic writes and recovery semantics.
- [ ] Complete guest lifecycle integration. Restore/cancellation hooks already exist in source, but actual host-selected mod activation and automatic LAN rejoin are not wired or validated in a real client.

### L1 — Capability negotiation and manifest

- [ ] Negotiate an optional `lanModSync` capability before modifying any guest files; retain interoperability with unmodified VCMI clients.
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

## Delivery rules

- Follow repository `AGENTS.md` and `CONTRIBUTING.md`. No automated upstream issues or pull requests.
- Keep LAN mod-sync and WebAssembly features separately gated and reviewable.
- Add tests for each significant behavioral change. Never mark a roadmap box complete based on code review alone when runtime integration is required.
- A host-provided mod may contain executable scripts. Treat it as untrusted: validate hashes and obtain trust before first use.
- Do not claim a working LAN transfer or web game until it has been tested end-to-end.
