# 04 — Virtual Mic

Status: `ready-for-agent`
Depends on: 02

## Goal

Play pre-recorded audio into the plugin exactly where a human's voice arrives, so
resampling, mute, streaming state and **Talk Target** fan-out all run unchanged.

## Scope

`UConvaiVirtualMicComponent : UConvaiAudioCaptureComponent` in the `ConvaiTests` module,
overriding `USynthComponent::OnGenerateAudio` to emit scripted samples instead of device
audio.

No change to the `Convai` module. The existing discovery path picks it up:
`FindFirstAudioCaptureComponent` → `GetOwner()->GetComponentsByInterface(UConvaiAudioCaptureInterface)`
([ConvaiPlayerComponent.cpp:467-495](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L467-L495)),
and `ReadRecordedBuffer` drains the same submix
([:530-546](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L530-L546)).

Verify at run time that the player component actually adopted the virtual component rather
than the real capture — the log line is *"Set alternative audio capture component"*. A test
that silently ran against a real, silent microphone is worse than no test.

**Step library.** First entries, callable from C++ scenarios:

- `SpeakWav(path, {gain, start_delay})` — stream a WAV at real-time pace
- `Silence(duration)`
- `SetTalkTargets({proxies})`
- `SayText(text)`

WAV fixtures: reuse `convai-livekit-cpp-p/tests/data/*.wav`, which already carries short,
medium and long utterances plus a transcript reference in `STT.json`.

**Assertion.** Audio survives the path: the bytes reaching `SendAudioToTalkTargets` match the
source WAV, resampled, within tolerance — and reach every **Talk Target**, not just the
**Primary Target**.

## Out of scope

Echo injection (issue 05). Multi-character routing scenarios (issue 09) — this issue only
proves the seam carries audio to N targets.

## Done when

A scenario speaks a WAV through the Virtual Mic, the plugin's own resample-and-send path
carries it, and a live character transcribes it recognisably. Word error rate against
`STT.json` is reported, not asserted.
