# VCMI Hourly Runner — current state

**Last management review:** 2026-10-31, Europe/Warsaw. This is a compact checkpoint, not a per-hour transcript. Prior progress reports remain available in the Git history of this file.

## Configuration

- Repo: `husk007/vcmi`; feature branch: `feature/lan-mod-sync-and-web-roadmap`; base: `develop` (read only).
- Rules: [ROADMAP.md](ROADMAP.md) M0–M5, [RUNNER.md](RUNNER.md), [AGENTS.md](AGENTS.md).
- Scheduled task: VCMI ROADMAP Runner (hourly in ChatGPT Tasks).
- 2026-10-29 baseline: **182 commits** ahead of `develop` at that point; commit count is not feature-completion evidence.
- **Do not append another hourly transcript to this file.** Keep it under ~6 KB and update only after a meaningful milestone or blocker change.

## Evidence and verification

| Area | Current evidence | Status |
| --- | --- | --- |
| M0 real vcmitest + LAN GoogleTests | [CI #41156884574](https://github.com/husk007/vcmi/actions/runs/41156884574) passed on `35d7122`: actual `vcmitest`, `vcmiserver` and `vcmiclient` built, **94/94 LanModSyncTest PASS** including independent-host TCP rejoin. GUI campaign flow remains untested with full proprietary assets. | BUILD_PASS / TEST_PASS (prior code) |
| LAN helper code and tests | Existing `ModSyncPlan`, durable temporary preset/recovery and SHA-256 archive cache passed the 71 committed VCMI tests; these are unit tests, not a real two-client LAN session. | TEST_PASS (unit only) |
| M1 host preview / capability negotiation | Existing opt-in UDP/TCP host manifest, legacy fallback and cache labels passed previous CI. LAN room preview now labels the individual installed-only mod activations/deactivations (pending the next CI), not just their totals. The opt-in host preview now reports whether lobby accepts players and blocks stale joins; CI #40976444839 passed. A rejected automatic post-relaunch join now immediately restores the original preset and exits the stale join UI (new change awaits CI). No archive transfer yet. | CODE_ONLY (new busy gate); TEST_PASS (previous) |
| M2 guest join with installed disabled mods | Required installed-but-disabled mod activation, guest-only deactivation and exact preset rollback passed independent-server tests. Token-free desktop restart after lobby/game/scenario results and completed-campaign epilogue/outro/results now builds and passes committed tests on `35d7122`. The graphical two-client session still needs licensed Heroes III assets. | TEST_PASS (non-GUI); LAN NOT TESTED |
| M3 repeated-session cache reuse | Existing persistent verified SHA-256 cache and host archive manifest are available in read-only preview. New implementation binds the approved automatic rejoin to **both** host mod metadata and advertised archive SHA-256/size; archive replacements require re-approval. No real transfer/activation from archive or zero-byte network counter yet. | CODE_ONLY (new rejoin binding) |
| M4 host-to-guest transfer | Added a separate opt-in 64 KiB archive-chunk packet path in an existing registered lobby session. The host serves only explicitly staged active root mods whose SHA-256 matches its offer. New real-TCP serializer/cache reuse regression awaits CI; **production guest UI has no request/consent/install flow yet**. | CODE_ONLY (transport slice) |
| M5 two-client LAN acceptance | No completed real LAN game with mod round trip documented. | NOT TESTED |
| Browser W0–W4 | Feasibility documentation only, no compiled browser client. | NOT STARTED |

## Specific recurring blocker — resolved as an execution dead end

Older runs repeatedly failed `git ls-remote` in the task's local container because DNS could not resolve `github.com`. This is **not** proof that GitHub or the connected GitHub tool is unavailable. The runner already reads/writes files via the GitHub connector. The scoped GitHub Actions workflow now supplies the full checkout and build path. **Do not redo identical DNS checks in future hourly runs.** If the CI workflow fails, inspect the job's actual logs and fix the concrete error.

The previous `RUNNER_STATUS.md` had grown to roughly 90,000 characters with repeated hourly partial reports. Its historic revisions are recoverable via Git. Further source-only cache edge-case hardening should not preempt host/guest LAN integration without a reproducible failure or direct dependency.

## Next executable tasks

1. Validate SHA-256 host-offer binding in the real VCMI build/test checkpoint, then review the guest's actual opt-in rejoin path.
2. Exercise the GUI epilogue → outro → scores → restored-preset relaunch with legally owned Heroes III data; the public CI fixture cannot establish LAN_PASS.
3. Wire the new registered-session chunk transport into the actual guest preview with first-use consent, exact SHA-256 cache reuse, and safe side-by-side installation; current TCP test guest is only a fixture.

## Evidence/reporting discipline

- `CODE_ONLY`: code/fixture committed, full build/test unavailable.
- `BUILD_PASS`: full project compiled in a named environment, with commit and run URL.
- `TEST_PASS`: committed `LanModSyncTest.*` executed under actual `vcmitest`.
- `LAN_PASS`: actual two-client join/play/restore verified with proof.
- No empty status-only commits. Do not mark M0–M5 or related L0–L4 checkboxes as complete without the matching evidence.

