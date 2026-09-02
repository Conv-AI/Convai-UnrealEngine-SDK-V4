# 06 — ERLE test in the DLL repo

Status: `ready-for-agent`
Repo: `convai-livekit-cpp-p`
Depends on: nothing — can run in parallel from day one

## Goal

Measure echo cancellation as a number, offline, so a regression is visible before it becomes
a symptom — and so algorithm failures can be told apart from plumbing failures.

## Why this is cheap

`IAudioEchoCanceller::ProcessMicrophoneStream(int16_t* audio_data, …)` cancels **in place**
— `src/convai/audio/i_audio_echo_canceller.h:41` in the DLL repo. The cancelled signal is
already in the caller's buffer, so no new API is needed. No network, no engine, milliseconds
per case.

## Scope

A GTest case alongside `tests/smoke_test.cpp`, driving `InternalAEC` and `ExternalAEC`
directly:

1. load a far-end WAV from `tests/data/`
2. synthesise near-end: `echo(far, delay, gain)` plus optionally clean player speech and noise
3. feed `ProcessReferenceStream(far)` then `ProcessMicrophoneStream(mic)` in 10 ms frames
4. read the buffer back

Report and assert:

- **ERLE** in dB over the echo-only region
- **near-end preservation**: correlation against the clean player signal during double-talk
- **convergence time**: frames until ERLE crosses a threshold
- sweeps over delay (0, 40, 120, 300 ms), gain, and soft-clip nonlinearity
- with and without `SetStreamDelay`, since the plugin never calls it — F5

Emit the numbers as structured metrics, not just pass/fail, so trends are visible across
runs.

## Bisection value

If ERLE is healthy offline, every reported symptom is plugin plumbing and issues 03 and 05
are correctly aimed. If ERLE is poor offline, no amount of Unreal testing would have found
it. Run this early for that reason alone.

## Done when

`ctest` runs the ERLE suite, the numbers land in the results JSON, and the delay sweep says
whether the missing `SetStreamDelay` call measurably matters.
