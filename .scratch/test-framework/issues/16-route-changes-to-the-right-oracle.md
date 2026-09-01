# 16 — Say which oracle grades what, because the obvious one lies

Status: `done` — 2026-08-07, session 5. `aec_atten_db` now travels with a `metric_notes` entry
written where the metric is emitted (ConvaiAecEchoOnlyScenario.cpp), stating what it separates
and that it cannot detect a broken canceller; the merged report carries a `tiers` block naming
what each tier grades and reporting an unrun tier as `not-run`, never as passing. Verified by
re-running F33's seed through the merged runner: offline tier red with the three test names in
the report and exit 1; seed reverted, 21/22 green with the F12 skip its own column. The metric
itself is unchanged.
Depends on: 13, 14

## Why

F33 seeded a one-token cancellation defect in the DLL — `/*reverse=*/true` → `false` in
`AudioProcessor::ProcessReferenceStream`, so the APM never learns the echo. Offline it takes
`aec_erle_test` from 0 failing to 3. In-engine it did not move the number:

| | Attenuation across the window |
|---|---|
| unseeded | 3.29, 2.68, 3.04, **2.09**, 3.34 dB |
| **seeded** | **2.45**, −0.34 dB |

One seeded run sits inside the healthy range whose minimum is 2.09. **The in-engine attenuation
metric cannot detect a broken canceller**, and F33 explains why: in-engine, echo *subtraction*
contributes nothing to begin with, so breaking it changes nothing. The ~3 dB is AEC3's
residual-echo suppressor gating on far-end activity.

That is fine as a fact and dangerous as a default. `aec_atten_db` is the most obvious
AEC-looking number in the report. An agent changing the canceller and seeing it hold at 2.45 dB
will conclude its change was safe. Nothing currently contradicts that.

## Scope

**Label the metric where it is emitted, not only in FINDINGS.** `aec_atten_*` should travel with
a statement of what it can and cannot separate: *AEC enabled vs disabled*, perfectly and with no
sample size required; *cancelling vs not cancelling*, one time in two. The report's consumer is
an agent that will not read this file.

**Say which tier grades which change.** A change to `convai-livekit-cpp-p` is graded by
`aec_erle_test`, which caught the seed deterministically in three seconds. A change to the
plugin is graded in-engine. A report from one tier alone should say what it did not cover.

**Do not delete or weaken `aec_atten_*`.** It is a good integration signal and F32 established
its control reads exactly 0.000 dB. The defect is the label, not the number.

## Verification

Re-run F33's seed. The loop must now produce something an agent can act on — either the offline
tier failing where it is consulted, or the report saying in words that the in-engine number does
not cover canceller regressions. Silence is the current behaviour and is what this issue exists
to end.

## Out of scope

Making the in-engine number sensitive to subtraction. That is not a reporting problem; per F33
there is no subtraction in-engine to be sensitive to, and the remaining hypothesis is stream
alignment. Sweeping `ConvaiClient::SetStreamDelay` — newly exported, see `a222ab8` — is the
experiment that would change this, and it is not this issue.
