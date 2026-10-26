# VCMI Hourly Runner Status

This file is updated by the scheduled task only when the outcome materially changes. It is an execution record, not evidence that a build or LAN test succeeded.

## Configuration

- Repository: `husk007/vcmi`
- Branch: `feature/lan-mod-sync-and-web-roadmap`
- Plan: [ROADMAP.md](ROADMAP.md)
- Rules: [AGENTS.md](AGENTS.md), [RUNNER.md](RUNNER.md)
- Scope: complete LAN milestones L0-L4 before WebAssembly W0-W4, except independent blocked work
- Scheduled execution: ACTIVE in ChatGPT Tasks, once per hour (first planned trigger 2026-10-26 16:48 Europe/Warsaw). The repository file itself is not an executor.

## Latest known state

- **Stage:** L0 — PARTIAL
- **First target:** run a full Linux build and `vcmitest` when a checkout becomes available; container GitHub DNS resolution remains unavailable. Next independent L0 prerequisite: implement a safe, explicit controlled-relaunch initiation path (fresh one-shot token, durable prepare, restart and room rejoin), then connect cleanup/rollback to session lifecycle. Client startup token propagation is now present.
- **Implemented in source:** `ModSyncPlan`, verified SHA-256 cache decision API, isolated temporary preset snapshot/restore, atomic replacement of settings JSON with an fsync/_commit step before rename, POSIX parent-directory fsync after rename, Windows MoveFileExW with write-through, transaction-like save-before-applying preset transitions, automatic restoration of incomplete LAN sessions, and token-matched one-shot controlled relaunch recovery with original preset preservation. The client now accepts `--lan-relaunch-token` and forwards it via `GameLibrary` and `CModHandler` to `ModManager` before mod discovery. A post-rename directory-sync failure is logged without falsely rolling back committed in-memory state.
- **Not yet implemented:** secure generation of a fresh relaunch token, automatic handoff to a spawned client, controlled process restart, room rejoin, transfer, persistent archive cache, authenticated manifest, join/leave lifecycle integration, or browser client. The new token is accepted at startup but nothing initiates the relaunch yet. The atomic writer is only smoke-tested on Linux; power-loss behavior on target filesystems and the Windows code path still require verification.
- **Tests:** 18 GoogleTest cases registered (one newly added for exact restoration after a serialized controlled relaunch); **vcmitest NOT RUN**. Earlier seven isolated C++20 scenarios using a minimal JsonNode-like double **PASS** under GCC and Clang (not a full VCMI build). This iteration: isolated Boost.Program_options parser smoke **PASS** with GCC 14.2 and Clang 17, after linking directly to the installed Boost 1.83 runtime SONAME because the development linker symlink is missing; ten source/API propagation assertions **PASS** by inspection through GitHub. Earlier atomic-writer standalone smoke tests **PASS**. No full VCMI build, Windows compilation, or multiplayer test was run; the container still cannot resolve github.com.
- **Production status:** no functional auto-transfer yet. Cache planner only decides whether verified data can be reused; package verification/storage/transport are not wired.

## Hourly result journal

Keep the most recent meaningful outcomes below. Each entry must use a real commit SHA or state `NO COMMIT`, and clearly label `DONE`, `PARTIAL`, or `BLOCKED`. Append only when there is meaningful new work or new evidence; avoid creating an empty status-only commit every hour.

| Run | Criterion | Status | Commit | Validation | Remaining / blocker |
| --- | --- | --- | --- | --- | --- |
| 2026-10-26 setup | Runner policy, hourly schedule, roadmap gates | DONE (configuration only) | See branch commit history | GitHub files verified; build NOT RUN | Begin L0 source validation and implementation on the next hourly run |

| 2026-10-26 17:48 CET | L0 atomic mod settings persistence and interrupted-session recovery | PARTIAL | [c81e08b](https://github.com/husk007/vcmi/commit/c81e08b875a655b85a241d64f995798305860636) (plus earlier code commits this iteration) | Standalone `g++ -std=c++20 -Wall -Wextra -Werror -pedantic` atomic writer smoke PASS; Boost→std path syntax PASS; `vcmitest` NOT RUN | Container `git ls-remote`: `Could not resolve host: github.com`; source checkout and full VCMI build unavailable. Complete power-loss durability and controlled-relaunch recovery before treating journaling as DONE. |

| 2026-10-26 18:46 CET | L0 durability: fsync and safe replace | PARTIAL | [d2db8d8](https://github.com/husk007/vcmi/commit/d2db8d8462ed0d197c41f4a623b408879f2feb03), [6e0bed3](https://github.com/husk007/vcmi/commit/6e0bed3ac906f2176138f0cd9dffb9a8bddf99cd), [4411571](https://github.com/husk007/vcmi/commit/4411571c4644dee15ff1016ef22b61a009ff8244) | Standalone GCC 14.2 and Clang C++20 smoke PASS; 10 GoogleTests NOT RUN; full build NOT RUN | File fsync and parent directory sync added (best-effort warning on metadata sync failure); Windows path untested. `git ls-remote` DNS still fails: `Could not resolve host: github.com`. Next: controlled-relaunch vs crash recovery without losing original preset. |

| 2026-10-26 19:50 CET | L0 one-shot controlled LAN relaunch and safe crash recovery | PARTIAL | [3b0513f](https://github.com/husk007/vcmi/commit/3b0513ff9571217bfe8c36c10ae32fd4dd0cf0ee), [97c74cf](https://github.com/husk007/vcmi/commit/97c74cf3616b899a50a64e9c606d761c7b927e96), [d6970e9](https://github.com/husk007/vcmi/commit/d6970e9090d0d96776c5b20971e2e056c01f0a40) (also test/documentation commits) | 7 standalone C++20 scenarios using a simplified JsonNode test double PASS under GCC/Clang; source/API review PASS; 17 GoogleTests NOT RUN, no full build | Container still cannot resolve github.com (git ls-remote failed). Token matching and one-time consumption implemented, but real launcher/CModHandler argument propagation, process relaunch, Windows behavior, and end-to-end testing are pending. |

| 2026-10-26 20:xx CET | L0 controlled-relaunch startup propagation (no automatic restart yet) | PARTIAL | [f12d4ec](https://github.com/husk007/vcmi/commit/f12d4ec8d92f7289af84aa96ea25a8f23e875c86), [e149c64](https://github.com/husk007/vcmi/commit/e149c64c8f79f3da8fce31430368fffce2afa882) and CModHandler/GameLibrary commits | Isolated Boost CLI parser smoke PASS: GCC/Clang with Boost 1.83 runtime SONAME; 10 GitHub source/API consistency assertions PASS; 18 GoogleTests registered but NOT RUN, full build NOT RUN | Startup option now reaches ModsPresetState recovery before filesystems load. No token generator, spawned restart, automatic room rejoin, or real game test. Container `git ls-remote` still fails: `Could not resolve host: github.com`. |
