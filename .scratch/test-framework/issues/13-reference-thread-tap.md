# 13 — A tap inside the reference thread, so F26 points somewhere

Status: `done` — 2026-08-07. Built, controlled, and it produced a negative result: see **F27**.
The feed is healthy on the runs that leak, so F3, F4 and F15 join F1 as eliminated mechanisms for
F26. One run of ten stalled the dispatch for 9 s and also leaked, confounded by a process-wide
stall and recorded as n=1.
Depends on: 03, 05
Blocks: 14

## Why

F26 measures echo cancellation leaking the character's own voice into the player's transcript,
2 runs in 9. The same canceller measured offline is healthy — 40–49 dB ERLE, converging in
200 ms (F14) — so the leak is in the integration and not the algorithm. That is as far as the
current instruments go.

Of the four candidates F26 named:

| Suspect | Status |
|---|---|
| F1 — a **Connection** not being fed | **Eliminated.** Deficit zero across ten runs, leaking ones included |
| F4 — wall-clock polled cadence | **Never measured.** See below |
| F3 — recorder off across conversion and fan-out | Unmeasurable while a **Connection** is live (F17) |
| F15 — intermittent near-total capture loss | Same |

Issue 03's monitor was pointed at F4 and cannot see it. `FReport::WallGap*` measures the spacing
between **submix buffer callbacks**, which is the audio device's fixed render cadence: ten runs
put its standard deviation between 4.922 and 4.929 ms, identical to three decimals whether the
run leaked or not. F4 lives in `FConvaiReferenceAudioThread::Run`'s 2 ms poll and its
asynchronous dispatch, which a submix listener never observes. The header has been corrected;
the gap in coverage has not.

So the framework can currently say *that* cancellation leaks and nothing about *why*. An agent
asked to fix it has no hypothesis to test and a 22%-noisy oracle to grade itself against.

## Scope

**Report the feed's cadence from inside the thread that produces it.**
`UConvaiSubsystem::GetReferenceAudioStatus()` already exists — session 1 added it for F2, which
is precisely the complaint that this path emits no signal at all. Extend it with what the thread
alone knows:

- chunks sent since start, and to how many **Connections**
- inter-chunk delay: last, mean, max, standard deviation, measured in `Run()` around its own
  dispatch rather than at a mixer callback
- samples dropped while the recorder was stopped, if that is knowable without a second tap
- the wall-clock stretch `ProcessCapturedAudio` spends between `StopRecording` and
  `StartRecordingRefrence` — F3's window, directly

Justified on its own merits per ADR-0005, not as a test hook: F2 records that there is no way to
tell from any log whether **Reference Audio** ever flowed, and this is that. It is the same
argument that justified `GetReferenceAudioStatus()` in the first place.

**Surface it in the monitor** so every scenario carries it, and in
`aec_echo_only_internal` next to the leak count, which is the correlation that matters.

## Assertion

None yet. Report the numbers first — a bound set before there is a baseline is a guess, and
issue 03 already declined to assert F4's jitter for that reason.

## Done when

Ten `aec_echo_only_internal` runs report per-run reference-feed cadence alongside
`transcripts_during_echo`, and the leaking runs can be compared against the clean ones on a
metric that actually describes the plugin's behaviour rather than the audio device's.

## Out of scope

Fixing anything. This issue only makes the mechanism visible; issue 14 acts on what it shows.
