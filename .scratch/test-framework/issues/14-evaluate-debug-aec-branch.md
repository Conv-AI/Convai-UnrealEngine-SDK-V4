# 14 — Measure the listener tap against the recorder tap

Status: `done` — 2026-08-07, see **F29**. The tap was built as a runtime switch and measured:
it fixes the feed completely (ratio 1.000, no recorder held, contention released) and does not
reduce the leak — 0/10 against 3/10, p = 0.21. Controls fired on both taps. The near-end half was
**not** measured. Two things stay open and are not this issue's: `SetStreamDelay` cannot be tested
until a DLL exports it, and settling a rate this size needs ~35 runs per arm.
Depends on: 05, 13

**Rewritten 2026-08-07.** The original text asked for ten runs on `feat/test-framework` and ten on
`origin/debug/aec`. That comparison cannot be run — see **F28**, and read it before this. The
question survives; the method does not.

## Why the branch cannot be the experiment

`origin/debug/aec` is one commit on `8744fdb5`, which is its merge base with this branch.
Everything that makes it incompatible landed on the main line afterwards:

- It constructs `FConvaiSubmixReferenceListener(Self->ConvaiClient.Get())` and keeps one listener
  per subsystem. `UConvaiSubsystem::ConvaiClient` no longer exists; each **Connection**'s client
  belongs to its **Session Proxy**
  ([ConvaiConnectionSessionProxy.h:254](../../Source/Convai/Public/ConvaiConnectionSessionProxy.h#L254)).
  Adopting the diff would feed the first **Connection** and silently starve every later one — and
  the suite's own map runs two, measured at `feed_client_sends / feed_chunks_sent` ≈ 1.99.
- It calls `ConnectionParams.Client->SetStreamDelay(...)`. The shipped `convai_client_dll.lib`
  exports 15 `ConvaiClient` methods and that is not one of them.

## What replaces it

**A runtime switch, not a branch.** Add the `ISubmixBufferListener` tap inside
`FConvaiReferenceAudioThread`, selected by a custom param, reusing the existing client registry
and `FanAudioChunkToClients` unchanged. Default stays the shipping recorder path.

This is a better experiment than the one it replaces: one binary, one build, one parameter
different. A branch checkout would have varied the compiler input as well as the tap, and F28's
`Convai.Build.cs` change (`bUsePrecompiled` commented out) means the two sides would not even
have been built the same way.

Keep from the branch what was right: whole 480-sample frames only, carrying the sub-frame
remainder between callbacks, and its rationale — the submix callback fires as the device consumes
audio, so the reference shares the timeline of the echo entering the microphone.

Do **not** port `SetStreamDelay`. It cannot link, and F5 measured it at −0.1 to +2.3 dB on a
stable delay, so it is not what this issue is asking about.

## The comparison

Ten `aec_echo_only_internal` runs each way, plus `aec_echo_only_none` on both to confirm the
control still fires. Report leak rate with its denominator, and issue 13's cadence metrics
alongside — the listener should move `feed_capture_ratio` to 1.0 and collapse
`feed_recorder_off_*` to nothing, and if it does not, the tap is not doing what it claims.

## What this can no longer claim

F27 measured F3, F4 and F15 directly and none of them is the mechanism behind the runs that leak.
The original issue's motivation — "this one change removes three of the four F26 suspects" — is
therefore a claim about suspects that have already been cleared. This comparison is now a control
on a weakened hypothesis, cheap enough to be worth running and not a candidate fix.

## The trap this issue still exists to avoid

Do not measure the echo leak alone and declare victory. F12 and F26 are different symptoms in
different repositories, and a gapless reference makes the far end *more* consistently active,
which is the condition under which F12's near-end suppression is worst. Issue 05's double-talk
variant is the scenario that would catch the trade and it is not built. Until it is, say in those
words that only half the question was measured.

## Done when

Leak rate is reported both ways with its sample size, the control is confirmed still firing on
both, the cadence metrics show the tap actually changed, and the near-end question is recorded as
unmeasured in those words.

## Out of scope

Making the listener the default. That is a human decision with an ADR attached — the one F13
notes was never written, and whose absence F28 makes concrete: `docs/adr/0001-aec-reference-source.md`
is cited by the branch's own header and exists nowhere.
