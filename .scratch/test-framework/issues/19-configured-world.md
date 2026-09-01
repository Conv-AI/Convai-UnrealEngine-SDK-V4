# 19 — Configured world: a character Blueprint, not a bare actor

Status: `done` — 2026-08-28 on `WebRTC-Video` (`55197a58`), single-character half. The Room half
is issue 09's business and lands on `feat/multi-character` on top of the same fixture.
Depends on: 02, 17

## Why

Every scenario spawned a bare `AActor` with native components. Nothing proved what a game
ships — a Blueprint with `BP_ConvaiChatbotComponent`, a face-sync component, a MetaHuman face
mesh and the plugin's face AnimBP — connects, answers, and moves its mouth. Lip-sync had no
oracle at all; F10 (starvation) was open with no mechanism and no metric.

## What landed

`FConvaiTestFixture::FOptions::ActorClassPath` spawns the Blueprint named by
`-ConvaiTestActorClass=/Game/….BP_X_C` and adopts its chatbot; the player and Virtual Mic stay
on the bare actor. Three scenarios, all `setup_failed` without the flag:

- `configured_text_roundtrip`, `configured_audio_roundtrip` — the existing round trips on the
  Blueprint (speaker name read from `Chatbot->GetName()`, no longer the literal `ConvaiChatbot`).
- `configured_lipsync` — three claims from public surface only: frames applied
  (`OnFacialDataReadyDelegate` → `face_frames_applied`), mesh moved (face mesh
  `CTRL_expressions_jawOpen` curve → `mesh_jaw_max`), settled after speech
  (`mesh_neutral_after_ms`). `face_starvation_max_ms` is reported, not asserted.

Verified live on `/Game/Blueprints/BP_Hana.BP_Hana_C`: 3/3 pass; 647 frames, jaw 0.30, neutral
1.7 s after the last chunk, one real 521 ms starvation window. Controls: `-LipSyncMode=Off` fails
with `no-face-frames-applied-during-speech`; `-ConvaiFaceSyncSimulateFreeze=1` moves
`face_starvation_max_ms` to 3001.

Found on the way: scenarios that left the Virtual Mic off were streaming the host microphone
through the player — room speech showed up as player transcripts. The fixture now always
registers a Virtual Mic; scenarios that never speak never start it.

## Out of scope

- Packet receipt: blendshape packets stay out of the packet log and arrive on the transport
  thread with no public event. The first applied frame is the earliest evidence a game gets.
- Neutral via `ConvaiGetFaceBlendshapes()`: it keeps the last frame by design; only the anim
  node fades. Neutral is read from the mesh curve.
- A starvation threshold. Rates first (`--repeat 10`), then a finding.
- Clone pair, explicit partner switch, gaze arm — `feat/multi-character`, see
  `Docs/ConvaiEndToEndReleaseTesting.md` §7.
