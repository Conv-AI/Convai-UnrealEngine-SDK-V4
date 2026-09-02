# Handoff — ConvaiWebRTC DLL A/B testing, and the plugin issues it exposed

2026-08-20. Repo `Convai-UnrealEngine-SDK-Dev`, branch `WebRTC-Video`, HEAD `61ccaf6a`.

## What happened

Three A/B sweeps of the `ConvaiTests` suite across four ConvaiWebRTC builds. Every
conclusion, number and issue is already written down — do not re-derive it:

| Artifact | What it holds |
|---|---|
| [`.scratch/test-framework/ISSUES.md`](.scratch/test-framework/ISSUES.md) | I1–I14, the issues the comparison exposed, with per-issue evidence and fix shapes. **Start here.** |
| [`.scratch/test-framework/FINDINGS.md`](.scratch/test-framework/FINDINGS.md) | F1–F38, the pre-existing plugin findings the red scenarios map to |
| [`docs/ConvaiTests.md`](docs/ConvaiTests.md) | how to build, run and read the suite; the DLL-swap procedure and its traps |
| commits `86bcc4f4`, `278ea00c`, `61ccaf6a` | the three documents above |
| `.testruns-ab/` (gitignored, ~130 MB) | 572 run logs and reports, `ledger*.json` with per-launch DLL-version attestation, and the swap + analysis scripts |

Headline: **all four builds are functionally indistinguishable on this suite.** 0.1.26,
0.2.10, 0.2.10-Hotfix1 and 0.2.12 all return `p ≥ 0.44` on every scenario comparison, most
at `p = 1.0000`. Zero regressions, zero fixes, across 572 launches. The one signal that
replicated is 0.2.10's disconnect latency against 0.1.26 (~half the median, tighter spread).

## The thing that matters most for the next session

**The remaining issues are plugin-side and test-framework-side, not DLL-side.** No DLL swap
will close them. Of I1–I14, only I5/I7/I8 were ever DLL-addressable and none moved across
three releases.

**A contemporaneous control arm is mandatory.** Three separate "fixes" were caught as backend
drift only because a control ran alternated in the same session — `aec_echo_only_none`,
`aec_double_talk`, and I5's missing `bot-turn-completed`. Two more apparent AEC fixes were
explained by 4–6× less echo energy on the passing run, visible only in
`echo_source_nonsilent_samples`. Never call a fix from a single arm.

**Most scenarios cannot detect anything.** 17 of 22 are deterministic (12 always pass, 5
always fail), so a green sweep is mostly evidence of insensitivity. Separating two builds'
pass rates would need 37,200 runs per arm. This is I9 and it is the reason "no change" reports
should be read carefully.

## Verified environment facts

- Engine: `E:\Software\UE_5.8`. `run.py`'s `--editor` default does not exist here; always pass it.
- Build with **no editor alive** or Live Coding fails the link with `LNK1104`.
- `--arg` needs the `=` form: `"--arg=-Foo"`. `--arg -Foo` fails argparse.
- Character: `TestCharacterID` is empty in `DefaultEngine.ini`. Prod ids are in
  `E:\Livekit\convai-livekit-cpp-p\build\windows-x64-tests\tests\Release\parallel.txt`, whose
  `auth_value` matches the project's configured API key. This study used
  `903b069a-485d-11f1-a04c-42010a7be02e`.
- `"--arg=-AllowStdOutLogVerbosity"` is required to get `ConvaiClient Version:` into each
  launch's own log; `Saved\Logs` keeps only 10 backups against 22 launches per pass.
- A full 22-scenario pass costs ~9 minutes.
- The plugin loads DLLs from `<plugin>\Binaries\Win64` by absolute path and only backfills
  **missing** files. `convai_client.dll` imports `AECwebrtc.dll` and `convai_http_helper.dll`;
  a partial copy fails to load with `GetLastError=126` and the suite keeps running, 9 scenarios
  still "passing". Full procedure in `docs/ConvaiTests.md`.

## State of the tree

- Working tree clean at `61ccaf6a`. Nothing uncommitted.
- Currently deployed DLL: **0.2.12** (`0.2.12.320+3b122f2`), all three locations sha256-verified.
- Plugin binary built from `278ea00c`-identical source; `Source/` unchanged since `ac69ea25`.
- `.testruns-ab/` holds the reusable harness: `swap-convai-dll.ps1` (wipe + install + verify
  every binary in every destination, refuses before wiping on an incomplete source),
  `ab_sweep.py` (alternating blocks, per-launch version gate), `ab_report.py` / `ab_metrics.py`
  / `ab_perf.py` (aggregation, Fisher exact and Mann-Whitney reusing `run.py`'s own statistics).
- Snapshots under `%TEMP%\...\scratchpad\`: `xhf` and `x212` intact; `x26` and `x210` were
  reaped by temp cleanup — re-extract from `E:\UEProjects\UE5.8\Dev_WebRTC\TestDlls\*.zip` if a
  0.1.26 or 0.2.10 arm is needed again.

## Next session

Fix the issues in `ISSUES.md` and prove each fix, in a loop. The full brief the user wants the
next agent to follow is in **`PROMPT.md`** at the plugin root.

Suggested skills: `tdd` for the framework issues that have a stated assertion (I2, I3, I10),
`diagnose` for I7's crash and I5's missing packet, `triage` if the issue list needs re-ordering.
Not `grill-with-docs` — the design arguments are settled and recorded.
