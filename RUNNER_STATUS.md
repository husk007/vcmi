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
| M0 real vcmitest + LAN GoogleTests | [CI #41140679762](https://github.com/husk007/vcmi/actions/runs/41140679762) passed on `b1a659c`, building real `vcmitest`, `vcmiserver`, `vcmiclient` and running LAN tests. Later gameplay-result relaunch changes await build/test. | BUILD_PASS / TEST_PASS (previous code); CODE_ONLY (new restart) |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache passed the 71 committed VCMI tests; these are unit tests, not a real two-client LAN session. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | Existing opt-in UDP/TCP host manifest, legacy fallback and cache labels passed previous CI. LAN room preview now labels the individual installed-only mod activations/deactivations (pending the next CI), not just their totals. The opt-in host preview now reports whether lobby accepts players and blocks stale joins; CI #40976444839 passed. A rejected automatic post-relaunch join now immediately restores the original preset and exits the stale join UI (new change awaits CI). No archive transfer yet. | CODE_ONLY (new busy gate); TEST_PASS (previous) |
| M2 guest join with installed disabled mods | Independent-host double rejoin passed in CI #41126748632, including required-mod activation, guest-only deactivation and exact preset rollback. New commit `a624d9d` uses the GUI shutdown path and argument-free desktop relaunch after restoring mods on lobby exit, connection failure, cancelled preview or rejected rejoin. Returning from gameplay to the menu now requests a token-free desktop relaunch after restoring the personal preset. A new scenario-results path defers LAN preset reload until victory/defeat screens and the optional high-score list are dismissed; campaign epilogues still retain the manual notice. Explicit application shutdown never relaunches. New path awaits CI and real GUI validation. | TEST_PASS (previous); CODE_ONLY (new automatic lobby restart) |
| M3 repeated-session cache reuse | Persistent verified cache helpers and read-only preflight exist; host manifest and real join/transfer counters are missing. | PARTIAL / NOT TESTED |
| M4 host-to-guest transfer | No verified authenticated network transfer of missing archives or integrated install path. | NOT IMPLEMENTED |
| M5 two-client LAN acceptance | No completed real LAN game with mod round trip documented. | NOT TESTED |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Validate the new automatic desktop relaunch after personal-preset restoration in real `vcmitest` and full `vcmiclient` compilation. Test that restart does not forward a LAN rejoin token.
2. Validate deferred scenario-results relaunch after score submission, cancellation, defeat and explicit app exit against a real GUI; preserve campaign epilogues.
3. Run the graphical two-client LAN scenario with legally owned Heroes III assets; CI's public fixture cannot establish LAN_PASS.
4. Finish M3/M4 only after an actual host–guest archive transfer and trust/redistributability gate.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

