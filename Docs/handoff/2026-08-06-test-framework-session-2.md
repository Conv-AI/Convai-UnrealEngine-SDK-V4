# Handoff — test framework implementation, session 2

Date: 2026-08-06
Follows: [2026-08-06-test-framework-implementation.md](./2026-08-06-test-framework-implementation.md) (session 1)
Branch: `feat/test-framework` (plugin). DLL work still on `feat/aec-erle-tests` in
`E:\Livekit\convai-livekit-cpp-p`, untouched this session.

## What this session was

Session 1's two mandated verifications, then issues 04 and the core of 05. Fourteen commits.
The maintainer pushed `feat/test-framework` to `origin` twice mid-session, so check
`git rev-list --count origin/feat/test-framework..feat/test-framework` before assuming anything
is unpushed.

Everything measured is in [FINDINGS.md](../../.scratch/test-framework/FINDINGS.md) — verdict
table at the top, F18 through F26. Per-issue state is in the commit messages and in
[PRD.md](../../.scratch/test-framework/PRD.md)'s sequencing table, which now runs to issue 15.
This document covers only what those do not.

## Read in this order

1. `.scratch/test-framework/FINDINGS.md` — verdict table, then **F26**, which is the session's
   result and the reason issues 13–15 exist.
2. `git log 311c3df8..feat/test-framework` — fourteen commits, each stating what was measured
   and what was refuted.
3. `.scratch/test-framework/issues/13`, `14`, `15` — the work queue, in that order.

## Where session 1's plan turned out to be wrong

Both of the verifications it mandated were built on false premises, and finding that out was
worth more than the code.

- **F18 is refuted.** The microphone is 106 dB down in the master mix, not audible. Its evidence
  was a *normalised ratio*, which reads ~0.84 whether the mic is at full strength or attenuated
  to nothing; the control that separates them by 95.7 dB did not exist until this session.
  There is no F18 → F12 chain.
- **The CVar the handoff proposed did not exist.** `au.EnableAudibleDefaultEndpointSubmixes` is
  really `au.submix.audibledefaultendpoints`, defaults to 0, and gates a loop that only admits
  `UEndpointSubmix`. Run as written it would have printed "no change" and been read as a
  refutation for the wrong reason.
- **Verification 2 was vacuous.** F12's fixture never put the player's voice in the reference —
  `farEnd` is `welcome.wav` alone (`aec_erle_test.cpp:227`). Nothing to re-measure.
- **F16 was partly wrong**, corrected while rewriting issue 12: 46 files not 43, three do
  reference `WITH_CONVAI_TESTS`, five console commands not three, and
  `GetSessionProxyForTesting()` *is* gated and does not ship.

## The result worth carrying forward

**F26.** In-engine, echo cancellation leaks the character's own voice into the player's
transcript, 2 runs in 9, with the `None` control firing 5/5 so the number means something. The
same canceller offline is healthy (F14, 40–49 dB ERLE). That is ADR-0004's bisection resolved:
**the algorithm is fine, the integration is not.**

The two symptoms are different bugs in different repositories, and conflating them is what cost
three sessions:

| Symptom | Where | Issue |
|---|---|---|
| "player speech cut or swallowed" | DLL — F12, reproduces offline in 3 s with no plugin | 15 |
| "character responds to its own voice" | plugin — F26, leaks only in-engine | 13, 14 |

## Traps this session hit that are not recorded elsewhere

- **The Bash tool is Git Bash, and MSYS rewrites `/Game/Maps/Landing` into
  `C:/Program Files/Git/Game/Maps/Landing`.** The run launches, loads the wrong map, produces no
  report and looks like a crash. **Use the PowerShell tool for `run.py`.** This cost a full run
  before it was spotted.
- **Any running Unreal editor blocks the build**, including one on a completely different
  project — Live Coding holds the lock process-wide. The message is
  `Unable to build while Live Coding is active`. It appears mid-log, so a `Select-String` filter
  on `error|Result:` hides it and the build looks like an unexplained `OtherCompilationError`.
  Capture the whole build log to a file when a failure has no error line.
- **Live scenarios hang after passing, at 5 launches in 10** (F25, the already-proven DLL
  deadlock). `run.py` now takes the report and kills the process, so this costs ~60 s instead of
  600. Orphaned editors survive a killed orchestrator — check for `UnrealEditor-Cmd.exe` with
  `Responding: False` before debugging anything else, and **check `ParentProcessId` before
  killing**: the maintainer runs their own `uvicorn` and VSCode Python processes.
- **`FFileHelper::SaveStringToFile` defaults to AutoDetect**, which writes UTF-16LE the moment
  any string is non-ASCII. One em dash in a fail reason made every report unreadable and aborted
  a whole batch. Fixed, but the class of bug will recur anywhere else the harness writes files.
- **Scenario names are prefix-colliding** (`reference_feed_capture` vs
  `..._under_load`). `run.py` launches one process per scenario using a new exact-match
  `-scenario=` argument; the substring `-filter=` is for selecting the *set*.

## The discipline lesson, stated plainly

Five conclusions were drawn and then had to be retracted this session. Four of the five came
from **single live runs**. The live scenarios talk to an LLM that says something different every
time; two consecutive runs of the same build produced opposite results. The 5- and 10-repeat
sweeps are what settled every one of them.

The worst of the five was in the framework's own code: `aec_echo_only`'s fixture self-check was
written as `EchoPeak <= 0.0f`, which only rejects *exactly* zero — so a run carrying 8.9e-06 of
echo, meaning none at all, passed and reported 126 "leaks" that were F24 hallucinations on
silence. That is precisely the failure the PRD's negative-control rule exists to catch, and the
check was too weak to catch it. **Assume the next plausible-looking result is wrong until its
control exists and the sample is bigger than one.**

## State of the suite

Twelve scenarios, five of which are controls. One process per scenario. On the Landing map an
offline scenario costs ~30 s and a live one ~60 s.

```
set CONVAI_UE_PROJECT=E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject
set CONVAI_UE_EDITOR=E:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
python Source/ConvaiTests/run.py --repeat 10 --filter aec_echo_only_internal --map /Game/Maps/Landing
```

Red on purpose, all reproducing shipped defects: `adopted_capture_component_routing` (F19),
`aec_echo_only_internal` intermittently (F26), and
`AecNearEndPreservation.ReferenceWithNoEchoMustNotSuppressThePlayer` in the DLL repo (F12). Do
not loosen these to get a green run.

## What is owed

**Issues 13, 14, 15** are the queue and are written up in full. Then 07–11, and issue 12's
actual work (its text is rewritten, nothing is implemented).

**Issue 05 is half done.** Injected Echo, the paired AEC run and its control exist and work. The
double-talk variant and the N > 1 variant do not — and the double-talk one matters more than its
position suggests, because it is the only thing that would catch issue 14's fix trading F12
against F26.

**Issue 04 is done.** Step library, WAV loading, live transcript at WER 0 against `STT.json`.

**Untested and it is the real gate on the maintainer's plan:** the PRD's **V5** — a fix agent,
given only `report.json`, locating and verifying a fix for a seeded bug without reading the
framework's source. The maintainer intends to run agents in a loop across both repos. V5 has
never been run, and until it is, nobody knows whether the report is actually actionable.

## Environment

Unchanged from session 1 and still true: no minimal test project, so runs use `Dev_WebRTC`'s
`Landing` map, which connects to the live backend. `TestCharacterID` and the API key live in
`[/Script/Convai.ConvaiSettings]` in the project's `DefaultEngine.ini`;
`UConvaiUtils::GetTestCharacterID()` reads it and accepts a `-ConvaiTestCharacterID=` override,
so the framework holds no credentials.

Issue 01 configurations 1, 3 and 4 remain unrun. Configuration 3 needs the machine's audio
endpoint disabled — ask first.

## Skills for the next session

- `tdd` — issue 13 is instrumentation with a measurement to satisfy, and 07–10 are test
  construction.
- `diagnose` — issue 14 is a controlled A/B with a stated hypothesis and a cheap discriminator.

Not `grill-with-docs` and not a re-walk of the design tree. It has now been argued against
measurement twice. Argue with FINDINGS and with issues 13–15.
