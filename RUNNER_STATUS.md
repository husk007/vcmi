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
| M0 real vcmitest + LAN GoogleTests | [CI #40846276198](https://github.com/husk007/vcmi/actions/runs/40846276198) on `7f288a7` built `vcmitest`, `vcmiclientcommon` and `vcmiservercommon`; **82/82 `LanModSyncTest.*` PASS** (2026-10-29). Full client executable and two-client LAN game remain unverified. | TEST_PASS (vcmitest) |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache passed the 71 committed VCMI tests; these are unit tests, not a real two-client LAN session. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | Optional UDP discovery and TCP query/response packet IDs, legacy fallback, host active mod list and guest installed-only preview compiled; the committed binary roundtrip test passed in CI #40831773720. SHA-256 host archive manifest still missing. | TEST_PASS (protocol fixture) |
| M2 guest join with installed disabled mods | Installed-only staging passed **79/79** tests in CI #40838909050 (2026-10-29). One-shot rejoin built and 81/81 tests passed in CI #40846276198. Failed-preview cleanup compiled and passed **82/82** in [CI #40860763346](https://github.com/husk007/vcmi/actions/runs/40860763346). Installed-only stage now checks actual dependency resolution before restart and rolls back on unresolved host mods; new check awaits integrated CI and two-client validation. | CODE_ONLY (new resolver check) |
| M3 repeated-session cache reuse | Persistent verified cache helpers and read-only preflight exist; host manifest and real join/transfer counters are missing. | PARTIAL / NOT TESTED |
| M4 host-to-guest transfer | No verified authenticated network transfer of missing archives or integrated install path. | NOT IMPLEMENTED |
| M5 two-client LAN acceptance | No completed real LAN game with mod round trip documented. | NOT TESTED |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Verify installed-only dependency resolver preflight in Linux CI, then exercise the restart/join path in a real client.
2. Exercise a disabled-but-installed guest mod through restart, join, disconnect and exact preset restoration in two LAN clients.
3. Complete M1 with a bounded host SHA-256 manifest in the negotiated opt-in response, and verify legacy fallback remains unchanged.
4. Next bind M3 hash-verification cache reuse and M4 host transfer; verify a second join transfers zero bytes.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

