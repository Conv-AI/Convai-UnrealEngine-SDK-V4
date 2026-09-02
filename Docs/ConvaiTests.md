# Running the ConvaiTests suite

`ConvaiTests` is a separate plugin module that drives the shipping `Convai` module through the
same extension points a customer would use — see [ADR 0005](adr/0005-tests-as-a-strippable-module.md).
Scenarios run inside a real Unreal process against the live backend; there is no mock server.

Everything below goes through `Source/ConvaiTests/run.py`, which drives three tiers — the
`Convai` module's own automation tests, the scenarios here, and the DLL's offline suite — and
merges them into one report with one exit code.

## Before the first run

**Build with no editor running.** Live Coding holds `UnrealEditor-ConvaiTests.dll` open and the
link fails with `LNK1104`.

```
& "E:\Software\UE_5.8\Engine\Build\BatchFiles\Build.bat" Dev_WebRTCEditor Win64 Development `
  -Project="E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" -WaitMutex
```

**Supply a character.** Eighteen of the thirty scenarios need one. Either set
`TestCharacterID` under `[/Script/Convai.ConvaiSettings]` in the project's `DefaultEngine.ini`,
or pass `-ConvaiTestCharacterID=` on the command line. Without it the runner refuses the sweep
after discovery, naming the scenarios that need one — a filter that selects only the other
twelve still runs. The framework itself holds no credentials.

## Running

```
python Source\ConvaiTests\run.py `
  --project "E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" `
  --editor  "E:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  --tier all --repeat 10 `
  "--arg=-ConvaiTestCharacterID=<character-id>"
```

Use PowerShell, not Git Bash — MSYS rewrites map paths like `/Game/Maps/Landing` into Windows
paths and the launch fails.

| Flag | Default | Notes |
|---|---|---|
| `--project` | `$CONVAI_UE_PROJECT` | required for the unit and engine tiers |
| `--editor` | `$CONVAI_UE_EDITOR` | the built-in default points at `C:\Program Files\Epic Games\`, which does not exist on this machine — always pass it |
| `--tier` | `all` | `unit` + `engine` + `dll`. `engine` runs the scenarios alone — use it unless you have `convai-livekit-cpp-p` built; `unit` is the module's automation tests in one launch; `both` is engine + dll, kept for existing callers |
| `--repeat` | `10` | the PRD default; findings carry an occurrence rate, so one run is not a result |
| `--filter` | — | substring match on scenario name. Under `--tier unit` alone it narrows the automation spec instead (`Connection` → `Automation RunTests Convai.Connection`); under `all`, `engine` or `both` the unit tier ignores it and runs the full `Convai.` set |
| `--map` | `/Engine/Maps/Entry` | a bare map, so scenarios spawn their own actors rather than inheriting the project's |
| `--out` | `.testruns` | resolved to an absolute path; the editor's cwd is the engine folder |
| `--timeout` | `600` | seconds per launch |
| `--arg` | — | extra editor argument, repeatable |

**Both `--arg -Foo` and `--arg=-Foo` work.** argparse reads a dashed value as the next option, so
`run.py` splices the separated form into the `=` form before parsing. In PowerShell the `=` form
still needs the quotes: `"--arg=-Foo"`.

Useful pass-throughs:

- `"--arg=-ConvaiTestCharacterID=<id>"` — the character to talk to.
- `"--arg=-AllowStdOutLogVerbosity"` — promotes `Log`-verbosity lines into each launch's captured
  stdout. Needed to see `ConvaiClient Version:` per launch; `Saved\Logs` keeps only ten backups
  against thirty launches, so it is the only per-launch record that survives a sweep.

## Reading the output

One directory per sweep, `.testruns/<timestamp>_<git-sha>/`:

| File | What it is |
|---|---|
| `merged.json` | the report — pass rates, findings, metrics, tier status |
| `report_NNN_<scenario>.json` | one scenario's result, status, metrics, findings |
| `run_NNN_<scenario>.log` | that launch's full stdout |
| `discover.log` | the scenario-listing launch |
| `unit/index.json` | the automation controller's report for the unit tier, what `tiers.unit` is read from |
| `unit.log` | the unit tier's launch, full stdout |

`merged.json` is copied into `.testruns/history/`, and each sweep diffs the previous one — both
finding rates and scenario pass rates — reporting `improved`, `worse`, `not-separable` or
`under-powered` with the runs per arm that would settle it. Per-scenario JSONL traces land
separately, in `<project>/Saved/ConvaiTests/<timestamp>/`.

The runner exits non-zero if anything failed, any launch produced no report, a scenario could not
start, or a requested tier did not run.

Read `pass_rate` as `passes/launches`, not as a verdict. Each scenario carries `pass_rate_ci95`
and a `flaky` flag, set once its own outcome disagrees with itself inside one sweep — `3/5 [FLAKY]`
and `3/5` mean different things. The diff's headline says how many scenarios could separate the
two sweeps at all:

```
  vs 0.1.26:
    0 of 22 scenarios could separate the two sweeps -- the rest would return this result
    against any build
```

which is the honest reading of a green sweep: 16 of the study's 22 scenarios returned *identical*
rates on both DLLs, so no sample size would have separated them (`ISSUES.md` I9).

## Unit tier

`--tier unit` runs the `Convai` module's own UE Automation tests — 111
`IMPLEMENT_SIMPLE_AUTOMATION_TEST` under `Source/Convai/Private/Tests`, every name under
`Convai.`, all `#if WITH_TESTS` — in one editor launch: editor context, no map, no `-game`,
`-NullRHI`. `--repeat` does not apply; they are deterministic and run once. The launch is

```
UnrealEditor-Cmd.exe <project>.uproject -nopause -NullRHI -ReportExportPath="<out>/unit" `
  -TestExit="Automation Test Queue Empty" -unattended -nosplash -stdout -NoLogTimes `
  -ExecCmds="Automation RunTests Convai.; Quit"
```

The controller writes `<out>/unit/index.json`, which is what `tiers.unit` in `merged.json` is
read from — `{status, total, passed, failed, not_run, failed_tests[{name, errors}], duration_s,
source, exec_cmds, report}`. Only when `index.json` is missing does the runner fall back to the
`Test Completed. Result={..} Name={..} Path={..}` lines in `unit.log`, and a launch that died
before the controller's `**** TEST COMPLETE. EXIT CODE: N ****` line is reported as not-run —
"editor exited (code N) before the automation run completed" — rather than as a short green list. "index.json lists no tests" is
the other not-run: the module was built without `WITH_TESTS`, or the spec matched nothing. The
console line is

```
  tier unit: P/T passed (Ns, from index.json)
```

with a `FAIL <name>: <first error>` line per failure. The history diff adds
`tiers.unit.diff.newly_failing` / `newly_passing` — a plain set difference of failing names
against the last sweep with the same `exec_cmds`, printed as `NEWLY FAILING <name>`. The exit
code is non-zero when any unit test failed or the tier was requested and did not run.

`--tier unit --filter Connection` narrows the spec to `Automation RunTests Convai.Connection`; a filter that
already starts with `Convai.` is used as-is. The same run by hand:

```
UnrealEditor-Cmd.exe <project>.uproject -ExecCmds="Automation RunTests Convai.; Quit" -unattended -nopause -nosplash -NullRHI -ReportExportPath=<dir>
```

and the per-test lines also land in the editor's own `<project>/Saved/Logs`.

The three tiers under the one runner:

```
run.py --tier all
  ├─ dll     aec_erle_test.exe             convai-livekit-cpp-p          offline, seconds
  ├─ unit    Automation RunTests Convai.   Source/Convai/Private/Tests   offline, one launch
  └─ engine  convai.tests.Run <scenario>   Source/ConvaiTests            live backend, one launch per scenario per repeat
        → one merged.json, one exit code
```

## Debug utilities, not tests

Three Blueprint-facing developer tools live beside the unit tests under
`Source/Convai/{Public,Private}/Tests`, `#if WITH_TESTS` and stripped from the public snapshot
with the folder; nothing runs them automatically.

- **Convai Record Replay** (`UConvaiReplayComponent`) — records a chatbot's incoming audio and
  FaceSync lipsync during a live session to `Saved/ConvaiRecordings/<folder>` and replays it
  through `IConvaiConnectionInterface`, all at once or in real-time chunks: one bot turn
  reproduced offline, the same bytes every time.
- `UConvaiAudioChunkTestProxy` (`Convai|Debug|Tests` async nodes) — drives a `USoundWave` into a
  `UConvaiAudioStreamer` in timed chunks, with presets that exercise the gear buffering.
- `UConvaiTestDebugLibrary::FreezeThreads` — blocks the game and/or audio thread to stress
  playback-time estimation.

## Scenarios

`convai.tests.List` enumerates them; `run.py` calls it for you.

| Group | Scenarios |
|---|---|
| session | `session_connect_disconnect`, `session_reconnect_warm`, `connection_invalid_character`, `component_lifecycle` |
| server reporting | `server_error_reaches_game` |
| conversation | `text_roundtrip`, `audio_roundtrip`, `action_dispatch`, `bot_emotion`, `live_player_transcript`, `live_player_transcript_aec_internal`, `live_player_transcript_aec_none` |
| AEC | `aec_echo_only_none`, `aec_echo_only_internal`, `aec_echo_only_disabled`, `aec_double_talk`, `injected_echo_reaches_the_mic`, `injected_echo_silent_control` |
| capture / routing | `virtual_mic_adoption`, `speak_wav_through_virtual_mic`, `adopted_capture_component_routing` |
| reference feed | `reference_feed_capture`, `reference_feed_capture_under_load`, `mic_in_reference_audio_control`, `mic_not_in_reference_audio`, `adopted_mic_not_in_reference_audio` |
| knowledge bank | `knowledge_bank_lifecycle` |
| configured world | `configured_text_roundtrip`, `configured_audio_roundtrip`, `configured_lipsync` |

Nineteen of the thirty connect to the backend and cost 30–90 s each; a full pass takes about
ten minutes.

### Knowledge Bank

`knowledge_bank_lifecycle` is **opt-in** the same way: the Knowledge Bank endpoints need an
Enterprise-plan API key, so without `-ConvaiKnowledgeBank=1` it reports `setup_failed`. With it,
the scenario uploads a document through the public REST proxies, polls the listing until the
server marks it available, connects it to the test character and reads the listing back,
disconnects and reads back, deletes and reads back; it passes when every read-back agrees with
the command before it (`upload_ms`, `available_ms`, `connect_readback_ms`,
`disconnect_readback_ms`, `delete_ms`, `delete_readback_ms`, `list_attempts`). Last passed live
2026-08-28, in 15.5 s.

### Configured world

The bare fixture proves the plugin on a plain actor with native components. The three
`configured_*` scenarios prove it on what a game ships: a character Blueprint spawned from
`-ConvaiTestActorClass=/Game/....BP_X_C`, with its own chatbot component adopted and the player
and Virtual Mic left on the bare actor. Without the flag they report `setup_failed`. In this
project the class is `/Game/Blueprints/BP_Hana.BP_Hana_C` -- chatbot, face sync, the MetaHuman
face mesh and the plugin's face AnimBP -- and the map stays `/Engine/Maps/Entry`; a level with
placed characters would auto-start them and take the session.

`configured_text_roundtrip` and `configured_audio_roundtrip` are the existing round trips on
that Blueprint. `configured_lipsync` sends a ten-word prompt and reads three things a game could
bind: `OnFacialDataReady` counts the frames the face-sync component applied
(`face_frames_applied`); the face mesh's `CTRL_expressions_jawOpen` anim curve, sampled while
`GetIsTalking()`, proves the Anim Blueprint's face-sync node moved the mesh (`mesh_jaw_max`); and
the same curve after speech proves it settled (`mesh_neutral_after_ms`). The longest stretch of
audible speech with no playable frame is reported as `face_starvation_max_ms` and not asserted --
F10 is open and its rate is unknown. Packet receipt is not observable through any public event;
the first applied frame is the earliest evidence a game gets either.

```
python Source\ConvaiTests\run.py `
  --project "E:\UEProjects\UE5.8\Dev_WebRTC\Dev_WebRTC.uproject" `
  --editor  "E:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  --tier engine --repeat 5 --filter configured `
  "--arg=-ConvaiTestActorClass=/Game/Blueprints/BP_Hana.BP_Hana_C" `
  "--arg=-ConvaiTestCharacterID=<id>"
```

## Running one scenario by hand

```
UnrealEditor-Cmd.exe <project>.uproject /Engine/Maps/Entry -game -unattended -nosplash -stdout `
  -ExecCmds="convai.tests.Run -scenario=text_roundtrip -report=C:\tmp\r.json quit"
```

Never add `-nosound`: the Reference Audio path needs a live audio mixer, and without one the
suite measures nothing while still reporting green.

The process exits 0 when every selected scenario passed and 1 otherwise, including when the
filter selected nothing. `run.py` does not grade it -- the report is the result -- so the exit
code is for humans and CI. `test_exit_code.py` is the check that keeps it honest:

```
python Source\ConvaiTests\test_exit_code.py --project <project>.uproject --editor <UnrealEditor-Cmd.exe> `
  [--failing-scenario <a scenario that currently fails without a backend>]
```

Three launches, no backend and no character. A launch that hangs in teardown is killed before it
can deliver a code (F25), so this is not a way to grade a sweep.

## Testing a different ConvaiWebRTC DLL

The plugin loads its DLLs from `<plugin>\Binaries\Win64` by absolute path, and
`EnsureThirdPartyLibrariesCopied` only fills that directory when a file is **missing** — it never
refreshes a stale one. Copying a new DLL into `Source\ThirdParty` alone is silently ignored, and
UBT re-stages it only when it actually rebuilds.

So when swapping releases: wipe `convai_client.dll`, `convai_http_helper.dll` and `AECwebrtc.dll`
from all three locations — `Source\ThirdParty\ConvaiWebRTC\lib\release\win64`,
`<plugin>\Binaries\Win64`, `<project>\Binaries\Win64` — then copy all three into each, and
confirm the hashes match. `convai_client.dll` imports the other two, so a partial copy makes it
fail to load with `GetLastError=126`, which reads like a broken release rather than a broken
install. `pull-convai-libs-to-ue.ps1` at the project root does the wipe-and-copy from a GitHub
release or a local build.

Then **confirm what actually loaded** before believing any number:

```
ConvaiSubsystemLog: ConvaiClient Version: 0.2.12.320+3b122f2
```

logged at `Display`, so it is in every connecting launch's `run_*.log` without any flag. The
merged report carries it beside `git_sha`, with the sha256 of the binaries in the directory the
plugin actually loads from:

```json
"convai_client": {
  "loaded_from": "...\Plugins\Convai-UnrealEngine-SDK-Dev\Binaries\Win64",
  "binaries": {"convai_client.dll": {"sha256": "7ec5675…", "bytes": 27637248, "mtime": "…"}, …},
  "versions_logged": ["0.2.12.320+3b122f2"],
  "launches_without_a_version": 0
}
```

and the console prints `ConvaiClient 0.2.12.320+3b122f2 [7ec567584f77 …]` at the end of a sweep.
Two entries in `versions_logged` means the DLL changed mid-sweep. The eleven scenarios that never
connect log no version, which is what `launches_without_a_version` counts; the hashes cover
them.

## Known quirks

Open issues are in [`ISSUES.md`](../.scratch/test-framework/ISSUES.md), the ones closed on
2026-08-20 in [`CLOSED-ISSUES.md`](../.scratch/test-framework/CLOSED-ISSUES.md), and the product
defects behind the red scenarios in [`FINDINGS.md`](../.scratch/test-framework/FINDINGS.md),
which carries a status index. Worth knowing before trusting a number:

- launches are no longer killed 2 s after their report: teardown legitimately takes up to ~5 s,
  and the 57% "hang on exit" rate was that threshold rather than a deadlock. `--report-settle`
  is 15 s now, and a full sweep goes from 13/24 to 0/24 (F25, I8). A launch that really does not
  come back is caught by `--timeout` and counted as `launches_without_report`
- nothing exercises transport reconnect (I14)
- `bot-turn-completed` goes missing on the speech-in path at a rate that moves with the backend,
  not with the build; `audio_roundtrip` names it as its own finding when it happens (I5)
- five scenarios are genuinely flaky; the report marks them `[FLAKY]` with an interval, and a
  single-repeat run of one is a coin toss presented as a verdict (I10)
- three scenarios are red for product reasons and are expected to stay red until F19, F26 and
  F12/F34 are fixed

Reports written before 2026-08-20 name their latency metrics `*_seconds` while holding
milliseconds, except `live_player_transcript`'s `connect_seconds`, which held real seconds (I2).
