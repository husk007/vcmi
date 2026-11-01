# VCMI Hourly Runner — current state

**Last management review:** 2026-11-01, Europe/Warsaw. This is a compact checkpoint, not a per-hour transcript. Prior progress reports remain available in the Git history of this file.

## Configuration

- Repo: `husk007/vcmi`; feature branch: `feature/lan-mod-sync-and-web-roadmap`; base: `develop` (read only).
- Rules: [ROADMAP.md](ROADMAP.md) M0–M5, [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md).
- Scheduled task: VCMI ROADMAP Runner (hourly in ChatGPT Tasks).
- 2026-10-29 baseline: **182 commits** ahead of `develop` at that point; commit count is not feature-completion evidence.
- **Do not append another hourly transcript to this file.** Keep it under ~6 KB and update only after a meaningful milestone or blocker change.

## Evidence and verification

| Area | Current evidence | Status |
| --- | --- | --- |
| M0 real vcmitest + LAN GoogleTests | [CI #41353785065](https://github.com/husk007/vcmi/actions/runs/41353785065) passed on `17e36ab` (104/104 LAN tests): real `vcmitest`, server and client built; delayed UDP opt-in, live ZIP refresh and selective transfer. GUI play still untested. | BUILD_PASS / TEST_PASS (prior code) |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache passed the 71 committed VCMI tests; these are unit tests, not a real two-client LAN session. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | Existing opt-in UDP/TCP host manifest, legacy fallback and cache labels passed previous CI. LAN room preview now labels the individual installed-only mod activations/deactivations (pending the next CI), not just their totals. The opt-in host preview now reports whether lobby accepts players and blocks stale joins; CI #40976444839 passed. A rejected automatic post-relaunch join now immediately restores the original preset and exits the stale join UI (new change awaits CI). Host ZIP transfer and staging are already implemented under M4. | CODE_ONLY (new busy gate); TEST_PASS (previous) |
| M2 guest join with installed disabled mods | Required installed-but-disabled mod activation, guest-only deactivation and exact preset rollback passed independent-server tests. Token-free desktop restart after lobby/game/scenario results and completed-campaign epilogue/outro/results now builds and passes committed tests on `35d7122`. The graphical two-client session still needs licensed Heroes III assets. | TEST_PASS (non-GUI); LAN NOT TESTED |
| M3 repeated-session cache reuse | Host manifest SHA-256 binds cache approvals and controlled rejoin. CI #41344249608 passed 103/103 real vcmitest tests including live host refresh: unchanged archive stays a cache hit; changed SHA-256 transfers only the modified root. GUI reuse still untested. | TEST_PASS (headless) |
| M4 host-to-guest transfer | [CI #41290727321](https://github.com/husk007/vcmi/actions/runs/41290727321) passed on `d5a4127`: production server serves two active mod roots (including an active submod) to two guests, supports SHA-256-verified cache reuse, and restores temporary presets; 102 tests passed. One-command publisher-to-client integration passed CI #41294799985. Linux launcher XDG config/data alignment passed CI #41303434591. Licensed-data GUI play remains untested. First-use trust dialog identifies root mods, enabled submods, versions and archive digests; CI #41323542905 passed. The host launcher refreshes modified active-root ZIPs automatically; CI #41334098057 and #41344249608 passed. First-use trust and ZIP staging are covered by earlier tests. | BUILD_PASS / TEST_PASS (prior) |
| M5 two-client LAN acceptance | Production-server test now checks host pre-start propagation to two simultaneous registered guests (commit `2db15b7`, new assertion awaits CI). Opt-in two-real-GUI-client lobby test and isolated Linux Mint launcher still lack full licensed-assets execution; no completed game proof. | CODE_ONLY (opt-in GUI) |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Verify host `LobbyPrepareStartGame` reaches both real LAN lobby guests in committed `vcmitest` (commit `2db15b7`).
2. Exercise the GUI epilogue → outro → scores → restored-preset relaunch with legally owned Heroes III data; the public CI fixture cannot establish LAN_PASS.
3. Run two GUI clients with licensed Heroes III data to check cache-only rejoin, no transfer and full restoration.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

