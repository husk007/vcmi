# Hourly VCMI Roadmap Runner

This file is the execution contract for the scheduled ChatGPT task working on this fork. The schedule is configured in ChatGPT Tasks, **not** GitHub Actions. Keeping this file in GitHub ensures that every new run reads the same rules even after the chat context changes.

## Target

- Repository: `husk007/vcmi` — https://github.com/husk007/vcmi
- Working branch: `feature/lan-mod-sync-and-web-roadmap`
- Base branch: `develop` (read-only for the runner)
- Authoritative plan: `ROADMAP.md`
- Coding policy: `AGENTS.md` and `CONTRIBUTING.md`
- Technical background: `docs/developers/Lan_Mod_Synchronization.md` and `docs/developers/WebAssembly_Port.md`
- Execution status: `RUNNER_STATUS.md`
- Frequency: once every hour, configured externally in ChatGPT Tasks.

## Per-run algorithm

1. Fetch fresh files and HEAD for the working branch from GitHub. Read `AGENTS.md`, this file, `ROADMAP.md`, and `RUNNER_STATUS.md`. Never rely on a previous run's in-memory state.
2. Review the current diff against `develop`; do not overwrite another contributor's updates. If HEAD changes during an edit, re-fetch and reconcile instead of force-pushing.
3. Select the **earliest actionable unchecked acceptance criterion** in L0-L4. Break a large checkbox into one independently verifiable subtask. Do not move to W0-W4 until LAN work is complete or genuinely blocked; record the reason when choosing another independent item.
4. Inspect the actual code and dependencies. Prefer the existing mod verification, `LobbyModsCheck`, `ModManager`, and launcher extraction code. Avoid parallel implementations.
5. Implement a focused code or test change on the working branch. Include coverage for visible new behavior. Do not merely paraphrase the plan or repeatedly generate another plan.
6. Validate what can actually be validated: compile/unit tests if an execution environment and dependencies are available; otherwise perform source/API inspection and record **not run**. Never invent successful CI or runtime testing.
7. Commit successful, coherent changes directly to the working branch using GitHub write tools. Do not use a PR as the only persistence mechanism. Do not create/merge PRs, create issues, post GitHub comments, update `develop`, or write to the upstream `vcmi/vcmi` repository.
8. Update `ROADMAP.md` only for genuinely completed and checked criteria. If implementation is committed but tests/integration are pending, keep the checkbox open and describe the verified progress in `RUNNER_STATUS.md`.
9. Record the newest meaningful result in `RUNNER_STATUS.md`, linking the exact commit. Report in Polish: selected criterion, files/commit, tests performed and results, still-missing work, blockers, and next criterion.
10. If blocked, do not loop forever on the same failed attempt. Record a precise blocker and try another independent task in the **same milestone**. Re-check blockers on future runs when conditions change. Do not label unfinished work as DONE.

## Definition of done

A roadmap checkbox may be marked `[x]` only when:

- Implementation is saved in the repository and the changed code has been checked for correctness.
- Applicable automated tests have passed. If execution is unavailable, prefer `PARTIAL` with tests `NOT RUN`, unless the task explicitly concerns only documentation.
- For user-facing functionality, the relevant integration or real-world test is complete (e.g., an actual two-client LAN session).
- Existing non-opt-in clients and original player configuration remain safe.

`PARTIAL` means concrete, committed progress but some acceptance criteria remain open. `BLOCKED` means an actionable external restriction, not a reason to rewrite unrelated code. `DONE` must not be used for an unbuilt feature.

## Hard feature requirements

- Joiner's active preset, disabled submods, selected original preset, and privately installed mod versions survive a LAN match and restore exactly on disconnect, host failure, cancellation, or crash recovery.
- Persist the rollback journal before any modification. Do not mark session restoration complete until recovery has been validated.
- Host mod archives persist in a local content-addressed cache. Compare SHA-256 against an authenticated host manifest. Matching cached packages must NOT be retransferred; existing matching installed mods require activation only. Changed packages transfer selectively.
- Host content is untrusted. Require first-use trust, verify integrity, constrain extraction, forbid arbitrary paths, and transmit only legally redistributable mods. Never transfer the commercial Heroes III data.
- Keep the feature behind capability negotiation; do not break unmodified VCMI clients. Do not start multiplayer until mod sets match.
- The WebAssembly port must not bundle copyrighted game files. Prefer a browser client with a native authoritative server bridged through WebSocket, and preserve desktop builds.

## Output and failure behavior

At the end of each run, communicate one compact Polish status with links to the branch and exact commit if any. If no commit was made, say so explicitly and why. Do not silently imply progress.

If GitHub write permissions, tools, build dependencies, or credential access are unavailable, do not fabricate work. Record the precise limitation when possible, and deliver a bounded patch or analysis in the current run. Do not attempt to use unapproved tokens or ask for secrets in code.

Do not edit this execution policy as a workaround for failed tests or to claim a stage complete.
