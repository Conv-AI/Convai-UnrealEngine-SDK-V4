# 17 — One command over both tiers, and a launch that survives the last one

Status: `done` — 2026-08-07, session 5. `run.py --tier {engine,dll,both}` (default `both`) runs
the DLL suite by incremental `cmake --build --target aec_erle_test` then the exe directly with
`--gtest_output=json` — direct because gtest_discover_tests registers per-case, so JSON through
ctest is 22 processes overwriting one file. One merged report: `tiers` block, skipped counted
separately, requested-tier-not-run fails the exit code. Orphan sweep at launch scoped by
.uproject path in the command line AND a dead parent; verified against two planted
UnrealEditor-Cmd.exe orphans — killed the one on this project, spared the ConvaiAssemblyStudio
one.
Depends on: 16

*2026-08-28: a third tier, `unit` — the `Convai` module's own automation tests, one editor
launch of `Automation RunTests Convai.` — runs under the same runner and the same rule:
`tiers.unit` in the one `merged.json`, not-run is never a pass, and a failed test or a requested
tier that did not run fails the one exit code. `--tier all` is the default now; `both` still
means engine + dll for the callers that spell it.*

## Why

The loop needs both repositories and they have different jobs — F33 is the proof: the offline
suite caught a seeded canceller defect that the in-engine tier missed entirely. An agent
therefore has to run `ctest` in `convai-livekit-cpp-p` *and* `run.py` here, and reconcile two
report formats itself. The PRD said the schemas would merge and they never did.

## Scope

**One entry point, one merged report, one exit code.** Whatever drives it — extending `run.py`
or a thin wrapper above it — the output is a single artifact carrying both tiers, each labelled
with what it covered. `aec_erle_test` already emits `--gtest_output=json`, and the merged
schema already carries dedup keys, occurrence rates and the rate-verdict machinery from
`bc71584b`.

A tier that was not run is reported as *not run*, never as passing. That is the same rule the
report already applies to a scenario whose launch produced nothing.

**Kill orphans before launching, not after.** `run.py` kills its own child once the report
lands (F25), and that is not enough: when the orchestrator itself dies the editor survives, and
the next build fails with `LNK1104` on `UnrealEditor-ConvaiTests.dll` or with
`Unable to build while Live Coding is active`. Neither message appears as an error line, so a
filtered build log shows only an unexplained `OtherCompilationError`. This cost time in session
2 and will cost an unattended loop far more.

Sweep at launch, scoped by `ParentProcessId` and project path to this suite's own processes.
**The maintainer runs their own `uvicorn` and VSCode Python processes and an editor on
`ConvaiAssemblyStudio`** — killing broadly would take those out.

## Done when

One command produces one report covering both tiers with an honest exit code, and a run whose
predecessor was killed mid-sweep starts cleanly without human intervention.

## Out of scope

CI. The PRD's non-goals still hold: runs are triggered manually and feed a fix agent, nothing
gates. `convai-livekit-cpp-p` already runs ctest on push to `staging-v2` and that is its own
arrangement.
