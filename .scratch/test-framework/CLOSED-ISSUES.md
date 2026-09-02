# Closed — the A/B study's issue queue, I1 to I13

Closed on 2026-08-20 and moved out of [`ISSUES.md`](ISSUES.md), which now carries only what is
still open. Kept whole rather than summarised: each `Status:` line records the method that first
showed the defect, the method that showed it gone, and the numbers on both sides. That is the
part a later session needs when a symptom comes back — a one-line "fixed" would not survive
contact with a regression.

Merged into `WebRTC-Video` as `6a5f31f1`, 26 commits. The runnable checks left behind live in
`Source/ConvaiTests/test_run.py`.

| | Issue | Closed by |
|---|---|---|
| I11 | `--arg` cannot pass a value beginning with `-` | splicing `--arg <v>` into `--arg=<v>` before argparse |
| I2 | Every latency metric is named `*_seconds` and carries milliseconds | 14 labels renamed to `*_ms`, one value rescaled |
| I13 | A second `convai_client.dll` sits under `Context/` | 762 MB release tree and its zip moved to `.testruns-ab/releases/` |
| I3 | A scenario that could not start is reported as `fail` | a `setup-failed` status, out of `pass_rate`, still non-zero exit |
| I6 | `audio_roundtrip` blames the character for a missing packet | `bot_spoke` from `GetIsTalking()`, and two findings instead of one |
| I4 | The suite cannot prove which DLL it graded | version at `Display`, plus binary hashes in every merged report |
| I1 | A stale DLL in `Binaries` beats a fresh one in `ThirdParty` | compare-before-skip and a Warning; `stale_against_staged` in the report |
| I12 | `TestCharacterID` is empty | `needs_character` per scenario; the runner refuses a sweep it cannot run |
| I7 | `UConvaiPlayerComponent::EndPlay()` null-derefs | `CONVAI_LOG` wrapped in `do { } while (0)` |
| I10 | Five scenarios flake with nothing marking them as flaky | `flaky` flag and `pass_rate_ci95` per scenario |
| I9 | 17 of 22 scenarios carry no discriminating power | reported, not fixed — separability verdicts in the scenario table |

---

## I11 — `--arg` cannot pass a value beginning with `-`

Status: `done` — 2026-08-20. `run.py` splices `--arg <value>` into `--arg=<value>` in
`splice_arg_values()` before argparse sees argv, so the form `--arg`'s own help text documents
now works; `docs/ConvaiTests.md`'s "needs the `=` form" paragraph was corrected with it.
Verified (tier 0): `python Source\ConvaiTests
un.py --tier dll --arg -AllowStdOutLogVerbosity`
errored with `argument --arg: expected one argument` before the change and reached the tier
logic after it. `Source/ConvaiTests/test_run.py` holds the check — two assertions, one that the
dashed value survives, one that no other token is rewritten and a trailing bare `--arg` still
reaches argparse as an error.

```
$ python run.py … --arg -AllowStdOutLogVerbosity
run.py: error: argument --arg: expected one argument
```

argparse reads the leading dash as the next option. Every editor argument worth passing
through starts with a dash, so the documented example in `--arg`'s own help text
(`--arg -AECReferenceTap=Listener`) does not work as written. `--arg=-Allow…` does.

**Fix shape.** One line — `nargs=...`, or just correct the help text to the `=` form.
Minor, but it is the first thing anyone hits when using the flag.

## I2 — Every latency metric is named `*_seconds` and carries milliseconds

Status: `done` — 2026-08-20. Renamed to `*_ms` at all 14 label sites across the five scenarios
that time anything, and the tracker's header now states the unit it records.

**Worse than filed.** `connect_seconds` carried *two* units in the same merged report:
`FConvaiTestLatencyTracker` milliseconds in `audio_roundtrip` and `text_roundtrip`, and real
seconds in `live_player_transcript*`, which built the metric by hand from `ConnectedAtSeconds`
([ConvaiLiveTranscriptScenario.cpp:215](../../Source/ConvaiTests/Private/Scenarios/ConvaiLiveTranscriptScenario.cpp#L215)).
`.testruns-ab/0_1_26/20260819_113212_ac69ea25/merged.json` has `connect_seconds: 3686.6` and
`connect_seconds: 3.41` in it. `.testruns-ab/ab_perf.py` pooled that key across scenarios, so its
`connect` series mixed the two; the study's quoted cold-connect and disconnect medians come from
`cold_connect_*`/`disconnect_*`, which were milliseconds throughout and are unaffected. The
live-transcript value is now scaled to milliseconds so one name means one unit; `ab_perf.py`
reads `*_ms` first and rescales the legacy live-transcript key when reading old reports.

Verified (tier 0, plus one live launch per shape): built the editor target, then
`--filter session --repeat 1` reported `cold_connect_ms 3527.1`, `disconnect_ms 5612.6`,
`warm_connect_ms 22538.3`, and `--filter live_player_transcript_aec_none --repeat 1` reported
`connect_ms 3355.8` — the same ~3.4 s that used to print as `connect_seconds: 3.41`.
`Source/ConvaiTests/test_run.py` greps the scenarios for `TEXT("*_seconds")` and fails if one
returns: 12 offenders on the parent commit, 0 now.

`FConvaiTestLatencyTracker::Mark` stores `(FPlatformTime::Seconds() - StartSeconds) * 1000.0`
([ConvaiTestEventRecorder.cpp:114](../../Source/ConvaiTests/Private/ConvaiTestEventRecorder.cpp#L114)),
and every caller labels the result `cold_connect_seconds`, `warm_connect_seconds`,
`disconnect_seconds`, `connect_seconds`, `player_transcript_seconds`,
`bot_turn_completed_seconds`.

Reports therefore read:

```
"cold_connect_seconds": 5992.7
"disconnect_seconds":   9795.0
```

A connect does not take 100 minutes. Anyone — or any agent — reading the report at face
value is out by 1000×, and a threshold written against the name rather than the observed
values would be off by the same factor.

**Fix shape.** Rename to `*_ms`, or divide. Renaming is safer: the recorded values in
`.testruns/history/` stay meaningful, and the oracle fingerprint already tracks constants
by name so the change is visible.

## I13 — A second `convai_client.dll` sits under `Context/`

Status: `done` — 2026-08-20. Moved, not deleted: `Context/ConvaiWebRTC-v0.2.10/` and
`Context/ConvaiWebRTC-v0.2.10.zip` now live under `.testruns-ab/releases/` as `x210` and the zip
beside it, and `swap-convai-dll.ps1` looks there by default.

**Bigger than filed.** Not one 26 MB DLL but the whole extracted release, every platform:
762 MB under `Context/`, plus the 234 MB zip it came from. The win64 subtree is in exactly the
`ThirdParty/ConvaiWebRTC/lib/release/win64` layout the swap script copies from, which is what
made it a foot-gun rather than clutter. The extracted tree is redundant with the zip
(`convai_client.dll` sha256 `b9a99db5…` in both, matching the hash filed here), so it can be
pruned to the win64 dir or dropped entirely without losing the release.

**Found while closing it.** The swap script's `-SourceRoot` pointed at a session scratchpad
under `%TEMP%`, and that directory has since lost `x26` — **0.1.26 is no longer on this machine
and no zip of it survives here**, so the 0.1.26 arm of the study cannot be re-run. `xhf` and
`x212` were still there and were copied into `.testruns-ab/releases/` before they went the same
way. `Context/` held the only remaining copy of the 0.2.10 binary, which is why this closed as a
move rather than a delete.

Verified (tier 0): `Source/ConvaiTests/test_run.py` greps the plugin for the five staged artifact
names outside `Source/ThirdParty`, `Binaries`, `.testruns` and `.testruns-ab` — 5 strays before
the move, 0 after. The swap script parses and its `$srcMap` now carries `0.2.12` as well.

```
Plugins\Convai-UnrealEngine-SDK-Dev\Context\ConvaiWebRTC-v0.2.10\ThirdParty\ConvaiWebRTC\lib\release\win64\convai_client.dll
  sha256 B9A99DB5…  == the 0.2.10 release binary
```

Harmless today — `Context/` is not on any load path, and the swap script reported it as a
stray before every one of the ten blocks. Recorded because it is precisely the shape of
thing that, combined with I1, produces an invalid sweep nobody notices. A 26 MB binary is
also not what `Context/` is for; the directory is raw notes.

## I3 — A scenario that could not start is reported as `fail`

Status: `done` — 2026-08-20. `FConvaiScenarioResult::bSetupFailed` carries the condition the 13
scenarios already knew about into the report: status `setup-failed`, its own `setup_failed` count
in the run summary and in each scenario's merged entry, a `Warning` instead of an `Error` in the
log, and `[SETUP-FAILED]` instead of `[FAIL]`. In `merge()` those launches leave `pass_rate`'s
denominator and the denominator behind every finding's occurrence rate — a launch that never
started could not have produced a finding either. `no_report` deliberately stays in both: a
launch that vanished is a failure of the run, not of its inputs. The sweep still exits non-zero.

Verified (tier 0 for the merge arithmetic, one live launch per side for the shape):

```
no character   text_roundtrip: 0/0 passed, 1 COULD NOT START     run.py exit=1
               report: {"passed": 0, "failed": 0, "setup_failed": 1}
                       {"name": "text_roundtrip", "status": "setup-failed"}
               log:    CONVAI_TESTS [SETUP-FAILED] text_roundtrip (0 ms)
                       Warning: CONVAI_TESTS could-not-start=setup failed; see trace
with character text_roundtrip: 0/1 passed                        run.py exit=1
               FINDING no-bot-transcript-for-text-prompt (1/1)   F23, still red
```

The control matters: the point of the change is that a real red stays red and only the
un-runnable case moves columns. `Source/ConvaiTests/test_run.py` asserts the merge arithmetic on
all four cases — mixed, `no_report`, all-setup-failed, and a finding's denominator.

**Noticed while closing.** The report carries `"schema": "convai-tests/1"` with the comment that
it exists "so the Python side can refuse a report it does not understand", and `run.py` never
reads it. Nothing validates either schema string today; the merged one is bumped to
`convai-tests-merged/5` here for the new field. Not fixed, not in this queue.

With `TestCharacterID` unset (I12), `text_roundtrip` returns:

```json
{"status": "fail", "duration_ms": 0.03,
 "fail_reason": "setup failed; see trace",
 "metrics": {"connected": 0, "bot_turns": 0}}
```

and the trace says `no test character; set TestCharacterID … or pass -ConvaiTestCharacterID=`.

That is a missing input, not a defect in the product. It is reported in the same column as
a real failure, so a sweep run without the character reads as twelve product failures.

The suite already holds the correct discipline in the other direction — `run_offline_tier`
insists that "a tier that did not run is reported as not run, never as passing", and the
F12 skip gets its own column precisely so a skip cannot be read as green. The inverse rule
is missing: **a scenario that could not run must not be reported as failing.**

**Fix shape.** A `setup-failed` (or `not-run`) status, counted in its own column in
`merge()` and excluded from `pass_rate`'s denominator the way `no_report` is not — with
the runner's exit code still non-zero, because a sweep that could not run is not a pass.

## I6 — `audio_roundtrip` blames the character for a missing packet

Status: `done` — 2026-08-20. The scenario had no way to see the character speak: the sink binds
transcription, failure, turn-completed and interrupted, and the bot's transcript never arrives at
all (F23 — `bot_transcript_chars` is 0 on **passing** runs too, all 26 of them). So the whole
response half rested on `OnBotTurnCompleted`, and its absence read as "the character did not
answer".

It now polls `UConvaiChatbotComponent::GetIsTalking()` each tick — the same public state
`aec_echo_only` already polls, so no new seam — latches it as `bot_spoke` with a
`bot_started_speaking_ms` milestone, and splits the failure in two:

| condition | reason |
|---|---|
| `bot_turns == 0`, character spoke or its transcript arrived | the character answered but the turn never completed (`no-bot-turn-completed-after-character-spoke`) |
| `bot_turns == 0`, neither | the character did not answer the player's speech (`no-character-response-to-speech`) |

Pass/fail is unchanged — a turn that never completed still fails. What changed is which of two
mechanisms the report names.

Verified (tier 1, plus fault injection for the branch the backend would not produce):

```
10 clean runs   10/10 pass   bot_spoke=1 on every one
                bot_started_speaking_ms leads bot_turn_completed_ms by 1.2-3.7 s
                -- which is the window the packet goes missing in
injected drop   0/1  fail    "the character answered but the turn never completed"
of OnBotTurn-            FINDING no-bot-turn-completed-after-character-spoke
Completed                evidence: '...the character spoke; no character transcript
                         arrived either, which is F23 and independent. Within 60 s
                         of speaking no OnBotTurnCompleted arrived...'
reverted        2/2  pass    injection gone from the tree, rebuilt, still green
```

The injection was one line in the working tree (`Turns` taken as empty), never committed: it
reproduces exactly I5's condition — the character answers, the completion packet does not
arrive — which the live backend was not producing this afternoon. `audio_roundtrip` ran 12/12
green today against 18/26 in the study, which is the same environment swing I5 records; that is
also why this closes on the report's text rather than on a rate.

The scenario derives `bot_turns` from `OnBotTurnCompleted`
([ConvaiAudioRoundtripScenario.cpp:188](../../Source/ConvaiTests/Private/Scenarios/ConvaiAudioRoundtripScenario.cpp#L188))
and, when it is zero, reports:

> the character did not answer the player's speech

and the finding evidence adds *"Within 60 s of speaking, no character transcript arrived."*

Both statements are false in all five observed failures. The character answered and was
heard; `bot-transcription` arrived; only the completion signal did not. The report sends a
reader after a broken conversation path when the actual defect is one missing packet
(I5) — the same class of misdirection F30 recorded, where correct evidence text pointed a
fix agent at the wrong mechanism.

**Fix shape.** Separate the two conditions the metric currently conflates. If
`bot-started-speaking` fired but `bot-turn-completed` did not, say that — the scenario
already sees both. Reserve "did not answer" for the case where the character genuinely
never spoke.

## I4 — The suite cannot prove which DLL it graded

Status: `done` — 2026-08-20. Two halves, because the version line only exists for a launch that
initialised a client and ten of the twenty-two scenarios never do.

* `UConvaiSubsystem::InitializeConvaiClient` logs the version at `Display` instead of `Log`, so
  it reaches stdout — and `run_*.log` — with no flag.
* `run.py` reads it per launch and merges the distinct set into `convai_client` beside
  `git_sha`, with `launches_without_a_version` counting the rest. Beside it, the sha256, size and
  mtime of `convai_client.dll`, `convai_http_helper.dll` and `AECwebrtc.dll` read from
  `<plugin>\Binaries\Win64` — the absolute path `Convai::StartupModule` loads by — so a sweep
  where nothing connected still says which binary was in place. The console prints
  `ConvaiClient <version> [<sha12> <mtime>]` at the end of every sweep.

Two entries in `versions_logged` means the library changed mid-sweep, which the A/B ledger had to
attest externally until now.

Verified (tier 0, before/after on the same scenario, **no** `-AllowStdOutLogVerbosity` in either):

```
before  ConvaiClient <none logged> [7ec567584f77 2026-08-20T14:05:37]   session_connect_disconnect 1/1
after   ConvaiClient 0.2.12.320+3b122f2 [7ec567584f77 2026-08-20T14:05:37]  session_connect_disconnect 1/1
```

The control was built by stashing only the one-line verbosity change, rebuilding, and running the
same connecting scenario — the hash half was already working, which is what makes the version
line the thing under test. `test_run.py` asserts the merge side: one version deduplicated across
launches, a launch without one counted rather than dropped, and two versions in a sweep both
listed instead of one winning silently.

The plugin logs `ConvaiSubsystemLog: ConvaiClient Version: 0.1.26.1+fdeec72` at
`InitializeConvaiClient` ([ConvaiSubsystem.cpp:1698](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1698)),
at `Log` verbosity. `Log` reaches `Saved\Logs` but not stdout, and `run.py` captures only
stdout. So the per-launch artifact the runner keeps does not contain the version.

`Saved\Logs` is not a fallback: `MaxLogFilesOnDisk=10`, against 22 launches per pass. Nine
launches in ten have their version evidence deleted before the sweep ends.

This matters more here than it looks. Given I1, "which DLL was loaded" is a question the
project has already got wrong once, and the merged report carries a git SHA, an oracle
fingerprint and a tier table but no record of the native library it graded.

**Worked around for this study** by passing `-AllowStdOutLogVerbosity` to every launch,
identically in both arms, which promotes `Log` onto stdout and put the version in all 220
run logs. That is a flag, not a fix.

**Fix shape.** Log the version at `Display`, and put it in the merged report next to
`git_sha` — one field, and every historical sweep becomes attributable. The 10 of 22
scenarios that never connect still would not emit it, but they do log
`LogConvai: Successfully loaded convai_client.dll`, so recording the loaded path and its
hash at startup would close that half.

## I1 — A stale DLL in `Binaries` beats a fresh one in `ThirdParty`, silently

Status: `done` — 2026-08-20. `EnsureThirdPartyLibrariesCopied` now compares instead of only
testing for existence, and logs at `Warning` naming both paths, both sizes and both timestamps.
It does **not** overwrite: staging a binary into `Binaries` by hand is how every DLL A/B in this
repo is run, and refreshing from `ThirdParty` on startup would silently undo the swap — the same
class of invalidated sweep pointed the other way.

**Size, not mtime.** An install writes both directories in whatever order it likes, so "the
staged copy is newer" is the normal state after `pull-convai-libs-to-ue.ps1` and a timestamp rule
would warn on healthy startups — the restore step of this issue's own verification left
`ThirdParty` three hours newer than `Binaries` with byte-identical content. Every ConvaiWebRTC
release differs in size (27,548,160 / 27,553,280 / 27,637,248), so size catches the real case.
The same-size-different-content case is caught by `run.py` instead, which hashes both copies per
sweep — cheap there, and a startup cost for every customer here.

Verified (tier 0, reproducing the 2026-08-07 incident): 0.2.10's `convai_client.dll` staged into
`Source/ThirdParty` with 0.2.12 left in `Binaries`.

```
stale     LogConvai: Warning: convai_client.dll in Binaries differs from the staged copy and is
          the one that will load: ...\Binaries\Win64\convai_client.dll (27637248 bytes,
          2026.08.20-08.35.37) vs ...\Source\ThirdParty\...\convai_client.dll (27548160 bytes,
          2026.08.20-11.40.35). ...
          STALE convai_client.dll: loaded 7ec567584f77, staged b9a99db54337
          ConvaiClient 0.2.12.320+3b122f2 -- the loaded copy, which is the point
restored  no warning, no STALE line, ThirdParty newer than Binaries by three hours
```

`test_run.py` covers the suite half on a temporary tree: identical copies are silent, a changed
staged copy is reported with both hashes, and a missing staged copy is not a disagreement.

`Convai::StartupModule` loads every DLL from `<plugin>\Binaries\Win64` by absolute path
([Convai.cpp:48](../../Source/Convai/Convai.cpp#L48)). `EnsureThirdPartyLibrariesCopied`
([Convai.cpp:182](../../Source/Convai/Convai.cpp#L182)) populates that directory **only
when a file is missing**. It never compares, never refreshes.

So the documented install location and the actual load location can disagree
indefinitely, and nothing says so. `pull-convai-libs-to-ue.ps1` happens to be safe
because it wipes all three locations first — but only a build re-copies them, and UBT
skips that when the target is already up to date. During this session's setup UBT
reported `Target is up to date` and copied nothing; a ThirdParty-only install would have
graded the previous release.

This is not hypothetical. It has already cost a sweep once: on 2026-08-07 a freshly
staged DLL was ignored and the engine produced a fully coherent-looking report against a
six-hour-old binary.

**Fix shape.** Compare before skipping — size and mtime, or a hash — and log at Warning
when Binaries and ThirdParty disagree. A silent stale-DLL load is the single cheapest way
to invalidate every number this suite produces.

**Out of scope.** Changing the load path. Loading from Binaries is correct for
blueprint-only projects; the defect is the copy predicate, not the destination.

## I12 — `TestCharacterID` is empty, so 12 of 22 scenarios cannot run as shipped

Status: `done` — 2026-08-20. `run.py` refuses the sweep after discovery, naming the override's
spelling and the scenarios that need it, instead of launching an editor per scenario to discover
the same missing credential eleven times. The value is still not held here: nothing was
committed, and the message points at the two places it can come from.

**Eleven, not twelve.** `connection_invalid_character` connects on a character it invents and
already calls `HasCredentials(Error, bRequireCharacter=false)`, so it runs fine without one — and
still produces F38 when it does. The requirement is read from the scenario rather than from a
list in the runner: `FConvaiTestScenario::RequiresTestCharacter()` defaults to
`RequiresLiveConnection()`, that one scenario overrides it to `false`, and `convai.tests.List`
reports it as `needs_character=<0|1>`. A List line without the field is read as needing one, so
an older binary makes the runner over-careful rather than under-.

Verified (tier 0):

```
no character, no filter                    run.py: error: 11 of the 22 selected scenarios need a
                                           character and TestCharacterID is empty. Pass
                                               "--arg=-ConvaiTestCharacterID=<character-id>"
                                           ... Needs one: aec_double_talk, ... text_roundtrip
                                           exit=2, zero scenario launches
no character, --filter                     connection_invalid_character: 0/1 passed
  connection_invalid_character             FINDING invalid-character-fails-silently (1/1)
```

`test_run.py` covers the parsing and the resolution order: `needs_character=0` respected, a bare
line treated as needing one, the command-line override winning over the ini the way
`UConvaiUtils::GetTestCharacterID` orders them, and an empty `-ConvaiTestCharacterID=` counting
as absent.

`Config/DefaultEngine.ini` line 116 is `TestCharacterID=`. Twelve scenarios need a
character and abort at setup without one (I3 covers how that is reported).

Not a defect exactly — the value is an environment credential and the framework
deliberately holds none, per the session-2 handoff. But the suite is currently unrunnable
from a clean checkout with no error that names the fix at the sweep level, only inside each
scenario's trace.

For this study the framework's own override was used, identically in both arms:
`-ConvaiTestCharacterID=903b069a-485d-11f1-a04c-42010a7be02e`, a prod character from
`convai-livekit-cpp-p`'s `parallel.txt`, whose `auth_value` matches the API key already in
`DefaultEngine.ini`.

**Fix shape.** Have `run.py` fail fast with the override's spelling when neither the ini
value nor the flag is present, rather than launching 12 editors that each discover it
independently.

## I7 — `UConvaiPlayerComponent::EndPlay()` null-derefs on 9 of 10 blocks

Status: `done` — 2026-08-20. **Not an `EndPlay` bug, and not a null component: a macro.**

```c
#define CONVAI_LOG(Category, Verbosity, Format, ...)   UE_LOG(Category, Verbosity, Format, ##__VA_ARGS__);    FConvaiLogger::Get().Log(FString::Printf(...));
```

Two statements, no `do { } while (0)`. So at
[ConvaiPlayerComponent.cpp:827](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L827),

```cpp
if (IsValid(OutSoundWave))
    CONVAI_LOG(ConvaiPlayerLog, Log, TEXT("...%f..."), OutSoundWave->GetDuration());
```

the `if` takes only the `UE_LOG`; the file-log statement runs unconditionally and evaluates
`GetDuration()` — a virtual call — on the null pointer the guard has just rejected. Reading a
vtable at offset 0 is the reported `EXCEPTION_ACCESS_VIOLATION reading address
0x0000000000000000`. `FinishRecording` returns null whenever nothing was recorded, which is the
normal case at teardown, so the crash fired on every launch that reached it with `IsRecording`
set.

**How it was pinned.** Four rounds of Display-verbosity markers through the teardown path, since
the callstack's line attribution is what made this look like a lifetime bug for two sweeps. The
markers ruled out every hypothesis in order: `AudioCaptureComponent` was live (both
`StopAudioCaptureComponent` log lines fired), `ReadRecordedBuffer` returned `n=0 ch=2.0
sr=48000.0` and `StopVoiceChunkCapture` took its early return, and the last marker before the
crash was the one immediately preceding the `if` — with the pointer printed as
`ptr=0000000000000000 valid=0`. A guard that rejects the pointer and crashes anyway is a macro,
not a lifetime.

**Fix.** Both `CONVAI_LOG` and `CONVAI_LOG_REF` wrapped in `do { } while (0)`; the five call
sites that leaned on the macro's own trailing semicolon now carry one. One brace-less call site
exists today — every future one would have been the same crash.

Verified (tier 2, alternated in one session on one character, plus a full-suite control):

```
adopted_capture_component_routing   crashed 11 of 13 launches before   Wilson 58-96%
                                    crashed  0 of  6 launches after    Wilson  0-39%
                                    Fisher exact p = 0.001032, runs_needed_per_arm = 4
full 22-scenario sweep after         0 crashes in 22 launches, 0 launches without a report
                                     17/22 passed; the 5 reds are exactly F19, F26, F12/F34,
                                     F38 and F23 -- the recorded common-fail set, unchanged
```

The macro is used by every log call in the plugin, which is why the whole suite was run rather
than the one scenario. F19's finding still fires, so the scenario's real failure is untouched.

**Left alone deliberately.** The macro still evaluates its arguments twice, so a call whose
argument has a side effect performs it twice. That is a separate defect from the crash and is
noted at the definition rather than fixed here.

**Not done.** The other half of the filed fix shape — the runner recording a crash signature as
its own field on the run, so a crash appears in the merged report rather than only in the log.
`launches_without_report` stays 0 through a crash that fires after the report is written, which
is exactly how this survived. Worth its own issue.
Verified against Hotfix1: **still open**, 4 of 5 blocks on the hotfix and 4 of 5 on the
control. 17 of 20 blocks across both sweeps. The crash site is plugin code, so no DLL
was ever going to close it.

```
LogWindows: Error: Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000000
LogWindows: Error: [Callstack] UnrealEditor-Convai.dll!UConvaiPlayerComponent::EndPlay()
                              ConvaiPlayerComponent.cpp:496
```

Scenario `adopted_capture_component_routing`. 0.1.26: 4 of 5 blocks. 0.2.10: 5 of 5.
Present on both releases, so not a DLL regression — F20's family, teardown reaching into
state that is already gone (`if (IsRecording) FinishRecording();` /
`if (IsStreaming) MuteStreamingAudio();`).

It costs no data today because it fires after the report is written — `no_report` was 0 in
both arms across 220 launches — which is exactly why it has survived: the suite stays
green-ish through a hard crash. A crash that a report cannot see is a crash that gets
found in a customer's build instead.

**Fix shape.** Guard on the same validity the rest of `EndPlay` checks, and make the
runner record a crash signature as its own field on the run so it appears in the merged
report rather than only in the log.

## I10 — Five scenarios flake at 20–40% with nothing marking them as flaky

Status: `done` — 2026-08-20. Each scenario in `merged.json` now carries `pass_rate_ci95` and a
`flaky` flag, set when its own outcome disagrees with itself inside one sweep, and the console
prints `3/5 passed [FLAKY, 95% CI 23%-88%]`. A `setup-failed` launch is excluded: a scenario that
could not start is not evidence of instability (I3), and a single-launch sweep cannot be flaky
because one launch cannot disagree with itself.

Verified (tier 0): `test_run.py` covers mixed outcomes (flagged, interval brackets the observed
rate), all-pass and all-fail (not flagged), a single failing launch (not flagged), and a
pass-plus-setup-failed pair (not flagged).

```
                                       0.1.26   0.2.10
aec_echo_only_disabled                 P F P P P   P P P P P
audio_roundtrip                        P P F F P   F F P P F
live_player_transcript                 P P P P P   P P F P P
live_player_transcript_aec_internal    P P F P P   P P P P P
live_player_transcript_aec_none        P P P P P   F P P P P
```

All five are live-backend, ASR- or response-dependent. Nothing in the report distinguishes
them from a deterministic result: `pass_rate` reports `4/5` for a flake and `4/5` for a
scenario that genuinely regressed once, and a single-repeat run (the default in practice
for quick checks) turns any of them into a coin flip presented as a verdict.

The PRD's rule that "a single failure in fifty runs is a finding" is honoured for
*findings*, which carry an occurrence rate and a Wilson interval. Scenario outcomes get
neither.

**Fix shape.** Give the scenario table what findings already have — occurrence rate, CI,
and a flag once an outcome is mixed across repeats. Not quarantine: these scenarios are
measuring something real and disabling them would hide I5.

## I9 — 17 of 22 scenarios carry no discriminating power

Status: `done` — 2026-08-20, as *reported*, not as fixed. The suite's sensitivity is unchanged;
what changed is that the report no longer hides it. `diff_against_previous` now runs the same
verdict rule over the scenario table that it already ran over findings — extracted as
`compare_rates()` so one place decides — and the console leads with

```
    0 of 22 scenarios could separate the two sweeps -- the rest would return this
    result against any build
```

followed by the scenarios that did move. Each comparison carries `was`, `now`, the 95% interval,
`p_value` and `runs_needed_per_arm`.

Verified (tier 0, on this study's own data): the five blocks per arm from `.testruns-ab/0_1_26`
and `.testruns-ab/0_2_10` pooled and fed through the new diff.

```
0 of 22 scenarios separated 0.1.26 from 0.2.10
{'not-separable': 22}                      (min_runs=5; at min_runs=10 all 22 read under-powered)
16 of them returned identical rates on both arms -- runs_needed_per_arm is None,
   which is the report saying no sample size would have separated them
```

The six that differed need 35–74 runs per arm to settle, against the 5 and 10 they got.
`test_run.py` asserts the shape on a synthetic pair of sweeps: a scenario that moved reads
`worse`, two that did not read `not-separable`, and the headline counts 1 of 3.

**Not fixed.** Making the suite able to answer "is this DLL better" is a different job — it needs
scenarios that fail for one cause and pass for another, which is I14's territory and the offline
tier's. This entry only stops a green sweep from reading as evidence of equivalence.


Classification across the full sweep:

```
stable pass  (PPPPP / PPPPP)   ████████████   12
common fail  (FFFFF / FFFFF)   █████           5
flaky        (mixed, both)     █████           5
regression                                     0
improvement                                    0
```

Twelve scenarios passed all ten times and five failed all ten times, on both releases.
Those seventeen cannot distinguish anything — they would return the same result against
any DLL that loads. The entire discriminating capacity of a 220-launch, three-and-a-half
hour sweep rests on five scenarios that are themselves flaky.

The arithmetic is stark: separating the observed pass rates (73.6% vs 72.7%) would need
**37,200 runs per arm**. The suite as constituted cannot answer "is this DLL better",
which is a reasonable thing for it not to do — but it currently gives no sign of that, and
a green sweep reads as evidence of equivalence when it is mostly evidence of insensitivity.

This is issue 16's principle applied one level up: the report should say what it did not
cover. A sweep whose comparison is under-powered should say so, the way
`diff_against_previous` already reports `under-powered` with `runs_needed_per_arm`.

**Fix shape.** Carry the same verdict machinery into the scenario table, not only the
findings list — a scenario whose two arms are identical is `not-separable`, and the report
should name how many of its scenarios were capable of separating anything at all.
