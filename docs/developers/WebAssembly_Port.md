# WebAssembly Browser Port: Technical Spike

## Objective

Run the VCMI game client in current desktop browsers, with local user-provided Heroes III game data, and join multiplayer games hosted by a native VCMI server on the same LAN.

The existing `lobby/web` directory implements a website for the global lobby. It is **not** a compiled game client. This project needs a separate browser runtime.

## Architecture

```text
Browser                                  Host on LAN
+----------------------------------+    +--------------------------------------+
| HTML / Canvas / Audio             |    | Native VCMI game server              |
| SDL2 / VCMI client compiled WASM |    |                                      |
| User-provided game assets in OPFS |    | WebSocket <-> existing TCP gateway   |
| Mod archive cache in OPFS/IDB    |    | Existing authoritative game logic    |
+----------------------------------+    +--------------------------------------+
                 |                                       |
                 +--------------- WebSocket ------------+
```

The native server remains authoritative initially. Do **not** attempt to make the web browser a listening TCP server or to run the Qt desktop launcher inside WASM.

## First feasibility questions

1. Which `lib`, `client`, `clientsdl2`, and `clientapp` dependencies compile under the current Emscripten C++20 toolchain?
2. Can we run SDL2 canvas rendering, input, music, and an event loop without blocking the browser main thread?
3. Which compiled runtime dependencies require native replacements or exclusion (LuaJIT, FFmpeg, Boost socket backend, system filesystem, thread/process spawning)?
4. Can we load a small legal test fixture map and render one interactive frame without original commercial assets?
5. Can a browser client reach the native server through a WebSocket adapter while preserving the existing serialized network protocol and version checks?

## Suggested implementation order

### W0 — Minimal compilation proof

- Build a separate Emscripten preset without altering native CMake presets.
- Compile a small shared subset of `lib` and one SDL2 canvas scene.
- Create a smoke test page that initializes, renders, and exits cleanly.
- Measure WASM size, initial memory footprint, and launch time.

### W1 — Resource and renderer adaptation

- Add a browser-supported virtual filesystem abstraction for user-imported game data.
- Persist imported data through IndexedDB/OPFS where supported, with clear storage quota handling.
- Audit thread behavior and adapt the frame loop to browser callbacks; do not block the UI thread with a synchronous infinite loop.
- Map mouse, keyboard, resize, and user-gesture audio initialization to SDL2.
- Load a small playable scenario with user-provided assets.

### W2 — Browser networking

- Add a narrow WebSocket transport interface in `lib/network/` behind a platform feature flag.
- Build a native gateway that forwards bounded framed VCMI packets between WebSocket and local TCP.
- Authenticate and validate the browser-origin handshake, restrict LAN binding, and reject oversized frames.
- Keep the current native server responsible for game rules, turns, saves, and mod compatibility.
- Exercise multiplayer with a browser guest and a Linux host.

### W3 — Mod cache and playable UI

- Reuse the manifest semantics from `Lan_Mod_Synchronization.md`.
- Store redistributable mod archives by verified digest; do not repeatedly download identical content.
- Provide the web player with a small file-import and session selection screen.
- Add save/load, audio, map selection, and compatible mod support one feature at a time.

### W4 — Local network release

- Test browser/Linux and browser/browser guests connected to a native server.
- Test Chromium and Firefox on Linux and Windows, and modern Safari if the platform constraints permit.
- Confirm deterministic turns, disconnect/reconnect, cross-platform saves, and file cache recovery.
- Publish engine source/build scripts and redistributable VCMI assets only.

## Browser-specific risks

- Native sockets are not available from browser JavaScript. WebSocket or WebTransport bridging is mandatory.
- LuaJIT uses native machine-code generation and is unsuitable for a straightforward browser build. Evaluate a compatible Lua interpreter and script compatibility.
- Pthreads and SharedArrayBuffer depend on browser isolation headers and platform support; minimize threads in the first proof.
- Large archives and maps may exceed browser storage quotas. Handle failures without losing the user's files.
- File System Access API support varies. Provide a portable file input/drag-drop fallback.
- Asset licensing is independent of source-code licensing. Do not publish, serve, or automatically transfer commercial Heroes III data; each user imports a lawful copy.
- A LAN HTTP endpoint can require special handling for mixed-content restrictions and secure origins when the browser page is served over HTTPS.

## Completion criteria

This is a technical research workstream until a reproducible build exists. A milestone counts as shipped only when it has a build command, a browser smoke test, and a documented failure/recovery path. A static lobby website or WASM hello-world by itself is not a playable VCMI port.
