# Plugin findings — 2026-08-06

What was found by reading the plugin and `Saved/Logs/Dev_WebRTC.log` while designing the test
framework. Recorded here because these are the framework's first targets, and because
several are testable before the framework exists.

Each entry states what is **confirmed** versus what is **hypothesis**, matching the
attribution discipline the framework itself must follow.

Reported symptoms, all observed by the maintainer: character responds to its own voice;
character interrupts itself mid-sentence; player transcript contains the character's words;
player speech cut or swallowed; degrades over time; worse with N > 1 connections.

**Revised 2026-08-06** during implementation. F1, F5, F7 and F11 were tested and their
original readings did not survive; F3 was confirmed and its window turned out to be wider than
described. F12 through F18 are new.

**Revised again 2026-08-06, session 2. F18 is refuted and F7 is restored.** The microphone
reaches the master mix 105.9 dB below what it emitted — inaudible, and below the quantisation
floor of the int16 conversion **Reference Audio** is delivered through. F18's 0.848 was a
*normalised ratio*, which reads the same for a microphone at full strength and for one
attenuated to nothing; the negative control that separates them did not exist until now and
separates them by 95.7 dB. There is no F18 → F12 chain. **F12 stands entirely on its own** and
never depended on it (see F12's note on its own fixture). What survives is F19: a
customer-supplied capture component *is* inside **Reference Audio**, at full strength.

Verdicts, current:

| | Verdict |
|---|---|
| F1 | Teardown race, not a general failure. The diagnostic gap it names is real |
| F2 | Confirmed, unchanged |
| F3 | Confirmed, and the puncture is wider than first described |
| F4 | Confirmed by reading, unmeasured |
| F5 | Confirmed, and measured: worth 0 to 2.3 dB. Not a symptom cause |
| F6 | Confirmed, unchanged |
| F7 | **Restored and now measured.** The microphone is 105.9 dB down in the master mix. Right answer, and its "read the parent name, not the behaviour" reasoning is now backed by a number |
| F8, F9, F10 | Confirmed, unchanged |
| F11 | **Refuted.** Allocates once, not per call, and 11.5 MB not 46 MB |
| F12 | **New, confirmed.** Explains "player speech cut or swallowed". Independent of F18 |
| F13 | **New, confirmed.** `origin/debug/aec` already rewrites the reference path |
| F14 | **New, confirmed.** Cancellation quality is healthy; the problem is plumbing |
| F15 | **New, confirmed.** The submix recorder loses ~99.6% of the audio in 2 runs of 5 |
| F16 | **New, confirmed.** The "dead" in-plugin harness compiles, runs and ships to customers |
| F17 | **New, confirmed.** No second master-submix recorder works while a Connection is live |
| F18 | **Refuted, session 2.** Both of its mechanisms are impossible in UE 5.8 source and its measurement could not tell the two cases apart |
| F19 | **New, confirmed.** A capture component supplied through the documented extension point renders into the master mix at −10.1 dB — that one *is* inside Reference Audio |
| F20 | **New, confirmed by crash, twice.** A recording player component that is destroyed with nothing captured aborts Editor and Development builds — `EndPlay` calls `FinishRecording()` itself |
| F21 | **New, confirmed.** The runner's exit code tracks neither the requested status nor the outcome. About the harness, not the plugin |
| F22 | **New, confirmed.** Every real final transcript is followed by an empty one also flagged final. Minor, but it clears any UI bound to the delegate |
| F23 | **New, confirmed, severity revised down.** The plugin drops every `bot-output` packet. It carries the response *text* and its spoken-progress state, not audio — the character is still heard |
| F24 | **New, confirmed.** A silent microphone stream produces hallucinated player transcripts — 162 in 20 s with AEC off, 23 with it on |
| F25 | **Not new.** The teardown hang is the PIE-stop deadlock already proven on 2026-07-28. What is new is that it fires headless too, at 5 launches in 10, and what that costs the suite |
| F26 | **New, confirmed, 1 run in 5.** In-engine, echo cancellation intermittently lets the character's own voice back in as the player's transcript. Offline the same canceller is healthy (F14), so this is plumbing |
| F27 | **New, measured, and a negative result.** The reference feed is healthy on the runs that leak. F3's puncture costs ~0.9%, F4's cadence is 11.4 ms not 10 ms but steady, and F15 did not fire once in ten runs. F26's mechanism is still unknown |
| F28 | **New, confirmed.** `origin/debug/aec` binds the reference tap to a subsystem member that no longer exists and calls a DLL symbol the shipped import library does not export. Its design survives; its diff does not |
| F29 | **New, measured.** A submix-listener reference tap fixes the feed — ratio 1.000, no recorder held — and does not reduce the leak: 0/10 against 3/10, p = 0.21. Not evidence either way, and by construction this design could not have produced evidence |
| F30 | **New. The PRD's V5 passed, and the report's weakest part was its prose.** A fix agent given only `report.json` found a seeded bug in two greps and verified the fix. The evidence text also pointed it at a wrong mechanism, and the report never carried the input to the failing decision |
| F31 | **New, confirmed, and it invalidates every live AEC measurement before it.** The plugin's own capture component keeps the host's microphone open after a third-party one is adopted, and both render into the submix the plugin records. F24 is re-explained; F26's rate is partly the room. Fixed, with a guard |
| F32 | **New, measured. The oracle exists, and the number it returns is 3 dB.** With the APM's other stages off the AEC-off control reads exactly 0.000 dB and the canceller reads 2.7 to 3.3 — against 40 to 49 dB for the same canceller offline (F14). Two earlier framings of the same metric are recorded as negative results: whole-window and per-second attenuation both measure AGC, not cancellation |
| F33 | **New, and the session's gate failing rather than passing.** A one-token defect that takes the offline ERLE suite from 1 red test to 4 leaves the in-engine attenuation at 2.45 dB, inside the healthy range. So the 3 dB is the suppressor, not subtraction — in-engine, echo subtraction contributes nothing measurable to break. V5 was not re-run |
| F34 | **New, confirmed in-engine on the first run, n=1.** With the player talking over the character the canceller attenuates by 14.7 dB across the window and 8.8 to 19.4 dB in every single second, and the player's transcript comes back empty. Against 0 to 3 dB when only the echo is present. F12, reproduced in the engine on the shipping configuration |

---

## Status index — 2026-08-20

Where each finding stands after the issue queue closed (`ISSUES.md`, `CLOSED-ISSUES.md`). The
verdict table above is the 2026-08-06 reading and is left as written; **this table supersedes it
where the two disagree.** `verified today` means it was re-checked against 0.2.12 in this
session's two full sweeps, not read off the entry.

### Open — a defect a customer can hit

| | What | Evidence today |
|---|---|---|
| **F12 / F34** | an active reference destroys the player's speech during double-talk. **Separated 2026-08-21: it is not the server** — it reproduces offline with no engine, no network and zero echo. The mechanism is `ep_strength.default_gain` in vendored `client-sdk-rust`; a measured one-field change clears the criterion but **was reverted unmerged**, because that source is not ours to change | `aec_double_talk` **0 of 5** — *measured 2026-08-21*. Not flaky: the arm had been scoring the character's transcript as the player's |
| **F26** | with AEC on, the character's own speech comes back as the player's transcript | `aec_echo_only_internal` failed 2 of 2 — *verified today* |
| **F8** | `final-user-transcription` is dropped: the server sends the player's final transcript and the plugin does not map the type | `Unknown server type 'final-user-transcription'` x14 in one 22-launch sweep, and no such string anywhere in `Source/Convai` — *verified today* |
| **F9** | *(server half only)* something upstream does not recognise the client version. The plugin half is fixed; the emitter is in none of the repositories on this machine | 3 to 17 `error-response` advisories per healthy session — *measured 2026-08-20*. See F9 for the handover |
| **F10** | lip-sync starvation: a multi-second stall in audio or face-data delivery | 20 starvation lines across 22 launches, against "confirmed once" when filed — *verified today, and more frequent than recorded* |
| **F22** | an empty transcript, also flagged final, follows every real one on the **player** path | still present — *verified today*. The character's half of this was the plugin's own and is fixed; see F23 |
| **F5** | `SetStreamDelay` is never called | no occurrence in `Source/Convai` — *verified today*. Measured worth 0 to 2.3 dB, so not a symptom cause |
| **F24** | a silent microphone produces invented player transcripts | not re-measured this session; F31's fix changed what reaches the server, so re-measure before acting |
| **F2 / F3 / F4 / F6 / F15 / F17** | the reference path: unobservable, restarted every ~10 ms, wall-clock polled, fighting over one recorder, dropping ~99.6% intermittently, and exclusive | not re-measured this session. F37 reports the publish decouple changed this architecture — re-measure before acting on any of them |

### Fixed

| | Fixed by |
|---|---|
| **F25** | **not a deadlock: the rate was the harness's own threshold.** `run.py` called a launch hung 2 s after its report and killed it; teardown legitimately takes up to ~5 s. Threshold raised to 15 s — 2026-08-20. Full sweep: 13/24 at 2 s, **0/24 at 15 s**, `p = 2.6e-05` |
| **F19** | an adopted capture component is given the plugin's submix at the adoption site, and restarted if it was already playing — 2026-08-20. Measured -105.8 dB into the master mix against -9.9 dB before, on the same tone in the same build |
| **F9**, plugin half | `error-response` is reported as what it is and reaches the game on a new `OnServerErrorEvent`; `OnError` stopped being static, so fatal errors reach `OnFailureEvent` — 2026-08-20. `server_error_reaches_game` is the check |
| **F16** | every file under `Source/Convai/{Public,Private}/Tests` is wrapped in `#if WITH_TESTS`, and `ConvaiTests` is a `DeveloperTool` module the release script strips — 2026-08-20. The harness is kept, not deleted; a Shipping build contains none of it. **Deleted 2026-08-28** — see the entry's closing note |
| **F21** | the code is armed in `Complete()` and delivered from `FConvaiTestsModule::ShutdownModule` — 2026-08-20. `RequestExitWithStatus(Force=false)` can never deliver one on Windows; `test_exit_code.py` is the check |
| **F23** | the character's answer now reaches the transcript delegate on both paths, and `bot-llm-stopped` finalises that text instead of `""` — 2026-08-20 |
| **F38** | a refused character now raises `OnFailureEvent` — 2026-08-20. Its polling half is still open, and is written up in the entry |
| **F20** | `PCMDataToWav` returns early on an empty buffer; the guard is in `ConvaiUtils.cpp` — *verified today*. The `EndPlay` crash that shared its scenario was a different defect, closed as I7 |
| **F31** | the default capture component is stopped when a third-party one is adopted; the guard is in `SetAudioCaptureComponent` — *verified today* |
| **F1** | a teardown race, not a general failure. The diagnostic gap it names is real and is what I4 closed |
| **F30** | both defects it exposed are fixed; the entry is kept for its lesson about evidence prose |

### Not a defect — measurement, refuted, or superseded

| | |
|---|---|
| **F7** | restored and measured: the microphone is 105.9 dB down in the master mix. Right answer |
| **F11** | refuted: allocates once, not per call, and 11.5 MB not 46 |
| **F18** | refuted: both mechanisms are impossible in UE 5.8 source |
| **F14, F27, F29, F32, F33, F35, F36, F37** | measurements and negative results from the AEC investigation. F37 is the current state of it; read it before forming a hypothesis about F26 or F12 |
| **F13, F28** | about `origin/debug/aec`: its design survives, its diff does not |

**Nothing here is scheduled.** The three reds a customer can hit today — F19, F26, F12/F34 — are
the plugin's own, are not in `ISSUES.md`'s queue, and none of them has an owner in this file.

---

## F1 — A Connection ran with no Reference Audio, and nothing said so

**Resolved 2026-08-06: a teardown race, not a general failure. The diagnostic gap it names
is real and unfixed.**

The original reading counted log lines:

```
OnConnectedToServer called SessionID     x2
Started reference audio capture thread   x1
```

and concluded one of two **Connections** got no **Reference Audio**. That inference does not
hold against the code. `UConvaiSubsystem::AttachReferenceAudioClient`
([ConvaiSubsystem.cpp:1204-1232](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1204-L1232))
starts the thread on the *first* **Connection** and every later one joins through `AddClient`
at [:1231](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1231), which logs nothing. One
"Started reference audio capture thread" for N **Connections** is the healthy shape, not a
missing client.

What the timestamps in `Dev_WebRTC-backup-2026.08.06-05.18.15.log` actually show:

| Time | Line |
|------|------|
| `05:14:41.345` | `BeginTearingDown` for `/Game/Maps/UEDPIE_0_Landing` |
| `05:14:41.348` | `Connection released — TTL is 0, disconnecting immediately` |
| `05:14:42.782` | `OnConnectedToServer` — SessionID `7de619f2` |
| `05:14:43.148` | `UWorld::CleanupWorld` |
| `05:14:43.156` | `No world was found for object … ConvaiConnectionSessionProxy_0` |
| `05:15:02` | a **new** PIE session begins (`World_1`) |
| `05:15:16.660` | `OnConnectedToServer` — SessionID `c452868b` |
| `05:15:16.708` | `Started reference audio capture thread` |

The first `OnConnectedToServer` landed **1.4 s after PIE began tearing down**, on a
**Connection** already released. The second belongs to a different PIE session entirely, and
its reference thread started 48 ms later — the healthy path. There was never a live
**Connection** without **Reference Audio**.

The original entry's own "Not established" note guessed this correctly, and the "Suggestive,
unproven" note about the proxy parented to `UnrealEdEngine_0:GameInstance_0` is the same
teardown artefact.

**What survives, and still matters.** `AttachReferenceAudioClient` returns silently on
`!Client` and on `!IsAECEnabled()`, and its one `Warning` — the null-`World` path at
[:1220](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1220) — never fired even in a run
that was demonstrably losing its `World`. A **Connection** whose canceller has no far-end
signal is still indistinguishable from a healthy one in any log. Issue 03's client-count
assertion is unchanged and still the right check; what changes is that it has nothing to
reproduce yet, so it must be driven by the deliberate connect-then-teardown case rather than
expected to fire on a normal run.

## F2 — The reference path is unobservable

**Confirmed.** The thread ran from `05:15:16` to `05:18:15` and emitted no output between
start and stop. No chunk count, no client count, no rate, no gap detection. There is no way
to tell from any log whether **Reference Audio** ever flowed.

This is why AEC "breaks a lot" and stays broken: the failure has no signal. It is also the
strongest argument for issue 03 preceding everything else.

## F3 — Reference capture stops and restarts the recorder every ~10 ms

**Confirmed by reading, and the mechanism is confirmed in UE 5.8 engine source. The gap is
larger than the original entry described.**

`Run()` queues an async audio-thread command that calls `ProcessCapturedAudio()` then
`StartRecordingRefrence()`
([ConvaiReferenceAudioThread.cpp:87-88](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L87-L88)),
and `ProcessCapturedAudio` drains via `MixerDevice->StopRecording(nullptr, …)`.

The engine side makes the puncture explicit. `FMixerSubmix::OnStopRecordingOutput` sets
`bIsRecording = false` (`AudioMixerSubmix.cpp:2377-2384`), and the audio render thread's
append is gated on that flag under `RecordingCriticalSection` (`:1686-1692`). Every buffer the
mixer renders while the flag is down is **dropped**, not buffered.

**The window is not the dispatch boundary — it is the whole conversion.** `StopRecording` is
the *first* statement of `ProcessCapturedAudio`
([:216](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L216)), and
`StartRecordingRefrence` only runs after the float→int16 loop, the resample, and the
`SendReferenceAudio` fan-out to every **Connection** have all completed
([:231-267](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L231-L267)). The
recorder is off for that entire stretch, on the audio thread, roughly 100 times a second, and
the cost grows with **Connection** count because the fan-out is inside the window.

**Fix direction, one line and independent of the framework:** call `StartRecordingRefrence`
immediately after `StopRecording` and do the conversion afterwards. `origin/debug/aec` takes
the larger option instead — see F13.

**Still needs measurement:** the gap as a fraction of rendered audio. Issue 03 measures it
against submix ground truth.

## F4 — The capture cycle is wall-clock polled, not sample-driven

**Confirmed by reading.** `Run()` sleeps 2 ms, tests `elapsed >= 10 ms`, then dispatches
*asynchronously* to the audio thread. Under load those commands queue and coalesce, so chunk
sizes and inter-chunk delay wander. Moving delay is the second classic AEC failure.

Chunk size is fixed at 480 samples (10 ms at 48 kHz), confirmed in the log, so variance shows
up as timing jitter and buffer backlog rather than as size changes.

## F5 — `SetStreamDelay` is never called

**Confirmed, and measured: it does not matter much.**

`IAudioEchoCanceller::SetStreamDelay` exists and no call site exists anywhere in the plugin.
The delay sweep in `tests/aec_erle_test.cpp` (issue 06) runs each delay twice, once with the
hint and once without:

| Echo delay | ERLE, hint unset | ERLE, hint set | Gain |
|---|---|---|---|
| 0 ms | 20.6 dB | 22.8 dB | +2.3 dB |
| 40 ms | 47.7 dB | 48.9 dB | +1.1 dB |
| 120 ms | 49.4 dB | 49.3 dB | −0.1 dB |
| 300 ms | 68.2 dB | 69.8 dB | +1.6 dB |

Between −0.1 and +2.3 dB on a *stable* delay. Worth adding, but it does not explain any
reported symptom, and a fix that only adds this call should not be expected to change
behaviour. `ExternalAEC::SetStreamDelay` is a logging no-op
(`src/convai/audio/external_aec.cpp:117-123`), so on the default `AECType::External` path the
call would do nothing at all.

Unmeasured, and the case that would actually justify the hint: an *unstable* delay, which is
what F3 and F4 produce. The offline sweep holds the delay fixed.

## F6 — A null submix makes the microphone and Reference Audio fight over one recorder

**Confirmed, with a deliberate repro.** The maintainer force-deleted
`/ConvAI/Submixes/AudioInput` on 2026-08-06 and reran; the log shows the warning firing at
`06:00:27.494` and a session proceeding anyway.

`SoundSubmix` is resolved by path at construction
([ConvaiPlayerComponent.cpp:110-131](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L110-L131)).
When it stays null, every `Cast<USoundSubmix>(nullptr)` becomes "the master submix", and
three call sites converge on the same process-global recorder:

| Caller | Call | Records |
|--------|------|---------|
| `StartVoiceChunkCapture` | `StartRecordingOutput(this, T, Cast<USoundSubmix>(nullptr))` ([:527](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L527)) | master |
| `ReadRecordedBuffer` | `StopRecording(nullptr, …)` ([:530-546](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L530-L546)) | master |
| `FConvaiReferenceAudioThread` | `StartRecordingOutput(World, 60.0f, nullptr)` / `StopRecording(nullptr)` | master |

Game thread and audio thread, each stopping and restarting the same recorder every ~10 ms —
exactly what `ConvaiReferenceAudioThread.h` warns about in its own header comment.

Two failures at once: the microphone signal becomes the game's own speaker output, and the
reference feed receives only the buffers the other side did not steal. Cancellation cannot
work even in principle.

**Observed on speakers:** no character audio, no player capture, loud high-pitched tone.

**Observed on headphones** (the discriminator run, 2026-08-06): constant noise, audible
self-hearing, player audio never reaches the server — but text input works and the character's
audio response plays normally.

Those two runs together settle the mechanism.

*Self-hearing* is confirmed: `UConvaiAudioCaptureComponent` is a `USynthComponent`
([ConvaiAudioCaptureComponent.h:101](../../Source/Convai/Public/ConvaiAudioCaptureComponent.h#L101)),
and a synth with a null `SoundSubmix` renders into the default chain — audible.

*Not acoustic feedback.* Headphones break the loop and the noise persists, so the
high-pitched tone on speakers was feedback stacked on top of an already-bad signal, not the
cause. `OnGenerateAudio` handles partial fills correctly — it returns `OutputSamplesGenerated`
and has an explicit silence branch — so the likeliest source is discontinuity as the capture
buffer starves and refills under the recorder churn. Not confirmed.

*Capture death is isolated.* Text input works and character audio plays, so the data channel
and the playback path are untouched. The only broken path is `ReadRecordedBuffer` calling
`StopRecording(nullptr)` on master while the reference thread stops and restarts that same
recorder roughly every 10 ms. Exactly what the collision above predicts.

**Not observed:** a self-hearing loop in the transcripts. Both turns transcribed cleanly
(`"hii"` → `"Hey there. What's up?"`) and the bot's words did not return as user
transcription.

Guarded only by a `Warning`. Live risk for customers if the `Content` folder — stripped as
"dependency-managed" by `push_to_public_v4.bat` — does not reliably deliver the asset.

**Fix direction, independent of the framework:** a missing submix should fail loudly rather
than silently degrade to the master submix, and the two recorders should never be able to
target the same submix.

## F7 — Is the microphone inside Reference Audio? No

**Restored 2026-08-06 (session 2) and now measured — see F18's refutation.** F18 briefly
overturned this entry on a normalised ratio that could not distinguish an audible microphone
from a silent one. With absolute level measured, the microphone arrives in the master mix
**105.9 dB** below what it emitted, against **−10.1 dB** for the same microphone with its
routing deliberately removed. `MuteMic` mutes. This entry's answer was right.

What was wrong with it is narrower and worth keeping: it inferred the answer from the asset's
parent name rather than from behaviour, and that inference happened to hold. F19 is the case
where the same routing is absent and the microphone really is inside **Reference Audio**.

**Original entry, kept for the record.** The maintainer checked the asset on 2026-08-06:
`/ConvAI/Submixes/AudioInput` has **Parent Submix = `/ConvAI/Submixes/MuteMic`**.

The microphone is deliberately routed into a muted submix, so it contributes silence to the
master mix and does not appear in **Reference Audio**. Cancellation is not attacking the
near-end talker by this route.

**Consequence: "player speech cut or swallowed" now has no explanation.** This was the
leading candidate. Remaining hypotheses, in rough order of suspicion:

- over-suppression during double-talk, i.e. an algorithm problem — issue 06's near-end
  preservation test is the direct probe
- a punctured or misaligned reference feed (F3, F4, F11) causing the canceller to misconverge
  and suppress the near end along with the echo
- server-side VAD gating: `ConvaiVadParams` defaults are `stop_secs 2.2`, `min_volume 0.6`,
  and nothing in the plugin sets them

That raises issue 06's priority: it is now the most likely place to find this symptom.

**Answered 2026-08-06 — see F12, which stands on its own.** The first hypothesis was right, and sharper than stated:
it is not double-talk over-suppression but suppression with *no echo at all*, triggered by
far-end activity alone. The third hypothesis is untested and now lower priority. Issue 06
found it, as predicted.

**Second-order observation.** Because `MuteMic` silences it, the microphone's *audible* path
is never exercised in normal operation. The submix chain is load-bearing — losing one asset
kills capture entirely (F6) — and nothing verifies it. The tone probe originally proposed for
issue 03 stays, downgraded from investigation to regression guard: assert the Virtual Mic's
signal does not appear in the reference feed, so a future reparenting of `AudioInput` cannot
regress this silently.

## F11 — 46 MB reserved 100 times per second, on the audio thread

**Refuted as stated, 2026-08-06, from UE 5.8 engine source. It allocates once, not per call,
and the figure is 11.5 MB, not 46 MB.**

The call site is real —
[ConvaiReferenceAudioThread.cpp:318](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L318)
does call `StartRecordingOutput(World, 60.0f, nullptr)` roughly every 10 ms on the audio
thread. The cost claim does not survive reading what it reaches.

`FMixerSubmix::OnStartRecordingOutput`, `AudioMixerSubmix.cpp:2370-2375`:

```cpp
RecordingData.Reset();
RecordingData.Reserve(ExpectedDuration * GetSampleRate());
bIsRecording = true;
```

Two corrections:

- **No channel factor.** `ExpectedDuration * GetSampleRate()` = 60 × 48000 = 2.88 M floats =
  **11.5 MB**. The original arithmetic multiplied by `NumChannels`, which the engine does not.
- **Not per call.** `TArray::Reset()` with the default argument keeps capacity whenever
  `NewSize <= ArrayMax` (`Array.h:2469-2489`), and `Reserve(N)` is a no-op once
  `ArrayMax >= N`. The 11.5 MB is reserved on the **first** call; every later call is a
  `Reset` on POD floats — no destructor work — followed by a no-op `Reserve`. There is no
  100 Hz allocation.

So F11 does not explain F3, F4 or F10, and the "single constant with a wide blast radius" it
promised does not exist. The 60-second figure is still wrong — 11.5 MB of resident slack for
a buffer that never holds more than 10 ms — but it is a tidiness issue, not a performance one.

**Negative result, recorded deliberately:** this was the highest-suspicion-per-line item in
the list and the profiler capture it called for is not needed. The audio-thread cost that *is*
real in this loop is F3's — the recorder being off across the whole conversion and fan-out —
which is a correctness problem, not an allocation one.

## F12 — An active reference with no echo destroys the player's speech

**Confirmed, reproduced offline and deterministically.** `tests/aec_erle_test.cpp` in
`convai-livekit-cpp-p`, test `AecNearEndPreservation.ReferenceWithNoEchoMustNotSuppressThePlayer`.
This is the explanation F7 left missing for *"player speech cut or swallowed"*.

Feed the canceller a live reference stream carrying the character's speech, and a microphone
carrying only the player — no echo at all, which is what headphones produce. The canceller
suppresses the player anyway.

**Independent of F18, and always was.** The session-2 kickoff describes this as measured "when
the reference contains the player's own voice", and proposes re-measuring it once F18 is fixed.
The fixture never did that. `BuildFixture` sets `farEnd` to `welcome.wav` alone
(`tests/aec_erle_test.cpp:227`) and mixes the near-end talker into `micIn` and `nearOnly` only
(`:239-240`); the player's voice is never in the reference. The mechanism here is far-end
*activity* gating the capture path, not correlation with the player. So there is nothing to
re-measure, F12's severity is unchanged by anything F18 did or did not say, and issue 05's shape
does not move on this account.

| Configuration | Near-end level, whole | Worst two quarters |
|---|---|---|
| Control — reference silent | −0.4 dB | −0.4, −0.5 dB |
| Internal, AEC only | **−11.5 dB** | **−72.8, −67.2 dB** |
| Internal, shipping (NS+AGC+HPF) | −2.7 dB | **−74.0, −76.0 dB** |
| External (`AECwebrtc`), AEC only | **−13.5 dB** | **−72.8, −67.2 dB** |

Three things follow.

**It is not an `AECType` choice.** Both backends do it, to within 2 dB. The shared WebRTC
AEC3 suppressor gates the capture path on far-end activity whether or not any correlated echo
is present.

**AGC hides it from aggregate statistics.** The shipping config's whole-run figure (−2.7 dB)
looks nearly healthy because AGC lifts the surviving quarters — q0 comes out at *+1.0 dB* —
while the annihilated quarters are worse than AEC-only, at −74 and −76 dB. Half the utterance
is gone and the average says otherwise. Any monitor that reports mean level will miss this.

**The relationship is inverted from intuition.** Suppression is worst when the echo is
*absent* and recovers as soon as any echo exists:

| Echo gain | Near-end level |
|---|---|
| 0.00 | −11.5 dB |
| 0.01 | −1.2 dB |
| 0.05 | −1.5 dB |
| 0.15 | −2.3 dB |
| 0.50 | −3.8 dB |

The better the player's acoustic isolation, the worse the plugin treats them. That matches
F6's headphone run exactly — *"player audio never reaches the server"* while text input and
character playback were fine — and it means the symptom is most likely to be reported by
headset users, which is most of them.

**Controls.** The silent-reference run at −0.4 dB and the fully-disabled-APM run at 0.0 dB /
correlation 1.000 are in the same file. Without them "the player survived" and "the fixture
fed silence" would be the same measurement.

**Caveat, stated because it is load-bearing.** This is measured with a frame-synchronous,
gapless reference feed. F3 says the plugin's real feed is punctured; a punctured feed might
*reduce* this gating by making the far-end look inactive. The interaction is unmeasured, and
it is a reason to fix F3 and re-measure rather than to assume the two compose.

**What it does not explain:** "character responds to its own voice". ERLE is healthy (F14),
so leaked echo is not the mechanism there.

## F13 — `origin/debug/aec` already replaces the reference capture, and nobody looked

**Confirmed.** The design session recorded `fix/aec` and `origin/debug/aec` as unexamined.
Checked 2026-08-06:

- `fix/aec` (`48512e96`, "fix: Play audio through android") is unrelated — Android playback
  and `ConvaiAndroidVoice`. Nothing touching F1–F11.
- `origin/debug/aec` (`340687d3`, "chore: add new submix") is a **substantive attempt at
  F3, F4 and F5**, and its one-line commit subject hides that completely.

It adds `FConvaiSubmixReferenceListener` — an `ISubmixBufferListener` on the main submix that
feeds `SendReferenceAudio` directly, carrying the sub-frame remainder between callbacks so
only whole 480-sample frames are fed — and stops starting `FConvaiReferenceAudioThread`
altogether. That removes the stop/start recorder loop, so F3's puncture, F4's polled cadence
and F11's reserve all cease to exist on that branch. It also adds the `SetStreamDelay` call
F5 names, behind a `UConvaiUtils::GetAECStreamDelayMs()` knob.

Two things to know before adopting it:

- Its comments cite `docs/adr/0001-aec-reference-source.md`, which **does not exist on that
  branch**, nor anywhere in the repo. The rationale it points at was never written down.
- F12 says the reference feed is not the whole story. A gapless reference makes the far-end
  *more* consistently active, which is the condition under which F12's suppression is worst.
  This branch could plausibly make the "player speech swallowed" symptom worse while fixing
  the "responds to its own voice" one. Both need measuring together, not in sequence.

The test framework is being built against `docs/test-framework`, which carries the shipping
`FConvaiReferenceAudioThread` path. That is deliberate — the framework must measure what
ships — but issue 03's monitor should be written so it can run against either implementation,
since the branch is a live candidate.

## F14 — Cancellation quality is not the problem

**Confirmed by measurement**, `tests/aec_erle_test.cpp` (issue 06). Recorded because the
bisection ADR-0004 promised is now resolved and it points the remaining work at the plugin.

| Case | ERLE | Convergence |
|---|---|---|
| Internal, AEC only, 120 ms delay | 49.4 dB | 200 ms |
| Internal, shipping config | 46.4 dB | 220 ms |
| External (`AECwebrtc`) | 41.5 dB | 200 ms |
| Soft-clipped echo path, gain 0.9 | 39.4 dB | 200 ms |
| 1% additive noise | 29.9 dB | 200 ms |
| Control — AEC disabled | 0.0 dB | never |
| Control — no reference stream | 0.2 dB | never |

Cancellation works, converges fast, and degrades gracefully under nonlinearity and noise. The
one soft spot is a **zero** echo delay, where ERLE falls to 20.6 dB — irrelevant acoustically,
but worth knowing if a future reference path ever aligns the streams exactly.

**Consequence:** every remaining symptom is either plugin plumbing (F3, F4, F6) or the
suppressor behaviour of F12. Issues 03 and 05 are aimed correctly.

## F15 — The recorder the reference path is built on drops nearly all of the audio, intermittently

**Confirmed by measurement**, issue 01's probe, 2026-08-06. Five identical headless `-game`
runs, three independent taps each. See [01-result.md](./issues/01-result.md).

| Run | `StopRecording`, null arg | `StopRecording`, explicit arg | `ISubmixBufferListener` |
|-----|-----|-----|-----|
| 1 | 270,848 | 270,848 | 274,944 |
| 2 | 270,848 | 270,848 | 275,456 |
| 3 | **1,024** | **1,024** | 275,968 |
| 4 | **1,024** | **1,024** | 275,456 |
| 5 | 271,360 | 271,360 | 274,432 |

**Two runs in five returned 1,024 samples where the listener returned ~275,000 — a 99.6%
loss, from an identical command line.** The listener varied by under 0.6% across all five.

**Wider sample, same day, through the framework's own `reference_feed_capture` scenario.**
Thirteen runs were observed in total across the standalone probe and the scenario:

| Batch | Runs | Catastrophic loss | Healthy loss |
|---|---|---|---|
| Standalone probe | 5 | 2 (99.6%) | 3 (~1%) |
| Scenario, single run | 1 | 1 (99.8%) | — |
| Scenario, batch of 3 | 3 | 0 | 3 (~1%) |
| Scenario, partial batch of 10 | 4 | 0 | 4 (0.7–1.1%) |

**Three of thirteen.** The healthy case loses 0.7–1.1%, which is the expected cost of the
recorder and the listener starting and stopping at slightly different instants. The failure
case is not a degraded version of that — it is near-total, and there is no middle ground in
any run observed. The 10-run batch was stopped after 4 for time, so its denominator is
partial and is reported as such rather than extrapolated.

A rate near one in four means a customer hits this regularly and no single session proves it,
which is exactly the shape the PRD's occurrence-rate reporting exists for.

`FConvaiReferenceAudioThread` is built entirely on `StartRecordingOutput` / `StopRecording`
([:318](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L318),
[:216](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L216)). On this evidence the
**Reference Audio** feed can be almost entirely absent for a whole session, with no log line,
which is F2's blind spot and would produce every symptom F1 predicted.

**Ruled out:** the submix argument. Null and explicit resolve to the same instance and agree
exactly in every run, so the `Cast<USoundSubmix>(nullptr)` idiom F6 flags is not the mechanism
here.

**Not established:** the cause of the intermittency, and whether the same rate holds in PIE
and packaged builds. Contributing factor identified: `FMixerSubmix::ProcessAudio` early-returns
on auto-disable (`AudioMixerSubmix.cpp:1410`) before the recording append (`:1686`), while the
disabled path still feeds submix listeners a zeroed buffer (`:1447`) — so the two taps diverge
by construction whenever the submix idles. That explains a divergence; it does not by itself
explain a 99.6% loss with audio continuously playing.

**Relationship to F3.** F3 says the recorder is off across the conversion and fan-out, which
predicts a steady partial loss. This is a different and larger effect: near-total loss, in
some runs and not others. Both point the same way.

**Consequence for the plan.** This is strong independent support for `origin/debug/aec`'s
`FConvaiSubmixReferenceListener` (F13) — the listener tap is the stable one, measurably, on
this machine. It also means issue 03's ground-truth listener is not merely a cross-check: it
is the only reliable measurement of the two, and the monitor should report the recorder's loss
rate as a first-class metric rather than assuming the feed arrived.

**Caveat on provenance.** An earlier version of the probe reported zero recorded samples in
runs where the listener saw audio. That probe had a defect — `StopRecording`'s out-params were
read in the same argument list as the call, whose evaluation order is unspecified. The table
above is from the corrected probe. The zeros from the buggy version are not evidence of
anything and are not counted here.

## F16 — The "dead" test harness is not dead. It compiles, runs, and ships

**Fixed 2026-08-20** on `fix/strip-in-plugin-harness`, by gating rather than deleting: the
harness is kept and still runs in Development, and compiles out of Shipping and Test. Tier 0.

**2026-08-28: harness deleted** on `refactor/one-test-runner` (22 files; the three Blueprint
debug utilities — Record Replay, the chunked-audio proxy, `FreezeThreads` — are kept as developer
tools on the maintainer's call, `WITH_TESTS` and stripped at release). `ActionResponse` and
`KnowledgeBank` ported as `action_dispatch` and `knowledge_bank_lifecycle`; the unit tests under
`Private/Tests` remain, `WITH_TESTS`-gated and now stripped by `push_to_public_v4.bat`.
`AConvaiConnectionTest`'s proxy-reuse checks (BasicReuse, DifferentCharacterID, DoubleAcquire,
ExpiryTimeout, RapidCycle) were not ported — an accepted, open coverage gap.

### What it was

**Confirmed 2026-08-06, re-confirmed 2026-08-20.** The PRD, ADR-0005, issue 12, the handoff and
the kickoff all state that the harness under `Source/Convai/{Public,Private}/Tests` "has never
compiled", because `PublicDefinitions.Add("WITH_CONVAI_TESTS=0")` is unconditional.

The macro is defined. **Almost nothing checks it, and what does check it is not the part that
runs.** Of the 46 files under the two `Tests` directories, three reference it —
`ConvaiTestMacros.h` itself, one block in `ConvaiTest_Audio.cpp:180`, one in
`ConvaiTest_EndToEnd.cpp:230` — plus `ConvaiPlayerComponent.h:200`, outside the Tests folders.
Nothing guarded the subsystem, which is the part that runs.

### The surface was larger than this entry recorded

The entry and issue 12 both describe the reachable surface as "the subsystem and its five console
commands". Reading every header in the tree, it is also **five reflected, Blueprint-facing
types**, all `CONVAI_API`, all in a `Runtime` module that ships:

| Type | What a customer sees |
|---|---|
| `UConvaiTestHarnessSubsystem` | `UGameInstanceSubsystem` + `FTickableGameObject`, no `ShouldCreateSubsystem` override, so it constructs and ticks on every game instance. Registers `Convai.Test.{Run,RunAll,Abort,Report,List}` and exposes 4 `BlueprintCallable` |
| `UConvaiReplayComponent` | `meta = (BlueprintSpawnableComponent)`, `DisplayName = "Convai Record Replay"` — an entry in the customer's Add Component menu, with ~12 Blueprint nodes |
| `UConvaiTestDebugLibrary` | `UBlueprintFunctionLibrary` — global Blueprint nodes under `Convai|Test|Debug` |
| `UConvaiAudioChunkTestProxy` | async Blueprint nodes under `Convai|Debug|Tests` |
| `AConvaiConnectionTest` | `Blueprintable` actor with 3 `BlueprintCallable` |

plus six `UConvaiTestBase` subclasses and several `BlueprintType` enums and structs.

### The two questions issue 12 left open, answered

**Side effects at construction:** `Initialize` calls `RegisterBuiltinTests()` — six
`RegisterTestClass` calls storing `UClass*`, no `NewObject` — then `RegisterConsoleCommands()` and
one `Log` line. `Tick` is a null check while no test is active. No file or network IO at
construction.

**Whether a command does something harmful in a customer build: yes.** `Convai.Test.Run` and
`RunAll` take a `CharacterID=` straight off the console and run live Convai sessions with the
packaged title's credentials, and the run writes a folder, `suite.json` and per-test text/JSONL
logs under the game's `Saved` directory (`ConvaiTestHarnessSubsystem.cpp:105,131,244`,
`ConvaiTestLogger.cpp:49-55,138`); `KnowledgeBank` writes and deletes a temp file. So in a shipped
title anyone who can reach the console could drive billed backend traffic and write files. That
is the security half of this finding, and it is what the fix closes.

### What changed, and what was kept

Deleting the tree was the first approach and was **reversed on the maintainer's instruction —
the harness is kept.** So:

- **Gated, not deleted:** all 46 files under `Source/Convai/{Public,Private}/Tests` are wrapped
  in `#if WITH_TESTS` / `#endif`. Nothing else in them changed.
- **Nothing was deleted.** `WITH_CONVAI_TESTS` stays at 0 with its comment corrected; the
  `GetSessionProxyForTesting()` block in `ConvaiPlayerComponent.h` stays. Turning that macro on
  would compile code that has never compiled, which is not this finding's business.
- **`ConvaiTests` in `Convai.uplugin` is `DeveloperTool`, not `Runtime`** — UBT excludes it
  wherever `bBuildDeveloperTools` is false, which is Shipping and Test.
- **`push_to_public_v4.bat`** removes `Source/ConvaiTests`, drops the module from the descriptor
  with `ConvertFrom-Json`, and fails closed on three things: the folder surviving, the descriptor
  still listing it, and any file under the in-plugin `Tests` folders missing its `WITH_TESTS`
  guard.

**Why `WITH_TESTS` and not `WITH_CONVAI_TESTS`.** UHT's preprocessor understands a fixed list of
names (`UhtCompilerDirective`, `UhtHeaderFileParser.cs:43-111`); anything else is
`Unrecognized`, and an unrecognized `#if` block is **skipped** (`:1193`), in every configuration.
Wrapping the UCLASSes in `#if WITH_CONVAI_TESTS` would therefore have dropped them from generated
code always, not just in Shipping. `WITH_TESTS` is on that list, and UBT defines it as
`Configuration != Test && != Shipping` (`UEBuildTarget.cs:6311,6334`) — the gate that was wanted,
under a name UHT scopes generated code by.

### What a customer loses

In a **Shipping or Test** build: the five `Convai.Test.*` console commands, the Record Replay
component and its Blueprint nodes, the `Convai|Test|Debug` library, the `Convai|Debug|Tests`
async nodes and the connection-test actor. A Blueprint that referenced any of them would fail to
resolve in those configurations. Nothing in the plugin's own `Content` or the project's references
any of them — checked by scanning both asset trees. In **Development and editor** builds nothing
changes at all.

### Verified

Both configurations built and both binaries read, against a control build of the same Shipping
target with the guards stashed out:

| Symbol | Shipping, control (unguarded) | Shipping, fixed | Development, fixed |
|---|---|---|---|
| `Convai.Test.Run` | 4 | **0** | 5 |
| `Convai.Test.RunAll` / `Abort` / `Report` / `List` | 2 / 1 / 1 / 1 | **0 / 0 / 0 / 0** | 2 / 1 / 1 / 1 |
| `ConvaiTestHarnessSubsystem` | 2 | **0** | 2 |
| `ConvaiReplayComponent` | 2 | **0** | 3 |
| `ConvaiTestDebugLibrary` | 2 | **0** | 2 |
| `ConvaiAudioChunkTestProxy` | 2 | **0** | 2 |
| `AConvaiConnectionTest` | 2 | **0** | 3 |
| `ConvaiPlayerComponent` (sanity: the plugin is in there) | — | 4 | — |

`Dev_WebRTC-Win64-Shipping.exe` 169,246,720 bytes unguarded against 168,810,496 guarded, a
436,224-byte difference. `Dev_WebRTCEditor Win64 Development`: Succeeded, no new warnings from
the Tests tree.

Module graph, from UBT's own `-Mode=JsonExport` on the `Dev_WebRTC` target — which is UBT
answering, not a grep:

| Configuration | Convai modules compiled |
|---|---|
| Shipping | `Convai`, `ConvaiVisionBase` |
| Development | `Convai`, **`ConvaiTests`**, `ConvaiVisionBase` |

The suite still runs on the Development build: `convai.tests.List` lists all 22 scenarios,
`test_exit_code.py` passes its three launches. `Convai.Test.List` and `Convai.Test.Run` in the
same launch produce nothing and `LogConvaiTest:` appears 0 times — before the fix,
`Convai.Test.Run` answered with a usage warning.

The release script's new steps were dry-run against a copy of the tree: `Source/ConvaiTests` and
the descriptor entry both removed, the other five modules and all 46 harness files untouched,
gates pass. Two negative controls, both caught: an unguarded file added under `Private/Tests`
fails the script's gate, and fails `test_run.py`'s new
`test_the_in_plugin_harness_is_guarded_out_of_shipping`. `test_run.py` is 11 of 11.

### The cost, stated

This is compile-time gating of the shipping module, which **ADR-0005 rejected**, and the reason
it gave holds: the `Convai` binary the suite runs against now has the harness compiled in and the
customer's Shipping binary does not, so they are no longer byte-identical. The divergence is
confined to passive surface — a subsystem that registers commands and ticks a null pointer — and
the alternative that would have avoided it, moving the harness to its own module, needs five
Convai-private headers (`Convai.h`, `ConvaiMergedObjectNavigation.h`,
`Utility/{ConvaiContextFormat,ConvaiSpatial,ConvaiUtf8String}.h`) made public and exported, which
ADR-0005 resists on its own terms. Recorded as an amendment on the ADR rather than left implicit.

---

## F17 — Nothing else in the process can record the master submix while a Connection is live

**Confirmed by measurement**, issue 03's monitor, 2026-08-06. Reproduces every run with a
live **Connection** and AEC enabled.

`FConvaiReferenceAudioThread::ProcessCapturedAudio` stops and restarts the one process-global
master-submix recorder roughly every 10 ms
([:216](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L216),
[:318](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L318)). Any second user of
that recorder is drained by it: an independent `StartRecordingOutput` / `StopRecording` over a
5 s window returned **1,024 samples against 466,432 rendered**.

Who this affects:

- **Customers.** `UAudioMixerBlueprintLibrary::StartRecordingOutput` is a Blueprint node. A
  game that records gameplay audio during a conversation gets almost nothing, with no error.
- **The plugin's own microphone path**, which is F6's mechanism exactly — `ReadRecordedBuffer`
  calls `StopRecording(nullptr, …)` on the same recorder. F6 arrived at this by deleting a
  submix asset; this shows the collision does not need the asset to be missing, only a second
  caller to exist.
- **The framework.** The monitor cannot measure the feed through the recorder while the feed
  is running, so issue 03's capture-ratio assertion is only valid with no live **Connection**.
  The monitor detects contention and reports it as its own condition rather than as capture
  loss.

**Consequence for F15.** The standalone F15 measurements were taken with no live Connection,
so they are not this. But it means any future capture-loss measurement has to state whether a
Connection was live, or the two get conflated.

**Fix direction:** the reference tap should not use the exclusive global recorder at all. An
`ISubmixBufferListener` composes — several can be registered on the same submix without
interfering — which is what `origin/debug/aec` already does (F13) and is now supported by
measurement rather than by preference.

### Two false findings this issue produced, and what killed them

Recorded because the negative-control rule is what caught both, and because a report read by
an agent is only as good as the things it refuses to claim.

- **"A live Connection was not receiving Reference Audio"** fired 2/2 runs. It was wrong.
  `RegisterLiveSession` runs the moment the connection thread starts, with the state still
  `Connecting` ([ConvaiConnectionSessionProxy.cpp:177](../../Source/Convai/Private/ConvaiConnectionSessionProxy.cpp#L177)),
  while `AttachReferenceAudioClient` only runs later from `OnSessionConnected`. The monitor was
  comparing reference clients against *registered* sessions rather than *connected* ones, so
  every Connection produced a deficit during its own handshake. Now counts only
  `EC_ConnectionState::Connected`. **F1 is therefore still not reproduced.**
- **"Capture lost 100% of rendered audio"** fired against a live Connection. That was F17
  above — the monitor's own instrument being drained — not the plugin's feed failing.

## F18 — F7 is overturned: the microphone *is* inside Reference Audio

**Refuted 2026-08-06, session 2, by the negative control it never had.** Both mechanisms this
entry proposed are impossible in UE 5.8 source, and the measurement it rests on reads the same
whether the microphone is audible or silent. F7 stands. The original entry is kept below in
full because the refutation is only legible next to it.

### The measurement could not tell the two cases apart

`mic_not_in_reference_audio` scored `ToneEnergyRatio` — tone energy divided by *total* energy.
That is a normalised quantity: it answers "what fraction of this mix is the tone", never "how
loud is the tone". Both scenarios now run, three times each, the second with the **Virtual
Mic**'s `SoundSubmix` deliberately cleared so it renders into the default chain:

| | Routed through `AudioInput` | Unrouted (control) |
|---|---|---|
| `tone_energy_ratio` | 0.848, 0.842, 0.842 | 0.802, 0.801, 0.801 |
| `tone_phase_rms` | 2.88e−6 | **0.177** |
| Attenuation, mic output → master mix | **−105.9 dB** | **−10.1 dB** |

**The ratio is the same in both columns. The level differs by 95.7 dB.** F18 read the ratio.
Run 0's 0.848 reproduces the original figure exactly, so this is the same instrument on the
same signal, not a different measurement disagreeing.

The control's `tone_energy_ratio_control` — the silence phase — was `0.000` in every run here,
as it was originally. That control proved the tone stopped when the mic stopped. It never
proved the tone was loud, and nothing else did either.

**−105.9 dB is not "quiet", it is absent.** `ProcessCapturedAudio` converts the reference to
int16 before `SendReferenceAudio`
([ConvaiReferenceAudioThread.cpp:231-267](../../Source/Convai/Private/ConvaiReferenceAudioThread.cpp#L231-L267)).
2.88e−6 × 32767 = 0.09, which rounds to zero. The microphone's signal cannot reach a canceller
through this path in the format the path uses.

### Both proposed mechanisms are impossible

The entry claimed `MuteMic`, being parentless, is a *default endpoint submix* summed into the
master output by `FMixerDevice::OnProcessAudioStream`. Two independent reasons it is not:

- **A plain `USoundSubmix` is never in that array.** The add is gated on
  `IsEndpointSubmix(&InSoundSubmix)` (`AudioMixerDevice.cpp:2779`), which is
  `IsA<UEndpointSubmix>() || IsA<USoundfieldEndpointSubmix>()` (`:1190-1193`). `UEndpointSubmix`
  is a sibling of `USoundSubmix` under `USoundSubmixBase`, not a parent of it
  (`SoundSubmix.h:339`, `:573`). `MuteMic` is a `USoundSubmix` — the guard reads
  `OutputVolumeModulation` off it, which only exists there.
- **The loop is off by default.** `EnableAudibleDefaultEndpointSubmixesCVar` initialises to `0`
  (`AudioMixerDevice.cpp:77`) and nothing in the engine config, the project config or the plugin
  sets it. The `for` over `DefaultEndpointSubmixes` at `:1730` did not execute in any run
  measured.

The CVar the handoff proposed as a one-line confirmation is also misnamed: it is
`au.submix.audibledefaultendpoints` (`AudioMixerDevice.cpp:79`), not
`au.EnableAudibleDefaultEndpointSubmixes`. Typed as written it is an unknown console command,
UE ignores it, and the run would have shown the tone ratio unchanged — which reads as
"mechanism refuted" for the wrong reason. **That check would have produced a false negative on
top of a false positive.**

### What actually happens

`RebuildSubmixLinks` gives a parentless, non-dynamic `USoundSubmixWithParentBase` the Base
Default submix as its parent (`AudioMixerDevice.cpp:2076-2087`), so the runtime chain is
`AudioInput(0 dB) → MuteMic(−96 dB) → BaseDefault → Master`. `MuteMic`'s gain is applied to its
own buffer before that buffer is mixed into its parent's
(`AudioMixerSubmix.cpp:1756-1783`), and −96 dB is well inside the `MIN_VOLUME_DECIBELS` clamp
of −160 (`AudioDefines.h:33`), so it is applied in full. −96 dB predicted, −105.9 dB measured;
the remaining ~10 dB is the same 16 kHz → 48 kHz path that costs the unrouted control 10.1 dB.

The guard's own diagnostic contributed to the error: it walks the *asset* graph and reported
"chain terminates at root: yes", which is true of the asset and false of the runtime graph.

### What this changes

- **There is no F18 → F12 chain.** F12 is unaffected — see the note added to it.
- **`MuteMic` needs no fix.** Not an explicit endpoint, not a volume change, not the CVar.
- **F19 is what survives**, and it was already written here as an unmeasured second-order
  consequence. It is now measured, and it is the real defect.

---

**Original entry, kept for the record. Its conclusion is refuted; its routing description and
its second-order consequence are correct.**

F7 concluded the microphone was safe because `/ConvAI/Submixes/AudioInput` has
**Parent Submix = `MuteMic`**, reasoning that a muted submix contributes silence to the master
mix. The routing is correct. The conclusion drawn from it is not.

Measured with a 1 kHz tone emitted through a **Virtual Mic** assigned that same submix:

| | Value |
|---|---|
| Submix chain, read at runtime | `AudioInput(0.0dB) -> MuteMic(-96.0dB)`, no parent above |
| Tone energy in master submix **while emitting** | **0.848** |
| Tone energy in master submix **after emitting stops** (control) | **0.000** |

The control is exactly zero, so all of that energy is the microphone's. **The submix named
`MuteMic` is set to −96 dB and still does not mute** — see the mechanism below. F7's inference
was drawn from the asset's parent name rather than from its behaviour.

**Consequence, and it is the important one.** F7's conclusion was used to eliminate the
leading explanation for *"player speech cut or swallowed"*, which is why issue 06 was moved up.
Issue 06 then found F12 independently: a canceller fed a reference containing the player's own
voice destroys that player, by 11.5 dB overall and to −73 dB in places. F18 says the plugin
routes the player's voice into exactly that reference. **F12 and F18 together are a complete
causal chain for the reported symptom**, from asset routing to destroyed near-end audio, and
each half was measured separately.

It also re-explains F6's "audible self-hearing" without needing a deleted asset: the
microphone is audible in the master mix in the *healthy* configuration.

**Mechanism, settled.** The full chain, read at runtime:

```
AudioInput(0.0dB) -> MuteMic(-96.0dB)      chain terminates at root: yes
```

`MuteMic` **is** authored at −96 dB. The asset is not misconfigured, and the obvious fix — "set
the mute submix to silent" — is already done.

The defect is that `MuteMic` has **no parent submix**. In UE a parentless submix is a *default
endpoint submix*, and `FMixerDevice::OnProcessAudioStream` sums those straight into the master
output in a second loop, separate from the main submix graph
(`AudioMixerDevice.cpp:1727-1741`):

```cpp
// Any endpoint submixes that don't specify an endpoint
// are summed into our master output.
Submix->ProcessAudio(Output);
```

So routing the microphone into a submix named `MuteMic` and setting it to −96 dB does not
remove it from the master mix; it re-adds it through the endpoint path. **The mute is
bypassed by the routing, not misconfigured.**

Fix directions, in rough order of confidence:

- Give `MuteMic` an explicit endpoint that discards audio, so it stops being a *default*
  endpoint submix and is no longer summed into the master output.
- The engine gates that loop on the `EnableAudibleDefaultEndpointSubmixes` CVar. Setting it to
  0 is a one-line mitigation to confirm the mechanism, but it is process-global and would
  affect any other parentless submix the game owns, so it is a diagnostic rather than a fix.
- Not: lowering `MuteMic`'s volume further. It is already at the floor.

**Unverified:** whether `AudioInput` also needs its own parent changed, and whether the game's
own audio relies on any parentless submix that the CVar mitigation would silence.

**Caveat on the instrument.** The **Virtual Mic** assigns `SoundSubmix` itself. It has to:
`UConvaiPlayerComponent` sets that submix only on the `UConvaiAudioCaptureComponent` it creates
in `OnComponentCreated`
([ConvaiPlayerComponent.cpp:128](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L128)),
never on a component adopted through `IConvaiAudioCaptureInterface`. So the measurement used
the same routing the real capture component gets. The first run of this scenario did *not*
assign it and produced the same result for the wrong reason; that run is not counted.

**Second-order consequence, unmeasured:** a customer supplying their own capture component
through the documented extension point receives no submix assignment at all, so their
microphone renders into the default chain — audibly, and into **Reference Audio**. That is a
worse version of this finding and nothing in the plugin warns about it.

*Session 2: measured. See F19 — this paragraph is the only part of F18 that was right.*

## F19 — A capture component supplied the documented way is inside Reference Audio

**Fixed 2026-08-20** on `fix/route-adopted-capture`. Tier 1, deterministic, measured against both
controls in the same build.

### What it was

**Confirmed by measurement**, scenario `mic_in_reference_audio_control`, 2026-08-06, 3 runs of 3.
This is F18's unmeasured second-order consequence, and it is the finding F18 should have been.

`UConvaiPlayerComponent` assigned `/ConvAI/Submixes/AudioInput` only to the
`UConvaiAudioCaptureComponent` it constructs itself in `OnComponentCreated`. A component adopted
through `IConvaiAudioCaptureInterface` — the documented extension point — never received it, and
an unassigned `USynthComponent` renders into the Base Default submix and straight into the master
mix.

| | Value |
|---|---|
| Tone RMS in master submix, plugin's own routing | 2.88e−6 |
| Tone RMS in master submix, adopted component | **0.177** |
| Attenuation, mic output → master mix | −105.9 dB vs **−10.1 dB** |

At −10.1 dB the microphone survives the int16 conversion, so it reaches every **Connection**'s
canceller as part of **Reference Audio**, and it is audible through the game's speakers. Nothing
warned.

### Assigning the submix is not routing it, and that is the whole finding

The obvious fix — assign `SoundSubmix` where the component is adopted — **was built, passed the
scenario, and moved no audio.**

`USynthComponent` copies `SoundSubmix` into the sound it plays, in `Initialize` and again in
`Start` (`SynthComponent.cpp:202`, `:480`), and `Start` returns immediately when the component is
already active (`:445-449`). A third party's component is normally already rendering by the time
`FindFirstAudioCaptureComponent` reaches it, so the assignment lands on a property nothing will
read again. The component keeps the routing it started with, for ever, while the property claims
otherwise:

```
                                   pointer-only fix      with the restart
adopted_component_routed           1, 1, 1               1, 1, 1     <- the structural assertion
mic_to_master_attenuation_db       -9.87, -9.89, -9.89   -105.80, -105.87, -105.87
tone_phase_rms                     0.1813, 0.1817, 0.1813  3e-06, 3e-06, 3e-06
```

**Identical on the assertion the old scenario made, 96 dB apart on the audio.** This is exactly
the failure mode the finding's own note warned about, reached by a different route than expected.

### What changed

`UConvaiPlayerComponent::RouteAdoptedCaptureComponent`, called from `SetAudioCaptureComponent`,
which is the single funnel every adoption route ends at — `FindFirstAudioCaptureComponent` and the
Blueprint call both pass through it. It:

- assigns `_FoundSubmix` when the adopted component is a `USynthComponent` with no submix, and
  **restarts it if it was already playing**, which is what makes the assignment take effect;
- leaves a submix the third party set deliberately alone, and warns — `StartVoiceChunkCapture`
  records `AudioCaptureComponent->SoundSubmix` and nothing else, so their audio will not be
  captured, and they should hear that from the plugin rather than discover it;
- warns when the component is not a `USynthComponent` (the interface says nothing about submixes,
  so this is legal and unroutable), and when the submix asset is missing (F6).

`SetAudioCaptureComponent` moved from `private:` to `public:`. It was already `BlueprintCallable`,
so the reflection system had it in every customer's reach while the C++ declaration said
otherwise; the header was disagreeing with what ships. Not widened for the test — the test is what
noticed.

### Verified

Tier 1, `--repeat 3`, no backend. All three arms in the same build on the same tone:

| Scenario | | Result |
|---|---|---|
| `adopted_mic_not_in_reference_audio` | **new** — unrouted at registration, then adopted | 3/3 pass, **−105.80, −105.87, −105.87 dB** |
| `mic_not_in_reference_audio` | the plugin's own routing | 3/3 pass, −105.90, −105.89, −105.90 dB |
| `mic_in_reference_audio_control` | unrouted, never adopted | 3/3 pass, −9.88, −9.89, −9.90 dB |
| `adopted_capture_component_routing` | the structural assertion | 3/3 pass, was 0/20 |

The adopted arm is the one this finding needed and did not have: it starts from exactly where the
control starts and differs only in whether the plugin gets to adopt it, so the 96 dB between them
is the fix and nothing else. The control still reads −9.9 dB, so the instrument can still see a
microphone in the master mix — a guard whose control has gone deaf proves nothing, which is F18's
lesson.

Adoption-dependent scenarios re-run for regression, since the fix restarts a component the plugin
does not own: `virtual_mic_adoption` 3/3, `speak_wav_through_virtual_mic` 3/3,
`injected_echo_reaches_the_mic` 3/3, `injected_echo_silent_control` 3/3.

**Not established, unchanged:** whether any shipped integration actually supplies its own
component. The defect is in the extension point, not in a known deployment.

---

## F20 — Ending a recording with nothing recorded aborts the process, and `EndPlay` does it for you

**Confirmed by two crashes**, 2026-08-06 session 2, deterministic on every attempt. Found by a
scenario calling `FinishRecording()` as ordinary teardown, then reproduced a second time by
merely destroying the actor.

**The trigger does not require anyone to call anything.**
`UConvaiPlayerComponent::EndPlay` runs `if (IsRecording) FinishRecording();`
([ConvaiPlayerComponent.cpp:453-454](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L453-L454)),
so a recording player component that goes away with an empty capture buffer takes the process
with it. That covers ending PIE, level travel, destroying the actor, and shutdown.
`StartRecording` empties `VoiceCaptureBuffer` and sets `IsRecording = true`
([:728-731](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L728-L731)), so the window is
open from the instant recording starts until the first chunk arrives — and stays open for the
whole session if the microphone never produces anything.

`FinishRecording()` is also `UFUNCTION(BlueprintCallable, Category = "Convai|Microphone")`
([ConvaiPlayerComponent.h:159-160](../../Source/Convai/Public/ConvaiPlayerComponent.h#L159-L160)),
and `StartRecording`'s own documentation tells the caller to use it afterwards — so the
explicit route is documented and the implicit route is automatic. Either way the process
asserts:

```
Assertion failed: (Index >= 0) & (Index < ArrayNum) [Array.h:1339]
Array index out of bounds: 44 into an array of size 44
  SerializeWaveFile()                          Audio.cpp:2335
  UConvaiUtils::PCMDataToWav()                 ConvaiUtils.cpp:1239
  UConvaiPlayerComponent::FinishRecording()    ConvaiPlayerComponent.cpp:755
  UConvaiPlayerComponent::EndPlay()            ConvaiPlayerComponent.cpp:456
  AActor::RouteEndPlay() / AActor::Destroyed() Actor.cpp:3230 / :3315
```

`SerializeWaveFile` sizes its output at 44 + `NumBytes` and then indexes `[44]` to `Memcpy` the
payload (`Audio.cpp:2335`). With `NumBytes == 0` the array holds exactly its 44-byte header and
the index is one past the end.

**The guard exists, three lines away, on the wrong call.**
`UConvaiUtils::PCMDataToSoundWav` opens with `if (InPCMBytes.Num() <= 44) return nullptr;`
([ConvaiUtils.cpp:1243-1245](../../Source/Convai/Private/ConvaiUtils.cpp#L1243-L1245)).
`UConvaiUtils::PCMDataToWav`, immediately above it at
[:1237-1240](../../Source/Convai/Private/ConvaiUtils.cpp#L1237-L1240), has none.
`FinishRecording` calls the unguarded one first, for a debug dump, and the guarded one after,
for its actual return value. Whoever added that guard hit this case and fixed one of the two
call sites.

**Build configurations.** `checkf` compiles to `CA_ASSUME` when `DO_CHECK` is 0
(`AssertionMacros.h:320`), so this is a hard crash in Editor and Development and a no-op in
Shipping and Test. Every developer integrating the plugin can hit it; a packaged Shipping game
cannot. That is a narrower blast radius than the callstack suggests and is stated here so the
severity is not overread.

**Second finding in the same function, unrelated to the crash.** `FinishRecording` writes
`Saved/AudioDebug/recorded_audio.wav` on every call
([ConvaiPlayerComponent.cpp:745-757](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L745-L757)),
unconditionally and in every configuration. A Blueprint node customers are told to call writes
the player's microphone audio to disk, with no opt-out and nothing in the documentation saying
so.

**Fixed, and verified.** `PCMDataToWav` now returns early on an empty buffer
([ConvaiUtils.cpp:1237-1252](../../Source/Convai/Private/ConvaiUtils.cpp#L1237-L1252)) — in the
shared function rather than at the one bad call site, because the other two callers guard
themselves today and nothing makes the next one do so. Re-run with the teardown restored: no
assertion, and `PCMDataToWav: no PCM data, no wav written` in the log where the crash used to
be.

The unconditional `Saved/AudioDebug/recorded_audio.wav` write is untouched and still wants a
CVar or a `#if !UE_BUILD_SHIPPING`.

### A second crash sits behind the first

**Confirmed, mechanism not established.** With the assert gone, the same teardown reaches
further down `FinishRecording` and dies differently:

```
Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0
  UConvaiPlayerComponent::FinishRecording()    ConvaiPlayerComponent.cpp:763
  UConvaiPlayerComponent::EndPlay()            ConvaiPlayerComponent.cpp:456
  AActor::Destroyed() / UWorld::DestroyActor()
```

Line 763 is `OutSoundWave->GetDuration()` under an `if (IsValid(OutSoundWave))` on 762, and
`PCMDataToSoundWav` returns null for this buffer, so the guard should hold and the attribution
is suspect — inlining would put `StopAudioCaptureComponent()` on 761 at the same address. That
function calls `ConvaiAudioCaptureComponent->Stop()` behind a `TScriptInterface` truth test
([ConvaiPlayerComponent.cpp:632-636](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L632-L636)),
which is the obvious candidate for a stale pointer during actor destruction — but that is a
hypothesis, not a finding.

**Only observed with an adopted capture component**, which is the unusual configuration, so it
may not be on the default path at all. Stated narrowly on purpose.

**Consequence:** the framework consequence below is unchanged. Fixing the first crash was
necessary and did not unblock teardown.

**Why there is no scenario for this.** The failure is a process abort, so a scenario asserting
on it takes the whole run down with it and reports nothing. Recorded as a finding with its
callstack instead. It becomes testable once the guard exists, as a regression test that
`FinishRecording` on an empty buffer returns null rather than dying.

**Consequence for the framework, and it is not small.** No scenario that starts recording can
destroy what it spawned, because destruction is the crash. `adopted_capture_component_routing`
therefore leaks its actor and its player component into every scenario that runs after it.

That is measured, not predicted. `virtual_mic_adoption` **passes when run alone** —
`observed_running=1`, 46,080 samples emitted, queue fully drained — and **fails in the same
build when the full suite runs**, with `adopted_capture_component_routing` ahead of it. Both
scenarios spawn a `UConvaiPlayerComponent` and call `StartRecording`, and the first one cannot
release it. Until F20 is fixed, suite order is load-bearing and scenario isolation is not
available at all. Issues 05 and 09 both spawn multiple player components and will hit this
first.

## F21 — The runner's process exit code does not track scenario outcome

**Fixed 2026-08-20** on `fix/runner-exit-code`. Tier 0, no backend.

### What it was

**Confirmed by measurement**, 2026-08-06 session 2, and again on 2026-08-20: a launch reporting
`CONVAI_TESTS summary passed=0 failed=1 setup_failed=0` exited **0**.

The handoff reports exit code 3 from a *passing* run that called
`RequestExitWithStatus(false, 0)`. With one scenario per process, six launches in one sweep:

| Scenario | Outcome | Exit code |
|---|---|---|
| `adopted_capture_component_routing` | fail | **3** |
| `reference_feed_capture` | fail | **0** |
| `reference_feed_capture_under_load` | fail | **0** |
| `mic_not_in_reference_audio` | pass | 0 |
| `mic_in_reference_audio_control` | pass | 0 |
| `virtual_mic_adoption` | pass | 0 |

### The cause, from UE 5.8 source

`FPlatformMisc::RequestExitWithStatus(/*Force=*/false, Code)` **cannot deliver a code on
Windows.** It ends at `PostQuitMessage(ReturnCode)`
(`Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp:1520`), and UE's own pump —
`WinPumpMessages`, `Runtime/ApplicationCore/Private/Windows/WindowsPlatformApplicationMisc.cpp:204`
— dispatches `WM_QUIT` without ever reading `wParam`. Nothing else carries the value:
`GuardedMain` returns `EngineInit()`'s `ErrorLevel` (`Runtime/Launch/Private/Launch.cpp:204`),
which is 0 for any launch that started, and **there is no `GExitCode` in UE 5.8**. Every engine
caller that actually wants a non-zero code passes `Force=true`, which is `TerminateProcess`.

So "the code was never set" — not "the process never got to return it". The two are separable
and were separated: the 0s are this, and the 3 is the other path,
`RequestExit(Force=true)` → `GIsCriticalError ? 3 : 0`. `adopted_capture_component_routing` was
the one scenario that crashed in teardown, which is the `EndPlay` defect closed as I7.

### What changed

`ConvaiTestExit::Arm(Code)` in `Complete()` records the code; `DeliverAtShutdown()`, called from
`FConvaiTestsModule::ShutdownModule`, forces it. Three constraints picked that seam:

- **After the teardown F25 hangs in.** Forcing from `Complete()` would exit before
  `GEngine->PreExit()` and hide the hang. `UnloadModulesAtShutdown` runs after the world, the
  rendering thread and the shader library are gone, and before `AppExit` tears `GLog` down. A
  launch that hangs never reaches it, stays killed by `run.py`, and still counts in
  `launches_hung_on_exit` — F25 remains exactly as measurable as it was.
- **Only when the code is non-zero.** A green launch shuts down on the untouched path.
- **`atexit` does not work here.** `UnloadModulesAtShutdown` does not free the module DLLs — the
  OS does, at process exit — and the CRT skips a DLL's `atexit` table when the process is
  terminating. An `atexit` version was built and measured first: all three launches still exited
  0.

One behavioural change beyond the code itself: **a filter that selected no scenario now exits 1**
and logs `CONVAI_TESTS selected no scenarios`. Nothing ran, so nothing was proved, and a typo in
a filter used to read as a green run. No assertion constant, fixture or arm was touched.

### Verified

Built `Dev_WebRTCEditor Win64 Development`; `python Source/ConvaiTests/test_exit_code.py` ran
three launches, no backend, no character:

| Launch | Summary | Exit before | Exit after |
|---|---|---|---|
| `virtual_mic_adoption` | `passed=1 failed=0 setup_failed=0` | 0 | **0** |
| `no_such_scenario_f21` | `passed=0 failed=0 setup_failed=0` | 0 | **1** |
| `adopted_capture_component_routing` | `passed=0 failed=1 setup_failed=0` | 0 | **1** |

`run.py` agrees with the report on both sides, and the forced exit does not read as a hang:

| Sweep | Report | `run.py` exit | `launches_hung_on_exit` |
|---|---|---|---|
| `.testruns/20260820_211749_a8e89bd0` | `virtual_mic_adoption: 1/1 passed` | 0 | 0 |
| `.testruns/20260820_211826_a8e89bd0` | `adopted_capture_component_routing: 0/1 passed` | 1 | 0 |

The shutdown tail of the forced launch is line-for-line identical to the pre-fix launch's, down
to `LogShaderCompilers: Display: Exiting ShaderCompilingThread` — nothing the log carries is lost
by terminating there. `python Source/ConvaiTests/test_run.py`: 10 of 10.

**Left deliberately undone.** `run.py` still derives failure from the reports and never reads the
exit code. The report is the result; the exit code is for humans and CI, and a sweep that graded
it would break on every launch F25 kills.

---

## Unrelated bugs found in the same log

## F26 — In-engine, cancellation lets the character's voice through one run in five

**Confirmed by measurement**, `aec_echo_only_internal` / `aec_echo_only_none`, 2026-08-06, five
paired runs. This is the reported symptom *"character responds to its own voice"*, reproduced
with an occurrence rate, and it is the first finding the framework has produced that the offline
tier could not.

The player says nothing. The character talks, its voice reaches the master submix, and Injected
Echo puts a delayed copy at gain 0.8 back into the microphone. The question is whether the
character's own speech returns as the *player's* transcript.

| Run | `Internal` echo peak | `Internal` leaked | `None` echo peak | `None` leaked |
|---|---|---|---|---|
| 0 | 0.175 | **0** | 0.159 | 27 |
| 1 | 0.191 | **5** | 0.178 | 21 |
| 2 | 0.175 | **0** | 0.176 | 28 |
| 3 | 0.177 | **0** | 0.177 | 25 |
| 4 | 0.185 | **0** | 0.160 | 19 |

**The control fires 5/5 and that is what makes the rest readable.** With `AECType=None` the leak
appears every single time, 19 to 28 transcripts, so the scenario can detect the failure it is
looking for. The PRD's V3 — "fails with `None`, passes with `Internal`" — is satisfied, and
`Internal` passing 4/5 is evidence rather than an absence.

**Every run carried a real echo**, peak 0.16 to 0.19 against a 0.01 floor, so no run in this
table is the empty-fixture case that an earlier version of this scenario passed by mistake.

**Why it matters more than the rate suggests.** F14 measured this same canceller offline at 40
to 49 dB ERLE, converging in 200 ms, degrading gracefully under nonlinearity and noise. The
algorithm is healthy. Integrated into the plugin it leaks one run in five. That is the
bisection ADR-0004 asked for, resolved in the direction of *plumbing*: the candidates are the
reference feed's puncture (F3), its wall-clock cadence (F4) and its intermittent near-total loss
(F15, three runs in thirteen — a rate in the same region as this one).

### The reference feed looks identical on leaking and clean runs

**Measured 2026-08-06.** Issue 03's monitor now rides along with the scenario, so every run
records the feed's health next to its leak count. Ten `Internal` runs:

| Run | Leaked | Echo peak | Connection deficit | Wall gap max | Wall gap σ | Rendered peak |
|---|---|---|---|---|---|---|
| 002 | **13** | 0.191 | 0 | 10.53 ms | 4.930 | 0.249 |
| 006 | **20** | 0.180 | 0 | 11.02 ms | 4.927 | 0.230 |
| 8 others | 0 | 0.156–0.198 | 0 | 10.6–12.9 ms | 4.922–4.929 | 0.20–0.26 |

The leaking runs are inside the clean range on every column. **F1 is eliminated as the
mechanism**: the deficit is zero in all ten, so no Connection went unfed, on the leaking runs
least of all.

Rate on this larger sample: **2 leaks in 9 valid runs**, consistent with the 1/5 above. The
tenth was rejected by the fixture floor — echo peak 1.8e-05 and rendered peak 2.3e-05, meaning
the character produced no audio — which is the 0.01 floor working as intended rather than a
result.

### Correction to issue 03's monitor: its wall-gap fields cannot see F4

`FReport::WallGap*` is documented as "where F4's jitter lives"
([ConvaiReferenceFeedMonitor.h:59-65](../../Source/ConvaiTests/Public/ConvaiReferenceFeedMonitor.h#L59-L65)).
It is not. Those gaps are measured between **submix buffer callbacks on the ground-truth
listener**, which is the mixer's own render cadence — and the standard deviation came out at
4.922 to 4.929 ms across ten runs, identical to three decimals, because that is a fixed
hardware-driven interval. F4's jitter is in `FConvaiReferenceAudioThread::Run`'s 2 ms poll and
its asynchronous dispatch, which this instrument never touches.

So F4 is **not** eliminated by the table above; it was never measured. Neither were F3 or F15,
because F17 means the recorder-based figures are meaningless while a Connection is live.

**What the next measurement needs:** a tap on the plugin's own reference dispatch rather than on
the mixer. `GetReferenceAudioStatus()` already exists for this — it was added in session 1 for
F2 — and reporting chunk count and inter-chunk delay from inside the thread would give the
cadence the monitor is currently only claiming to measure.

*Built in session 3. See **F27**, which measures that cadence and does not explain this.*

**Caveat on the sample.** One machine, one character, one map. The rate establishes that the
leak happens, not how often.

## F27 — The reference feed is healthy on the runs that leak

**Measured 2026-08-07**, issue 13's tap, ten `aec_echo_only_internal` runs on the Landing map.
This is a negative result and it is the point of the issue: F26's three remaining suspects are
all in the reference feed, and the feed is now measurable from inside while a **Connection** is
live, which F17 said no external instrument could do.

`FConvaiReferenceAudioThread` now reports what it sent and how it was paced, through
`GetReferenceAudioStatus()`. Per run, against the leak count in the same report:

| Run | Leaked | Echo peak | Capture ratio | Dispatch gap mean | Dispatch gap σ | Dispatch gap max | Recorder off max |
|---|---|---|---|---|---|---|---|
| 000 | **16** | 0.190 | 0.991 | 11.51 ms | 0.63 ms | 13.57 ms | 0.49 ms |
| 004 | **1** | 0.184 | 0.993 | 11.32 ms | **123.59 ms** | **8988.56 ms** | **28.47 ms** |
| 7 others | 0 | 0.15–0.19 | 0.991–0.993 | 11.32–11.49 ms | 0.48–0.73 ms | 13.17–13.90 ms | 0.49–0.87 ms |
| 001 | — | **0.0001** | 0.991 | 11.33 ms | 0.57 ms | 13.30 ms | 0.50 ms |

Run 001 is not a result: its echo peaked at 1.1e-04 against the 0.01 floor, so the microphone was
clean and the fixture check rejected it. Nine valid runs, **two leaked** — the same 2 in 9 F26
reported, from an independent sweep.

**The larger leak had a textbook feed.** Run 000 leaked sixteen transcripts with every cadence
column inside the clean range. Whatever lets the character's voice through, it is not visible in
the feed on that run.

### What the three suspects actually cost

**F3 — the recorder off across conversion and fan-out: real, and small.** The recorder is stopped
for **0.06 to 0.08 ms** on average per cycle, against an 11.4 ms cycle, and the capture ratio is
**0.991 to 0.993** in all ten runs. So the puncture drops about **0.9%** of rendered audio. F3's
mechanism is confirmed exactly as described and its magnitude is now known: it is not a
cancellation-breaking hole.

**F4 — the wall-clock poll: confirmed as a bias, refuted as jitter.** The dispatch interval is
**11.3 to 11.5 ms**, never the 10 ms `CaptureInterval` asks for, because `Run()` sleeps 2 ms and
tests `elapsed >= 10 ms` — the interval quantises up to the next 2 ms tick. It is *steady*: σ is
0.48 to 0.73 ms across eight of nine valid runs. The second-order effect is that chunks leave in
**bursts**: `ProcessCapturedAudio` drains its buffer in a `while` loop, so 1.14 chunks per
dispatch on average go out back to back rather than one every 10 ms. Measured, not blamed.

**F15 — near-total capture loss: did not fire once in ten runs.** The ratio never left 0.991–0.993.
This is the same instrument in the same position as F15's — `StopRecording(nullptr, …)` on the
master submix — so the failure mode was reachable and did not occur. Issue 14's lead that F15's
23% and F26's 22% might be the same runs is **not supported**: 0 of 10 runs lost capture and 2 of
9 leaked.

### The one run that is not like the others

Run 004 is an outlier on every cadence column at once — a **9.0 second** gap between dispatches,
σ of 123 ms against 0.6 ms elsewhere, and a recorder-off window of 28 ms against 0.9 ms. It also
leaked, once.

**It is one run and it is confounded.** Its capture window was 59.8 s against 26–41 s for every
other run, so the whole process was stalled, not the feed alone; a hitch that halts the audio
thread halts this loop with it. Cause and common cause cannot be separated at n=1. Recorded
because a 9-second silence in the far-end signal is exactly the condition under which a canceller
would misconverge, and because no instrument before this one could have seen it.

**What would settle it:** more runs, and a discriminator that separates a feed stall from a
process stall — the monitor's rendered-buffer count over the same second would do it, since a
process-wide hitch stops both and a feed-only stall stops one.

### Controls

`reference_feed_capture` on `/Engine/Maps/Entry`, three runs, no **Connection** and therefore no
capture thread: **all twelve metrics read zero**, while the mixer rendered at peak 0.74–0.85 and
an uncontended recorder captured 99.6%. So the tap reads zero exactly when the feed does not
exist, and is not reporting something ambient.

The pair's `None` run is **not** a control for these numbers, and the first version of this work
assumed it was. `AEC` and `AECType` are separate custom params
([ConvaiUtils.cpp:843](../../Source/Convai/Private/ConvaiUtils.cpp#L843)), so `AECType=None`
selects a null canceller and leaves the feed running: the `None` run reports `ref_aec_enabled=1`
and 3,318 chunks sent. It controls the canceller, which is what issue 05 built it for.

**Internal consistency, three ways.** Captured samples ÷ 2 channels ÷ 480 equals the chunk count
to within one in every run; dispatch count × 1.14 chunks equals it too; and the recorder-off
fraction (0.07 ms of 11.4 ms = 0.6%) matches the capture loss (0.9%). The three are derived from
different counters at different points in the loop, so agreeing is evidence the instrument is
measuring what it claims.

**What this leaves.** F1 was eliminated in session 2, and F3, F4 and F15 are eliminated here as
the mechanism for the runs that actually leaked. F26 has no surviving suspect in the reference
feed. The remaining candidates are on the other side of `SendReferenceAudio` — alignment between
the far-end stream and the microphone stream, and the canceller's own state in-engine — neither
of which this repository can currently observe.

## F28 — `origin/debug/aec` is written against an architecture the plugin no longer has

**Confirmed by reading and by symbol table, 2026-08-07.** Issue 14 asked for ten runs on the
current branch and ten on `origin/debug/aec`, same machine, same everything. That comparison
cannot be run: the branch is not a variant of the current code, it is a rewrite of a version that
has since been replaced. Recorded before anything was built on it.

`origin/debug/aec` is a single commit (`340687d3`, "chore: add new submix") on `8744fdb5`, which
is also its merge base with `feat/test-framework`. Everything below changed on the main line
*after* that point.

**It binds the reference tap to one client per process, and the plugin now has one per
Connection.** The branch constructs `FConvaiSubmixReferenceListener(Self->ConvaiClient.Get())`
and stores a single `SubmixReferenceListener` on the subsystem, guarded by
`if (Self->SubmixReferenceListener.IsValid()) return;` — so the first **Connection** wins and
later ones are never fed. `UConvaiSubsystem::ConvaiClient` **no longer exists**: the member is
gone from [ConvaiSubsystem.h](../../Source/Convai/Public/ConvaiSubsystem.h) and
`ConvaiSubsystem.cpp` contains zero uses of it. Each **Connection**'s client is now owned by its
**Session Proxy** —
[ConvaiConnectionSessionProxy.h:254](../../Source/Convai/Public/ConvaiConnectionSessionProxy.h#L254),
reached through `GetClient()` at
[:226](../../Source/Convai/Public/ConvaiConnectionSessionProxy.h#L226) — and registered through
`AttachReferenceAudioClient(SessionProxy->GetClient())`
([ConvaiSubsystem.cpp:1181](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1181)).

This is not hypothetical on the map the suite runs. F27's sweep measured
`feed_client_sends / feed_chunks_sent` ≈ **1.99** across all ten runs, so two distinct clients
were being fed the whole time.

The same commit also changed `FConvaiReferenceAudioThread`'s constructor from
`(ConvaiClient*, UWorld*)` to `(UWorld*)` plus `AddClient` — visible in the branch's own diff
context — which is the same architectural move. The branch predates it.

**It also calls a symbol the shipped import library does not export.**
`ConnectionParams.Client->SetStreamDelay(AECStreamDelayMs)` at its `ConvaiSubsystem.cpp:464`,
against a declaration the branch adds to
`Source/ThirdParty/ConvaiWebRTC/include/convai/convai_client.h`. The binaries are byte-identical
between the two branches — `git diff --name-only` lists only the header — and
`convai_client_dll.lib` exports exactly 15 `convai::ConvaiClient` methods:

```
Connect  Disconnect  GetActiveAECType  Initialize  IsConnected  SendAudio  SendImage
SendMessageWithLabel  SendRawMessage  SendReferenceAudio  SetConvaiClientListner
SetLogTag  StartAudioPublishing  StartVideoPublishing  StopVideoPublishing
```

`SetStreamDelay` is not among them. The only `*StreamDelay*` export in the whole library is
`livekit::AudioProcessingModule::setStreamDelayMs`. The branch was developed against a DLL build
that was never committed here.

**What it does not change.** The *idea* is untouched and still good: an `ISubmixBufferListener`
composes where the global recorder does not (F17), and its header's design note — that the submix
callback fires as the device consumes audio, so the reference shares the echo's timeline — is the
right argument. What is dead is the diff, not the design.

**Consequence for issue 14.** Rewritten. The comparison is now a runtime switch inside
`FConvaiReferenceAudioThread` rather than a branch checkout, which is a strictly better
experiment: one binary, one build, one custom param different, so nothing else can move. See
F29.

**Caveat, and it is why this matters less than it did.** Issue 14's motivation was that the
listener removes F3, F4 and F15 at once. F27 measured all three directly and none of them is the
mechanism behind the runs that leak. So this comparison is now a control on a hypothesis already
weakened, not a candidate fix. Its value is that it is cheap and it settles the question.

## F29 — A perfect reference feed does not stop the leak

**Measured 2026-08-07.** Issue 14's comparison, run as a runtime switch rather than a branch
checkout because F28 says the branch cannot be built against this code. One binary, one custom
param different: `AECReferenceTap=Recorder` is the shipping tap, `Listener` registers an
`ISubmixBufferListener` on the main submix and starts no runnable at all. Conversion, resample,
framing and fan-out are one shared function called by both, so nothing downstream of the samples
differs.

### The tap did what it claims

Ten `aec_echo_only_internal` runs each. Every run carried real echo — peak 0.14 to 0.18 against
the 0.01 floor — and `feed_tap_is_listener` records which path each took rather than trusting the
flag.

| | Recorder | Listener |
|---|---|---|
| `feed_capture_ratio` | 0.98–1.00 | **1.000** in all ten |
| `feed_recorder_off_max_ms` | 0.39–10.43 | **0** in all ten |
| `feed_dispatch_gap_mean_ms` | 11.33–11.55 | **5.33** in all ten |
| `feed_dispatch_gap_stddev_ms` | 0.55–0.71 (one run 93.24) | 4.87–4.92 |
| `ref_recorder_contended` | 1 in all ten | **0** in all ten |
| `feed_client_sends / feed_chunks_sent` | 1.98–2.00 | 1.96–2.00 |

So F3's 0.9% puncture is gone, F17's contention is gone — a second master-submix recorder works
again while a **Connection** is live — and the cadence moves onto the device clock, where its
5.33 ms and 4.9 ms σ match the monitor's independent submix figures. The fan-out survives: both
**Connections** are still fed, which is precisely what `origin/debug/aec`'s one-client design
would have broken (F28).

### And it did not reduce the leak

| Tap | Runs leaked | Leaked transcripts on those runs |
|---|---|---|
| Recorder | **0 / 10** | — |
| Listener | **3 / 10** | 40, 16, 18 |

Fisher's exact test, two-tailed: **p = 0.21**. That is not a difference, in either direction.

**This design could not have produced a significant result and that is worth saying plainly.**
With three leak events across twenty runs, the most extreme possible split still gives p = 0.105,
so no allocation of these events would have cleared 0.05. Separating a ~5% rate from a ~30% one at
80% power needs roughly 35 runs per arm — about six hours of wall clock at 60 s per launch. The
10-per-arm sweep the issue asked for was never enough, and running it was still the right call
because it is what showed the tap works.

**When the listener leaks, it leaks as hard as no canceller at all.** The `None` control produced
7 to 31 transcripts; the listener's leaking runs produced 16, 18 and 40. Whatever fails on those
runs is not partial degradation.

### Controls

`aec_echo_only_none`, three runs on each tap, same binary:

| Tap | Control fired |
|---|---|
| Recorder | **2 / 3** — 7 and 20 transcripts; one run returned none |
| Listener | **3 / 3** — 31, 23, 21 transcripts |

So the scenario can still detect the failure it looks for on both taps, and the `Internal` columns
above are readable. The recorder-side miss is recorded rather than smoothed: its echo peaked at
0.16, so the fixture was sound and no player transcript came back anyway. Across all sessions the
control has now fired 7 times in 8, which is a property of the control worth tracking rather than
assuming.

### Two caveats, and the first is load-bearing

**This measured the listener alone, not what the branch intended.** `origin/debug/aec` added the
listener and the `SetStreamDelay` call in the same commit, and F28 says that call cannot link
against the shipped import library. The listener moves the reference onto the device clock, which
changes the far-end signal's alignment against the microphone's — and a fixed delay hint is
exactly what one adds to correct that. The branch author's own parameter comment says so:
*"the submix reference leads the mic echo by the device output latency; a fixed delay can trim
that residual instead of relying on the APM's internal estimator."* So a reasonable reading of
these three leaking runs is that the tap is half of a change, and the half that compensates for it
is the half that will not build here. Testing that needs a DLL that exports `SetStreamDelay`.

**Only the echo leak was measured. The near-end half was not measured.** Stated in those words
because issue 14 requires it: a gapless reference makes the far end more consistently active,
which is the condition under which F12's near-end suppression is *worst*, and this sweep contains
nothing that would have noticed the player being destroyed. Issue 05's double-talk variant is the
scenario that would catch the trade and it is still not built. Nothing here says the listener is
safe for "player speech cut or swallowed" — it says nothing about it at all.

### One thing it did settle

Recorder run 000 stalled **6.2 s** between dispatches with a 10.4 ms recorder-off window and a
dispatch σ of 93 ms, and **did not leak**. F27 recorded a 9 s stall that did leak, at n=1 and
confounded by a process-wide hitch. There is now a stall of the same kind on the other side of the
outcome, so the stall-causes-the-leak reading is weaker still.

### Where this leaves F26

The feed can be made perfect and the character still hears itself. Combined with F27 — the feed
was already healthy on the runs that leaked — the reference feed is now eliminated as F26's
mechanism twice over, once by measuring it and once by fixing it. What remains is on the far side
of `SendReferenceAudio`: stream alignment, and the canceller's own state in-engine. Neither is
observable from this repository today, and the `SetStreamDelay` seam that would probe the first is
the symbol the DLL does not export.

## F30 — V5 passed, and the report's prose was the part that misled

**Run 2026-08-07.** The PRD's **V5** — *"a fix agent, given only `report.json`, can locate and
verify a fix for a seeded bug without reading the framework's source"* — had never been run, and
it is the gate on running agents in a loop across both repositories. It passes.

### The experiment

A one-character regression was seeded in the plugin:
`FindFirstAudioCaptureComponent`'s guard became `AudioCaptureComponents.Num() > 1`
([ConvaiPlayerComponent.cpp:469](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L469)), so
adoption required *two* components implementing `IConvaiAudioCaptureInterface` where the normal
case has one. Realistic on purpose: it is one character, it reads as correct at a glance, and it
breaks only the documented extension point while leaving the default path working.

Baseline `virtual_mic_adoption` **3/3 pass**. Seeded, **0/3**, with `virtual-mic-not-adopted`
firing 3/3. A deterministic oracle, which the live scenarios are not.

A fix agent was given the merged `report.json` and the plugin source, and barred from
`Source/ConvaiTests/`, `.scratch/`, `Docs/handoff/`, and from `git diff` / `git status`.

### Result

It found the exact line, fixed it, rebuilt, and re-ran to **3/3 passed** with no findings. The fix
restored the committed original byte for byte — verified afterwards by `git status` coming back
clean, so it was the right fix rather than a different one that happened to go green.

Its route: the `evidence_samples` text names the interface, the consuming class, and the method
that should have fired, and **two greps** got from there to the function. `occurrence_rate: 3/3`
told it up front the failure was deterministic, so it never went looking for a race.

**What the report could not do:** name the defect. It located the *function*; the off-by-one came
from reading the line. That is the correct division of labour and not a complaint.

### The two real defects it exposed, both now fixed

**The evidence text asserted a mechanism it had no evidence for.** It read *"a
UConvaiVirtualMicComponent … was registered on the owning actor **before** UConvaiPlayerComponent"*.
True of the fixture, and it reads as a claim that *ordering* is the variable — an initialisation
race. Coherent, and wrong. The agent reported it would have chased that had the `> 1` not happened
to appear in its grep output. **A finding's evidence is read as causal whether or not it is meant
that way**, and this one laundered a setup detail into a suspected mechanism. Rewritten so every
clause is a fact about state rather than sequence.

**The report carried the outcome of the failing decision and never its input.** "Not adopted" does
not distinguish *no component was found* from *one was found and rejected* — different bugs in
different functions. `adoption_candidates` is now reported, and on the seeded run it reads **1**,
sitting in the same report as a `> 1` comparison.

Both verified by re-seeding: the finding now says *"the owning actor carried 1 component(s)
implementing IConvaiAudioCaptureInterface … so the component was on the actor and was not
adopted."* Then restored, rebuilt, and re-run to 3/3.

### The larger recommendation, not taken yet

The agent's own preference was a **captured log excerpt around the failure window**, over a
bespoke metric. The plugin already logs the two branches — `"Started alternative audio capture"`
and `"Started default audio capture"`
([ConvaiPlayerComponent.cpp:568](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L568),
[:573](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L573)) — and either string in the
report would have said which branch ran in one token. That generalises across every scenario where
`adoption_candidates` helps one. Recorded for issue 08 rather than built here.

### What V5 does not establish

One bug, seeded by the same person who wrote the report format, deterministic, and offline. It
says the report is actionable for a failure the suite already models well. It says nothing about
the intermittent live findings — an agent handed F26 at 3/10 has a noisy oracle and no mechanism,
which is the case F27 and F29 just failed to improve.

The `git diff` ban is also an artefact: a real fix agent in the maintainer's loop would have git,
and against an uncommitted seed that would have been the whole answer. Seeding through a commit
would remove the artefact and is what a repeat should do.

## F35 — A fixed stream-delay hint does not restore in-engine cancellation

**Measured 2026-08-07, and it kills the hypothesis F33 left as the only survivor in its stated
form.** `ConvaiClient::SetStreamDelay` is now reachable: the plugin reads an `AECStreamDelayMs`
custom param in `FConvaiConnectionThread::Run` after `Initialize` and calls it once per client,
logging `SetStreamDelay(N ms) -> accepted`. The scenario records the resolved value per run as
`aec_stream_delay_ms` (−1 means the call was never made, distinct from a hint of 0), so every
run in a sweep is labelled by the report rather than by the shell history.

Coarse scan, `aec_erle_internal` on Landing, n=1 per arm, then n=3 on baseline and the two most
promising values:

| Arm | n | `aec_atten_db` | `aec_atten_at_peak_db` |
|---|---|---|---|
| no hint | 1+3 | 3.27–5.90 | 3.07–13.14 |
| 0 ms | 1+3 | 4.61–7.54 | 3.09–15.61 |
| 60 ms | 1 | 5.05 | 24.65 |
| 120 ms | 1 | 3.25 | 12.64 |
| 180 ms | 1 | 7.04 | 6.39 |
| 240 ms | 1+3 | 3.10–7.76 | 8.52–14.37 |
| 300 ms | 1 | 3.41 | 10.26 |

**Every hint arm overlaps the no-hint arm on every attenuation column.** The coarse scan's
apparent signal (baseline at-peak 3.07 vs hint arms 6–25) evaporated at n=3, which is rule 3
doing its job. The leak fired inside hint arms too — 11 transcripts at 60 ms, 19 at 300 ms, 2/3
runs at 0 ms, 1/3 at 240 ms — and every leak-rate comparison came back `under-powered`, so the
transcript oracle decided nothing here; the attenuation columns are the result. Nothing
approached the 40–49 dB the same canceller does offline (F14), where F5 already measured the
hint on a *stable* delay to be worth −0.1 to +2.3 dB.

**What it leaves.** The alignment hypothesis survives only in a form a fixed hint cannot fix —
see F36, which this sweep's traces produced. The actuator stays wired and harmless: no param, no
call.

## F36 — Cancellation converges, then loses alignment: the mic stream runs slow against the reference

**Measured 2026-08-07, from F35's own traces, and it is the named mechanism F26 has lacked for
five sessions.** Three observations, each measured, that fit one story.

### The canceller starts strong and decays to a floor, every run

The per-second `aec_block_series` from every failed `aec_erle_internal` run in the sweep (eight
traces, both reference taps) has the same shape: the **first block after the character speaks
reads 12–25 dB** (seven of eight traces), decays within 5–15 s to a hard floor of
`~335>~290 (+1.2dB)` — the same RMS pair to ±5 across runs, a steady far-end being cancelled at
1.2 dB — and **recovers transiently to 8–15 dB only during loud speech bursts**. All twenty
blocks in these runs are loud blocks, so the floor is not a silence artefact.

That is a converged filter progressively losing alignment and re-locking when the matched filter
gets strong far-end activity, not a filter that never converges. **It refines F33**: subtraction
does run in-engine — for seconds at a time.

### The microphone stream is 1–2.5% short, in every run, on both taps

Across all 19 valid shipping-path runs of the sweep, the canceller's own counters over the same
window read `aec_mic_chunks` **1939–1971** against `aec_ref_chunks` **1977–2017**. The mic
stream delivers 1–2.5% less audio than the reference over the same window — a relative drift of
**10–25 ms per second of audio**, orders beyond any clock-skew tolerance. AEC3 re-estimates
delay on strong far-end activity, which is exactly the transient recoveries above.

The loss site is the mic capture cycle: `UpdateVoiceCapture` stops the AudioInput-submix
recorder, then reads, converts, resamples and **sends to the DLL**, then restarts it — every
~10–16 ms, with the recorder off for the whole processing tail
([ConvaiPlayerComponent.cpp:510](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L510)).
F27 measured the reference's tighter stop/start cycle at 0.6–0.9% loss; the mic's heavier gap
loses more.

**It re-reads F29.** A gapless *reference* against a punctured *mic* increases relative drift —
so the listener tap could not help and directionally should hurt, which is what F29 saw (3/10
leaked listener vs 0/10 recorder, p = 0.21). The two punctured streams partially cancel each
other's drift; fixing one side alone breaks the balance.

### A gapless mic tap produces the first offline-scale cancellation ever seen in-engine

`MicCaptureTap=Listener` (new, default off) replaces the mic recorder cycle with an
`ISubmixBufferListener` on the AudioInput submix, mirroring F29's reference tap. Five
consecutive runs with the tap on put `aec_atten_at_peak_db` at **37.0, 40.2, 45.6, 53.2 and
54.1 dB** — F14's offline range, which no recorder-path run in any session has approached
(session-wide recorder maximum: 25.6). When the streams align, the in-engine canceller subtracts
at full strength.

**And the tap as built is not a fix.** Sending device-clocked mic audio through
`SendAudio` collided with the DLL's paced publish path wherever the send ran: from the render
callback it stalled the whole mixer 14–23 s per run; drained on the game thread it dilated
headless game time ~2.2× (the DLL pushes at real time and the caller absorbs the pacing);
drained on the audio thread the dilation remained. In all tap runs the reference feed's capture
ratio halved (0.52–0.55, against 0.99 shipping and 1.000 in F29), the window's wall length
stretched to ~44 s, and window-mean attenuation stayed ~3 dB because the far-end stream was now
the starved one. One submix instance confirmed (a double-registration explanation is
eliminated); the remaining suspects are `aecMutex_` contention between `SendAudio` and
`SendReferenceAudio` and the publish pacing itself. **No tap run's pass or zero-leak is credited
as improvement** — the window semantics differ under dilation.

### What would settle it

The mic and reference streams need to be gapless *and* identically clocked, with the DLL able to
absorb a device-clocked mic stream without pacing backpressure on the caller's thread — a
producer/consumer decouple on `SendAudio`'s publish side (DLL repo), or an equivalent redesign
of the capture cycle. That is a design decision across both repositories, not an evening's
patch, and it is the maintainer's call.

## F39 — Both streams on submix listeners hold parity at 1.000, and the recorder path never does

**Measured 2026-08-31 on `fix/aec-reference-alignment`, both repositories, one build, one
hour.** This is F36's named mechanism closed, and F37's configuration landed on a mainline
branch for the first time.

### What shipped

- **DLL** (`convai-livekit-cpp-p`, `fix/aec-reference-alignment`): `captureFrame` moved off the
  producer's thread onto a per-client publish thread, so the transport's real-time pacing never
  reaches the thread that produced the audio — and never stalls `SendReferenceAudio`, which
  takes the same `aecMutex_`. `SendReferenceAudio` gained the remainder carry the mic side
  already had; it had been dropping the sub-480 tail of **every** call. Both directions now
  re-block through one `ChunkQueue`. Per-client AEC counters and `SetStreamDelay` are on the
  public header, so the host can read alignment instead of inferring it.
- **Plugin**: `ISubmixBufferListener` taps on the master submix (far end) and the AudioInput
  submix (near end), both default-on (`ReferenceCaptureTap` / `MicCaptureTap`, `Recorder`
  restores the old path). One `FConvaiPcmReblocker` carries resampler phase, averages channels
  and re-chunks across callbacks. `UConvaiSubsystem::SendAudio` refuses a displaced player
  session.

### The numbers, `aec_echo_only_internal`, n=10 per arm

| | taps (default) | recorder (`-MicCaptureTap=Recorder -ReferenceCaptureTap=Recorder`) |
|---|---|---|
| `aec_stream_parity` (mic chunks / ref chunks) | **1.0000** [0.9996, 1.0000] | 0.9923 [0.9871, 0.9952] |
| `feed_capture_ratio` | **1.0000** [1.000, 1.000] | 0.9909 [0.9883, 0.9932] |
| `feed_recorder_off_max_ms` | **0** | 0.85 |
| `aec_dropped_frames` / `aec_queue_depth` | 0 / 0 | 0 / 0 |
| leaked (`transcripts_during_echo` > 0) | 0 of 10 | 1 of 10 |

The parity and capture-ratio ranges do not overlap. That is the first deterministic in-engine
separation in this investigation — every earlier one rode on the transcript oracle, which F32
measured as a coin.

### What it does not claim

**The transcript oracle did not separate the two arms.** 0/10 against 1/10 is p ≈ 1.0; the
shipping path leaked far less today than the 20–30% history or F37's 7/8. Do not read the leak
column as the result — the result is the parity column.

**The fixture is live, which is what makes 0/10 mean anything.** Same build, same hour:
`aec_echo_only_none` leaked 4 of 5 and `aec_echo_only_disabled` leaked 5 of 5, both of which
pass by leaking. Against those controls the canceller-on arm is 0 of 15.

### Two defects the instrument caught in its own change

1. **The two mic capture paths were both live.** Guarding only `UpdateVoiceCapture` left the
   output recorder armed by `StartRecording`, `UnmuteStreamingAudio` and the mic-health
   restart, so every `StopVoiceChunkCapture` routed a second copy: parity read **2.08**, the
   publish queue sat at its 100-frame cap and shed 2631 frames. The guard belongs in
   `StartVoiceChunkCapture`/`StopVoiceChunkCapture`, where all four callers pass.
2. **`ResampleAudio` was not downmixing.** With `reduceToMono` it took channel 0 and shared one
   accumulator across channels. `mic_tap_channels` reads 2, so the AudioInput submix is stereo
   and half the near-end signal was being discarded on the recorder path.

### Still open

- `aec_double_talk` / F12 is untouched by this, and that was measured rather than assumed:
  **0 of 3 on both arms**, taps and recorder, same build. `near_end_wer` 1.0 with
  `near_end_wer_against_character` also 1.0, so it is over-suppression, not the character
  returning as the player — and `aec_stream_parity` reads 1.000 on the failing runs, so it is
  not drift either. It is the AEC3 suppressor gating on far-end activity, measured offline,
  and `AecNearEndPreservation.ReferenceWithNoEchoMustNotSuppressThePlayer` is still
  `GTEST_SKIP`. The fix is in vendored `client-sdk-rust`, outside this branch's boundary.
- `AECStreamDelayMs` is wired and defaults to unset. F35 swept it against a drifting stream and
  got nothing; with drift gone it has never been swept. `aec_stream_delay_ms` labels the arm,
  −1 meaning the hint was never called.
- `aec_last_atten_db` is a last-value snapshot and reads 0 when the window ends in silence. It
  is not an ERLE oracle; the DLL still exposes no residual.

## F37 — The publish decouple plus a single mic producer put the gapless tap on honest clocks, and the leak separates for the first time

**Measured 2026-08-07, the session F36 asked for.** The chain ran three layers deeper than
the named experiment, and each layer was measured before the next was touched.

### The decouple works, and it un-masked two more defects

`SendAudio`'s publish side is now a producer/consumer pair in the DLL (ADR 0003 there):
`SendProcessedAudioToServer` enqueues and returns; a per-client publish thread is the only
caller of `captureFrame`, so its real-time pacing never blocks the caller — which used to
block **while holding `aecMutex_`**, which is why F36's tap runs stalled the mixer 14–23 s
and halved the reference feed at once. With the decouple in, tap runs read capture ratio
0.98–1.00 (was 0.52–0.55), recorder-off max 0.2–0.5 s (was 25.4 s), and wall durations
back at ~29 s — the dilation that discredited every earlier tap number is gone.

Two things then surfaced, in order:

1. **A stale staging path.** `Convai.cpp` loads DLLs from `Binaries\Win64` and copies from
   `Source\ThirdParty` only when the file is *missing* — `stage-convai-client.bat` alone
   leaves the engine grading the old DLL. One full sweep was invalidated by this and
   re-run; LOOP.md's build table now carries the extra copy step.
2. **Two microphone producers per session.** With the tap honest, `aec_mic_chunks` read
   2.2–3.8× `aec_ref_chunks`. The Landing map's player component and the scenario's
   spawned one both stream the same AudioInput submix into the *same* session proxy (the
   chatbot hands both the same client). The recorder path had always masked this — two
   stop/read/restart cycles against one submix recording split the stream ≈50/50 and sum
   to 1× — but every submix listener receives the full render, so the tap doubled it.
   A DLL-boundary trace also caught the guard's own first draft creating a 2.2M-frame
   (46 s) stale-recording dump at teardown: a refused component's armed recording rotted
   and flushed into `SendAudio` in 8 calls, straight into the stats window.

The fix (plugin, ADR 0006): a session proxy carries a mic-producer claim. First streamer
claims; a refused component skips capture entirely and warns once; capture state
(armed recording, tap buffer) is discarded on every claim transition, so stale audio can
never be sent late. `SendAudioToTalkTargets` sends only to claimed proxies.

### The numbers, all on one build, one evening

`aec_erle_internal`, n=3 per arm unless stated:

| Arm | mic/ref chunks | `aec_atten_db` | `aec_atten_at_peak_db` |
|---|---|---|---|
| shipping recorder (F35 baseline) | 1939–1971 / 1977–2017 | 3.27–5.90 | 3.07–13.14 |
| MicCaptureTap only, guarded | 2000–2003 / 1982–1987 | 5.31–6.66 | 3.64–7.99 |
| both taps, guarded | 2000–2002 / 2000–2002 | 5.24–9.14 | 7.79–13.0 |
| both taps, clean rebuild (n=2) | 2000–2079 / 2000–2002 | 9.48–13.17 | 13.23–14.44 |

Mic/ref parity at ±0.1% is the first time the two streams have ever agreed in-engine —
F36's 1–2.5% deficit is gone, and with it the drift mechanism.

**The transcript oracle separated.** `aec_echo_only_internal` ×10 per arm, same build,
same hour, fixture-rejected runs (echo peak ~0) excluded from both denominators:

- both taps + guard: **1/8 leaked**
- shipping recorder path: **7/8 leaked**

Fisher exact p ≈ 0.010 two-sided — the first leak-rate comparison in this project that is
powered rather than `under-powered`. The leaking shipping runs also show the loop
mechanically: their reference counters inflate to 2759–4306 chunks as the character keeps
answering itself.

### What it does not close

- `aec_double_talk` fails 3/3 with the taps on — but F34 measured it failing on shipping
  before any of this session's changes existed. Unchanged, not traded; it is F12's
  criterion and stays red until that fix.
- Window attenuation is 5–13 dB, not the 37–54 dB the dilated tap runs appeared to show
  (those numbers were never credited, and this session shows what honest windows read).
  Per-run variance remains: runs that lock read 18–19 dB at-peak, runs that don't sit at
  1–2 dB. Parity killed the *drift*; the per-run *delay lock* is the remaining lever, and
  F35's dead fixed-hint experiment was run against the drifting stream — with drift gone,
  `AECStreamDelayMs` may deserve one more sweep.
- The shipping-arm leak rate on this build (7/8) sits above the historic 20–30%. The
  producer guard concentrates all recorder-cycle gaps into a single component's stream
  where the accidental two-component interleave used to spread them; if that reading is
  right, guard-without-tap is a mild regression on the recorder path, and the taps should
  become the default in the same change that ships the guard. One evening's rate against
  history is not proof — flagged, not concluded.

## F38 — A bad character ID fails silently: the plugin knows, the game never finds out

**Measured 2026-08-19, n=1, on `connection_invalid_character`'s first live run.** The scenario
was written as the in-engine counterpart of the DLL harness's `error_handling`, and it fired on
the run that introduced it.

`UConvaiChatbotComponent::StartSession()` on `00000000-0000-0000-0000-00000000dead`. The plugin
resolves the character over REST first, and the service refuses it:

```
ConvaiBotHttpLog: Warning: HTTP request failed with code 404, and response:
  {"ERROR": "Incorrect Character ID provided. Please check the Character ID."}
ConvaiChatbotComponentLog: Warning: OnConvaiGetDetailsCompleted: Could not get character
  details for charID:"00000000-0000-0000-0000-00000000dead"
ConvaiSubsystemLog: Error: Failed to connect to Convai service
```

So the plugin knows, and says so three times — to the log. What the *game* is given over the
same 45 s:

| Route a game has | Value |
|---|---|
| `OnFailureEvent` broadcasts | **0** |
| `GetChatbotConnectionState()` reached `Connected` | no |
| `GetChatbotConnectionState()` ever reached `Connecting` | **no** |
| state at the end | `Disconnected` |

Every route is silent. The component never leaves `Disconnected`, which is also the state it
was in before `StartSession` was called, so polling cannot distinguish "refused" from "not
started yet" either. A game that binds `OnFailureEvent` to show an error shows nothing, and a
game that polls for `Connected` waits forever.

**Confirmed:** the numbers above, from the scenario's own report, and the log lines.

**Fixed 2026-08-20, on `WebRTC-Video`.** The hypothesis was right: the site is
`OnConvaiGetDetailsCompleted`, whose empty-details branch logged a warning, broadcast
`OnCharacterDataLoadEvent_V2(this, false)` and returned. It had no route to `OnFailureEvent` at
all — the delegate is broadcast in exactly one other place, `OnFailure`, for a failed response.
The branch now broadcasts it too, marshalled to the game thread the same way.

```
                          before (26/26 in the study, 2/2 again today)   after
failure_events            0                                             1
ever_connecting           0                                             0
resolution                45 s scenario timeout                         0.5-1.6 s
connection_invalid_character  fail                                      3/3 pass
```

`ever_connecting` staying 0 is what attributes the pass: the scenario also accepts a
`Connecting -> Disconnected` transition as a legitimate report, and that is not what happened —
the delegate is.

**Still true, and not fixed here:** the component never leaves `Disconnected`, so a game that
polls `GetChatbotConnectionState()` instead of binding the delegate still cannot tell "refused"
from "not started yet". Giving the state machine a terminal failure state is a larger API change
than raising an existing delegate, and it belongs in its own issue.

## F34 / F12 — In-engine, the canceller removes the player five times harder than it removes the echo

**Not fixed. Separated 2026-08-20, decisively, and the fix is blocked at a named place three
vendored layers down.** The separation is what the handoff asked for first, and it is done:
**this is not the server.**

### What it was

**Measured 2026-08-07, n=1, on `aec_double_talk`'s first live run.** The player speaks fixture
`S1` while the character is talking and Injected Echo is running. The player's transcript came
back **empty**, word error rate 1.0, against whole-window attenuation **14.69 dB** and 8.8 to
19.4 dB in every single second — against 0 to 3 dB when only the echo is present (F32, F33).

### The separation: it is not the server

Two independent lines, and they agree.

**1. It reproduces offline, with no engine, no network, and no echo at all.** The oracle already
exists, in `convai-livekit-cpp-p` on branch `feat/roster-rooms`:
`tests/aec_erle_test.cpp`, `TEST(AecNearEndPreservation, ReferenceWithNoEchoMustNotSuppressThePlayer)`.
It is **skipped**, deliberately — commit `7a606c8`, *"skip the F12 criterion instead of leaving CI
red"* — with the assertion left exactly as written, because it **is** the definition of fixed.
`docs/AEC_NEAR_END_SUPPRESSION.md` on the same branch is the full write-up. Its measurements:

| Configuration | Near-end level, whole | Worst two quarters |
|---|---|---|
| Control — reference silent | −0.4 dB | −0.4, −0.5 dB |
| Internal, AEC only | **−11.5 dB** | −72.8, −67.2 dB |
| Internal, shipping (NS+AGC+HPF) | −2.7 dB | **−74.0, −76.0 dB** |
| External (`AECwebrtc`), AEC only | **−13.5 dB** | −72.8, −67.2 dB |

The fixture never puts the player in the reference. So it is not correlation between the streams
— it is far-end **activity** gating the capture path. And it **inverts with echo level**: 0.00
gain costs 11.5 dB, 0.01 costs 1.2, 0.50 costs 3.8. *The better the player's acoustic isolation,
the worse they are treated* — headset users get the worst of it. Both backends land within 2 dB
of each other, which points at the shared WebRTC AEC3 suppressor rather than at either wrapper.

**2. The server does not gate user audio on bot speech.** Read in `core-service` today:
`_muted` on the STT service is driven from exactly one place, `stt-toggle`
(`handlers/rtvi_client_msg_handler.py:5497-5517`), and the plugin sends that only from
`UnmuteStreamingAudio`/`MuteStreamingAudio`. Nothing in `pipelines/bot.py` gates or drops an
`InputAudioRawFrame`, and the `UserStartedSpeakingFrame` work in
`handlers/rtvi_client_msg_handler.py:1196-1240` is turn-tracking bookkeeping, not audio
suppression. Nothing was changed in that repository.

So the candidate the handoff listed second is eliminated, and by a route that does not depend on
the backend's mood.

### The mechanism, measured — and then reverted

**Not fixed. The experiment below was run, then backed out in full**, because the change lives in
`client-sdk-rust`, which is vendored upstream and **not ours to modify**. It is written up because
the measurements are the useful part and nobody should have to re-derive them.

**The lever is `ep_strength.default_gain`**, in `client-sdk-rust/webrtc-sys/src/apm.cpp`, applied
through `BuiltinAudioProcessingBuilder::SetEchoCancellerConfig`. It is the strength AEC3 assumes
for the echo path *before, and in the absence of,* anything to converge on. At upstream's 1.0 a
canceller with no echo at all behaves as though a full-strength echo were present, and the
suppressor masks accordingly — which is exactly why the damage is worst with no echo, recovers as
soon as any appears, and hits headset users hardest.

Measured on `tests/aec_erle_test.cpp`, `AecNearEndPreservation`, with the `GTEST_SKIP()` removed:

| `default_gain` | suppression | worst quarter | 6 dB criterion | ERLE + double-talk |
|---|---|---|---|---|
| **1.0** (upstream) | **11.11 dB** | **−72.8 dB** | FAIL | pass |
| 0.5 | 9.15 dB | −72.5 dB | FAIL | pass |
| 0.1 | 4.13 dB | −7.5 dB | PASS | pass |
| 0.01 | 1.24 dB | −3.7 dB | PASS | pass |
| 0.001 | 0.09 dB | −0.7 dB | PASS | pass |

At 0.1 the offline suite went 21/22 → 22/22 and `ctest` 27/27, on both the `feat/roster-rooms`
line and, cherry-picked forward onto `1a477bc4`, the deployed 0.2.12 line. **ERLE and double-talk
pass at every row**, so the mechanism is not a trade against F26 — that was the risk worth
checking.

**Negative result, recorded so it is not repeated.** The suppressor's `dominant_nearend_detection`
is the obvious hypothesis and is **not** the mechanism. Sweeping `enr_threshold` (0.25 → 50),
`trigger_threshold` (12 → 1) and `hold_duration` (50 → 200), alone and in combination, moved the
near end by **at most 1.2 dB** against an 11 dB defect and never cleared the criterion. Forcing the
gentle `nearend_tuning` unconditionally moved it 1.2 dB as well.

**What was reverted, and to what.** `convai-livekit-cpp-p` is back at `hotfix/0.2.10-Hotfix1` @
`6d260943`, worktree clean, submodule at `06371a33`, `webrtc-sys/src/apm.cpp` byte-identical to
upstream, and the two working branches deleted. `prebuilts/rustlibs/release/win64` was re-fetched
from the official release; the four host-build artifacts that fetch does not supply
(`livekit_ffi.dll`, `.dll.lib`, `.dll.exp`, `.pdb`) were removed rather than left as
locally-modified binaries in a prebuilts directory. **`build/windows-x64-tests/` still contains
objects compiled from the modified source and should be cleaned before it is trusted.** The
plugin's deployed `convai_client.dll` was never touched: sha `7ec567584f77` throughout.

**What this needs to move:** a decision about `client-sdk-rust`. The change is one field, additive
and optional in shape, so it is plausibly a patch upstream would take — but that is the owner's
call, not a session's. Until then F12 stays open, and `aec_double_talk` reads 0/5 honestly, so it
can grade the fix the day one lands.

### The in-engine arm was scoring the wrong speaker, and it is not flaky

`aec_double_talk` first re-measured 2 of 3 passing — "flaky red", as the handoff records it. It
is not flaky. `FinishDoubleTalk` picked the player's transcript like this:

```cpp
for (int32 i = TranscriptsAtSpeechStart; i < All.Num(); ++i)
    if (All[i].bFinal && !All[i].Text.IsEmpty())
        Best = All[i].Text;          // no speaker filter
```

`All` is `Sink->Transcripts()` — **both** components. The player and the character broadcast on
the same delegate shape with themselves as `Speaker`, and during double-talk both are talking, so
whichever spoke last won. When the character finished last, its words were scored against the
player's fixture; when the player did, the arm passed. That is the coin toss that read as
flakiness, and `UConvaiTestEventSink` has had `TranscriptsFrom(SpeakerName)` for exactly this
since `text_roundtrip` needed it.

Filtered to `Speaker == "ConvaiPlayer"`, on the same build:

| `--repeat 5` | run 0 | run 1 | run 2 | run 3 | run 4 |
|---|---|---|---|---|---|
| status | fail | fail | fail | fail | fail |
| `near_end_wer` | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| `near_end_wer_against_character` | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| `near_end_transcript_empty` | 0 | 0 | **1** | 0 | 0 |
| `transcripts_during_echo` | 10 | 19 | **0** | 15 | 20 |
| `echo_source_nonsilent_samples` | 267,862 | 308,830 | 263,059 | 375,531 | 303,766 |

**0 of 5.** F12 reproduces in-engine on every run once the character's words stop being credited
to the player. Run 2 is F34's original signature exactly — nothing at all came back. The echo
check the handoff insists on: 263k–375k non-silent echo samples on every run, so none of these is
a quiet-run artefact.

What comes back on the player's path is neither speaker's words — `"4, 67."`, `"There's been a"` —
which is what a recogniser does with a quarter of an utterance at −74 dB.

### The oracle now says which defect it saw

`near_end_wer_against_character` is new and reported on every run: the same transcript scored
against what the **character** said. F26's leak and F12's over-suppression both land on this arm
as "the player's words did not come back", and their fixes are opposites — suppress less versus
suppress more — so an agent looping on this scenario could "fix" one by causing the other. A
non-empty player transcript closer to the character's words than to the player's now raises
`character-voice-returned-as-player-during-double-talk` instead.

It did not fire here: 1.0 against both on all five runs, so this is not the leak. Its limit is
worth stating — word error rate normalises by reference length, so a two-word hypothesis scores
~1.0 against a fifteen-word reference whoever said it. The discriminator is conservative by
construction: when it cannot tell, it reports destruction, which is the finding that was already
there.

**No Tier 2 sweep was run.** Alternated arms with Fisher exact are for grading a change, and
there is no change to grade — 0/5 needs no statistics.

### Harness change that came out of this

`run.py --tier dll` reported `build-failed: cmake --build failed` with
`MSBUILD : error MSB1009: Project file does not exist` — which reads as a broken build tree. The
real cause is that the offline oracle is on a different branch of the DLL repo from the one
checked out (`hotfix/0.2.10-Hotfix1`, while the deployed DLL is 0.2.12.320 from a third). The tier
now checks for `tests/aec_erle_test.cpp` first and names the branch:

```
TIER offline_dll NOT-RUN: no aec_erle_test.cpp in this checkout of the DLL repo
(branch hotfix/0.2.10-Hotfix1); the offline AEC oracle lives on the branch that
carries tests/aec_erle_test.cpp
```

---

## F38 — A bad character ID fails silently: the plugin knows, the game never finds out

**Measured 2026-08-19, n=1, on `connection_invalid_character`'s first live run.** The scenario
was written as the in-engine counterpart of the DLL harness's `error_handling`, and it fired on
the run that introduced it.

`UConvaiChatbotComponent::StartSession()` on `00000000-0000-0000-0000-00000000dead`. The plugin
resolves the character over REST first, and the service refuses it:

```
ConvaiBotHttpLog: Warning: HTTP request failed with code 404, and response:
  {"ERROR": "Incorrect Character ID provided. Please check the Character ID."}
ConvaiChatbotComponentLog: Warning: OnConvaiGetDetailsCompleted: Could not get character
  details for charID:"00000000-0000-0000-0000-00000000dead"
ConvaiSubsystemLog: Error: Failed to connect to Convai service
```

So the plugin knows, and says so three times — to the log. What the *game* is given over the
same 45 s:

| Route a game has | Value |
|---|---|
| `OnFailureEvent` broadcasts | **0** |
| `GetChatbotConnectionState()` reached `Connected` | no |
| `GetChatbotConnectionState()` ever reached `Connecting` | **no** |
| state at the end | `Disconnected` |

Every route is silent. The component never leaves `Disconnected`, which is also the state it
was in before `StartSession` was called, so polling cannot distinguish "refused" from "not
started yet" either. A game that binds `OnFailureEvent` to show an error shows nothing, and a
game that polls for `Connected` waits forever.

**Confirmed:** the numbers above, from the scenario's own report, and the log lines.

**Fixed 2026-08-20, on `WebRTC-Video`.** The hypothesis was right: the site is
`OnConvaiGetDetailsCompleted`, whose empty-details branch logged a warning, broadcast
`OnCharacterDataLoadEvent_V2(this, false)` and returned. It had no route to `OnFailureEvent` at
all — the delegate is broadcast in exactly one other place, `OnFailure`, for a failed response.
The branch now broadcasts it too, marshalled to the game thread the same way.

```
                          before (26/26 in the study, 2/2 again today)   after
failure_events            0                                             1
ever_connecting           0                                             0
resolution                45 s scenario timeout                         0.5-1.6 s
connection_invalid_character  fail                                      3/3 pass
```

`ever_connecting` staying 0 is what attributes the pass: the scenario also accepts a
`Connecting -> Disconnected` transition as a legitimate report, and that is not what happened —
the delegate is.

**Still true, and not fixed here:** the component never leaves `Disconnected`, so a game that
polls `GetChatbotConnectionState()` instead of binding the delegate still cannot tell "refused"
from "not started yet". Giving the state machine a terminal failure state is a larger API change
than raising an existing delegate, and it belongs in its own issue.

## F34 — In-engine, the canceller removes the player five times harder than it removes the echo

**Measured 2026-08-07, n=1, on `aec_double_talk`'s first live run.** Issue 05's double-talk
variant, built this session as the in-engine tier of the DLL's
`AecDoubleTalk.NearEndSurvivesSimultaneousEcho`. It fired immediately.

The player speaks fixture `S1` — *"Hey, what time is it right now?"* — into the microphone while
the character is talking and Injected Echo is running. What came back from the server as the
player's transcript was **empty**. Word error rate 1.0.

The per-second series is the part worth reading, because every block behaves the same way:

```
1103>129(+18.7dB)  951>134(+17.0dB)  816>147(+14.9dB)  915>145(+16.0dB)  1142>123(+19.4dB)
1065>136(+17.9dB) 1135>161(+17.0dB) 1169>154(+17.6dB)  992>186(+14.5dB) 1109>163(+16.7dB)
 …  twenty blocks, 8.8 to 19.4 dB, not one of them below 8
```

Whole-window attenuation **14.69 dB**.

**Against 0 to 3 dB on the same canceller in the same build when only the echo is present**
(F32, F33). The canceller removes the near-end talker roughly five times harder, in energy
terms, than it removes the thing it exists to remove.

This is F12 — measured offline in `tests/aec_erle_test.cpp` and red there for two sessions —
reproduced in the engine, on the shipping configuration, through the plugin's real capture
path. It is also the guard the kickoff asked for: an agent looping on the echo leak can "fix"
it by suppressing harder, and this is the scenario that would notice.

**Caveats.** One run. The threshold it fires on is a word error rate at or above 1.0, which is
"nothing the player said came back" rather than a quality bar — chosen because what F12's
over-suppression leaves is not a degraded transcript but none, and because issue 04 is right
that a real WER threshold would fail on the recogniser's model changing. An empty transcript
could in principle have another cause; the twenty blocks of 9 to 19 dB suppression in the same
run are what make that reading unlikely, and they are in-process rather than from the server.

## F33 — A defect that provably breaks cancellation offline does not move the in-engine number

**Measured 2026-08-07, and it is the session's gate failing rather than passing.** The PRD's V5
was to be re-run with a *seeded cancellation defect* in the DLL, against F32's new attenuation
oracle. The seed went in, the oracle did not notice, and the agent half was never reached
because there was nothing in the report for an agent to find.

### The seed

One token in `AudioProcessor::ProcessReferenceStream`
(`src/convai/audio/audio_processor.h:72`): `/*reverse=*/true` became `/*reverse=*/false`, so
the far-end stream is handed to `processStream` instead of `processReverseStream`. The APM
never learns the echo. It is a plausible slip — the two calls are three lines apart and differ
by one bool — and it leaves every counter healthy: `ref_calls` and `ref_chunks` keep
incrementing, nothing logs an error.

**It is a real defect and that is established independently.** Offline, `tests/aec_erle_test.cpp`
goes from 1 failing test to 4: `AecErle.InternalAecCancelsALinearEchoPath` (ERLE must exceed
6 dB), `AecErle.ShippingConfigIsMeasuredToo`, `AecDoubleTalk.NearEndSurvivesSimultaneousEcho`,
alongside the F12 test, which was still red when this was measured. *The F12 test is skipped
as of `7a606c8`, so the same seed now reads 0 failing to 3 — `build.yml` runs ctest on every
push to `staging-v2` and a permanently-red suite cannot separate a known defect from a new
one. Its assertion is unchanged.*

**And the seeded binary is the one the engine ran.** `convai_client.dll` is byte-identical
(md5 `787641b0…`) across the build output, the staged tree, and both runtime copies under
`Dev_WebRTC/Binaries/Win64`.

### The result

| | Attenuation across the window |
|---|---|
| `aec_erle_internal`, unseeded | 3.29, 2.68, 3.04, 2.09, 3.34 dB — 5 of 5 above the 1.0 dB floor |
| `aec_erle_internal`, **seeded** | **2.45** and **−0.34** dB — 1 of 2 above the floor |
| `aec_erle_disabled`, either | 0.0000 dB |

One seeded run sits **inside** the unseeded range. The oracle separates *AEC enabled* from *AEC
disabled* perfectly — 0.000 against ~3, no sample size required — and separates *cancelling*
from *not cancelling* one time in two, which is the coin this session existed to get away from.

Two runs is a small sample and it is enough for this conclusion, because the question is not
what the seeded rate is: a single seeded run reading 2.45 dB, inside a healthy range whose
minimum is 2.09, is already a false negative and no further runs remove it.

### What that says, and it is about the plugin rather than about the oracle

**The ~3 dB in F32 is not echo subtraction.** A change that demonstrably destroys echo
subtraction leaves it intact, so whatever produces those 3 dB is the AEC3 residual-echo
suppressor gating on far-end activity — the same mechanism F12 measured destroying the near-end
talker with no echo present at all. In-engine, the canceller's *subtraction* contributes
nothing measurable to begin with, which is why breaking it changes nothing.

That is a stronger statement of F32 than F32 makes, and it is consistent with everything
before it: F14 says the algorithm is healthy offline, F27 and F29 say the reference feed is
healthy and stays healthy when made perfect, and this says the subtraction is not happening
in-engine regardless. The remaining candidate is unchanged and is now the only one: the far-end
stream and the microphone stream are not aligned, so there is nothing for the filter to
subtract. `ConvaiClient::SetStreamDelay` exists and is reachable for the first time; sweeping
it is the next experiment and this session did not run it.

### What it means for V5

**V5 was not re-run and the gate is not passed.** Handing an agent a report that does not
contain the seeded bug would measure the agent, not the framework. The honest statement is that
the framework now has a deterministic oracle for *"is echo cancellation enabled and doing
anything"* and does not yet have one for *"is echo cancellation working"* — and that the second
one cannot be built until something in-engine makes subtraction produce a signal, because there
is currently no configuration of this plugin in which it does.

**Not established:** whether a seed aimed at the suppressor rather than at subtraction would be
caught. It would be — `aec_erle_disabled` reads exactly 0.000 — but that is a different defect
class from the one the gate asked about.

## F32 — The canceller is running, and in-engine it removes about 3 dB

**Measured 2026-08-07.** This is the session's target — an oracle for the AEC bug that does not
route through the live server's speech recogniser — and the first two attempts at it failed.
Both failures are recorded because each one is a specific claim about what cannot be measured.

The DLL now exports `ConvaiClient::GetAECStats()` per **Connection**: mic and reference chunk
counts, and cumulative sums of squared microphone samples either side of cancellation. Sums
rather than levels, so a caller snapshots at each end of its own window and differences them.
The plugin samples that once per second *of audio* — paced by the canceller's own chunk
counter, not by `DeltaSeconds`, which is clamped headless and produced 6 blocks in one run and
20 in another over the same 20 s window.

### First attempt: a whole-window energy ratio measures AGC

| | Attenuation across the window |
|---|---|
| `aec_echo_only_internal` (cancellation on) | −5.1, −2.9, −1.4, −1.3, +10.4, +13.0 dB |
| `aec_echo_only_disabled` (cancellation off) | −4.3, −0.7, −0.4 dB |

The distributions overlap completely and the canceller is frequently *below* the control. This
is F12's warning arriving on schedule — *"AGC hides it from aggregate statistics… any monitor
that reports mean level will miss this"* — and the mechanism is the same one F12 named: gain
control has a target level, so the output tends toward that target regardless of what
cancellation removed on the way.

### Second attempt: per-second blocks do not fix it either

`aec_atten_best_db`, the loudest second of each run: **7.4, 8.9, 9.2, 11.0, 19.1, 19.5** with
cancellation on against **8.8, 9.0, 9.5** with it off. Better separated than the whole-window
figure and still overlapping. Blocking makes the number legible; it does not remove the
confound.

### What works: turn the other APM stages off

`aec_erle_internal` and `aec_erle_disabled` are the same scenario with `NoiseSuppression`,
`GainControl` and `HighPassFilter` all `0`, so the only stage that can change the level is
cancellation. This is what F14 did offline, for the same reason.

| | Attenuation across the window |
|---|---|
| Cancellation off — the control | **0.0000 dB**, twice, `rms_out` equal to `rms_in` to the digit |
| Cancellation on | **3.29, 2.68, 3.04 dB** |

**The control is exactly zero because an APM with every stage disabled is a pass-through**, so
the separation here is structural rather than statistical: it does not need a sample size, and
`aec_echo_only_none` — which has no canceller at all and reports zero for the opposite reason —
is not what anchors it. `aec_erle_disabled` asserts on the 0.000 rather than assuming it, so a
future build where some stage is still processing fails the control instead of silently
un-anchoring the measurement.

### And the number itself is the finding

**Three dB.** The same canceller, measured offline in `tests/aec_erle_test.cpp`, returns
**40 to 49 dB** (F14) and converges in 200 ms. In-engine, against a 120 ms echo at gain 0.8, it
removes three.

That is F26 stated as a quantity instead of as a rate. It also explains why the transcript
oracle was so noisy: at 3 dB of cancellation whether the residue crosses the server's VAD and
gets transcribed is close to a coin, which is exactly what 1/5, 2/9, 0/10, 3/10 and 4/5 look
like. The leak rate was measuring the recogniser's threshold, not the canceller's state.

**What it is not.** It is not the reference feed — F27 measured that healthy on the runs that
leaked and F29 made it perfect and watched the leak continue. It is not the canceller's
algorithm — F14. What is left is what F29 also arrived at: alignment between the far-end stream
and the microphone stream. `ConvaiClient::SetStreamDelay` is now exported and reachable from
the plugin through `UConvaiConnectionSessionProxy::SetStreamDelay`, so that hypothesis has an
actuator for the first time; nothing in this session has swept it.

**Caveats, both load-bearing.** Three runs on each AEC-only arm, one machine, one character,
one map. And this measures the *shipping* configuration's canceller with its other stages
disabled — a customer runs with NS and AGC on, where F12 says the interaction is not additive.
The 3 dB is the canceller's contribution, not a prediction of what a customer hears.

## F31 — The plugin sends the host's microphone even when a third-party one is adopted

**Confirmed by measurement, 2026-08-07, and it invalidates every live AEC measurement in
sessions 1 through 3.** Found while trying to explain why the canceller's input level did not
match what the fixture had injected — a question no metric before this one could ask, because
the fixture measured what it emitted and the canceller measured what arrived and nobody
compared them.

`UConvaiPlayerComponent::OnComponentCreated` constructs and registers a
`UConvaiAudioCaptureComponent`
([ConvaiPlayerComponent.cpp:121-127](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L121-L127)).
A `USynthComponent` auto-activates on registration, so `OnBeginGenerate`
([ConvaiAudioCaptureComponent.cpp:480-503](../../Source/Convai/Private/ConvaiAudioCaptureComponent.cpp#L480-L503))
opens the host's capture device and starts capturing **before** `BeginPlay` reaches
`FindFirstAudioCaptureComponent` ([:826](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L826)).
Adoption swaps which component receives `Start()` and `Stop()`
([:556-572](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L556-L572)) and leaves the
default one running. Both render into `/ConvAI/Submixes/AudioInput`, and that is the submix
`StartVoiceChunkCapture` / `ReadRecordedBuffer` record
([:527](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L527),
[:535](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L535)) — so what reaches the
server is the sum.

### The two measurements that settle it

**A run with no audio anywhere and a busy microphone.** `aec_echo_only_internal`, 2026-08-07:
the mixer's rendered peak was **2.9e-06**, the Virtual Mic's emitted peak was **1.6e-06** — the
game produced nothing and the fixture emitted digital silence — and the canceller still
processed **2946 chunks at RMS 769**. The server returned player transcripts reading *"Are you
serious about"*. A later run returned *"اور"*. Those are not hallucinations on a silent stream;
they are the room.

**A microphone louder than anything the fixture emitted.** On another run the canceller's
loudest second measured RMS **10862** (0.33 of full scale) against a Virtual Mic whose single
loudest *sample* for the whole run was **0.168**. No gain in the capture path explains eight
times the source's peak; a second source does.

### What it costs

- **F24 is re-explained and its severity moves.** "A silent microphone produces hallucinated
  player transcripts", 162 in 20 s with AEC off — the microphone was not silent. The
  seven-fold AEC/None difference F24 reports is the canceller working on real room audio,
  which is the opposite of the "STT hallucinating on a near-silent stream" reading.
- **F26's rate is partly a property of the room.** The leak has read 1/5, 2/9, 0/10, 3/10 and,
  on the morning of 2026-08-07, 4/5 — with the plugin unchanged between the last two. A rate
  that moves that far on unchanged code was never going to bisect anything, and the reason is
  now visible.
- **Every live AEC number in sessions 1-3 was measured through this.** F27's and F29's feed
  measurements are unaffected — they instrument the reference path, not the microphone — but
  everything keyed on `transcripts_during_echo` was graded on a microphone carrying the room.

### It ships

This is not a harness artefact. A customer supplying their own capture component through the
documented extension point — the case F19 is about — gets the plugin's own microphone stream
mixed into theirs and sent to the server. F19 said their component is never routed to the
submix; this says the plugin's own component is still capturing after theirs is adopted. The
two compose into: their audio goes to the master mix and the host's default microphone goes to
Convai.

### Fixed, and verified

`SetAudioCaptureComponent` now stops the default component when an alternative is adopted —
the one place both the Blueprint entry point and `FindFirstAudioCaptureComponent` route
through. Three runs after the fix: `default_capture_active` **0** in all three, and the
canceller's input tracks the Virtual Mic's emission at a ratio of **1.61, 1.86, 1.82**. The
residual ~5 dB is the mixer's stereo submix being summed back to mono in
`UConvaiUtils::ResampleAudio`, and it is the same on every run.

**The guard matters more than the fix.** `aec_echo_only_*` now fails any run where the
canceller received more than 6 dB above what the Virtual Mic emitted over the same window, and
reports `default_capture_active` beside it. Without that, the next thing that renders into
`AudioInput` reopens this silently — and the echo floor cannot catch it, because a run carrying
the room has a perfectly healthy echo peak.

**Not established:** whether adoption itself is intermittent. Two runs in the sweep before the
guard existed had the Virtual Mic emitting pure silence while the canceller saw RMS 793 and
1060, which is what a *failed* adoption would look like — `StartAudioCaptureComponent` starts
the default component deliberately when no alternative was found
([:566](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L566)). `GetComponentsByInterface`
returning `[0]` makes the choice order-dependent. The guard now fails those runs rather than
grading them, which is enough for the framework; whether it happens in a customer build is
open.

## F23 — The plugin drops `bot-output`, which is the character's entire response

**Confirmed by measurement**, `aec_echo_only_internal`, 2026-08-06. This is why issue 05's
paired run cannot execute, and it is a shipped defect in its own right.

A live character was prompted with `SendText` and answered. The log:

```
ConvaiSubsystemLog: Warning: OnDataPacketReceived: Unknown packet type 'bot-output'.
   ... 41 times
```

`ToPacketType` maps 22 packet type strings — `bot-transcription`, `bot-tts-text`,
`bot-started-speaking`, `bot-llm-text` and the rest — and `bot-output` is not among them
([ConvaiSubsystem.cpp:1406](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1406)), so it falls
through to `EC_PacketType::Unknown` and is logged and discarded at
[:2038](../../Source/Convai/Private/ConvaiSubsystem.cpp#L2038).

**The string appears nowhere in this repository.** Not in the code, not in the wire contract at
`Context/rtvi.md`, not in any document. The server is sending a packet type the plugin has never
heard of and the contract has never recorded. This is F8's failure mode again — F8 is
`final-user-transcription`, dropped the same way — so it is a pattern rather than an incident.

**What it carries, measured.** The `Unknown` branch now logs the packet's shape, and the answer
is text, not audio:

```
Unknown packet type 'bot-output' (280-294 bytes, data keys:
  aggregated_by, segment_id, spoken_progress, spoken_status, text, will_be_spoken)
```

Under 300 bytes and no audio field, so the character's voice is not in here — it rides the
LiveKit media track and plays normally. What is lost is the response text, its segmentation, and
`spoken_progress` / `spoken_status` / `will_be_spoken`, which is the server telling the client
how far through speaking it is and what it still intends to say.

**Severity revised down, and my first reading of this was wrong.** It was recorded here as "the
character's whole response goes in the bin… answers and is never heard". That conflated the
packet drop with a single run in which the character happened not to respond at all. A later run
of the same scenario had `character_spoke=1` and Injected Echo measuring the character's voice at
peak **0.176**, with `bot-output` still being dropped throughout. The character is heard. The
drop costs the text stream and the progress signal, which matter for subtitles, for barge-in
timing and for anything coordinating animation against speech — not for audio.

**Still worth fixing**, for the same reason F8 is: the server is sending a documented-nowhere
packet type that the plugin silently discards, and the contract at `Context/rtvi.md` has no entry
for it. The next such addition will be discarded the same way.

**Fixed 2026-08-20, and the mechanism recorded above is no longer the one that matters.**
`bot-output` appears in **zero of the 440 study logs** and in none of today's, so the backend
stopped sending it somewhere between 0.1.26 and 0.2.10. The symptom outlived the carrier: the
character's words still reached no delegate, for two different reasons, one per path.

```
text prompt    bot-llm-text x1-19 per turn, no bot-transcription at all
               plugin: case EC_PacketType::BotLLMText: break;   -- recognised, discarded
spoken prompt  bot-transcription x1 per turn, before bot-llm-stopped
               plugin: broadcast with IsFinal=false, then OnBotLLMStopped broadcast
                       OnTranscriptionReceived("", true, true)
```

The empty final is the plugin's own, not the server's. It is why the sink's own docstring says
"the last final is always blank" and works around it, and why `bot_transcript_chars` was 0 on
**passing** `audio_roundtrip` runs — 26 of 26 in the study. A game showing subtitles on
`IsFinal` cleared them a moment after the character spoke; a game keyed on `IsFinal` alone saw
nothing at all.

`UConvaiSubsystem` now assembles the turn's text — reset at `bot-llm-started`, appended from
`bot-llm-text`, replaced by `bot-transcription` where the server sends one — broadcasts the
running text as non-final, and finalises *that* at `bot-llm-stopped` instead of `""`.

```
text_roundtrip            0/26 in the study, 0/1 earlier today    3/3 and 1/1 after
bot_transcript_chars      0                                       5, for a prompt answered "Hello"
delegate, verbose log     (nothing from the chatbot)              "Transcription received: Hello"
                                                                  twice: streaming, then final
full 22-scenario sweep    17/22 before                            19/22 after; the two extra
                                                                  greens are this and F38
```

The two live-transcript reds in that sweep are the known flake, not this change: 15 of 15 passes
across two reruns on the same binary, and the chatbot broadcasts **no** transcripts at all in
those scenarios, so nothing of this change reaches them.

**Narrowed alongside it.** `live_player_transcript*` asserted on `LatestFinalText()` — the last
final from *any* speaker. Once the character's answer became a non-empty final, that check could
be satisfied by the character's words while its finding still read "no final player transcript"
and `word_error_rate` scored the character's answer against the player's expected line. It now
filters by speaker, as `audio_roundtrip` and `text_roundtrip` already did.

**Not established, still:** what consumes `spoken_progress` on other clients, and whether
`bot-output` returns. `ToPacketType` still has no entry for it — mapping a packet type nothing
sends, whose fields cannot be verified against a live server, would be guessing.

## F24 — A silent microphone produces invented player transcripts

**Confirmed by measurement**, `aec_echo_only_*`, 2026-08-06. Found while looking for something
else, which is what the always-on parts of the framework are for.

The Virtual Mic streamed nothing but silence for the whole window — no fixture, no speech, and
per F23 no character audio to echo. Transcripts came back anyway, attributed to the player:

| AECType | Non-empty player transcripts in a 20 s silent window |
|---|---|
| `Internal` | 23 |
| `None` | **162** |

Representative, with the microphone carrying silence:

```
"Buttons are in the camper, however, Sanda doesn't play right without it,
 or Baki doesn't even know that Baki is"
"Do you think about the Bible? Remember, Mara found her whole head."
```

**Why it matters.** A player who is not speaking — muted, no microphone, or simply quiet —
generates a stream of fabricated utterances, and the character answers them. That is a
conversation the user did not have.

**The AEC column is the interesting part.** Cancellation is not supposed to be what stops this;
the seven-fold difference says the suppressor's noise-floor handling is doing the gating that
the server's VAD is not. So *echo cancellation is load-bearing for correctness here, not just
for quality*, and any change that weakens it — including a fix aimed at F12's over-suppression —
should be re-measured against this number.

**Not established:** whether this is the server's STT hallucinating on a near-silent stream, or
the plugin sending buffers it should be gating locally. `ConvaiVadParams` defaults are
`stop_secs 2.2` and `min_volume 0.6`, and nothing in the plugin sets them (F7), which makes the
second possibility worth eliminating first because it is the cheaper fix.

## F25 — The teardown "hang" is mostly the harness's own two-second threshold

**Overturned 2026-08-20.** This entry previously recorded the rate as a DLL deadlock that the
plugin could not fix. A stack captured from a live headless launch, and 40 launches measured
without killing any of them, say otherwise: **the process is not deadlocked, and `run.py` was
killing it about two seconds before it would have exited.**

### What the entry used to say

`aec_echo_only_internal` wrote its report and never exited: message pump blocked, 5 GB resident,
nine minutes until killed. Attributed to the 2026-07-28 PIE-stop deadlock — a lost wakeup inside
`LocalParticipant::unpublishTrack`'s `fut.get()` with no timeout
(`src/livekit/local_participant.cpp:236-239`) — and measured at 331/572 = 57.9% of launches
across the A/B study, 25/44 = 56.8% again on 2026-08-20, unmoved by 0.1.26, 0.2.10 or Hotfix1.

### The stack, from a launch caught in the act on 0.2.12

Non-invasive `cdb -pv` attach on a headless launch that `run.py`'s own rule called hung:

```
ntdll!ZwWaitForAlertByThreadId
KERNELBASE!SleepConditionVariableSRW
convai_client!std::condition_variable::wait_for<...,std::_Associated_state<int>::_Test_ready>
convai_client!std::_State_manager<int>::wait_for
convai_client!convai::ConvaiClientImpl::Disconnect+0x441
UnrealEditor_Convai!UConvaiSubsystem::DisconnectSession+0x3f5
UnrealEditor_Convai!UConvaiConnectionSessionProxy::Disconnect
UnrealEditor_Convai!UConvaiConnectionManager::ReleaseConnection
UnrealEditor_Convai!UConvaiChatbotComponent::StopSession
UnrealEditor_Convai!UConvaiChatbotComponent::EndPlay
UnrealEditor_Engine!UWorld::EndPlay -> UGameEngine::PreExit
UnrealEditor_Cmd!FEngineLoop::Exit+0x424 -> GuardedMain
```

It is **`wait_for`, not `fut.get()`.** `std::_State_manager<int>::wait_for` is
`finished_fut.wait_for(kDisconnectTimeout)` in `ConvaiClientImpl::Disconnect`
(`src/convai/convai_livekit_client.cpp:551`) — the **bounded** five-second wait on the detached
teardown worker. The deployed DLL already runs the teardown on its own thread and stops waiting
after `kDisconnectTimeout = 5s`. The game thread was inside a wait that ends on its own.

So the recorded mechanism does not describe what 0.2.12 does. And it explains I8's other
puzzle — Hotfix1 measuring 63/110 against a control's 63/110: **bounding the wait was already
done, and it could not move a number that was not measuring the wait.**

### The measurement: never kill, and see

Eight launches of `aec_echo_only_internal`, watched to natural exit, nothing killed. Time from
the report landing to the process exiting:

```
0.6, 1.4, 1.4, 2.2, 0.6, 1.8, 0.6, 2.2  seconds     8 of 8 exited by themselves
```

`run.py` killed at `REPORT_SETTLE_S = 2.0`. Two of those eight are past it — by 0.2 s.

### The A/B, at the scale the original number came from

Full engine tier, all 24 scenarios, same build, same hour, one sweep per arm:

| `--report-settle` | launches | `launches_hung_on_exit` | rate | `launches_without_report` |
|---|---:|---:|---:|---:|
| **2.0 s** (the old constant) | 24 | **13** | 54.2% (Wilson 35–72%) | 0 |
| **15.0 s** | 24 | **0** | 0% (Wilson 0–14%) | 0 |

**Fisher exact two-sided `p = 2.6e-05`.**

And the 2 s arm reproduces the history it is being compared with: 13/24 against the study's
pooled 331/572, `p = 0.83`. So this is the same phenomenon that was measured all along, and
raising one constant takes it to zero.

**Across everything run on 0.2.12 today — 24 + 8 + 8 = 40 launches — not one failed to exit by
itself.**

### What changed

`REPORT_SETTLE_S` 2.0 → **15.0**, and a `--report-settle` flag so the threshold can be measured
rather than assumed. 15 s clears the DLL's own 5 s bound with room for engine shutdown behind it.
Nothing in the plugin's teardown was touched, and nothing in the DLL: there was no deadlock here
to fix. The comment at the kill site now says what teardown actually costs.

### What this does not say

- **It does not say the deadlock never happens.** F25's original observation — nine minutes and
  5 GB resident — is not a 2.2-second teardown, and the 2026-07-28 cdb stacks are from the editor
  on an older DLL where `Disconnect` had no bounded worker. That defect was real. What is refuted
  is that the 57% headless rate was measuring it.
- **0/24 is not "never".** Wilson puts the ceiling at 14%; a launch that genuinely wedges would
  now be caught by the 600 s timeout and reported as `launches_without_report`, which is a louder
  signal than the one it replaces.
- The plugin-side workaround I8 asked about — moving `Disconnect` off the game thread with a
  deadline — **should not be built.** It would reorder teardown into the concurrency shape that
  previously produced the `remove_track` access violation and the `StopRemoteAudioReader`
  use-after-free, to fix something the evidence says is not broken.

### Separately, and still open: `livekit::shutdown()` is never called

Every launch, on stderr:

```
[livekit] [warning] SDK was not shut down before process exit. Use livekit::shutdown()
```

`FfiClient::~FfiClient` prints that when the lifecycle state is still `Initialized`
(`src/livekit/ffi_client.cpp:177-184`), so `livekit_ffi_dispose()` never runs and the Rust
runtime is torn down by process exit rather than by the SDK. Nothing in `src/convai/` calls
`livekit::shutdown()`. It did not cause the rate above — it is present on the clean launches
too — but it is the SDK telling us we are exiting wrong on every single run, and it is a
plausible candidate for the *real* nine-minute hang. Worth a look by whoever owns the DLL; not
this repository's to fix.

---

## F22 — An empty final transcript follows every real one

**Confirmed by measurement**, `live_player_transcript`, 2026-08-06, 3 runs of 3. Minor, and
recorded because it is a trap for anyone binding the delegate rather than a fault in the plugin.

The server delivers the player's transcript through
`UConvaiConversationComponent::OnTranscriptionReceivedDelegate` as a stream of partials followed
by a final — and then a **second event, also flagged final, with empty text**:

```
final=0  text=Hey, what time is it
final=0  text=Hey, what time is it right
final=1  text=Hey, what time is it right now?
final=1  text=
```

So "the last final transcript" is always blank. A Blueprint binding this delegate to a subtitle
widget shows the line and then clears it. `UConvaiTestEventSink::LatestFinalText` takes the last
*non-empty* final for exactly this reason.

**Not established:** whether the empty event is a deliberate end-of-turn marker or drift of the
kind F8 records. It is one packet's worth of reading in the server contract
(`Context/rtvi.md`) to settle, and nobody should build on the current behaviour until it is.

**Halved 2026-08-20.** The character's side of this was never the server's: `OnBotLLMStopped`
called `OnTranscriptionReceived("", true, true)` itself, so the plugin manufactured an empty
final for every bot turn. It now finalises the turn's actual text (F23). What remains — and was
observed again today on `live_player_transcript` — is the empty final on the **player** path,
which does come from the server, and which the paragraphs above describe.

## F8 — `final-user-transcription` is dropped

**Confirmed**, 16 occurrences: `OnDataPacketReceived: Unknown server type
'final-user-transcription'`. The server sends the player's final transcript and the plugin
does not handle it. Wire-contract drift, in the plugin rather than in any test corpus.

## F9 — Client version not recognised by the server

**Plugin half fixed 2026-08-20** on `fix/server-error-reaches-game`. **Server half is a handover,
below — the emitter is in none of the repositories on this machine.** Tier 1.

### What it was

**Confirmed:** `Server compatibility notice: Client version unknown. Compatibility issues may
occur.`

Two halves, and only one of them is this repository's.

### The plugin half: no server error of any severity could reach a game

Worse than the entry recorded. Both error paths dead-ended:

| Path | What it did |
|---|---|
| `error-response` | logged at Warning under the hardcoded text `Server compatibility notice: %s`, whatever the server actually said, then dropped |
| `error` (RTVI fatal) | called `UConvaiSubsystem::OnError`, which logged and returned |

`OnError` was **`static`** (`ConvaiSubsystem.h:331`). A static member cannot see
`CurrentCharacterSession`, so the fatal path had no route to a session even in principle — the
mislabelling was the visible half of a subsystem that could not report a server error at all.

Meanwhile `IConvaiConnectionInterface::OnFailure(FString)` existed, `UConvaiChatbotComponent`
implemented it, and it raised `OnFailureEvent` — the delegate F38's fix used. **Nothing in the
subsystem ever called it.**

### Why the obvious fix is wrong, with the number

The obvious move is F38's: route `error-response` to `OnFailureEvent`. Measured on a healthy
connected session (`connected=1`, `bot_turns=1`, `failures=0`), the server sends that advisory
**3 to 17 times per session** — 1, 1, 2, 3, 3, 3, 8, 8, 17 across the sessions run today. So it
was built and measured as a control:

```
                                   correct fix        "cheap fix": error-response -> OnFailure
server advisories received         8, 8, 3            1, 17
OnFailureEvent fired               1, 1, 1            3, 19
server_error_reaches_game          3/3 pass           0/2 fail
```

**19 failure events on a connection that never broke.** A game bound to `OnFailureEvent` would
report the session as failed nineteen times while it was working.

### What changed

- `UConvaiSubsystem::OnError` is no longer static, and forwards to a new
  `UConvaiSubsystem::OnServerError(Message, bFatal)` which walks both live sessions and calls
  the connection interface.
- The `ErrorResponse` case logs `Server error-response: %s` — what the server said — and calls
  the same path with `bFatal=false`.
- `IConvaiConnectionInterface::OnServerError(Message, bFatal)` is new, with a default that
  forwards a fatal error to the existing `OnFailure` and drops the rest, so every existing
  implementer keeps exactly the behaviour it had.
- `UConvaiChatbotComponent` overrides it and broadcasts a new
  `OnServerErrorEvent(Chatbot, Message, bFatal)`, marshalled to the game thread the way
  `OnFailureEvent` already is. **`OnFailureEvent` keeps its meaning: fatal only.** It also carries
  no message, which is why the new one exists — the server's text had nowhere to go.

### Verified

`server_error_reaches_game`, new, live, `--repeat 3`, on `903b069a`. It drives both severities
through `IConvaiConnectionInterface::OnServerError` — public, on a public interface the chatbot
publicly implements, and the exact call the subsystem makes. The subsystem's packet sink is
private and ADR-0005 will not widen private surface for a test, so the decode above that seam is
covered by a source check instead.

| | run 0 | run 1 | run 2 |
|---|---|---|---|
| `injected_advisory_delivered` | 1 | 1 | 1 |
| `injected_fatal_delivered` | 1 | 1 | 1 |
| `failure_events` | 1 | 1 | 1 |
| `server_sent_errors` (reported, not asserted) | 8 | 8 | 3 |

`test_run.py` gains `test_a_server_error_packet_is_not_logged_and_dropped`, a grep check on the
decode half, with three negative controls all run and all caught:

| Mutation | Result |
|---|---|
| baseline | passes |
| restore the `Server compatibility notice` format string | `the hardcoded compatibility-notice label is back` |
| delete the `OnServerError` call from the `ErrorResponse` case | `the ErrorResponse case no longer routes the message` |
| make `OnError` static again | `OnError is static again, so it cannot reach a session` |

`test_run.py` is 12 of 12. The cheap-fix control above is the scenario's own negative control.

**What the scenario cannot see:** the subsystem's packet decode and session routing, because
reaching them needs the private sink. The live server covers that half in practice — every
`server_sent_errors` count above arrived through the real path — but it is not the pass
condition, so the day the advisory stops the scenario still passes.

### The server half — handover, not fixed

**The string is in none of the eleven repositories under `E:\Convai\Git`.** Full-tree grep for
`Client version unknown`: zero files. Not `core-service`, not `ConvAI_Middleman`.

What the two repositories I can read do with `client_version`:

| Site | What it does |
|---|---|
| `core-service/models/api.py:306` | declares it `Optional[str] = None` on the invocation metadata |
| `core-service/server.py:253`, `:1153` | copies it into the api-event payload |
| `core-service/utils/api_event.py:234`, `utils/connect_timing.py:244` | records it as a tag |
| `ConvAI_Middleman` | never mentions it |

**Nothing branches on it.** It is recorded and never read back, so neither service can be the
one deciding the version is unknown. `core-service/docs/versioned-character-runtime.md` is about
*character* versioning and names Character API (`api2-stg.convai.com`) as a separate service —
that is the first place to look, along with the pipecat/RTVI layer.

What the plugin actually sends (`ConvaiSubsystem.cpp:565-605`):

| Field | Value today |
|---|---|
| `invocation_metadata.client_version` | `PIE_20260820_180030` — the **app's** version, a PIE session timestamp in the editor, overridable with `-ClientVersion=` |
| `extra_metadata.convai_plugin_version` | the plugin's own version, from the `.uplugin` |
| `extra_metadata.convai_client_version` | `0.2.12.320+3b122f2`, the native DLL |

So the SDK version the server presumably wants **is already being sent**, one level down in
`extra_metadata`. `client_version` carries the app's, and the PIE timestamp in it is deliberate:
it makes every PIE session traceable.

**The question for whoever owns the emitter**, and it is not answerable from here: what does it
expect in `client_version`, and should the plugin move `convai_plugin_version` into it? Doing
that unasked would change what `api_event` and `connect_timing` record for every UE session, and
would lose the app's own version unless it moves the other way. Not guessed at.

**Also worth their attention:** the advisory is sent 3 to 17 times per session, not once, and
`error-response` is not documented in `Context/rtvi.md` at all.

---

## F10 — Lip sync starvation

**Confirmed**, once: `Lip sync starvation exceeded fallback window by 5.004s`. A five-second
stall in audio or face-data delivery.
