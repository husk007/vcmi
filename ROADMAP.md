# Fork Roadmap: LAN Mod Sync and Browser Client

This roadmap describes opt-in features developed on `husk007/vcmi`. It does not change upstream VCMI behavior unless the feature is explicitly enabled. Work in small, reviewable changes and verify each milestone before marking it complete.

## A. LAN multiplayer: host-managed mod set (first priority)

**Goal:** Joining a game should automatically prepare the host's compatible gameplay mods, use previously downloaded packages without retransmitting them, and restore the guest's exact prior active preset after the match.

### L0 — Compatibility, cache decisions, and reversible preset foundation — PARTIAL

- [x] Reuse `ModVerificationInfo::verifyListAgainstLocalMods` and existing `LobbyModsCheck` metadata.
- [x] Add a pure `ModSyncPlan` for missing, disabled, excessive, and version-mismatched mods.
- [x] Plan reuse of a previously verified cached archive when its SHA-256 matches the host manifest.
- [x] Add a LAN-only preset snapshot helper retaining the original active preset and submod settings.
- [x] Add targeted unit tests for both behaviors.
- [ ] Verify compilation and run `vcmitest` on a compatible Linux build.
- [ ] Persist LAN session journal with crash-safe atomic writes and recovery semantics.
- [ ] Connect the helper to the guest lifecycle; the current APIs are not yet invoked by the UI.

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

- [ ] Create an on-disk atomic restore journal **before** changing the active preset.
- [ ] Create a temporary `__vcmi_lan_session` preset, preserving the full original preset (including enabled and disabled submods) and the original preset name.
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
