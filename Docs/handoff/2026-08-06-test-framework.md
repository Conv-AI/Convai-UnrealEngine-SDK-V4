# Handoff — plugin test framework design

Date: 2026-08-06
Branch: `docs/test-framework`, based on `feat/multi-connection`
Repos: `Convai-UnrealEngine-SDK-Dev` (plugin), `convai-livekit-cpp-p` (`convai_client.dll`, at `E:\Livekit\convai-livekit-cpp-p`)

## What this session was

A design interview, not implementation. The maintainer asked for an automated test framework
that mimics human interaction with the plugin — scripted audio through the real microphone
path, text, several characters at once, actions, objects — with echo cancellation as the
first target because it is failing often and no existing test can see it.

Nothing was built. Four commits, all documentation.

## Read these, in order

1. `.scratch/test-framework/PRD.md` — the design. Decisions, architecture, tiers T0-T4,
   sequencing, verification gates V1-V5, open questions.
2. `.scratch/test-framework/FINDINGS.md` — F1-F11, each marked confirmed or hypothesis.
   These are the framework's first targets and several are actionable without it.
3. `.scratch/test-framework/issues/01-12` — implementation issues, ordered.
4. `Docs/adr/0004-test-layer-partition.md` — why AEC quality is measured in the DLL repo
   rather than Unreal.
5. `Docs/adr/0005-tests-as-a-strippable-module.md` — why the framework is a separate module
   that plugs in through existing extension points.
6. `CONTEXT.md` — glossary. Use its terms in code, comments, commits and tests. **Virtual
   Mic** and **Injected Echo** were added this session.

## Branch state

Branched from `feat/multi-connection`, which carries the documentation infrastructure these
docs depend on — `CONTEXT.md`, ADR-0001 through 0003, `Docs/agents/`, `CLAUDE.md`. All
cross-references resolve.

A rebase onto `WebRTC-Video` was tried and reverted. Recorded here so nobody repeats it:
`WebRTC-Video` is an ancestor of `feat/multi-connection` (15 behind, 0 ahead) and predates
all of that infrastructure, so the rebase conflicted on `CONTEXT.md` and left ADR-0004,
ADR-0005 and issue 09 citing ADRs that do not exist there. If these docs are ever needed on
`WebRTC-Video`, the prerequisite doc commits have to travel with them.

## What is decided

The PRD carries the detail. The decisions most likely to be second-guessed, and why they went
the way they did:

- **The report's consumer is a fix agent, not a human.** That is the root of the statistical
  findings, the mechanical-attribution rule, and the replay requirement.
- **The JSON scenario front-end is built last** (issue 11), not first, even though low-friction
  authoring was a stated priority. Writing the vocabulary before the scenarios means inventing
  a language for work not yet done.
- **Unreal does not measure AEC quality.** It measures the feed. ERLE lives in the DLL repo
  where the cancelled signal is readable in place.
- **The acoustic rig calibrates the synthetic path; it is never the default run.** Its results
  depend on room noise and output volume, which is poison for an agent loop.
- **The fix agent does not get write access to assertions, invariants or fixtures.** The
  cheapest path to green is weakening the oracle.

If a future session wants to reverse one of these, the reasoning is in the PRD and the ADRs —
argue with that, not from scratch.

## What is open

- **F7** — does `/ConvAI/Submixes/AudioInput` put the microphone inside **Reference Audio**?
  Evidence against it (see F6's repro), but not settled. Decided test is a tone probe through
  the Virtual Mic, added to issue 03, not an asset inspection.
- **F11** — `StartRecordingOutput(World, 60.0f, nullptr)` reserves ~46 MB and runs every
  ~10 ms on the audio thread, in the healthy path. Unmeasured. Candidate cause for F3, F4 and
  F10 simultaneously. Highest suspicion-per-line-of-code in the findings.
- **Headphones rerun** of the deleted-submix case, to separate acoustic feedback from a
  sample-rate defect. Maintainer action, not yet done.
- Whether `Content` — stripped as "dependency-managed" by `push_to_public_v4.bat` — reliably
  delivers the submix asset to customers. If not, every customer hits F6.

## Suggested next actions

Two tracks, independent:

- **Issue 06** (ERLE in `convai-livekit-cpp-p`) depends on nothing and can start immediately.
  It bisects the problem: healthy ERLE offline means every reported symptom is plugin
  plumbing, and issues 03/05 are aimed correctly.
- **Issue 01 → 03** on the Unreal side. Issue 03 needs no backend, no microphone and no AEC,
  and F1, F3, F4, F7 and F11 all get quantified inside it.

F11 does not need the framework at all — a profiler capture on the audio thread during a live
session would settle it in minutes.

## Skills for the next session

- `tdd` — issues 03 through 10 are test construction; red-green-refactor fits directly.
- `diagnose` — for F11 and F7, which are bug investigations rather than framework work.
- `to-issues` — if the PRD needs further breakdown once implementation starts.
- `grill-with-docs` — only if a decision above is being reopened. The design tree was walked
  once already; re-walking it without new information wastes the session.

## Watch out

- The existing harness under `Source/Convai/{Public,Private}/Tests` is **dead code** —
  `WITH_CONVAI_TESTS=0` is unconditional at `Source/Convai/Convai.Build.cs:258`, so it has
  never compiled. Do not treat it as a working framework to extend. Issue 02 says what to
  salvage; issue 12 removes the rest.
- `convai-livekit-cpp-p/tests/harness/` is a real, working harness with the architecture this
  design mirrors. Read `tests/ARCHITECTURE.md` before writing the Unreal equivalent rather
  than reinventing the scenario registry, event recorder and watchdog.
- Test runs must not pass `-nosound`. The entire **Reference Audio** path needs a live
  `Audio::FMixerDevice`.
- Branches `fix/aec` and `origin/debug/aec` exist and were not examined this session. Check
  them before starting AEC work — they may already contain attempts at F1-F11.
