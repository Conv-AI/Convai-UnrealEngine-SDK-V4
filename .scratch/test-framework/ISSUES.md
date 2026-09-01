# Open issues from the ConvaiWebRTC A/B study

Found on 2026-08-19 across two 220-launch alternating sweeps of the whole scenario suite against
0.1.26, 0.2.10 and `0.2.10-Hotfix1`, and re-measured on 2026-08-20 against 0.2.12 with the fixes
from that queue applied. Evidence lives in `.testruns-ab/` — 440 run logs, 440 reports, and a
per-launch DLL-version attestation.

**Eleven of the fourteen are closed.** They are in [`CLOSED-ISSUES.md`](CLOSED-ISSUES.md) with
their verification intact, and merged into `WebRTC-Video` as `6a5f31f1`. What is left is three
issues that each need something this repository does not contain.

| | Issue | Attribution | Status | Blocked on |
|---|---|---|---|---|
| I5 | `bot-turn-completed` goes missing, on both releases | environment / backend | `needs-info` | a server-side trace for a session where the packet did not arrive |
| I8 | F25's exit hang fires on 57% of launches, and no DLL build moves it | plugin / DLL | `needs-triage` | a decision: bounded wait in `convai-livekit-cpp-p`, or a teardown reordering here |
| I14 | Nothing exercises transport reconnect | test framework | `ready-for-human` | `ReconnectKind`'s contract, which only `convai-livekit-cpp-p` has |

Findings behind the scenarios that are still red — F19, F26, F12/F34 — are tracked in
[`FINDINGS.md`](FINDINGS.md), which carries its own status index.

---

## Session of 2026-08-20/21 — what moved

Six of the seven findings in `Docs/handoff/2026-08-20-plugin-defect-queue.md` are fixed and
merged to `WebRTC-Video` (11 commits, `a8e89bd0..c680d20c`). Details are in each finding's entry
in `FINDINGS.md`; this is the index.

| | Outcome |
|---|---|
| **F21** | fixed. The exit code could never have worked — `RequestExitWithStatus(Force=false)` ends at `PostQuitMessage` and UE's pump drops `wParam`. Armed in `Complete()`, delivered from `ShutdownModule`. `test_exit_code.py` is the check |
| **F16** | fixed by gating, not deleting — the harness is kept. All 46 files under `Source/Convai/*/Tests` are wrapped in `#if WITH_TESTS`; `ConvaiTests` is a `DeveloperTool` module the release script strips. Proven against a control Shipping build: 5 console commands and 5 reflected types present there, zero in the fixed one |
| **F9** | plugin half fixed — `error-response` is reported as what it is and reaches the game on a new `OnServerErrorEvent`, and `OnError` stopped being `static`. Server half handed over: the string is in none of the eleven repos on this machine |
| **F19** | fixed. The obvious version — assign the submix at adoption — passed the scenario and moved no audio; a `USynthComponent` that is already playing keeps its old routing. Restarting it moves the mic from −9.9 dB to −105.8 dB in the master mix |
| **F25** | **closed as a measurement artefact.** Not a deadlock. See I8 |
| **F12 / F34** | **still open.** Separated from the server, mechanism identified and measured, fix reverted unmerged — it is in vendored `client-sdk-rust` |

Two oracle defects were found and fixed along the way, and both had been producing wrong answers:

- `aec_double_talk` scored the last final transcript from **either** speaker, so during
  double-talk the character's words were often graded against the player's fixture. That coin
  toss is what read as flakiness. Filtered to the player, the arm is **0/5**, not 2/3.
- `run.py` called a launch hung 2 s after its report while teardown legitimately takes up to 5 s,
  which manufactured the 57% hang rate. `--report-settle` is 15 s now.


## Verification — `0.2.10-Hotfix1.293+ee6e1c0-dirty`, 2026-08-19 evening

Five blocks of the hotfix alternated against five blocks of a **contemporaneous 0.2.10
control**, same plugin binary, same character, same flags. The control is the whole
point: run against the morning's numbers alone, three separate results would have been
reported as hotfix fixes, and all three were the backend having a better evening.

| | 0.2.10 control | Hotfix1 | |
|---|---:|---:|---|
| pass rate | 83/110 (75.5%) | 85/110 (77.3%) | `p = 0.8740` |
| hung on exit | 63/110 | 63/110 | `p = 1.0000` |
| `EndPlay` crash | 4 of 5 blocks | 4 of 5 blocks | unchanged |
| cold connect | 3890 ms | 3724 ms | `p = 0.684` |
| disconnect | 6047 ms | 5849 ms | `p = 0.631` |

**What the control caught.** Three apparent fixes that were not:

```
                        morning     evening     evening
                        0.2.10      control     hotfix1
aec_echo_only_none      PPPPP       FFPPP       PPPPP      control flaked
aec_double_talk         FFFFF       FFFFP       FFFPP      control improved too
bot-turn-completed      2/5         4/5         5/5        backend, both arms
```

`aec_double_talk`'s first hotfix pass ended a run of **17 consecutive failures** across
every arm since the study began. It also carried 40% less echo to cancel (171,722
non-silent samples against ~290,000 on the failures), so quieter echo explains it at
least as well as better cancellation — and the control passing it on the very next block
settles the question. The last three attempts in the study, one control and two hotfix,
all passed.

**What replicated.** 0.2.10's disconnect advantage over 0.1.26 held across two
independent sessions — 5711 ms in the morning, 6047 ms in the evening, against 0.1.26's
9795 ms. That was the one real finding of the first sweep and it survives.

**What was not measured.** See I14. The hotfix's only new exports are transport-reconnect
test hooks, and no scenario drives them, so "unfixed" for the transport work should be
read as *not covered*, not as *tried and failed*.


---

## I5 — `bot-turn-completed` goes missing 5 runs in 10, on both releases

Status: `needs-info` — **escalated 2026-08-20, unchanged and now easier to see.** The in-engine
tier still cannot separate "the server never sent it" from "the client never surfaced it", which
is what the entry says it needs, and nothing in this repo can answer that. What did change:

* I6 gives the report the vocabulary for it. A run where the character answers and only the
  completion packet goes missing now says so, with `bot_spoke=1, bot_turns=0` and its own
  finding, instead of "the character did not answer the player's speech".
* Today's rate is 0 in 12 — `audio_roundtrip` passed 12 of 12 across two batches this
  afternoon, against 18 of 26 in the study. Both arms moving together across sessions is what
  the entry already recorded as the environment signature.

Still needs a server-side trace for a session where the packet did not arrive, or the DLL
harness's equivalent. Ask before investing.
Verified against Hotfix1: **still open, and the second sweep strengthens the environment
attribution.** The packet arrived on 9 of 10 evening runs — 5/5 hotfix, 4/5 control —
against 5 of 10 that morning. It moved for *both* arms at once, which is what an
environment cause looks like and what a DLL fix does not.

`audio_roundtrip` speaks a WAV, the server transcribes it, the character answers. In
**every one of the ten runs across both releases** the character answered: `bot-llm-text`,
`bot-transcription`, `bot-tts-started`, `bot-started-speaking` and `bot-stopped-speaking`
all fired. Whether the scenario passed was decided entirely by whether the trailing
`bot-turn-completed` arrived.

```
                 R1     R2     R3     R4     R5
0.1.26        btc=2  btc=2  btc=0  btc=0  btc=2      3/5
0.2.10        btc=0  btc=0  btc=2  btc=2  btc=0      2/5
              (botspeak=2 in all ten)
```

The packet sequences are equivalent up to `bot-stopped-speaking`; only the tail differs.
0.2.10 delivers `bot-turn-completed` normally in `aec_double_talk`, `aec_echo_only_*` and
`text_roundtrip`, so this is not a release dropping a packet type.

**Attribution: environment or backend, not the DLL** — it reproduces at similar rates on
both. Fisher exact across the arms: `p = 1.0`.

It is still worth an entry, because the effect is product-visible: any plugin or game
logic keyed on turn completion is unreliable half the time on the speech-in path, and
nothing in the plugin notices or reports the omission.

**What would answer it.** A server-side trace for a session where the packet did not
arrive, or the DLL harness's own equivalent — the question is whether the server never
sent it or the client never surfaced it, and the in-engine tier cannot separate those.

## I8 — F25's exit hang fires on 130 of 220 launches and 0.2.10 does not fix it

Status: `ready-for-human` — **closed as a measurement artefact, 2026-08-20.** The rate was
`run.py` killing launches 2 s after their report while teardown legitimately takes up to
~5 s. A stack from a launch caught in the act has the game thread in
`ConvaiClientImpl::Disconnect`'s **bounded** `wait_for`, not in `unpublishTrack`'s
`fut.get()`. Full sweep, same build, same hour: **13/24 hung at a 2 s threshold, 0/24 at
15 s, `p = 2.6e-05`**, and the 2 s arm matches the historic 331/572 at `p = 0.83`. 40
launches measured on 0.2.12, none failed to exit by itself. `REPORT_SETTLE_S` is now 15 s.
**Do not build the plugin-side workaround below** — see F25 for why. Everything from here
down is the superseded reading, kept because its arithmetic is still the right arithmetic.

Re-measured on 0.2.12 with every fix in this stack applied, two full sweeps: **25 of 44 launches
hung, 56.8% (Wilson 42-70%)**, against the study's pooled 331/572 = 57.9%, `p = 1.0000`. Nothing
here moved it and nothing here was expected to.

The wait is `LocalParticipant::unpublishTrack`'s `fut.get()` with no timeout, in vendored LiveKit
code in `convai-livekit-cpp-p` (F25, located 2026-08-07). What is worth adding from this side:
**both of the plugin's `Disconnect()` calls are synchronous on the calling thread and hold
`ConvaiClientMutex`** — [ConvaiSubsystem.cpp:1434](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1434)
and [:1759](../../Source/Convai/Private/ConvaiSubsystem.cpp#L1759) — so at teardown the game
thread is what blocks inside that `fut.get()`.

So there is a plugin-side lever: run the disconnect on its own thread with a deadline. It is not
obviously right, and it is not free:

* it reorders teardown so `room->disconnect()` can begin with an `unpublishTrack` in flight,
  which is the concurrency shape F25 names as having previously produced the `remove_track`
  access violation and the `StopRemoteAudioReader` use-after-free;
* the mutex would then be held by a thread the game thread has stopped waiting for;
* a process-exit hang is not obviously cured by moving the block off the game thread, because
  the process still has to reach exit with that thread outstanding;
* and separating 55% from 10% needs **16 runs per arm** by `run.py`'s own arithmetic — roughly
  20 minutes of wall clock per side, alternated, which is affordable but only worth spending on
  a change someone wants.

A bounded wait in the DLL is the correct fix and is owned there. Ask before spending the session
on the plugin-side workaround.
Verified against Hotfix1: **still open**, and now measured three ways. 63/110 on the
hotfix against 63/110 on the control — the same number, `p = 1.0000`. Across both sweeps
the rate sits at 58–60% on every one of the four arms tested. Nothing in 0.1.26, 0.2.10
or Hotfix1 moves it.

`launches_hung_on_exit`, pooled over this sweep:

| | hung | of | rate |
|---|---:|---:|---:|
| 0.1.26 | 66 | 110 | 60.0% |
| 0.2.10 | 64 | 110 | 58.2% |

Fisher exact `p = 0.8910`. **The upgrade does not move it.** Anyone hoping the newer DLL
resolves the PIE-stop / teardown deadlock should not upgrade for that reason.

Worth recording alongside it: 0.2.10's *measured* disconnect latency is materially
tighter — median 5.7 s against 9.8 s, and 8 of its 10 samples inside a 400 ms band where
0.1.26 spans 5.6–23.8 s. So the teardown path did change, and changed for the better,
without changing how often it hangs outright. Those are two different failure modes in the
same code and the sweep separates them.

## I14 — Nothing exercises transport reconnect, which is what Hotfix1 appears to change

Status: `ready-for-human` — **escalated 2026-08-20, unchanged.** `ReconnectKind`'s values and the
contract each implies are still not in this repo, and the deployed 0.2.12 header in the tree
still exports none of the three hooks to the plugin, so the assertions cannot be written from
anything here. Someone with `convai-livekit-cpp-p` needs to state the intended behaviour first.
Flagged, not guessed.

`0.2.10-Hotfix1` exports exactly three symbols that 0.2.10 does not, and all three are
test hooks:

```
convai::testing::BeginTransportObservation()
convai::testing::ObservedConnectionState()          -> const char*
convai::testing::SimulateTransportReconnect(ReconnectKind) -> bool
```

That names the target. Nothing in `ConvaiTests` drives them, and the plugin does not
import them either. `session_reconnect_warm` — the closest scenario — reconnects through
the plugin's own `StartSession` / `StopSession` API on a component it keeps alive; it
never drops the transport underneath a live session, which is the case
`SimulateTransportReconnect` exists to produce.

So the sweep's verdict on the hotfix is **0 of 13 issues fixed, and the thing it actually
changes was not measured.** Those are different statements and the report should not blur
them. A suite that returns "no change" for a build carrying a targeted fix is reporting
its own coverage, not the build's quality — the same failure mode as F33, where a defect
that broke three offline tests left the in-engine number inside its healthy range.

**Scope.** A scenario that connects, calls `BeginTransportObservation`, drops the
transport via `SimulateTransportReconnect` for each `ReconnectKind`, and asserts on what
the plugin does next: whether the session recovers, whether `OnFailureEvent` fires when it
does not, whether audio resumes, and what `ObservedConnectionState` reports throughout.

**Why it is `ready-for-human` and not `ready-for-agent`.** The hooks are undocumented
here — `ReconnectKind`'s values and the contract each implies are not in the public
header, so the assertions cannot be written from what is in this repo. Someone with
`convai-livekit-cpp-p` needs to state the intended behaviour first. Until then this is a
known blind spot, not a task.

**Out of scope.** Testing the hooks themselves. They are DLL-side test scaffolding and
`aec_erle_test`'s tier owns them; this issue is about the in-engine consequence.

---

## The scenarios that are red for product reasons

These failed on **every** DLL tested, so they are not regressions of any release. The first three
columns are runs failed out of runs attempted per arm in the 2026-08-19 sweeps; the last is
2026-08-20 on 0.2.12, after the fixes in `CLOSED-ISSUES.md`.

| Scenario | Finding | 0.1.26 | 0.2.10 | Hotfix1 | 0.2.12, today |
|---|---|---|---|---|---|
| `adopted_capture_component_routing` | F19 — a capture component supplied through `IConvaiAudioCaptureInterface` is adopted but never assigned a `SoundSubmix`, so a customer's microphone renders into the master mix and into Reference Audio | 5/5 | 10/10 | 5/5 | **2/2 failed, still open** |
| `aec_echo_only_internal` | F26 — with AEC enabled, the character's own speech returns as the player's transcript | 5/5 | 10/10 | 5/5 | **2/2 failed, still open** |
| `aec_double_talk` | F12, F34 — with the character speaking, the player's speech does not come back | 5/5 | 9/10 | 3/5 | 1 of 2 failed, **flaky red** |
| `connection_invalid_character` | F38 — a session on an impossible character fails without raising `OnFailureEvent` | 5/5 | 10/10 | 5/5 | **fixed, 4/4 green** |
| `text_roundtrip` | F23 — a text prompt produces no character transcript on the transcript delegate | 5/5 | 10/10 | 5/5 | **fixed, 5/5 green** |

`aec_double_talk` stopped being deterministic late in the second sweep: it failed 17 times
running and then passed its last three attempts — one control, two hotfix — and both arms turned
together, so that was the evening's quieter echo rather than either binary. It passed again today
on one of two launches. Treat it as flaky, not fixed; F12 and F34 are open.

The three that remain red are the plugin's own defects, and none of them is in this file's queue.
They are tracked in [`FINDINGS.md`](FINDINGS.md).

## Not an issue, recorded as a negative result

**All three builds are functionally indistinguishable on this suite.** Every scenario
comparison in both sweeps returned `p ≥ 0.44`, most of them `p = 1.0000`:

```
0.1.26   81/110  73.6%   ┐ sweep 1, alternated, morning
0.2.10   80/110  72.7%   ┘ p = 1.0000

0.2.10   83/110  75.5%   ┐ sweep 2, alternated, evening
Hotfix1  85/110  77.3%   ┘ p = 0.8740
```

The one signal that survived both sweeps is 0.2.10's connect and disconnect latency
against 0.1.26 — roughly half the median with a far tighter spread, `p = 0.0524` on
disconnect in sweep 1 and reproduced in sweep 2's control (6047 ms against 0.1.26's
9795 ms). Hotfix1 does not move it further. Full method in `.testruns-ab/`.

Separating 0.1.26's and 0.2.10's pass rates would need **37,200 runs per arm**, which is
the number that says this suite was never going to answer "which DLL is better" — see I9.

All three builds are ABI-compatible with the checked-in header: `convai_client.h` is
byte-identical in both release zips and in the tree (`md5 9cb79ea5…`; Hotfix1's copy
differs only in line endings), and exports grow strictly — 40 → 676 → 679, none removed.
Swapping needs no rebuild, which is what made two 10-block alternating designs affordable.
`AECwebrtc.dll` is identical across all three and was never a variable; Hotfix1 also ships
a `convai_http_helper.dll` distinct from both releases'.
