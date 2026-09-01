# 01 — Push-to-talk holds the mic open but the server VAD still ends the turn

Status: ready-for-human
Reported: 2026-08-24, user bug report (Manual/GitHub install, UE 5.5, plugin 4.0.0-beta.27, Blueprint + C++)
Log: `Context/convai push to talk log.txt`
Fixed by: `fix/push-to-talk-turn-end` (`c241313a`) — needs a live-server run before it can be closed

## Symptom

> pressing push to talk does not stop the npc from responding, if the user takes the shortest break
> or breath it will start responding interrupting the user … It also seems like editing the vad
> settings does nothing.

## What the log shows

One connection, one `Started Streaming Audio` (line 605, at connect — not at a press), one
`Stopped Streaming Audio` (line 1701, at the release). Between them the reporter held the button
and spoke; the character answered five times.

| Log line | Evidence |
|---|---|
| 605 | `Started Streaming Audio` fires at connect, before any press |
| 986, 1664 | `Warning: UnmuteStreamingAudio: already streaming!` — the presses were no-ops |
| 1329 → 1338 | final `" Stop."` at `01:50:11.901`, next speech `" Ke"` at `01:50:12.715` — **0.81 s** of silence |
| 1360 → 1372 | that gap produced `user-stopped-speaking` → `bot-llm-started`, mid-hold |
| — | no `VAD overrides — confidence: …` line anywhere: no `vad_params` was sent at `/connect` |

## Three defects behind it

1. **The session opens the microphone.** `UConvaiPlayerComponent::StartSession()` called
   `UnmuteStreamingAudio()` unconditionally. With `bAutoInitializeSession` on (the default) the
   stream is live from connect, the first press hits "already streaming!", and the first release
   is the first thing push-to-talk actually does — inverted for one cycle, and a hot mic before it.
2. **Release never ends the turn.** `MuteStreamingAudio()` sent only `stt-toggle {muted:true}`.
   The RTVI contract's `force-user-stopped-speaking` (`Context/rtvi.md:640`) — the message that
   exists for exactly this — was never sent, and had no constant in `ConvaiConstants::WebRTC::MessageType`.
   So the end of a turn was always the server VAD's silence timer, measured at ~0.8 s here.
3. **The VAD knobs cannot reach that timer.** `FConvaiVADSettings::bUseServerDefault` defaults to
   `true` and gates all four fields, so `GetConnectionParams` leaves them at `-1.0f` and the
   subsystem omits `vad_params` entirely. They are also read once, at `/connect`, so a runtime
   edit changes nothing until reconnect. Both are silent.

## The fix

`Enable Push To Talk` — the variable `BP_ConvaiPlayerComponent` has always had — makes the three
agree. No new switch and no rewiring:

- The capture device opens exactly as it does for an open mic. Push-to-talk gates *delivery*, via
  the existing `bMute` soft-mute, so the mic test, the input levels and the AEC reference feed all
  keep working — `UnmuteStreamingAudio()` just sends `stt-toggle {muted:true}` and sets `bMute`.
- `bMute` gains a `BlueprintSetter`. The Blueprint's press and release already write it, so the
  release is where `force-user-stopped-speaking` goes, before `stt-toggle {muted:true}` and in that
  order, so the turn ends on audio the server already has. Only the setter moves the gate:
  `MuteStreamingAudio()` still means "close the stream", and the mic test still writes `bMute`
  directly, because a local recording is not the player yielding a turn.
- `UConvaiPlayerComponent::IsPushToTalkEnabled()` finds the switch with
  `FindFProperty<FBoolProperty>(GetClass(), "EnablePushToTalk")` — the name the editor gave the
  Blueprint variable, with no `b` prefix. A native property of that name
  would shadow the Blueprint one and split the behaviour across two checkboxes — which is exactly
  what the first attempt did.
- `/connect` carries `vad_params.stop_secs = PushToTalkStopSecs` (default 3600 s), overriding an
  explicit `StopSecs` — the two settings disagree about who ends the turn and the button is the one
  the player can see. The connection belongs to a character, not a player, so the subsystem resolves
  the timer across `RegisteredPlayerComponents` via `UConvaiPlayerComponent::ResolvePushToTalkStopSecs`
  rather than from whichever proxy happens to be connecting.

Push-to-talk is a per-player input policy, so it reads from the component the input already talks to —
not from project settings next to the VAD knobs it overrides.

Barge-in is unaffected: `start_secs` is untouched, so speaking during a bot reply still fires
`user-started-speaking` and interrupts.

## Verified

- `Dev_WebRTCEditor Win64 Development` builds clean.
- `Convai.PushToTalk.ResolvesVadStopSecs` and `Convai.PushToTalk.ConnectParamsKeepServerVadDefault`
  pass; full `Convai` automation filter 103/103.
- **Not verified:** that the server honors a 3600 s `stop_secs`. Nothing in `Context/rtvi.md`
  documents `vad_params` bounds, and no live session was run. If the pipeline clamps it, defect 3
  needs a server-side answer — a manual/push-to-talk turn mode — and defects 1 and 2 still stand
  on their own.

## Comments

- 2026-08-25: **Regression caught in PIE, fixed.** The first cut had `StartSession()` skip
  `UnmuteStreamingAudio()` when push-to-talk was on, so the button owned the capture device. Enabling
  the checkbox in a project with no button bound killed the microphone outright — `Dev_WebRTC.log`
  09:51 and 09:53 sessions show `Push To Talk is on - microphone stays closed…` with no
  `Started Streaming Audio` and no `Started default audio capture` after it; the only input that
  reached the character in either session was a typed `user_text_message`. Nothing in
  `Dev_WebRTC/Content` references `UnmuteStreamingAudio`, so no wiring existed to save it.
  Push-to-talk now gates delivery through `bMute` and leaves the device alone.
- 2026-08-25: **Second regression, same root: a duplicated switch.** The fix added a native
  `bEnablePushToTalk` and two new press/release nodes, but `BP_ConvaiPlayerComponent` has declared
  `EnablePushToTalk` and its own `StartPushToTalk`/`StopPushToTalk` all along, and they hold the
  button by writing `bMute`. The native property shadowed the Blueprint one, so ticking either
  checkbox got half the behaviour: the Blueprint one ran the press and release with no end-of-turn
  signal, the native one sent the signals with nothing ever unmuting. The native property and the
  two nodes are gone; the setter on `bMute` carries the behaviour and `IsPushToTalkEnabled()` reads
  the Blueprint switch by name.

- 2026-08-24: The reporter's other complaint — "the vad could be completely disabled/skipped" —
  is what `PushToTalkStopSecs` approximates. A real "no server VAD" mode would have to come from
  the backend; the client has no lever beyond the timer.
