# 18 — Stop the fix agent from editing the oracle

Status: `done` — 2026-08-07, session 5. `run.py` fingerprints the named constants (as source
text), the `kArms` table, sha1s of `Data/*` and of the DLL's `tests/aec_erle_test.cpp`, stores
the fingerprint in every merged report, and diffs it against the previous one. A change prints
`ORACLE CHANGED <name>: <was> -> <now>` and lands in `oracle_changed`, whatever the outcomes; a
renamed constant or deleted file reads `NOT FOUND` / `FILE MISSING` rather than as silence.
Verified: moved `MinWindowErleDb` 1.0 → 0.5, next run reported it; reverted, the run after
reported the move back. The edit is not prevented, per the out-of-scope clause.
Depends on: 16

## Why

The PRD is explicit: *"The cheapest path to green is weakening an assertion. Assertions,
invariants and recorded fixtures are out of scope for the fix agent; changing them is a separate
pass with a human in the loop."* Nothing enforces it. An agent told to make the AEC tests pass
can pass them by moving a constant, and the report will look identical to a real fix.

This is not hypothetical and the evidence is in this repository's own history. Session 2's
fixture self-check was written as `EchoPeak <= 0.0f`, which only rejects exactly zero — so a run
carrying 8.9e-06 of echo, meaning none at all, passed and reported 126 leaks that were F24
hallucinations on silence. That was an accident by an agent trying to be careful. An agent
optimising for green will do it deliberately and faster.

## Scope

**Name the constants that are the oracle.** They are few and they are known:

- `MinWindowErleDb` and `ControlErleToleranceDb` (`ConvaiAecEchoOnlyScenario.cpp`) — F32's
  attenuation floor and the tolerance on a control that must read 0.000 dB
- `MinAudibleEchoPeak` — the fixture floor that F33's predecessor slipped past
- `MaxAttenuationDb` (`ConvaiMicNotInReferenceScenario.cpp`) — F18's replacement oracle
- the arm table in `kArms`, particularly `bExpectLeak` and `bAssertAttenuation`
- `tests/aec_erle_test.cpp`'s thresholds in `convai-livekit-cpp-p`, including the skipped F12
  criterion, whose assertion is unchanged and is the acceptance test for that fix

**Detect a change to any of them and report it as its own condition**, distinct from a test
outcome. A diff check in the runner is enough; this does not need policy machinery. The point is
that "the suite went green" and "the suite went green because a threshold moved" must not be the
same observation.

**Cover the fixtures too.** `Source/ConvaiTests/Data/*.wav` and `STT.json` are the recorded
inputs the PRD names alongside assertions.

## Done when

A run whose oracle constants or fixtures differ from the previous run says so in the report,
loudly, whatever the test outcomes were.

## Out of scope

Preventing the edit. A human changing a threshold with a reason is the separate pass the PRD
describes, and this issue must not obstruct it — only make it visible.
