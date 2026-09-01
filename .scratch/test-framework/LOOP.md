# The AEC fix loop — brief for the fix agent

This is not the framework brief. `KICKOFF.md` is for sessions that *build* the framework; this is
for a session that *uses* it to fix the echo cancellation defect. Edit this file when the
hypothesis changes.

**To start a loop, paste this:**

> You are the fix agent for the Convai AEC defect. Read
> `.scratch/test-framework/LOOP.md` and follow it. Work one hypothesis at a time and stop when
> the brief says to stop.

---

## What you are fixing

**F26** — in-engine, echo cancellation lets the character's own voice back into the microphone
uplink, so the character responds to itself. Measured across sweeps at 0/10, 2/9 and 3/10.

Read `FINDINGS.md` F26, F27, F29, F32 and F33 before touching anything. The short version of what
is already eliminated, so you do not re-run it:

| Candidate | Status |
|---|---|
| F1 — a **Connection** never fed | Eliminated. Deficit zero across ten runs including the leaking ones |
| F3 — recorder puncture | Measured. Costs ~0.9% of the feed |
| F4 — polled cadence | Measured. 11.4 ms, steady |
| F15 — intermittent capture loss | Did not fire once across the sweep |
| Reference feed generally | Made *perfect* with a listener tap (F29). Leak unchanged, 0/10 vs 3/10, p = 0.21 |
| The canceller's subtraction | F33: seeding a defect that breaks it offline does not move the in-engine number, because in-engine it contributes nothing to begin with |

| H1 — a fixed `SetStreamDelay` hint | **Dead** (F35). Swept 0–300 ms, n=3 on the key arms; every arm overlaps baseline on every attenuation column. Matches F5's offline ≤2.3 dB |

### The mechanism is named. Do not re-derive it.

**F36.** Alignment was the right family and a fixed hint was the wrong actuator. The microphone
stream runs **1–2.5% slow** against the reference — `aec_mic_chunks` 1939–1971 against
`aec_ref_chunks` 1977–2017 over the same window, in all 19 valid runs, on both reference taps.
So the canceller converges, reads 12–25 dB on the first block after the character speaks, then
**loses alignment over 5–15 s** down to a 1.2 dB floor, re-locking only on loud bursts.

The loss site is the microphone cycle: it stops the recorder, reads, converts, resamples and
sends, then restarts, every ~10–16 ms
([ConvaiPlayerComponent.cpp:510](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L510)).
This also re-reads F29 — fixing only the reference side makes the *relative* drift worse, which
is the direction its 3/10 vs 0/10 already pointed.

**And the fix direction is demonstrated.** `MicCaptureTap=Listener` — a gapless mic tap, built in
that session and defaulted off — produced **37.0, 40.2, 45.6, 53.2, 54.1 dB at-peak** across five
consecutive runs. The recorder path's maximum across every session is 25.6. That is offline-scale
cancellation (F14's 40–49 dB) reached in-engine for the first time.

### What is in the way, and it is the current task

The tap is not yet a fix. The DLL's publish path paces to real time; absorbing a gapless feed
stalled the mixer's render thread, dilated headless game time ~2.2×, and halved the reference
feed's capture ratio. **Zero-leak results from tap runs were correctly not credited** — their
window semantics are dilated, so the numbers do not mean what they appear to.

**The next experiment, named by the session that found this:** a producer/consumer decouple on
`SendAudio`'s publish side in the DLL repo — queue and drain in real time rather than pacing the
caller — then re-run the tap arms. If window attenuation follows the at-peak numbers, F26 closes.

**You are authorised to implement that.** See the rails below.

## Before your first edit

**Branch in both repositories.** The DLL repo sits on `staging-v2`, which CI builds on every push
— do not commit to it. Branch: `fix/aec-<slug>`.

- plugin — `e:\UEProjects\UE5.8\Dev_WebRTC\Plugins\Convai-UnrealEngine-SDK-Dev`, from
  `fix/aec-stream-delay-sweep` if it still holds the actuator and the mic tap, otherwise from
  `feat/test-framework`
- DLL — `E:\Livekit\convai-livekit-cpp-p`, from `staging-v2`

Do not push. Do not merge. Do not open a PR. Those are the maintainer's.

The previous session left `MicCaptureTap=Listener` on the plugin branch, defaulted off, and left
the DLL repo clean at `staging-v2`. Start from that tap rather than rebuilding it.

## The loop

**fix → build what you changed → test → read the verdict → stop or repeat.**

The middle step is the one that bites. `run.py` builds *one* of the three things that can be
stale, so the rest is on you.

### Build what you changed

| You changed | Rebuild with | Does `run.py` do it? |
|---|---|---|
| DLL source, testing offline | nothing | **Yes.** `--tier dll` runs `cmake --build --target aec_erle_test` incrementally |
| DLL source, testing in-engine | `scripts\build.bat tests`, then `scripts\stage-convai-client.bat <plugin>\Source\ThirdParty\ConvaiWebRTC release` | **No** |
| Plugin C++ | UE's `Build.bat`, below | **No** |

**This is the trap.** After a DLL edit, `--tier both` will rebuild the test binary and *not*
restage the DLL the engine loads — so the offline tier grades your new code and the engine tier
grades the old, in one report that looks entirely coherent. Stage before you believe an engine
number.

```
# DLL, from E:\Livekit\convai-livekit-cpp-p
.\scripts\build.bat tests
.\scripts\stage-convai-client.bat e:\UEProjects\UE5.8\Dev_WebRTC\Plugins\Convai-UnrealEngine-SDK-Dev\Source\ThirdParty\ConvaiWebRTC release

# AND copy into the path the engine actually loads from — Convai.cpp loads
# Binaries\Win64 and only copies from ThirdParty when the file is MISSING,
# so staging alone leaves the engine grading a stale DLL (cost a full sweep
# on 2026-08-07). Verify the timestamp before believing an engine number.
Copy-Item "<plugin>\Source\ThirdParty\ConvaiWebRTC\lib\release\win64\*" "<plugin>\Binaries\Win64\" -Force

# plugin, from anywhere
& "E:\Software\UE_5.8\Engine\Build\BatchFiles\Build.bat" Dev_WebRTCEditor Win64 Development `
    -Project="E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" -WaitMutex
```

`build.bat tests` selects the `windows-x64-tests` preset, which is the tree `run.py` expects *and*
the one `stage-convai-client.bat` falls back to — one build serves both tiers. Without the `tests`
argument you get a different preset and the DLL tier reports `not-run`.

A UE build that fails with no error line in the output is almost always **Live Coding**: some
editor is open, possibly on another project. Capture the whole log before believing anything else.

### Test

```
# cheap, deterministic, seconds — grades canceller changes. No editor or project needed.
python Source/ConvaiTests/run.py --tier dll

# the full picture — DLL suite then ten engine sweeps, ~10-12 min
python Source/ConvaiTests/run.py --tier both --repeat 10 --filter aec_echo_only --map /Game/Maps/Landing
```

Needs `CONVAI_UE_PROJECT` and `CONVAI_UE_EDITOR`; the DLL repo resolves from `CONVAI_DLL_REPO`.
**Run these from PowerShell** — Git Bash rewrites `/Game/Maps/Landing` into a Windows path and the
run silently loads the wrong map.

### Stop or repeat

Stop when the "what done looks like" section below is satisfied, or when a "when to stop and ask"
condition fires. Otherwise take the next hypothesis. Do not repeat the same change hoping for a
better sample — if the verdict says `under-powered`, raise `--repeat`, do not re-roll.

## Reading the result

**Which tier grades what.** This is the trap the whole of issue 16 exists for.

- A change to the **canceller** is graded by the **DLL tier**. It caught F33's seeded defect
  deterministically. Trust it.
- A change to the **plugin** is graded by the **engine tier**.
- `aec_atten_db` **cannot detect a broken canceller.** It carries a `metric_notes` entry saying
  so. It separates AEC on from AEC off perfectly and nothing finer. Never conclude a canceller is
  healthy because this number held.

**The verdict field, not your own arithmetic.** Every finding is diffed against the previous run
as a rate and carries `improved`, `worse`, `not-separable`, `under-powered` or `not-comparable`.
`under-powered` means *run more*, not *no change*. The engine tier's own numbers say ~35 runs per
arm are needed to separate a 5% rate from a 30% one.

**`oracle_changed`.** If it is set and you did not deliberately and openly change a threshold,
something is wrong with your working tree — find out what before reading any other number.

**Runs the fixture rejected are not results.** Echo peak ~1e-04 against the 0.01 floor means the
character never spoke. Exclude them from the denominator; do not treat them as passes or failures.

## Rules that are not negotiable

1. **Do not edit the oracle.** Thresholds, `kArms`, fixtures, `tests/aec_erle_test.cpp`'s
   assertions. Changing one is the cheapest path to green and it is the maintainer's call, not
   yours. The runner will print `ORACLE CHANGED` and you will have wasted the iteration.
2. **Do not un-skip or re-tune the F12 test** to make a sweep look better. Its assertion is the
   acceptance criterion for a different fix.
3. **n = 1 is not evidence.** Two consecutive runs of the same build have produced opposite
   results in this project more than once.
4. **Do not fix the leak by suppressing harder.** `aec_double_talk` exists to catch exactly that —
   the player talking over the character must survive to the server. If it regresses while the
   leak improves, you have traded F26 for F12 and that is not a fix.
5. **Report negative results.** Three of this project's most useful findings were negatives. A
   hypothesis you killed is worth as much as one you confirmed — write it into `FINDINGS.md`.

## What done looks like

**Window attenuation follows the at-peak numbers** — the ~3 dB the shipping path reads today
moves toward the 37–54 dB the gapless tap already demonstrates — with game time *not* dilated, so
the window semantics are comparable. Then the leak rate moves from its ~20–30% baseline to zero
across enough runs for the verdict to read `improved` rather than `under-powered`, **with
`aec_double_talk` still passing** and the DLL tier still green.

The trap specific to this fix: a tap run whose game time is dilated will show a beautiful
attenuation number and a zero leak, and mean nothing. Check `aec_mic_chunks` against
`aec_ref_chunks` and the reference feed's capture ratio before believing any tap result. The
previous session caught this on itself; do the same.

## What you are authorised to do

**Change, build and test freely in both repositories.** Instrumentation, probes, extra logging,
throwaway experiments, temporary CVars — none of that needs asking. Build both sides as often as
you need. The only hard boundary is **push, merge and PR, which need the maintainer's word.**

**Implement the fix, in either repository, including changes to the audio paths.** That covers the
`SendAudio` publish decouple, the microphone capture cycle, and making `MicCaptureTap=Listener`
the default if it earns it. Design changes are in scope; write an ADR under `Docs/adr/` when you
make one, which is the thing `origin/debug/aec` skipped and F28 charged it for.

**Debug scaffolding is free; shipping it is not.** Temporary instrumentation does not have to be
pretty and does not need justifying while you are using it. It must not survive into a commit
that claims to be a fix, and a debug knob that stays must be defaulted off and named as an
experiment — `MicCaptureTap=Listener` is the pattern. Grep your own tags out before committing;
the previous session did exactly this and said so.

**Leave both repositories buildable at the end of a session**, on their branches, with a stated
verdict on the shipping path. If you leave an experiment on, say which numbers it invalidates.

An earlier version of this brief said to stop and ask on any architectural change. That halted a
session at the exact moment it had earned the right to continue — it had named F36 and shown
37–54 dB — so the rule was wrong, not the session. **Diagnosis that stops short of the fix is not
the goal.**

## When to stop and ask

- **The fix wants an assertion, threshold, fixture or `kArms` entry changed.** Still absolute.
- **Anything wants a push, a merge, or a PR.** Those are the maintainer's.
- **Two hypotheses die in a row and you have no third.** Report the two deaths; they are results.
- **The fix would regress `aec_double_talk` or the DLL tier** and you cannot see how to avoid it.
  Say so rather than trading F26 for F12.

Stopping with a named mechanism and no fix is a good session. Stopping with a fix that traded one
defect for another is not.
