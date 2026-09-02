# Unreal Plugin Test Framework

Status: `ready-for-agent`
Repos: `Convai-UnrealEngine-SDK-Dev` (plugin), `convai-livekit-cpp-p` (`convai_client.dll`)
Branch: `docs/test-framework`
Related: [ADR-0004](../../Docs/adr/0004-test-layer-partition.md), [ADR-0005](../../Docs/adr/0005-tests-as-a-strippable-module.md), [FINDINGS.md](./FINDINGS.md)

## Goal

An automated test framework for the Unreal plugin that mimics human interaction — speaks
pre-recorded audio into the real microphone path, sends text, drives several characters at
once, exercises actions and objects — and emits a report an agent can act on without a human
in the loop.

Echo cancellation is the first target because it is the feature failing most often and the
one no current test can see.

## Non-goals

- **AEC algorithm quality.** Measured in `convai-livekit-cpp-p` against
  `IAudioEchoCanceller` directly, where the cancelled signal is readable. See ADR-0004.
- **The deep protocol matrix** — error-code tables, reconnect permutations, chaos. Those live
  in that repo's existing `tests/harness` and are not duplicated here.
- **Shipping test code to customers.** The framework is a separate module, stripped at
  release. See ADR-0005.
- **A CI gate.** Runs are triggered manually today and feed a fix agent. Nothing is
  "gating"; findings carry attribution and confidence instead of pass/fail.
- **Cost control.** Live backend use is unconstrained for now.

## Consumer

The report is read by an **agent**, not a human. The loop is: run → report → agent fixes →
run again. Every design choice below follows from that.

Three consequences:

1. **Fixes must be falsifiable.** A bug found live cannot be re-run identically, so every
   live failure mints a replay fixture from the packet and audio window that produced it.
   Live finds bugs; replay proves them dead.
2. **Findings are statistical.** Each scenario runs N times (default 10) and a finding
   carries its occurrence rate. `12/50` and `50/50` are different objects, and a fix that
   moves `12/50` to `0/50` is evidence where a single green re-run is not.
3. **The oracle is not the agent's to edit.** The cheapest path to green is weakening an
   assertion. Assertions, invariants and recorded fixtures are out of scope for the fix
   agent; changing them is a separate pass with a human in the loop.

## Architecture

### Test layers

| Tier | Where | Network | Owns |
|------|-------|---------|------|
| T0 | DLL repo, GTest | no | AEC quality: ERLE, near-end preservation, delay and gain sweeps, double-talk |
| T1 | Unreal, offline | no | Reference feed correctness: rate, channels, continuity, gaps, alignment. Payload handling |
| T2 | DLL repo, `convai_harness` | yes | Transport, protocol, multi-client demux, chaos. **Already exists** |
| T3 | Unreal, live | yes | Virtual Mic → Talk Targets, Lease/Orphan reuse, actions, objects, lip sync, teardown |
| T4 | Acoustic rig | yes | Real device clock drift, convergence. Calibration and pre-release only |

### How the framework attaches to the plugin

A separate `ConvaiTests` module, listed in `ConvAI.uplugin` and removed by the release
script. It reaches the plugin only through extension points the plugin already offers third
parties — no `#if`, no test-only branches, no new public API in the shipping module. The
`Convai` binary under test is byte-identical to the one that ships.

- **Virtual Mic**: `UConvaiVirtualMicComponent : UConvaiAudioCaptureComponent`, overriding
  `USynthComponent::OnGenerateAudio` to emit scripted WAV plus **Injected Echo**. Discovered
  by the existing `FindFirstAudioCaptureComponent` → `GetComponentsByInterface` path
  ([ConvaiPlayerComponent.cpp:467-495](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L467-L495)),
  and drained by the existing `ReadRecordedBuffer` submix read
  ([:530-546](../../Source/Convai/Private/ConvaiPlayerComponent.cpp#L530-L546)).
- **Synthetic server traffic**: the already-public
  `UConvaiConnectionSessionProxy::HandleDataPacketReceived`.

Where an invariant needs an observation point the plugin does not expose, that hook is a
design change justified on its own merits — not a test back door.

### Audio injection seam

Injected at the **Virtual Mic**, not at `Proxy->SendAudio()`. The existing tests use the
latter, which bypasses resampling, mute, streaming state and — critically — the **Talk
Target** fan-out, so it cannot test AEC or routing under more than one **Connection**.

Because there is no real microphone in a default run, the fixture synthesises the echo:
an independent `ISubmixBufferListener` on the master submix, delayed and attenuated, mixed
into the Virtual Mic feed. Deliberately *not* the same tap the reference thread uses — two
independent taps mean a broken reference path leaves the echo still flowing, so the test
fails loudly instead of silently. The fixture self-asserts that injected echo was non-silent,
so "no audio device" cannot read as a pass.

### Scenario authoring

Steps are a **C++ library**. A JSON scenario file is one front-end to that library, not the
framework itself. C++ scenarios call `SayText()`, `SpeakWav()`, `InjectEcho()`,
`ExpectEvent()` directly and wrap them in whatever control flow they need. Both register in
one registry, share one event recorder, and emit identical artifacts.

The JSON front-end is built **last**, extracted from what actually recurred across ten
written scenarios. Designing the vocabulary first means inventing a language for work not yet
done. Accepted cost: low-friction authoring arrives late.

Rule: the moment a scenario wants a loop or a conditional, it is a C++ scenario.

### Assertions

| Class | Example | Deterministic |
|-------|---------|---------------|
| 1. Event shape | ordering, counts, latency budget, no duplicate transcript | yes |
| 2. Routing / identity | which component, which **Talk Target**, distinct session ids | yes |
| 3. Response semantics | "did it answer the question" | no |
| 4. Action selection | "pick up the cup" → `PickUp(Cup)` | LLM-dependent |
| 5. ASR of Virtual Mic audio | player transcript ≈ known WAV text | ASR-dependent |

Every feature splits in two:

- **"the plugin handles the payload"** — synthesise the server payload, feed it through
  `HandleDataPacketReceived`, assert parse → object resolution → Blueprint dispatch.
  Deterministic, offline. This is where plugin bugs live.
- **"the server produces the payload"** — live, tolerance-based (name-set membership, WER
  threshold, N-of-M), reported as `model-dependent`.

**Negative-control rule.** Any assertion of the form "X did not happen" requires a control
in the same scenario proving X *can* happen. Otherwise a silently broken fixture reads as a
pass forever. The AEC on/off pairing is the first instance of this.

### Always-on invariant monitors

Scenarios only assert what their author imagined. Invariants run across every scenario
regardless of what it tests, and can produce findings on *passing* scenarios:

- reference-audio client count matches live **Connection** count
- reference feed delivers chunks at the expected rate, without gaps or size variance
- no duplicate player transcript across **Talk Targets** (only **Primary Target** delivers)
- no callback delivered after teardown
- attach/detach of reference clients balances to zero at end of run
- no delegate fired on an unexpected thread
- no unexplained `OnFailure`, no unknown server packet type
- latency percentiles within declared budgets

### Report

Machine-readable, stable schema, persisted per run and keyed by git SHA so cycles can diff.

- **Attribution is mechanically derived, never inferred.** "`RemoveClient` never called for
  client 0x…, attach 3, detach 2" qualifies. "Probably a lifetime bug" does not.
- **Hypotheses ship in a separate `hypotheses` array**, never mixed into `findings`, and the
  agent is told they are unverified leads.
- **Evidence chain per finding**: the invariant that tripped, the event window, the replay
  fixture path, the repro command.
- **Deduplicated by root cause.** One cause failing 40 scenarios is one finding with 40
  occurrences, not 40 bugs.
- **Occurrence rate and prior-run comparison** on every finding.
- Schema aligns with `convai_harness`'s `--results-json` so one runner merges both layers.

### Execution

- **Default host**: a minimal test project committed with the plugin — bare map, no content.
  Scenarios spawn actors and components at runtime, so no level content is needed.
- **Secondary host**: the `Dev_WebRTC` dev project, run periodically, to catch integration
  bugs a minimal project hides.
- **Acoustic rig**: laptop speakers + microphone. Used to *calibrate* the synthetic
  **Injected Echo** against a real acoustic path — run the same scenario both ways and
  compare ERLE and leak behaviour — and for pre-release runs. Never the default: its result
  depends on room noise, mic gain and output volume, and it serialises the machine.
- **Driver**: Python orchestrates — launches, matrixes, retries, merges reports. It does not
  assert. Ordering facts on UE delegates are frame-accurate in-process observations, and
  round-tripping them through a socket would measure the IPC.
- Credentials via env vars or a gitignored config file, matching the DLL harness's
  CLI > env > file precedence.
- Runs must **not** pass `-nosound`: the whole **Reference Audio** path needs a live
  `Audio::FMixerDevice`.

## Sequencing

Ordered so the first real AEC finding arrives before the framework is finished.

| # | Issue | Delivers |
|---|-------|----------|
| 01 | Audio-device spike | Proves the master submix renders on the target machine. Gates everything |
| 02 | Module, project, runner, report, N-repeat, history | The loop exists. First scenario is AEC, not text |
| 03 | Reference-feed monitor | Kills or confirms every suspect in FINDINGS.md — no backend, no mic, no AEC |
| 04 | Virtual Mic | Scripted audio through the real capture path |
| 05 | Injected Echo + paired AEC on/off | End-to-end AEC behaviour with a negative control |
| 06 | ERLE test in DLL repo | Bisects algorithm vs plumbing |
| 07 | Record → replay fixtures | The agent can prove fixes |
| 08 | Invariant monitors | The general bug engine |
| 09 | Multi-character scenarios | Talk Targets, Primary Target, Lease/Orphan reuse |
| 10 | Actions, Objects, Tracked Properties | Deterministic payload tier, then live semantic tier |
| 11 | JSON scenario front-end | Extracted from what recurred |
| 12 | Release stripping | `.uplugin` edit, push script, `WITH_CONVAI_TESTS` |

Added 2026-08-06, after the framework produced F26 and could not explain it. These three are
diagnosis and routing rather than framework construction, and they are what stands between "the
suite detects the bug" and "an agent can fix it and prove it".

| # | Issue | Delivers |
|---|-------|----------|
| 13 | Reference-thread tap | Cadence measured where it is produced, so F26 points at a mechanism instead of at nothing |
| 14 | Measure `origin/debug/aec` | The branch that removes F3, F4 and F15 at once, finally compared against the current one |
| 15 | Hand F12 to the DLL | Routes the half of "AEC is broken" that lives in the canceller, not in the plugin |

Added 2026-08-07, after F33. The framework detects and diagnoses; these are what make it safe to
put an agent behind unattended. They are loop readiness, not framework construction.

| # | Issue | Delivers |
|---|-------|----------|
| 16 | Route changes to the right oracle | The in-engine attenuation number stops implying it can detect a broken canceller, because F33 proved it cannot |
| 17 | One runner over both tiers | One command, one merged report, one honest exit code — and a launch that survives the last run's orphans |
| 18 | Protect the oracle | A threshold moving becomes visible in the report, so "green" and "green because the bar moved" stop looking alike |

**All three closed 2026-08-07, and none of them found what they were looking for.** 13 built the
tap and F27 came back negative: the feed is healthy on the runs that leak, so F3, F4 and F15 join
F1 as eliminated. 14 could not run as written — F28, the branch targets an architecture the plugin
no longer has — so the tap became a runtime switch instead, and F29 says a perfect feed does not
stop the leak either. 15's write-up is in the DLL repo next to the failing test; sending it is a
human step.

F26 now has **no surviving suspect on this side of `SendReferenceAudio`**. What is left is stream
alignment and the canceller's in-engine state, and the seam that would probe the first —
`ConvaiClient::SetStreamDelay` — is not exported by the shipped DLL.

### Session 4, 2026-08-07 — the DLL side

The DLL now exports `GetAECStats()` per **Connection** and `SetStreamDelay()`, so both of the
things the paragraph above says are unreachable are reachable. What that bought:

| | |
|---|---|
| **The microphone was carrying the room** | F31. The plugin's own capture component keeps the host's device open after a third-party one is adopted, and both render into the submix it records. Every live AEC number in sessions 1-3 was measured through it. Fixed, and guarded by an invariant a future regression cannot pass silently |
| **The canceller's state is now readable per run** | F32. With the APM's other stages off the AEC-off control reads exactly 0.000 dB and the canceller reads 2.7 to 3.3, against 40 to 49 dB for the same canceller offline |
| **And it is the suppressor, not subtraction** | F33. A one-token defect that takes the offline ERLE suite from 1 red test to 4 leaves the in-engine figure at 2.45 dB, inside the healthy range. There is nothing to break because nothing is being subtracted |

**The gate did not pass.** V5 was to be re-run against a seeded cancellation defect; the seed
does not appear in the report, so handing it to an agent would have measured the agent. What
the framework has now is a deterministic oracle for *"is cancellation enabled and doing
anything"* and no oracle for *"is cancellation working"* — and the second cannot be built until
some configuration of this plugin makes subtraction produce a signal.

**The next experiment is a `SetStreamDelay` sweep**, for the first time possible. Alignment is
the last surviving hypothesis, F33 narrows it rather than weakening it, and the actuator is
now on the public class and reachable from the plugin through
`UConvaiConnectionSessionProxy::SetStreamDelay`.

Issue 03 needs no network, no microphone and no AEC, and every confirmed suspect in
FINDINGS.md lives inside it. It is the fastest path to a true finding.

## Verification

- **V1** — the spike shows non-silent buffers reaching `FanAudioChunkToClients` on the target
  machine with no audio endpoint assumptions left implicit.
- **V2** — the reference-feed monitor reproduces Finding 1 of FINDINGS.md, or proves it was a
  teardown race.
- **V3** — the paired AEC scenario fails with `AECType=None` and passes with `Internal`. If
  the off-run passes, the fixture is broken, not the plugin.
- **V4** — synthetic **Injected Echo** and the acoustic rig agree on ERLE within a stated
  tolerance, justifying the synthetic path for all later runs.
- **V5** — a fix agent, given only `report.json`, can locate and verify a fix for a seeded
  bug without reading the framework's source. **Met 2026-08-07 — see F30.** Found in two
  greps, fixed, verified 3/3, and the fix matched the original byte for byte. It also
  exposed two defects in the report itself, both fixed: evidence prose that implied a
  mechanism it had no evidence for, and a finding that carried the outcome of a decision
  without its input.

## Open questions

- Does `AudioInput` (`/ConvAI/Submixes/AudioInput`) route to the master submix? If it does,
  the player's own voice is inside **Reference Audio** and cancellation will attack the
  near-end talker. Not readable from source — needs checking in-editor.
- Does the `Content` folder, stripped as "dependency-managed" by `push_to_public_v4.bat`,
  reliably deliver that submix asset to customers? If not, `_FoundSubmix` is null and the
  microphone read silently becomes a master-submix read.
