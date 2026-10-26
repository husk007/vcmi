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
- **First target:** run a full Linux build and `vcmitest` when a full checkout can be obtained; the current container still cannot resolve github.com. Next independent L0 prerequisite: distinguish a controlled relaunch from an interrupted session without bypassing crash recovery.
- **Implemented in source:** `ModSyncPlan`, verified SHA-256 cache decision API, isolated temporary preset snapshot/restore, atomic replacement of settings JSON with an fsync/_commit step before rename, POSIX parent-directory fsync after rename, Windows MoveFileExW with write-through, transaction-like save-before-applying preset transitions, and automatic restoration of an incomplete LAN session at next startup. A post-rename directory-sync failure is logged without falsely rolling back committed in-memory state.
- **Not yet implemented:** controlled-relaunch handshake/recovery semantics, network transfer, persistent archive cache, authenticated manifest, join/leave lifecycle integration, and browser client. The new atomic writer is only smoke-tested on Linux; power-loss behavior on target filesystems and the Windows code path still require verification.
- **Tests:** ten GoogleTest cases registered; **vcmitest NOT RUN**. Standalone C++20 atomic-writer smoke tests **PASS** under GCC 14.2 and Clang (replacement, on-disk content, cleanup, blocked staging path, original config preserved on failure). Previous Boost-path-to-std::filesystem conversion syntax check **PASS**. No full VCMI build, Windows compilation or multiplayer test was run.
- **Production status:** no functional auto-transfer yet. Cache planner only decides whether verified data can be reused; package verification/storage/transport are not wired.

## Hourly result journal

Keep the most recent meaningful outcomes below. Each entry must use a real commit SHA or state `NO COMMIT`, and clearly label `DONE`, `PARTIAL`, or `BLOCKED`. Append only when there is meaningful new work or new evidence; avoid creating an empty status-only commit every hour.

| Run | Criterion | Status | Commit | Validation | Remaining / blocker |
| --- | --- | --- | --- | --- | --- |
| 2026-10-26 setup | Runner policy, hourly schedule, roadmap gates | DONE (configuration only) | See branch commit history | GitHub files verified; build NOT RUN | Begin L0 source validation and implementation on the next hourly run |

| 2026-10-26 17:48 CET | L0 atomic mod settings persistence and interrupted-session recovery | PARTIAL | [c81e08b](https://github.com/husk007/vcmi/commit/c81e08b875a655b85a241d64f995798305860636) (plus earlier code commits this iteration) | Standalone `g++ -std=c++20 -Wall -Wextra -Werror -pedantic` atomic writer smoke PASS; Boost→std path syntax PASS; `vcmitest` NOT RUN | Container `git ls-remote`: `Could not resolve host: github.com`; source checkout and full VCMI build unavailable. Complete power-loss durability and controlled-relaunch recovery before treating journaling as DONE. |

| 2026-10-26 18:46 CET | L0 durability: fsync and safe replace | PARTIAL | [d2db8d8](https://github.com/husk007/vcmi/commit/d2db8d8462ed0d197c41f4a623b408879f2feb03), [6e0bed3](https://github.com/husk007/vcmi/commit/6e0bed3ac906f2176138f0cd9dffb9a8bddf99cd), [4411571](https://github.com/husk007/vcmi/commit/4411571c4644dee15ff1016ef22b61a009ff8244) | Standalone GCC 14.2 and Clang C++20 smoke PASS; 10 GoogleTests NOT RUN; full build NOT RUN | File fsync and parent directory sync added (best-effort warning on metadata sync failure); Windows path untested. `git ls-remote` DNS still fails: `Could not resolve host: github.com`. Next: controlled-relaunch vs crash recovery without losing original preset. |
