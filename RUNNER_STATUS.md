# VCMI Hourly Runner — current state

**Last management review:** 2026-10-30, Europe/Warsaw. This is a compact checkpoint, not a per-hour transcript. Prior progress reports remain available in the Git history of this file.

## Configuration

- Repo: `husk007/vcmi`; feature branch: `feature/lan-mod-sync-and-web-roadmap`; base: `develop` (read only).
- Rules: [ROADMAP.md](ROADMAP.md) M0–M5, [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md).
- Scheduled task: VCMI ROADMAP Runner (hourly in ChatGPT Tasks).
- Existing source work before this review: **182 commits** ahead of `develop`, touching 26 files; these are not 182 completed features.
- **Do not append another hourly transcript to this file.** Keep it under ~6 KB and update only after a meaningful milestone or blocker change.

## Evidence and verification

| Area | Current evidence | Status |
| --- | --- | --- |
| M0 real vcmitest + LAN GoogleTests | [CI #41058914568](https://github.com/husk007/vcmi/actions/runs/41058914568) compiled real `vcmitest`, `vcmiserver`, `vcmiclient`, but **91/92 LAN tests passed**. Crash recovery restored the personal preset; test baseline omitted VCMI's automatic `core` mod. Fixture corrected; CI pending. No two-GUI-client LAN match. | BUILD_PASS; TEST_FAIL (1) |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache passed the 71 committed VCMI tests; these are unit tests, not a real two-client LAN session. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | Existing opt-in UDP/TCP host manifest, legacy fallback and cache labels passed previous CI. LAN room preview now labels the individual installed-only mod activations/deactivations (pending the next CI), not just their totals. The opt-in host preview now reports whether lobby accepts players and blocks stale joins; CI #40976444839 passed. A rejected automatic post-relaunch join now immediately restores the original preset and exits the stale join UI (new change awaits CI). No archive transfer yet. | CODE_ONLY (new busy gate); TEST_PASS (previous) |
| M2 guest join with installed disabled mods | Earlier real CI passed installed-only staging, restart/rejoin, host-required disabled mod preview and live TCP rejoin (91/91 in CI #41044274620). Separate `vcmiclient` processes resume and recover after a crash, but CI #41058914568 fails its assertion on the auto-normalized `core` mod. Fixture corrected; CI pending. No actual two-GUI-client game or end-of-game restore evidence. | CODE_ONLY (new); TEST_PASS (earlier) |
| M3 repeated-session cache reuse | Persistent verified cache helpers and read-only preflight exist; host manifest and real join/transfer counters are missing. | PARTIAL / NOT TESTED |
| M4 host-to-guest transfer | No verified authenticated network transfer of missing archives or integrated install path. | NOT IMPLEMENTED |
| M5 two-client LAN acceptance | No completed real LAN game with mod round trip documented. | NOT TESTED |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Validate the corrected process crash-recovery baseline in scoped CI; if passing, test the actual graphical LAN join/rejoin and restoration.
2. Exercise an installed-but-disabled mod through restart, rejoin and exact restoration with two actual LAN clients.
3. Add explicit first-use trust and license checks before any host archive transfer; keep proprietary game assets excluded.
4. Bind M3 cache reuse and M4 host transfer after host package identity/version validation.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

