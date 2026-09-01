# 01 — Audio device spike: result

Run 2026-08-06 on the maintainer's machine (Windows 11 Pro 26200, UE 5.8, WASAPI, 48 kHz
stereo output). Probe: `convai.test.SubmixProbe` in the `ConvaiTests` module.

## Verdict on V1

**The master submix renders.** Non-silent PCM is available in a headless `-game` run with an
audio endpoint present, at 48 kHz stereo, which is what **Reference Audio** capture needs.

**But `StartRecordingOutput` / `StopRecording` is not a dependable way to get it.** See F15 —
that is the substantive finding of this issue and it outweighs the yes/no.

## Per configuration

| # | Configuration | Result |
|---|---|---|
| 1 | Editor PIE, endpoint present | **not run** — PIE is not driven headlessly yet. The editor world *without* PIE was run and is not a usable configuration: zero active sources, nothing renders |
| 2 | `UnrealEditor-Cmd.exe <project> -game`, endpoint present | **renders.** 5 repeats, listener saw ~275,000 samples every run, peak 0.70–0.90 |
| 3 | Same, Windows audio endpoint disabled | **not run** — needs the machine's audio hardware disabled, which was not done without asking |
| 4 | Packaged Development build | **not run** |

Configurations 1, 3 and 4 are open. Configuration 3 is the one that decides whether every
test box needs an audio endpoint, and it is still unanswered.

## Measurements, configuration 2

Command:

```
UnrealEditor-Cmd.exe <project> /Game/Maps/Landing -game -unattended -nosplash -stdout
    "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0"
    -ExecCmds="convai.test.SubmixProbe 3 quit"
```

Three independent measurements per run — the recorder with a null submix argument (what the
plugin passes), the recorder with an explicit main-submix argument, and an
`ISubmixBufferListener` on the main submix:

| Run | Recorder, null arg | Recorder, explicit arg | Listener |
|-----|-----|-----|-----|
| 1 | 270,848 | 270,848 | 274,944 |
| 2 | 270,848 | 270,848 | 275,456 |
| 3 | **1,024** | **1,024** | 275,968 |
| 4 | **1,024** | **1,024** | 275,456 |
| 5 | 271,360 | 271,360 | 274,432 |

The null and explicit arguments agree exactly in every run, so passing `nullptr` for the
submix — which `FConvaiReferenceAudioThread` does — resolves to the main submix correctly.
That hypothesis is refuted.

## Machine requirements every later issue must assume

- **An audio endpoint must be present.** With `-nosound` the probe reports `mixer_device=no`:
  there is no `Audio::FMixerDevice` at all, so the entire **Reference Audio** path is absent
  rather than silent. The PRD's rule is confirmed mechanically.
- **Pin `[Audio] UnfocusedVolumeMultiplier=1.0`.** `FApp::GetVolumeMultiplier()` was observed
  at `0.0` in headless runs. At zero the mixer still renders buffers and the submix still
  fires listeners — it renders *silence*. Every AEC assertion would pass on zeros. The value
  depends on window focus and therefore varies between otherwise identical runs, so it must be
  pinned rather than hoped for.
- **A run must assert non-silence, not just non-emptiness.** The two failure modes are
  distinct and were both observed: zero samples returned, and the right number of samples all
  of which are zero. The probe reports peak and RMS separately from sample count for this
  reason.
- **The master submix auto-disables when nothing is playing.** `FMixerSubmix::ProcessAudio`
  early-returns at `AudioMixerSubmix.cpp:1410` *before* the recording append at `:1686`, so a
  silent game yields **no recorded samples at all** rather than silence. Note the disabled path
  still calls `SendAudioToSubmixBufferListeners` at `:1447` with a zeroed buffer — so a
  listener sees silent buffers where the recorder sees nothing. Any test that measures the
  reference feed must keep audio playing or it is measuring the auto-disable.

## Deviation from the issue as written

The issue says to drive a character to speak. That needs a backend and credentials, which the
headless configurations exist to run without. The probe plays `/Engine/EngineSounds/WhiteNoise`
instead, which answers the same question — does the master submix render — using nothing but
the engine. Whether **Reference Audio** reaches `FanAudioChunkToClients` is issue 03's
measurement.

## Disposition

The probe is **not** thrown away. Its `ISubmixBufferListener` is the ground-truth tap issue 03
requires, and the recorder-versus-listener comparison is the mechanism that produced F15. It
folds into issue 03 as the issue anticipated.
