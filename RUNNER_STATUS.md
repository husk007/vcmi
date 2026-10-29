# VCMI Hourly Runner — current state

**Last management review:** 2026-10-29, Europe/Warsaw. This is a compact checkpoint, not a per-hour transcript. Prior progress reports remain available in the Git history of this file.

## Configuration

- Repo: `husk007/vcmi`; feature branch: `feature/lan-mod-sync-and-web-roadmap`; base: `develop` (read only).
- Rules: [ROADMAP.md](ROADMAP.md) M0–M5, [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md).
- Scheduled task: VCMI ROADMAP Runner (hourly in ChatGPT Tasks).
- Existing source work before this review: **182 commits** ahead of `develop`, touching 26 files; these are not 182 completed features.
- **Do not append another hourly transcript to this file.** Keep it under ~6 KB and update only after a meaningful milestone or blocker change.

## Evidence and verification

| Area | Current evidence | Status |
| --- | --- | --- |
| M0 full VCMI build + real LAN GoogleTests | [GitHub Actions run #40792010925](https://github.com/husk007/vcmi/actions/runs/40792010925) was started on 2026-10-29. **Result not yet verified.** | RUNNING / CHECK LOGS |
| LAN helper code and tests | `ModSyncPlan`, temporary LAN preset journal, SHA-256 cache and many `LanModSyncTest.*` cases are committed. Numerous isolated GCC/Clang reproductions passed, but no actual `vcmitest` result has been recorded yet. | CODE_ONLY |
| M1 capability negotiation and authenticated host manifest | Existing `LobbyQueryState` / `LobbyModsCheck` supports a host-mod preview. New opt-in negotiation/manifest is not connected. | NOT IMPLEMENTED |
| M2 guest join with installed disabled mods | Temporary preset/restore hooks exist, but a working join → temporary activation → relaunch/rejoin → restoration flow is not proven. | PARTIAL / NOT TESTED |
| M3 repeated-session cache reuse | Persistent verified cache helpers and read-only preflight exist; host manifest and real join/transfer counters are missing. | PARTIAL / NOT TESTED |
| M4 host-to-guest transfer | No verified authenticated network transfer of missing archives or integrated install path. | NOT IMPLEMENTED |
| M5 two-client LAN acceptance | No completed real LAN game with mod round trip documented. | NOT TESTED |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Inspect [LAN CI run #40792010925](https://github.com/husk007/vcmi/actions/runs/40792010925); if it fails, fix the actual CMake/compile/test error first (M0). Do not claim PASS without a successful run.
2. Continue M1 while the build is pending: use `lib/networkPacks/PacksForLobby.h`, `server/NetPacksLobbyServer.cpp`, `client/CServerHandler.cpp`, and `client/globalLobby/GlobalLobbyRoomWindow.cpp` to negotiate host capabilities and present the mod diff. Design the protocol change to preserve compatibility with unmodified VCMI clients.
3. After M1, implement M2: activate **already-installed** matching mods using the temporary LAN preset, relaunch/rejoin, then restore the original preset; this requires no new transfer protocol.
4. Next bind M3 hash-verification cache reuse and M4 host transfer; verify a second join transfers zero bytes.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

