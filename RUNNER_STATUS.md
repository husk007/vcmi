# VCMI Hourly Runner — current state

**Last management review:** 2026-11-11, Europe/Warsaw. This is a compact checkpoint, not a per-hour transcript. Prior progress reports remain available in the Git history of this file.

## Configuration

- Repo: `husk007/vcmi`; feature branch: `feature/lan-mod-sync-and-web-roadmap` (merged into `develop` by #1); current work: `ccr-acf01786-wxedmj`, PR #2.
- Rules: [ROADMAP.md](ROADMAP.md) M0–M5, [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md).
- Scheduled task: VCMI ROADMAP Runner (hourly in ChatGPT Tasks).
- 2026-10-29 baseline: **182 commits** ahead of `develop` at that point; commit count is not feature-completion evidence.
- **Do not append another hourly transcript to this file.** Keep it under ~6 KB and update only after a meaningful milestone or blocker change.

## Evidence and verification

| Area | Current evidence | Status |
| --- | --- | --- |
| M0 real vcmitest + LAN GoogleTests | [CI #41580945912](https://github.com/husk007/vcmi/actions/runs/41580945912) passed on `7a82d33`: full Linux client/server/vcmitest build, **104/104** LAN tests on the independent host. Local rerun on `30eb687` (Ubuntu 24.04, GCC 13, 2026-11-04): same 104/104 with the CI fixture script, and full `ctest --preset linux-gcc-test` **1361/1361**. Game startup with owned data: licensed-data GUI acceptance (`66b67d2`). Full CI `github.yml` on PR #2 builds Windows, Linux AppImage and Android; iOS and macOS on Intel fail. | BUILD_PASS / TEST_PASS |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache pass in the committed suite, now 105 `LanModSyncTest.*` tests. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | Opt-in TCP preview and legacy fallback are integrated. `35d9cbd` + `7a6e204` transmit selected-map human slots (unknown stays hidden). Commit `d563c44` rejects over-capacity real TCP registrations; [CI #41580945912](https://github.com/husk007/vcmi/actions/runs/41580945912) passed 104/104 on `7a82d33`. Preview packet fields are documented in `docs/developers/Lan_Mod_Synchronization.md`. | BUILD_PASS / TEST_PASS (validated commit) |
| M2 guest join with installed disabled mods | Activation of installed-but-disabled mods, guest-only deactivation and exact preset rollback pass against the independent server. GUI relaunch, rejoin and restore pass in the licensed-data GUI acceptance (`66b67d2`, 5/5). On Android the preset journal failed on storage without hard links until `3ed70a0`; afterwards the relaunch worked on two phones. | TEST_PASS (GUI, licensed data); Android relaunch verified |
| M3 repeated-session cache reuse | Host manifest SHA-256 binds cache approvals and controlled rejoin. CI #41344249608 passed 103/103 real vcmitest tests including live host refresh: unchanged archive stays a cache hit; changed SHA-256 transfers only the modified root. GUI reuse still untested. | TEST_PASS (headless) |
| M4 host-to-guest transfer | Transfer, trust dialog and side-by-side staging: CI #41290727321, #41323542905. The host packs its active mods itself (`LanHostModPublisher`, `ed15552`). Android, two phones, `a59571f`: the guest downloaded 596 MB, cached and unpacked four roots, relaunched and rejoined; the game started with the host's mods. Unpacking no longer freezes the GUI (`3f70c14`). | TEST_PASS; Android download verified |
| M5 two-client LAN acceptance | Licensed-data GUI acceptance passed 5/5 on `66b67d2` (loopback, one machine). Android, two phones: after the game start the guest neither received nor sent packets, while the server kept sending and neither side reported a disconnection. Heartbeats are traced since `d229432` to locate the stall. | LAN_PASS pending; guest stall open |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Owner test on two phones with `d229432` or later: A) identical mods, no sync; B) with sync. Report whether the guest screen turned off, export logs from both phones. The heartbeat trace shows when the connection stopped.
2. Fix the cause of the guest stall if it is in VCMI; otherwise document the phone setting.
3. Port the LAN code from `std::filesystem` to `boost::filesystem` before an upstream PR; iOS and macOS on Intel do not build.
4. A client can terminate the server by sending identifiers of a mod the host does not load (`HeroTypeID::decode` in `LobbySetMap`); handle it as a rejected request.
5. Windows test; PS4 P0–P4 stays gated until M5 passes.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.
