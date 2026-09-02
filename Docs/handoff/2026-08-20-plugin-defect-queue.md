# Fix the seven plugin defects behind the red scenarios

Date: 2026-08-20
Follows: [2026-08-20-dll-ab-study.md](./2026-08-20-dll-ab-study.md), and the issue queue that
came out of it (now closed — see below).
Branch: `WebRTC-Video`, 27 commits ahead of `origin/WebRTC-Video`, **not pushed, no PR**.

Your job is **F19, F12, F34, F25, F9, F21, F16**. They are the defects a customer can hit that
nobody has owned. Three of them cannot be answered from this repository alone; the repositories
that can answer them are named per finding.

## Where things stand

The previous session closed eleven of fourteen issues from the A/B study and two product
findings (F23, F38), merged as `6a5f31f1`. What that changed for you:

- The suite is **19 of 22 green**. The three reds are F19, F26 and F12/F34 — two of which are
  yours. A green run is now worth something it was not before: latency metrics carry real units,
  a scenario that could not start no longer reports `fail`, every merged report names and hashes
  the DLL it graded, and the scenario table marks flakes and says how much of the suite could
  separate anything at all.
- `EndPlay` no longer crashes teardown. That was `CONVAI_LOG` expanding to two statements with
  no `do { } while (0)`, so a brace-less `if (x) CONVAI_LOG(...)` ran the file log
  unconditionally. **If you write `if (cond) CONVAI_LOG(...)` anywhere, it is one statement
  now** — but the macro still evaluates its arguments twice.
- `Source/ConvaiTests/test_run.py` holds ten runnable checks for the runner itself. Run it after
  touching `run.py`; it needs no engine and no backend.

## Read in this order

| Path | Why |
|---|---|
| `.scratch/test-framework/FINDINGS.md` — **status index at the top** | Every finding classified open / fixed / measurement, with what was re-verified against 0.2.12 on 2026-08-20. Your seven are in the open table. Read each one's full entry before touching its code. |
| `.scratch/test-framework/ISSUES.md` | Only the three open issues now. I8 is F25 seen from the suite's side and carries the arithmetic for verifying a teardown change. |
| `.scratch/test-framework/CLOSED-ISSUES.md` | How the last eleven were closed. Read I7's entry before you trust any callstack in this codebase, and I1's before you trust that the DLL you think you are running is the one that ran. |
| `Docs/ConvaiTests.md` | Build, run, flags, output layout, known quirks. |
| `Docs/adr/0004-test-layer-partition.md`, `Docs/adr/0005-tests-as-a-strippable-module.md` | Binding. F16 is a direct consequence of 0005 not being finished. |
| `convai-livekit-cpp-p/docs/adr/0001-vendor-upstream-livekit-cpp-in-tree.md` | In the **DLL** repo, not this one. Binding for F25: `src/livekit/` is vendored upstream, so a patch there is a divergence to justify. |

Do not re-derive what those establish. If you disagree with a recorded conclusion, say so and
show the evidence — two entries in FINDINGS were overturned that way, and one of them
(F18 → F12) had a whole causal chain built on it.

## Ground rules

**1. Never weaken the oracle to go green.** `run.py` fingerprints the assertion constants in
`ORACLE_CONSTANTS` and the `kArms` table and reports any change as its own finding. Moving a
threshold, renaming a constant, deleting a fixture or flipping an arm's bool produces a report
indistinguishable from a real fix. If a threshold is genuinely wrong, change it in its own commit
and justify it from measurements.

**2. A fix is not fixed until it is measured against a control.** The backend drifts hour to
hour. Three "fixes" in the A/B study were backend drift, caught only because a control arm ran
alternated in the same session. Two apparent AEC fixes were runs that simply had 4–6x less echo
energy — visible only in `echo_source_nonsilent_samples`, not in pass/fail. Before/after on one
arm proves nothing.

**3. For any AEC result, check `echo_source_nonsilent_samples` and `mic_window_rms` on the
passing runs before claiming a cancellation improvement.** A quiet run passes for the wrong
reason. This is the single most repeated mistake in this project's history.

**4. Respect the module boundary.** `ConvaiTests` reaches the shipping module only through public
extension points — no `#if`, no test-only branches, no widening private API (ADR 0005).

**5. Fix root causes, not the caller the finding names.** F19's evidence names a scenario; the
defect is in the extension point. I7's callstack named `EndPlay`; the defect was a macro three
files away. Grep every caller before editing one.

**6. Branch and commit per finding.** `git checkout -b fix/<slug>` from `WebRTC-Video`. Never
commit on `main`/`master`/`stg`. Conventional Commits, imperative subject under 50 characters.

**7. Update the tracker as you go.** A finding you close gets its entry in `FINDINGS.md` updated
in place — what changed, what you ran, the numbers on both sides — and its row moved in the
status index. A finding you could not close gets a note saying why, not silence.

**8. Backend repos are read-mostly.** `core-service` and `ConvAI_Middleman` are live services.
Read them to understand the contract and to locate a defect. Do not push changes there as part of
this session without saying so explicitly and getting agreement — a plugin-side workaround that
is honest about the server's behaviour is usually the better first move, and sometimes the
finding's whole value is a precise bug report handed to that repo's owner.

## The repositories

| Path | What it is | Which findings need it |
|---|---|---|
| `E:\UEProjects\UE5.8\Dev_WebRTC\Plugins\Convai-UnrealEngine-SDK-Dev` | this plugin, branch `WebRTC-Video` | all seven |
| `E:\Livekit\convai-livekit-cpp-p` | the native `convai_client` DLL and its gtest suite; `src/livekit/` is vendored upstream | F25 |
| `E:\Convai\Git\core-service` | the real-time pipeline behind the character: pipecat 1.5, RTVI packets, VAD/STT, turn and interruption handling, `client_version` in invocation metadata | F9, F12/F34 |
| `E:\Convai\Git\ConvAI_Middleman` | the REST/gRPC service in front of it (Flask blueprints, `grpc_app.py`, Python 3.12) | F9 if the version check is not in core-service |

Both backend repos have their own agent instructions — `core-service/AGENTS.md`,
`ConvAI_Middleman/CLAUDE.md`. Read them before writing anything there.

---

## F21 — the in-engine runner's exit code does not track the outcome

**Confirmed again 2026-08-20.** A launch that reported `CONVAI_TESTS summary passed=0 failed=0
setup_failed=1` still exited **0**, even though `Complete()` calls
`FPlatformMisc::RequestExitWithStatus(/*Force=*/false, 1)`
([ConvaiTestRunner.cpp](../../Source/ConvaiTests/Private/ConvaiTestRunner.cpp), `Complete()`).

Harmless today only because `run.py` grades the report file and ignores the exit code — which is
itself worth keeping in mind: the harness works around this rather than depending on it.

**Where to look.** Whether `RequestExitWithStatus`'s code survives `-game -unattended` shutdown
in UE 5.8, or whether something later in teardown overwrites `GExitCode`. The interesting case is
that the same launch is *also* frequently killed by the runner after its report lands (F25), so
distinguish "the code was never set" from "the process never got to return it".

**Done looks like.** A launch whose scenario failed exits non-zero, a launch whose scenario
passed exits zero, and `run.py` can be shown to agree with the report on both. Cheapest tier:
one scenario, two runs, no backend needed if you use a scenario with
`RequiresLiveConnection() == false`.

**Trap.** Do not make `run.py` depend on the exit code once it works. The report is the result;
the exit code is for humans and CI.

## F16 — the "dead" in-plugin test harness compiles into the shipping module

**Confirmed again 2026-08-20, and it is not gated.** `Source/Convai/Private/Tests/` holds
**31 files**; only two of them mention `WITH_CONVAI_TESTS`, and the macro is defined `0` in
[Convai.Build.cs](../../Source/Convai/Convai.Build.cs). So the other 29 compile into `Convai`,
a `Runtime` module shipped to customers, and
`Source/Convai/Private/Tests/ConvaiTestHarnessSubsystem.cpp` — which has no guard at all —
registers `Convai.Test.Run` and `Convai.Test.RunAll` as console commands in a customer build.

`Convai.uplugin` also ships `ConvaiTests` as a `Runtime` module. That is a second copy of the
same problem and was the reason the orchestrator once silently drove the wrong harness (F16's
own entry).

**Where to look.** ADR-0005 is the argument; issue `.scratch/test-framework/issues/12-release-stripping.md`
is the plan, and F16 records that its premise is wrong — this is removing *live, reachable,
customer-facing* code, not deleting dead source, so it is a behavioural change.

**Done looks like.** A Shipping build of the plugin contains neither harness: no
`Convai.Test.*` console commands, no `ConvaiTests` module. A Development build still runs the
suite. State plainly which files were deleted versus gated, and what a customer loses.

**Trap.** Check what else includes those headers before deleting. `ConvaiPlayerComponent.h` has a
`WITH_CONVAI_TESTS` block of its own, and `Public/Tests/ConvaiTestMacros.h` is included from
outside the Tests folder.

## F9 — the server does not recognise the client version, and the plugin files the reply as a "notice"

**Confirmed again 2026-08-20**, once per connecting launch:
`Server compatibility notice: Client version unknown. Compatibility issues may occur.`

Two halves, and the second is the one this repo owns.

**What the plugin sends.** `UConvaiUtils::GetClientVersion()`
([ConvaiUtils.cpp:696](../../Source/Convai/Private/ConvaiUtils.cpp#L696)) returns a **PIE session
timestamp**, not a version: today's runs advertised `client_version: PIE_20260820_180030`. In a
packaged build it takes the non-editor branch further down the same function. `-ClientVersion=`
overrides it. The native DLL's own version travels separately as
`extra_metadata.convai_client_version` and was `0.2.12.320+3b122f2`
([ConvaiSubsystem.cpp:574-600](../../Source/Convai/Private/ConvaiSubsystem.cpp#L574)).

**What the server does with it.** `core-service` reads `client_version` out of the invocation
metadata (`server.py:253`, `server.py:1153`, `models/api.py:306`, `utils/api_event.py:234`) and
has a `docs/versioned-character-runtime.md` describing version-aware character connections. **The
string "Client version unknown" is in neither backend repo** — find who emits it before assuming
which service decides. Candidates: the character-API service that
`versioned-character-runtime.md` names, `ConvAI_Middleman`, or the pipecat/RTVI layer.

**The plugin's half.** That text arrives as an **`error-response`** packet and is logged as
`Server compatibility notice: %s` at Warning
([ConvaiSubsystem.cpp:2711-2722](../../Source/Convai/Private/ConvaiSubsystem.cpp#L2711)) and then
discarded. Every `error-response` the server ever sends is labelled a compatibility notice and
never reaches the game — the same shape as F38, which was fixed by raising `OnFailureEvent`.

**Done looks like.** Either the plugin advertises a version the server recognises, or the server
is shown to be wrong about it and that is filed where it belongs — plus, in this repo,
`error-response` stops being mislabelled and reaches the game. Say which of the two you did.

**Trap.** `GetClientVersion()`'s PIE timestamp is deliberate: it exists so every PIE session is
traceable. Do not delete that property; find out what the server needs alongside it.

## F19 — a capture component supplied the documented way is inside Reference Audio

**Still red, verified 2026-08-20: `adopted_capture_component_routing` failed 2 of 2 launches.**
Failed 20 of 20 across the A/B study.

A capture component supplied through `IConvaiAudioCaptureInterface` — the documented extension
point — is adopted but never assigned a `SoundSubmix`, so it renders into the master mix. Two
consequences from one cause: the customer's microphone is **audible through the game's speakers**,
and it is inside **Reference Audio**, which is the input the canceller subtracts. Nothing warns.

**Where the code is.** `UConvaiPlayerComponent::SetAudioCaptureComponent`
([ConvaiPlayerComponent.cpp:520](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L520))
adopts it and stops the default one. The default component gets its submix at construction, in
`OnComponentCreated` ([:118](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L118)), from
`/ConvAI/Submixes/AudioInput.AudioInput`.

**Fix direction, from the finding:** assign the submix where the component is *adopted*, not
where it is constructed, so discovery and routing cannot come apart. A warning when an adopted
component has no `SoundSubmix` is the smaller version and does not fix the default.

**Verification.** Tier 1 — this is deterministic. `--filter adopted_capture_component_routing
--repeat 3`, and the control is the plugin's own routing measured on the same tone in the same
build: `mic_not_in_reference_audio` / `mic_in_reference_audio_control`. Neither number means
anything alone. Run all three.

**Trap.** The Virtual Mic the suite adopts is itself a third-party capture component, so this
finding and the test framework's own microphone share a mechanism. A fix that makes the scenario
pass by special-casing `UConvaiVirtualMicComponent` fixes nothing.

## F12 / F34 — the player's speech is destroyed during double-talk

**Still red, verified 2026-08-20: `aec_double_talk` failed 1 of 2 launches — flaky red, not
fixed.** It failed 17 times running in the A/B study and then passed three attempts on both arms
at once, which is what backend drift looks like.

F12: with an active reference and no echo, the player's speech does not come back. F34 is the
same thing measured in-engine: with the player talking over the character, the canceller
attenuates by **14.7 dB across the window and 8.8–19.4 dB in every single second**, and the
player's transcript comes back empty — against 0–3 dB when only the echo is present. The
canceller is removing the near end along with the echo.

**Read before hypothesising.** F27, F29, F32, F33, F35, F36 and **F37** — several obvious
hypotheses are already refuted, and F37 is the current state of the investigation. F33 in
particular: a defect that provably breaks cancellation offline did not move the in-engine number,
so the 3 dB the in-engine metric reads is the suppressor, not subtraction.

**Both sides are in play, and this is why the backend repos are listed.** The scenario measures
"the player's transcript did not come back", which can be the canceller destroying the audio
before it is sent, or the server not transcribing what it received. Separate them before fixing
anything:

- Plugin side: `aec_erle_test` in `convai-livekit-cpp-p` is the offline oracle for the canceller
  itself; `run.py --tier dll` runs it. The in-engine attenuation metric carries a `metric_notes`
  entry saying what it cannot separate — read it.
- Server side, in `core-service`: `handlers/transcription_handler.py`,
  `handlers/rtvi_client_msg_handler.py` (the `UserStartedSpeakingFrame` interruption branch),
  `pipelines/bot.py` for the VAD/STT wiring, and `models/rtvi.py`. The question to answer is
  whether the server suppresses or discards user audio while the bot is speaking.
- Already ruled out, so do not spend time on it: the **input gate** in
  `actors/room_session_coordinator.py` / `actors/bot_actor.py` is roster-rooms only — it raises
  unless `room_kind == "multi_character_v0"` — and the AEC scenarios are single-character. The
  plugin's `ToggleSTT` is driven only by `UnmuteStreamingAudio`/`MuteStreamingAudio`
  ([ConvaiPlayerComponent.cpp:1030, 1050](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L1030)),
  not by bot speech.

**Verification.** Tier 2, and this one genuinely needs it: the scenario is flaky, so before/after
on one arm is worthless. Alternate arms in one session with `.testruns-ab/ab_sweep.py`, report
Fisher exact and `runs_needed_per_arm`, and check the echo-energy metrics on every passing run
before claiming anything.

## F25 — the teardown hang

**Re-measured 2026-08-20: 25 of 44 launches hung, 56.8%** (Wilson 42–70%), against the study's
pooled 331/572 = 57.9%, `p = 1.0000`. No DLL build tested has ever moved it. Tracked from the
suite's side as **I8**, whose entry carries the full arithmetic.

The wait is one line: `LocalParticipant::unpublishTrack` calls `unpublishTrackAsync(...)` then
`fut.get()` with no timeout —
`E:\Livekit\convai-livekit-cpp-p\src\livekit\local_participant.cpp:236-239`, verified present
today. The resolving event is delivered by the process-global **FFI Dispatcher Thread**, and on a
disconnect the server has already begun it can simply never arrive.
`ConvaiClientImpl::Disconnect` reaches it through the pre-unpublish at
`src/convai/convai_livekit_client.cpp:466-484` — the mitigation put a blocking call on the path
it was protecting.

**What this repo contributes.** Both of the plugin's `Disconnect()` calls are synchronous on the
calling thread and hold `ConvaiClientMutex` —
[ConvaiSubsystem.cpp:1434](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1434) and
[:1759](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1759) — so at teardown the game thread
is what blocks inside that `fut.get()`.

**Two routes, and they are not equivalent.**

1. **Bounded wait in the DLL** (correct fix). `src/livekit/` is vendored upstream per ADR-0001, so
   a timeout on `fut.get()` itself is a vendoring divergence and belongs in a patch upstream would
   take. The Convai-layer alternative is to give the pre-unpublish its own thread and a deadline —
   which reorders teardown so `room->disconnect()` can begin with an `unpublishTrack` in flight,
   the same concurrency shape that previously produced the `remove_track` access violation and the
   `StopRemoteAudioReader` use-after-free.
2. **Plugin-side**: move the disconnect off the game thread with a deadline. Cheaper to try,
   but a process-exit hang is not obviously cured by moving the block off one thread, and the
   mutex would then be held by a thread nobody waits for.

**Verification.** Tier 2, alternated, and it is affordable: separating 55% from 10% needs
**16 runs per arm** by `run.py`'s own `runs_needed_per_arm` — roughly 20 minutes per side. There
is no excuse for shipping this one unmeasured.

**Trap.** `run.py` kills a hung launch after its report settles, so the suite stays green through
the hang and `launches_without_report` stays 0. The number that moves is `launches_hung_on_exit`,
and nothing else will tell you.

---

## Suggested order

1. **F21** — cheapest, no backend, and it makes every later run's exit code mean something.
2. **F16** — mechanical but behavioural; do it while the tree is quiet, and rebuild Shipping to
   prove it.
3. **F9** — the plugin half (`error-response` mislabelled and dropped) is small and self-contained;
   the server half is a question you can hand over with a precise trace.
4. **F19** — deterministic, tier 1, and the control scenarios already exist.
5. **F12 / F34** — the hard one. Separate plugin from server first; do not start by changing the
   canceller.
6. **F25** — last, because it is the riskiest change and the only one whose correct fix lives in
   another repository.

## Environment

Build. No editor may be running, or Live Coding fails the link with `LNK1104`:

```powershell
& "E:\Software\UE_5.8\Engine\Build\BatchFiles\Build.bat" Dev_WebRTCEditor Win64 Development -Project="E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" -WaitMutex
```

Run the suite. PowerShell, not Git Bash — MSYS rewrites map paths:

```powershell
python Source\ConvaiTests\run.py --project "E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" --editor "E:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" --tier engine --repeat 3 --filter <scenario> "--arg=-ConvaiTestCharacterID=903b069a-485d-11f1-a04c-42010a7be02e"
```

- `--arg -Foo` and `--arg=-Foo` both work now; in PowerShell the `=` form still needs the quotes.
- The runner refuses a sweep with no character configured and names the scenarios that need one.
- A full 22-scenario pass costs about 9 minutes; the AEC scenarios are ~60 s each.
- The deployed DLL is `0.2.12.320+3b122f2`. Every merged report now records the version and the
  sha256 of what was in `Binaries\Win64`, and flags `stale_against_staged` if the staged copy
  disagrees. If you swap a DLL, follow `Docs/ConvaiTests.md` — all three locations — and check
  the console line before believing the run.
- Prod character IDs:
  `E:\Livekit\convai-livekit-cpp-p\build\windows-x64-tests\tests\Release\parallel.txt`.
- Releases for A/B swaps and the A/B harness are in `.testruns-ab/` (gitignored). **0.1.26 no
  longer exists on this machine** — its source directory was in a temp scratchpad that has since
  been cleaned. 0.2.10, 0.2.12 and Hotfix1 are in `.testruns-ab/releases/`.
- `python Source\ConvaiTests\test_run.py` — ten checks on the runner itself, no engine, no backend.

## Verification tiers

Pick the cheapest tier that can actually detect the defect, and say which you used.

- **Tier 0** — deterministic, no backend. F21, F16, and the plugin half of F9. Verify by running
  the affected thing and reading the output. Leave one runnable check behind.
- **Tier 1** — one scenario, few runs. F19: it has failed 20 of 20, so three clean passes plus
  three control runs on the unfixed build is convincing.
- **Tier 2** — alternated sweep with a control. F25 and F12/F34, and anything expressed as a
  rate. `.testruns-ab/ab_sweep.py` alternates blocks and gates every block on the logged DLL
  version; `ab_report.py` aggregates and reuses `run.py`'s statistics. Reuse them.

## Definition of done

A finding is closed when the defect no longer reproduces by the method that first showed it, a
check exists that would fail if it came back, the change is committed on its own branch, and its
entry in `FINDINGS.md` records what changed and how it was verified — including the control.

Anything you cannot close gets an entry saying why and what would answer it. **Do not close a
finding by declaring it not-a-bug without evidence, and do not report "all tests pass" as
progress** — the suite's own report will tell you how many of its scenarios could separate
anything at all, and the answer is usually "none".

## Suggested skills

- `diagnose` for F12/F34 and F25 — both are rate-shaped and both have a history of confident
  wrong answers.
- `tdd` for F21 and the plugin half of F9 — each has a stated assertion you can write first.
- `triage` if the seven need re-ordering once the cheap ones land.
