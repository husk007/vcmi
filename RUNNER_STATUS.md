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
| M0 real vcmitest + LAN GoogleTests | [CI #40868852556](https://github.com/husk007/vcmi/actions/runs/40868852556) built `vcmitest`, `vcmiclientcommon` and `vcmiservercommon`; **83/83 `LanModSyncTest.*` PASS** (2026-10-29). Full client executable and two-client LAN game remain unverified. | TEST_PASS (vcmitest) |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache passed the 71 committed VCMI tests; these are unit tests, not a real two-client LAN session. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | UDP/TCP opt-in, legacy fallback and installed-only preview compiled. New SHA-256/size manifest and guest cache counts are unverified: [CI #40877059153](https://github.com/husk007/vcmi/actions/runs/40877059153) failed at `vcmitest` build because `BinaryDeserializer` rejects unsigned 64-bit values. Commit `c6a80c0` uses signed 64-bit archive size (max 2 GiB), checks nonpositive sizes in UI and extends roundtrip test; rerun pending. Legacy packets unchanged. | CODE_ONLY (host archive manifest) |
| M2 guest join with installed disabled mods | Installed-only staging passed **79/79** tests in CI #40838909050 (2026-10-29). One-shot rejoin built and 81/81 tests passed in CI #40846276198. Failed-preview cleanup compiled and passed **82/82** in [CI #40860763346](https://github.com/husk007/vcmi/actions/runs/40860763346). Installed-only stage checks actual dependency resolution before restart and rolls back on unresolved host mods; **83/83** tests passed in [CI #40868852556](https://github.com/husk007/vcmi/actions/runs/40868852556). Two-client validation remains pending. | TEST_PASS (unit; LAN unverified) |
| M3 repeated-session cache reuse | Persistent verified cache helpers and read-only preflight exist; host manifest and real join/transfer counters are missing. | PARTIAL / NOT TESTED |
| M4 host-to-guest transfer | No verified authenticated network transfer of missing archives or integrated install path. | NOT IMPLEMENTED |
| M5 two-client LAN acceptance | No completed real LAN game with mod round trip documented. | NOT TESTED |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Check the re-triggered integrated CI for signed archive-size TCP serialization; if green, proceed with two-client M2 smoke test.
2. Exercise an installed-but-disabled mod through restart, rejoin and exact restoration with two actual LAN clients.
3. Add explicit first-use trust and license checks before any host archive transfer; keep proprietary game assets excluded.
4. Bind M3 cache reuse and M4 host transfer after host package identity/version validation.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

