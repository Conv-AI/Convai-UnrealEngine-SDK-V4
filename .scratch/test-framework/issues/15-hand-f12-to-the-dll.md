# 15 — Hand F12 to whoever owns the canceller

Status: `wontfix` — **closed session 4. There is nobody to hand it to; the canceller is ours.**
Depends on: 06

**Closed 2026-08-07, session 4.** The premise is gone: this issue routed F12 to an owner
outside the repository, and `convai-livekit-cpp-p` is now in scope. The write-up
(`docs/AEC_NEAR_END_SUPPRESSION.md`) and the test
(`AecNearEndPreservation.ReferenceWithNoEchoMustNotSuppressThePlayer`) are already in that
repo next to the code, which is where a handover would have put them anyway. The test is
skipped rather than red: `build.yml` runs ctest on every push to `staging-v2`, and a suite
that always fails cannot separate this defect from a new one. Its assertion is unchanged and
is the acceptance criterion for the fix — one line removes the skip.

What replaces it is a guard rather than a message: `aec_double_talk` is the in-engine tier of
`AecDoubleTalk.NearEndSurvivesSimultaneousEcho` — the player speaks over the character and the
near end has to survive to the server. It exists so that an agent looping on the echo leak
cannot "fix" it by suppressing harder and destroy the player's speech with nothing noticing,
which is the one genuinely dangerous gap in the loop.

**2026-08-07.** Written up as `docs/AEC_NEAR_END_SUPPRESSION.md` in `convai-livekit-cpp-p`, on
`feat/aec-erle-tests` next to the test (commit `6e6b649`, local, not pushed). It carries the
finding, both tables, the reproduce command, the caveat below, and an explicit statement that this
is *not* the "responds to its own voice" symptom — which is the confusion that cost three sessions.

What remains is not something this repository can do: a person has to send it to the owner of
`convai_client.dll` and get an answer to the question in it. Nothing here is blocked on that.

## Why this is a separate issue

F12 and F26 are both called "AEC is broken" and they are different bugs in different repositories.
Keeping them in one bucket is why the symptom list in FINDINGS took three sessions to separate.

| Symptom | Where | Evidence |
|---|---|---|
| "player speech cut or swallowed" | **DLL** | F12, reproduced offline in 3 s with no plugin present |
| "character responds to its own voice" | **plugin** | F26, leaks in-engine while the same canceller is healthy offline (F14) |

Issues 13 and 14 chase the plugin half. This one routes the other half, because nothing in this
repository can fix it.

## What to hand over

`tests/aec_erle_test.cpp` in `convai-livekit-cpp-p`, branch `feat/aec-erle-tests`, test
`AecNearEndPreservation.ReferenceWithNoEchoMustNotSuppressThePlayer`. Red on purpose. Runs in
about three seconds with no network and no microphone.

The finding: an **active reference carrying the character's speech, with no echo at all in the
microphone**, costs the near-end talker 11.5 dB overall and drives two quarters of the utterance
to −73 dB.

| Configuration | Near-end level, whole | Worst two quarters |
|---|---|---|
| Control — reference silent | −0.4 dB | −0.4, −0.5 dB |
| Internal, AEC only | **−11.5 dB** | −72.8, −67.2 dB |
| Internal, shipping (NS+AGC+HPF) | −2.7 dB | −74.0, −76.0 dB |
| External (`AECwebrtc`), AEC only | **−13.5 dB** | −72.8, −67.2 dB |

Three points that make it the DLL's rather than a configuration mistake:

- **Both backends do it**, within 2 dB. It is the shared WebRTC AEC3 suppressor gating the
  capture path on far-end *activity*, not on correlated echo.
- **It inverts with echo level.** 0.00 → −11.5 dB, 0.01 → −1.2 dB, 0.50 → −3.8 dB. The better
  the player's acoustic isolation the worse they are treated, so headset users get the worst of
  it — which is most users.
- **AGC hides it from averages.** The shipping config reads −2.7 dB overall while individual
  quarters sit at −74 dB. Any monitor reporting mean level will call this healthy.

## What to ask for

Not a fix in a particular place — the question is whether AEC3's suppressor can be configured or
driven so that far-end activity alone does not gate the capture path, or whether the plugin has
to compensate. That is a decision for whoever owns the canceller.

## The caveat that must travel with it

F12 was measured with a frame-synchronous, gapless reference. The plugin's real feed is punctured
(F3) and intermittently near-absent (F15), and a punctured feed makes the far end look *less*
active, which would *reduce* this gating. So the two halves interact: issue 14's fix to the feed
could make this symptom worse while fixing the other one. They need measuring together, which is
issue 05's double-talk variant, and that is not built.

## Done when

The owner of `convai_client.dll` has the test, the table and the caveat, and has said whether
this is theirs to fix or ours to work around.
