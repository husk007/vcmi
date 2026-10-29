# VCMI — Hourly Implementation Runner

This is the execution contract for the scheduled ChatGPT task. The actual schedule is managed in ChatGPT Tasks. GitHub Actions is used only for real build/test evidence, not for driving the hourly agent.

## Fixed scope

- Repo: `husk007/vcmi`; branch: `feature/lan-mod-sync-and-web-roadmap`; base `develop` stays unchanged.
- Read fresh at the start: `AGENTS.md`, `ROADMAP.md`, `RUNNER.md`, **the top summary** of `RUNNER_STATUS.md`, relevant source files, and the latest GitHub commits.
- Follow the **M0–M5 critical path** at the top of `ROADMAP.md` (existing L0–L4 are detailed requirements; W0–W4 only after LAN MVP).
- No upstream PRs/issues, GitHub comments, force-push, merge, or changes to `develop`. Never alter another contributor's commit.

## Each hourly run: real feature progress

1. Read the latest branch HEAD and the M0–M5 milestone state; check GitHub Actions for a real run on this branch.
2. If a validation run exists, inspect it. A **real compile error** in changed code is the top-priority fix. If there is no run, check GitHub Actions availability at most once per **changed** status and continue with M1 after recording the blocker. Do not repeatedly run `git ls-remote` after a known DNS failure.
3. Select one focused, **user-visible vertical-slice** subtask from the earliest incomplete M1–M4 milestone. Examples: host manifest in existing `LobbyModsCheck`, capability handshake, guest preview, enabling an already installed mod, relaunch/rejoin, transfer of one trusted archive, local cache hit. Preserve the existing network protocol's fallback for older clients.
4. Inspect exact surrounding code, types, serialization boundaries, and earlier commits. Reuse code already added in `lib/modding/` and `client/`. Do not create a second cache or second LAN session manager.
5. Implement the smallest coherent patch plus its meaningful regression/fixture test. Prefer completing a call chain through host → guest preview → action over adding another isolated helper.
6. Run the actual test/build where available. Otherwise run bounded static verification against exact repository files and report **CODE_ONLY / TESTS_NOT_RUN**. A test double, algorithm copied into a standalone file, or a GitHub blob comparison is not an integrated VCMI build.
7. Save the code/test patch as a real commit on the feature branch, using fresh file SHAs. No redundant status-only commits.
8. After a meaningful feature slice or real compiler fix, request **one** validation run by updating `.github/lan-mod-sync-validation-trigger.txt` with the commit SHA and reason (or use `workflow_dispatch` when available). The workflow only runs when this trigger or its YAML changes. Do not trigger a full build on every small commit.
9. Update `ROADMAP.md` only when its precise acceptance criteria have been proven. Update the compact `RUNNER_STATUS.md` only when the milestone, evidence, blocker, or next step actually changes.
10. Report in Polish: M milestone, functional change and linked commit (or NO COMMIT), real test/CI URL and result (or NOT RUN), blocker, and next **different** executable action.

## Priority and anti-loop policy

- **Priority 1:** Obtain a real repository compilation and `LanModSyncTest.*` result (M0), and fix actual build failures. The scoped workflow is `.github/workflows/lan-mod-sync-validation.yml`.
- **Priority 2:** If CI is unavailable after one check, implement M1 (host capability + manifest + preview). Never keep extending L0 just because `vcmitest` cannot run in the current chat container.
- **Priority 3:** M2 working join with already-installed mods and restoration; then M3 persistent local reuse; then M4 host transfer; then M5 real LAN acceptance; finally browser W0–W4.
- A cache checksum or journal security fix is high priority **only if** there is a demonstrated issue blocking the current slice, a concrete test failure, or a documented security vulnerability. A speculative rare edge case does not outrank M1/M2.
- **Two-strikes rule:** after two runs with the same inaccessible build/checkout dependency, record it and work on a different M1/M2 code task. Do not retry DNS or install the same missing tool hourly.
- **Progress rule:** if three consecutive commits do not complete or substantially advance an M1–M5 call path, next run must attempt a functional integration step rather than more helper hardening. If impossible, report the **exact dependency** and move to another independent functional step.
- Do not split a simple feature into many micro-commits to manufacture hourly activity. If nothing safe is actionable, report NO COMMIT and the precise user action needed.
- Keep `RUNNER_STATUS.md` under ~6,000 characters. Store only one current summary, the most recent 4 meaningful milestones, and one known blocker per unique cause. Historical reports are in Git commit history; do not accumulate one large paragraph per hour.

## Acceptance/evidence labels

- `CODE_ONLY`: committed source/test, but not built in the real project.
- `BUILD_PASS`: actual project builds on a named environment with the run/commit reference.
- `TEST_PASS`: committed tests run inside actual VCMI and pass, with evidence.
- `LAN_PASS`: a genuine host and guest completed the stated workflow; device details and steps recorded.
- `BLOCKED`: missing external capability (e.g., GitHub Actions disabled) with a single exact workaround.

Only check ROADMAP items when the relevant evidence exists. Code-only snippets cannot meet runtime or real-build gates. Do not claim a fully functional auto-sync or WebAssembly build before end-to-end verification.

## Safety requirements that cannot be dropped

- Snapshot the guest's entire personal mod preset, selected preset name, and disabled submod flags **durably before** changing mods. Restore after disconnect, cancellation, crash, and failed join; do not overwrite personal mod versions.
- Persist redistributable host packages in a verified content-addressed cache, keyed by canonical mod identity and SHA-256. A second join with unchanged host contents must transfer **0 bytes**, reusing cached archives or only activating already installed mods.
- Validate host permissions/trust, manifest integrity, extraction paths/size, compatibility, and content hashes; never transfer original commercial Heroes III assets.
- Gate protocol extensions by explicit feature negotiation to avoid breaking clients without LAN sync.
- A browser client later needs a WebSocket gateway to the authoritative native server; it must not distribute proprietary game data.

## CI notes

The upstream-style workflow `.github/workflows/github.yml` does not run on this feature branch. Use the new scoped `lan-mod-sync-validation.yml` for an actual source checkout, CMake compilation and `LanModSyncTest.*`; initial creation should cause a **single** run if Actions is enabled for this fork. It supports manual dispatch and checkpoint-triggered reruns. A successful GitHub write does **not** prove GitHub Actions executed. Check the run list and logs before marking a build PASS. If quota/permissions disable Actions, surface that limitation once and proceed with source-level M1 work until the owner enables CI.

## Final checkpoint

Before advancing to WebAssembly, finish the M0–M5 LAN gates: host manifest, installed-mod join, cache reuse without retransmission, secure transfer, exact post-game restoration, compatibility fallback, and real two-client LAN game.
