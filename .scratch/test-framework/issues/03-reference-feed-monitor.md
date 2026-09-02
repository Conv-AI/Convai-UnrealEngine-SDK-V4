# 03 — Reference-feed monitor

Status: `ready-for-agent`
Depends on: 02
Targets: [FINDINGS.md](../FINDINGS.md) F1, F2, F3, F4

## Goal

Make **Reference Audio** observable, then assert it is correct. No backend, no microphone, no
echo cancellation involved.

## Why this is the highest-value issue

Every confirmed suspect in FINDINGS.md lives here, and the reference path currently emits
nothing between start and stop (F2) — a **Connection** running with no far-end signal looks
identical to a healthy one. This is the fastest path to a true finding, plausibly before the
rest of the framework is finished.

## Scope

**Observation.** Capture what `FConvaiReferenceAudioThread::FanAudioChunkToClients` actually
sends, with timestamps, per client. Prefer polling public state — `NumClients()` is already
public — over new hooks. Where a hook is genuinely required, justify it on its own merits per
ADR-0005 and note it here.

**Ground truth.** An independent `ISubmixBufferListener` on the master submix, so the fed
reference can be compared against what the mixer actually rendered. Independence matters:
sharing the reference thread's own tap would make a dead reference path look like silence
rather than a failure.

**Assertions.**

- attached reference clients equals live **Connection** count, sampled per second — F1
- chunks arrive at the expected rate; measure and bound the gap between `StopRecording` and
  the next `StartRecordingOutput`, expressed as fraction of rendered audio missing — F3
- inter-chunk delay variance stays within a stated bound under load — F4
- sample rate 48 kHz, mono, chunk size 480 samples
- reference content correlates with submix ground truth above a stated threshold, at a
  measured offset — this is the assertion that catches a punctured or misaligned feed
- attach and detach balance to zero at end of run

**Load variants.** Run the rate and variance assertions at idle and under frame-time
pressure, since F4 predicts degradation only under load.

**Cost of the capture loop — F11.** Measure allocation volume and audio-thread time across
`StartRecordingOutput(World, 60.0f, nullptr)`, called roughly every 10 ms. Compare against
the same loop with the duration sized to one chunk. If the 60-second reserve allocates per
call, it is a one-line cause with a blast radius covering F3, F4 and F10.

**Microphone-in-reference regression guard — F7.** With no character speaking, have the
**Virtual Mic** emit a distinctive tone and assert it does *not* appear in the reference feed.

F7 is resolved — `AudioInput`'s parent is `MuteMic`, so the microphone is muted out of the
master mix — but that routing lives in a `.uasset` no code references by structure. Reparenting
`AudioInput` would put the player's voice into **Reference Audio** with no compile error and no
log line. This assertion is the only thing that would catch it. Needs issue 04.

**Reproduce F1.** Drive connect and immediate teardown, the shape that produced the
unmatched connection in the log, and determine whether it is a teardown race or a general
failure. Report which.

## Out of scope

Fixing anything. This issue makes failures visible and mechanically attributable; fixes are
the agent loop's job.

## Done when

The monitor runs inside any scenario, emits per-second reference-feed metrics into the
report, and produces a finding with an evidence chain when any assertion above fails. F1 is
either reproduced or shown to be a teardown race, and F3's gap is quantified rather than
suspected.
