# 05 — Injected Echo and the paired AEC scenario

Status: `ready-for-agent`
Depends on: 03, 04

## Session 4 status

**Injected Echo, the fixture self-assertion and the paired run are built and have run.** The
double-talk variant is built (`aec_double_talk`) and the N > 1 variant is not.

Three things changed the shape of this issue and are recorded here so the next reader does not
re-derive them:

- **"Zero player transcripts" is the wrong assertion to lean on, and now has company.** It
  routes through a live LLM and a remote recogniser and has read 1/5, 2/9, 0/10, 3/10 and 4/5
  across five sweeps of code that did not change between the last two. `aec_erle_internal` /
  `aec_erle_disabled` are the same scenario with the APM's other stages off, where the control
  reads exactly 0.000 dB and the assertion is on the canceller's own attenuation. See F32.
- **The microphone was carrying the room.** Every AEC run before 2026-08-07 was graded on a
  signal that included the host's real microphone (F31). The scenario now fails any run where
  the canceller received more than 6 dB above what the Virtual Mic emitted.
- **The `None` arm was never a control for anything but the transcript.** It leaves the client
  with no canceller, so every AEC counter reads zero. `aec_echo_only_disabled` (AEC=0,
  AECType=Internal) is the arm where the canceller exists and cannot cancel.

## Goal

Make echo cancellation observable end to end in Unreal, with a negative control that proves
the test can fail.

## Scope

**Injected Echo.** An independent `ISubmixBufferListener` on the master submix, delayed by
`D` and attenuated by `g`, mixed into the **Virtual Mic** feed.

Independence from the reference thread's tap is the point: if the reference path dies, the
echo keeps flowing, the microphone stays dirty, and the test fails loudly. Sharing a tap
would make both go silent together and read as a pass.

**Fixture self-assertion.** Assert the injected echo was non-silent above a threshold for the
expected duration. Without this, "no audio device" is indistinguishable from "cancellation
worked".

**Knobs**, so the failure boundary can be swept rather than guessed: delay (0, 40, 120,
300 ms), gain, optional soft-clip nonlinearity, optional additive noise.

**Paired run.** The same scenario twice, `AECType=Internal` then `AECType=None`, set through
`UConvaiUtils::SetCustomParam` before connect — it is read at connect time
([ConvaiSubsystem.cpp:417](../../Source/Convai/Private/ConvaiSubsystem.cpp#L417)), so the
value applies per **Connection**.

Assertions during an echo-only window, with no player speech:

- zero player transcripts, and no `final-user-transcription`
- no false barge-in: `OnActiveSpeakerChanged` does not flip, character speech is not cut
- **the `None` run must violate both.** If it does not, the finding is "fixture broken", not
  "AEC works" — report it that way

**Double-talk variant.** Player speech plus echo simultaneously: assert the player's
transcript still arrives and resembles the source WAV. This is the case that catches
cancellation destroying the near-end talker, which is one of the reported symptoms and is
what F7 predicts if `AudioInput` routes to master.

**N > 1 variant.** Two **Connections**, echo containing both characters' voices. Per
CONTEXT.md, one **Reference Audio** capture serves every **Connection** precisely so each can
subtract the others. Assert neither character reacts to the other's voice through the
player's microphone.

## Out of scope

Measuring how *well* cancellation works. ERLE is issue 06, in the DLL repo, where the
cancelled signal is readable.

## Done when

The paired scenario runs ten times, the `Internal` run passes, the `None` run fails, and both
outcomes appear in the report with occurrence rates. The double-talk and N > 1 variants run
and report.
