# Handoff — test framework implementation, session 1

Date: 2026-08-06
Follows: [2026-08-06-test-framework.md](./2026-08-06-test-framework.md) (the design session)
Branches: `feat/test-framework` (plugin, from `docs/test-framework`), `feat/aec-erle-tests`
(`convai-livekit-cpp-p`, from `feat/multi-connection-log-sink`). **Neither pushed. No PR.**

## What this session was

Implementation of the design in `.scratch/test-framework/`. Issues 01, 02, 03, 06 and the core
of 04 are built and were run this session. Six new findings, F12–F18, three of which change
decisions the design rested on.

Everything measured is in [FINDINGS.md](../../.scratch/test-framework/FINDINGS.md) — verdict
table at the top, F12–F18 in full below it. Per-issue outcomes are in the commit messages and
in [issues/01-result.md](../../.scratch/test-framework/issues/01-result.md). This document
covers only what those do not: the state of the tree, what contradicted the design, and the
traps that cost time.

## Read in this order

1. `.scratch/test-framework/FINDINGS.md` — start with the verdict table and the **F18 → F12**
   chain flagged in the header. That chain is the session's main result.
2. `git log docs/test-framework..feat/test-framework` — eight commits, each message states what
   was measured and what was skipped.
3. `.scratch/test-framework/PRD.md` and `issues/` — still the plan of record, with the
   corrections in the next section applied.

## Where the design turned out to be wrong

These are recorded in FINDINGS but repeated here because they change what the next session
should do, not just what it should believe.

- **F16 — `Source/Convai/{Public,Private}/Tests` is not dead code.** All 43 files lack any
  `WITH_CONVAI_TESTS` guard, so `UConvaiTestHarnessSubsystem` constructs on every game instance
  and registers `Convai.Test.Run` in shipping builds. **Issue 12's premise is wrong**: it is
  removing live, reachable, customer-facing code from a shipping module, which is a behavioural
  change. Rewrite the issue before working it.
- **F18 overturns F7.** The microphone *is* inside **Reference Audio**. Combined with F12 this
  is a complete causal chain for "player speech cut or swallowed". The fix is an explicit
  endpoint on `MuteMic`, not a volume change — it is already at −96 dB and is bypassed because
  it is parentless.
- **F11 refuted.** No profiler capture needed; the design's "best findings-per-effort" item
  does not exist.
- **The PRD's `UConvaiVirtualMicComponent : UConvaiAudioCaptureComponent` cannot be written.**
  That base class is `UCLASS()` with no `CONVAI_API`. The Virtual Mic implements
  `IConvaiAudioCaptureInterface` on a plain `USynthComponent` instead; discovery is by
  interface, so the seam is unaffected. ADR-0005's route was verified to work across a module
  boundary.
- **Issue 01's "drive a character to speak" was not followed.** The probe plays an engine sound
  instead, so the headless configurations need no credentials. Reasoning in
  `issues/01-result.md`.

## What runs today

```
# DLL repo — 22 GTest cases, ~3 s
cmake --build --preset windows-x64-tests --target aec_erle_test
build/windows-x64-tests/tests/Release/aec_erle_test.exe --gtest_output=json:erle.json

# Plugin — 4 scenarios
set CONVAI_UE_PROJECT=E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject
set CONVAI_UE_EDITOR=E:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
python Source/ConvaiTests/run.py --repeat 10 --filter reference_feed --map /Game/Maps/Landing
```

Scenarios: `reference_feed_capture`, `reference_feed_capture_under_load`,
`virtual_mic_adoption`, `mic_not_in_reference_audio`. Console commands are `convai.tests.*`
(plural — `convai.test.*` collides with the F16 harness and loses).

**Expected state, not failures to fix blindly:**
`AecNearEndPreservation.ReferenceWithNoEchoMustNotSuppressThePlayer` is red on purpose (F12).
`mic_not_in_reference_audio` is red on purpose (F18). Both reproduce shipped defects. Do not
loosen either threshold — the PRD puts assertions out of scope for the fix agent.

## Traps, each of which cost real time

- **`-ExecCmds` values containing spaces do not survive Python's `subprocess` list quoting.**
  UE runs the command and then exits before the next frame, so the scenario never ticks and the
  run looks like a crash. `run.py` assembles the command line as one string for this reason;
  do not "tidy" it back into a list.
- **A headless run is unfocused, so `FApp::GetVolumeMultiplier()` can be 0.** The mixer still
  renders buffers and listeners still fire — with silence. Every audio assertion passes on
  zeros. `run.py` pins `[Audio] UnfocusedVolumeMultiplier=1.0`; keep it.
- **`FMixerSubmix::ProcessAudio` early-returns on auto-disable *before* the recorder append but
  *after* notifying submix listeners** (`AudioMixerSubmix.cpp:1410` vs `:1447` vs `:1686`). A
  quiet scenario measures the auto-disable. Scenarios keep a sound retriggering.
- **`StopRecording` writes out-params and returns the buffer by reference.** Argument
  evaluation order is unspecified; the call must complete before the values are read. This
  produced a fake "recorder returns nothing" result once already.
- **`FString::ParseIntoArray` resets its output array**, so parsing several args into one
  accumulator silently keeps only the last.
- **The plugin's own reference capture owns the global master-submix recorder** (F17). Nothing
  else in the process can record while a **Connection** is live. Any capture-ratio measurement
  must state whether one was.

## Three false findings were produced and killed

Written up in FINDINGS under F17. Worth knowing because the next session will hit the same
class of error: a connection-deficit invariant comparing against *registered* rather than
*connected* sessions; a capture-loss finding that was the monitor's own instrument being
drained; and the F7 guard firing on an unrouted Virtual Mic. The negative-control rule caught
all three. Treat a new finding as unproven until its control exists.

## What is owed

**Not started:** issues 05, 07, 08, 09, 10, 11. Issue 12 needs its premise rewritten first
(F16).

**Partly done:** issue 04 has the Virtual Mic and the adoption proof, but not the step library
— `SpeakWav` with real WAV loading, `Silence`, `SetTalkTargets`, `SayText`. WAV fixtures and a
loader already exist at `convai-livekit-cpp-p/tests/harness/wav_loader.{h,cpp}` and
`tests/data/*.wav` with `STT.json`; port rather than rewrite. Issue 03's F4 bound is measured
and reported but deliberately not asserted — idle and loaded showed the same jitter, so there
is no baseline to set a bound from yet.

**Unrun:** issue 01 configurations 1 (editor PIE), 3 (audio endpoint disabled) and 4 (packaged
Development). **Configuration 3 is the one that matters** — it decides whether every test box
needs an audio endpoint, and it needs the machine's audio device disabled, which was not done
without asking.

**Environment gaps:** no minimal test project, so runs use `Dev_WebRTC`'s `Landing` map, which
connects to the live backend and makes an offline scenario cost a handshake and ~90 s.
`/Engine/Maps/Entry` was tried and exits immediately in `-game`. The runner's process exit code
was observed as 3 on one passing run where `RequestExitWithStatus(false, 0)` was called —
unexplained, and it matters because issue 02 requires the exit code to be meaningful.

## Immediate next actions, in the order I would take them

1. **Verify F18's fix.** Set `EnableAudibleDefaultEndpointSubmixes=0` and re-run
   `mic_not_in_reference_audio`. If the tone ratio collapses, the mechanism is confirmed and the
   real fix is an explicit endpoint on `MuteMic`. This is the highest-value action available and
   costs one run.
2. **Re-measure F12 with F18 fixed.** F12's severity was measured with the player's voice in the
   reference. If F18's fix removes it, the symptom may resolve without touching the canceller.
   The two were measured separately and have never been run together.
3. Rewrite issue 12 against F16, then issue 05 (it needs the Virtual Mic, which now exists).

## Skills for the next session

- `diagnose` — for step 1 and 2 above. Both are bug investigations with a stated hypothesis and
  a cheap discriminator, which is what that loop is for.
- `tdd` — issues 05 and 07–10 are test construction.
- `to-issues` — if issue 12 needs restructuring rather than editing after F16.

Not `grill-with-docs`: the design tree was walked in the previous session and again against
measurement in this one. Re-walking it without new information wastes the session. The places
where the design was wrong are listed above and in FINDINGS; argue with those.
