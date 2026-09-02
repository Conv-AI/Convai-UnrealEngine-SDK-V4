# 02 — Walking skeleton: module, project, runner, report

Status: `ready-for-agent`
Depends on: 01

## Goal

The whole pipe, end to end, carrying exactly one scenario. Breadth comes later.

## Scope

**`ConvaiTests` module.** New module in `Source/`, registered in `ConvAI.uplugin`. Depends on
`Convai`. Uses only `CONVAI_API` surface — no `#if`, no test-only branches in the shipping
module (ADR-0005).

Port the shape of `convai-livekit-cpp-p`'s `tests/harness`, which is proven and already
matches this design:

- `Scenario` base with `name()`, `deadline()`, `run()`, self-registration by macro
- an event recorder implementing the plugin's delegate surface, with
  `waitFor(predicate, timeout)`, `expectNoneOfKind`, `countOfKind`, `drain`, `snapshot`
- a latency tracker with idempotent, thread-safe named milestones
- a watchdog that aborts at 2x the declared deadline after dumping the recorder

The existing `UConvaiTestHarnessSubsystem` under `Source/Convai/*/Tests` is dead code — it
has never compiled (see issue 12). Take what is useful (the per-run folder layout, the
JSONL trace format) and leave the rest.

**Minimal test project.** Bare map, no content, `ConvaiTests` enabled. Scenarios spawn actors
and components at runtime, so no level content is required.

**Runner.** `UnrealEditor-Cmd.exe <Project> -game -ExecCmds="…"`, exit code plus
`report.json`. The current harness sets no process exit code; this one must.

**Python orchestrator.** Launches, applies the repeat count, merges results, writes history.
It does not assert.

**Report.** Schema aligned with `convai_harness --results-json` so one runner merges both
layers. Per finding: evidence chain, occurrence rate, dedup key, prior-run comparison.
Mechanical attribution only; a separate `hypotheses` array for anything inferred.

**Repetition and history.** N=10 default, per-scenario override. Occurrence rates on every
finding. Runs persisted keyed by git SHA so cycles diff. No silent retry-until-green — a
single failure in 50 runs is a finding.

**One scenario:** the AEC smoke from issue 05's eventual shape, stubbed to whatever is
runnable today. Not text — the first scenario should be on the critical path.

## Out of scope

Virtual Mic, echo injection, invariants, replay, JSON scenarios. All later issues.

## Done when

`python run.py --filter=<name>` launches the editor, runs one scenario ten times, writes
`report.json` with occurrence rates, and exits non-zero on failure. A second run diffs
against the first.
