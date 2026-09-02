# 01 — Audio device spike: prove the master submix renders

Status: `ready-for-agent`
Blocks: everything

## Goal

Establish, on the machine the suite will run on, that `Audio::FMixerDevice` initialises and
the master submix actually renders — so **Reference Audio** capture has something to capture.

Throwaway code. ~30 lines. Delete or fold into issue 03 afterwards.

## Why first

The entire AEC plan assumes `StartRecordingOutput` / `StopRecording` on the master submix
returns non-silent buffers in the configurations the suite will use. If it does not — for
example because a headless run gets a null renderer that skips submix rendering — the machine
requirements for every later issue change. Cheap to settle now, expensive to discover at
issue 05.

## Scope

Drive a character to speak, then confirm non-silent PCM reaches
`FConvaiReferenceAudioThread::FanAudioChunkToClients`, under each configuration:

1. Editor PIE, audio endpoint present
2. `UnrealEditor-Cmd.exe <Project> -game`, audio endpoint present
3. Same, with the audio endpoint disabled in Windows
4. Packaged Development build

For each, record: does the mixer device exist, does `StopRecording` return samples, are they
non-silent, what sample rate and channel count.

Explicitly confirm that runs must not pass `-nosound`, and note anything else that silences
the path.

## Out of scope

Any assertion about AEC, echo, or the microphone. This measures one thing.

## Done when

A short note in this directory records the result per configuration, and states the machine
requirements every later issue must assume. If configuration 3 fails, say so plainly — that
means every test box needs an audio endpoint, real or virtual.
