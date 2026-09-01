# Kickoff — the test framework

One file, kept current. Previous sessions each had their own `KICKOFF-N.md`; they are merged
here and deleted, because four of the five were stale within a session and a reader had no way
to tell which. Git history has them.

**To start a session, paste this:**

> Continue the automated test framework for the Convai Unreal plugin. Read
> `.scratch/test-framework/KICKOFF.md` and follow it.

**When a session ends, edit this file** — the queue, the traps, and the session log at the
bottom. Do not add `KICKOFF-6.md`.

---

## Where things are

The framework detects the AEC failure and has diagnosed it down to one surviving hypothesis.
Session 5 built the loop-readiness pieces — one runner over both tiers, labelled oracles, an
orphan sweep, a fingerprint on the thresholds. What remains before unattended verification is
cheap is 07's determinism.

Both repositories are in scope:

- plugin — `e:\UEProjects\UE5.8\Dev_WebRTC\Plugins\Convai-UnrealEngine-SDK-Dev`, branch
  `feat/test-framework`
- DLL — `E:\Livekit\convai-livekit-cpp-p`, branch `staging-v2`

The maintainer pushes the plugin branch. Run
`git rev-list --count origin/feat/test-framework..feat/test-framework` before assuming anything
is unpushed.

## Read first, in order

1. `.scratch/test-framework/FINDINGS.md` — the verdict table, then **F33**, then F27, F29, F31,
   F32. **Do not trust an F-number without reading its current entry.** F18 is refuted, F7 is
   restored, F16 is corrected, F23's severity was revised down, and F25 turned out to be a
   deadlock proven months earlier.
2. `.scratch/test-framework/issues/` — `16`, `17`, `18` are the queue. `13`, `14` are closed with
   the measurements that closed them; `15` is `wontfix`.
3. `CONTEXT.md` — the glossary. "Connection", "Session Proxy", "Talk Target", "Primary Target",
   "Reference Audio", "Virtual Mic", "Injected Echo" are load-bearing names, not synonyms.
4. `Docs/handoff/2026-08-06-test-framework-session-2.md` — the fullest record of the traps.
5. In the DLL repo: `CLAUDE.md`, `CONTEXT.md`, `docs/AEC_NEAR_END_SUPPRESSION.md`.

## The queue

**16, 17 and 18 are done** — session 5, close notes in the issue files. What they add, in one
line each: `aec_atten_db` travels with a `metric_notes` label saying it cannot detect a broken
canceller; `run.py --tier both` runs the DLL's gtest suite and the engine tier under one merged
report and one honest exit code, sweeping orphaned editors before launching; and the oracle
constants, `kArms`, and fixtures are fingerprinted, with any movement reported as
`ORACLE CHANGED`, its own condition. The F33 seed was re-run through the merged runner as 16's
verification: offline tier red with the three test names in the report, exit 1.

**07 — replay fixtures** is the head of the queue, the highest-value thing left. It turns a
ten-run statistical verification into a deterministic one.

Deferred and not part of the AEC loop: issues 09, 10, 11, 12.

## What the current session is not

**The `SetStreamDelay` sweep.** F33 leaves stream alignment as the only surviving F26 candidate,
and `a222ab8` exports the symbol for the first time, so it is finally reachable. It is the next
real experiment and it is not this queue — doing it before the loop is safe means doing it by
hand again.

## Building and staging the DLL

`scripts\build.bat tests`, then `scripts\stage-convai-client.bat <destination> release`.

**The `tests` argument is not optional.** It selects the `windows-x64-tests` preset, which is the
build tree `run.py --tier dll` expects and the one `stage-convai-client.bat` falls back to when
there is no plain release tree — so one build serves both tiers. Without it the DLL tier reports
`not-run`.

Neither `run.py` nor the stage script rebuilds the **plugin**. After a plugin C++ change:

```
& "E:\Software\UE_5.8\Engine\Build\BatchFiles\Build.bat" Dev_WebRTCEditor Win64 Development `
    -Project="E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" -WaitMutex
```

And after a DLL change, staging is what the *engine* tier reads — `--tier dll` rebuilds the test
binary incrementally but does not restage. Skip it and the offline tier grades new code while the
engine tier grades old, in one report that looks coherent.

**The destination is `<plugin>\Source\ThirdParty\ConvaiWebRTC`.** The script's own header comment
says `Plugins\Convai\ThirdParty\ConvaiWebRTC`, which is wrong for this layout — staging there
creates a second unused tree, the build keeps linking the old import library, and a new symbol
appears not to exist. Verify the export after staging; the import library is ground truth, the
header is not:

```
grep -a -o "?GetAECStats@ConvaiClient@convai@@[A-Za-z0-9_@$?]*" \
  Source/ThirdParty/ConvaiWebRTC/lib/release/win64/convai_client_dll.lib
```

## Running the suite

```
set CONVAI_UE_PROJECT=E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject
set CONVAI_UE_EDITOR=E:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
python Source/ConvaiTests/run.py --repeat 10 --filter aec_echo_only_internal --map /Game/Maps/Landing
```

One process per scenario. Offline scenario ~30 s, live ~60 s.

All three tiers run by default (`--tier all`; `both` is still engine + dll): the DLL suite first — incremental target build, then
the exe directly, seconds — and the engine launches after. `--tier dll` needs no editor or
project at all and is the cheap check for a DLL change. The DLL repo resolves from
`CONVAI_DLL_REPO`, default `E:\Livekit\convai-livekit-cpp-p`. A requested tier that could not
run fails the exit code; it is never reported as passing.

## Traps that fail silently

- **The Bash tool is Git Bash, and MSYS rewrites `/Game/Maps/Landing`** into
  `C:/Program Files/Git/Game/Maps/Landing`. The run loads the wrong map, writes no report, and
  looks like a crash. **Use the PowerShell tool for `run.py`.**
- **PowerShell eats a leading `-` in an argument value.** `--arg "-Foo=Bar"` fails with "expected
  one argument"; write `"--arg=-Foo=Bar"`.
- **Any running Unreal editor blocks the build** through Live Coding, including one on a
  different project. The message appears mid-log, so a filtered build log shows only an
  unexplained `OtherCompilationError`. **Capture the whole log when a failure has no error line.**
- **Live runs hang after passing, ~5 in 10** (F25, a proven DLL deadlock). `run.py` takes the
  report and kills the process. Orphans survive a killed orchestrator — look for
  `UnrealEditor-Cmd.exe` with `Responding: False`, and check `ParentProcessId` before killing.
- **The character does not always speak.** Runs rejected by the fixture floor with echo peak
  ~1e-04 against a 0.01 threshold are the floor working, not a result. **Do not count them in any
  denominator.**
- **`-ExecCmds` values containing spaces do not survive Python's list quoting**; `run.py` builds
  the line as one string on purpose.
- **A headless run is unfocused**, so `FApp::GetVolumeMultiplier()` can be 0 and every audio
  assertion passes on silence. `run.py` pins `[Audio] UnfocusedVolumeMultiplier=1.0`.
- **Runs must not pass `-nosound`** — without it there is no `Audio::FMixerDevice` at all.
- Console commands are `convai.tests.*`. `convai.test.*` collides with the F16 harness and loses.
- `run.py` uses an exact `-scenario=` per launch because scenario names are prefix-colliding;
  `-filter=` selects the set.
- **`AEC` and `AECType` are separate custom params** (`ConvaiUtils.cpp:843`). `AECType=None`
  leaves the reference feed running, so it is not a control for feed metrics.

## Working rules

Commit per completed sub-task, staging only files you touched. **Never say built / tested /
passing without having run it that session** — on failure show the actual output, and say plainly
what you skipped.

**Do not draw a conclusion from one live run.** The live scenarios route through an LLM that
answers differently every time. `run.py` emits a verdict per finding — `improved`, `worse`,
`under-powered`, `not-separable` — with the runs needed. Trust that over your reading of a single
sweep.

**The negative-control rule is not optional**, and the framework's own fixture check has failed
it: `EchoPeak <= 0.0f` only rejects exactly zero, so a run carrying no echo at all passed and
reported 126 hallucinated leaks as real. Every new metric needs **a configuration where it must
read a known value, run and recorded.** For attenuation that is AEC disabled, where it must read
~0 dB — and it does, exactly 0.000.

Update FINDINGS.md with what you learn, **including negative results** — F27, F29 and F33 are all
negative and all three were worth more than the positive results around them.

Comment the why, never the what. No summary markdown files at the end unless asked.

**If the design contradicts the running code, surface it with file and line and stop before
building on it.** Four times in session 1, four in session 2, twice in session 3. F28 killed an
entire planned issue and was worth more than the code it replaced.

**Do not re-walk the design tree.** It has been argued against measurement four times. Argue with
FINDINGS and with the open issues.

## Session log

Kept because every session so far has been sent after something that turned out to be false, and
knowing which claims have already collapsed is the cheapest context there is.

| Session | Told to | What actually happened |
|---|---|---|
| 1 | Build issues 01–12 from the PRD | Built 01, 02, 03, 06 and half of 04. Refuted F11 and F1. Found F12–F18 |
| 2 | Confirm F18's mechanism, then re-measure F12 | **Both premises were false.** F18 refuted — the mic is 106 dB down, and its evidence was a normalised ratio that reads the same either way. The CVar named did not exist. F12's fixture never contained the player's voice, so there was nothing to re-measure. Closed 04, built 05's core, found F19–F26 |
| 3 | Issues 13, 14, 15 | 13 and 14 closed with **negative** results — the reference feed is healthy on the runs that leak, and making it perfect does not stop the leak. F28 killed issue 14's planned method mid-session. V5 passed on a plugin bug |
| 4 | Move the oracle off the server's transcript, DLL now in scope | Built the per-client AEC stats API and the attenuation oracle. Then **F33**: a seeded cancellation defect did not move it, because in-engine there is no subtraction to break |
| 5 | Issues 16, 17, 18 — loop readiness | Built and closed all three. F33's seed re-run through the merged runner: caught offline, named in the report, exit 1. Orphan sweep verified against planted processes. The one engine smoke run leaked (F26) — n=1, recorded, not counted |
