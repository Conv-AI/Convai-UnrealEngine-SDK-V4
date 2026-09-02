# 08 — Always-on invariant monitors

Status: `ready-for-agent`

**Added 2026-08-07 from F30.** Carry a captured `CONVAI_LOG` excerpt around the failure window
into each finding. V5's fix agent asked for it by name and preferred it to the bespoke metric that
was added instead: the plugin already logs which branch it took at the point most findings are
about — `"Started alternative audio capture"` versus `"Started default audio capture"` is one
example — and either string in the report answers in one token what a metric answers for one
scenario. Fixed cost per finding, general benefit.
Depends on: 02, 03

## Goal

Find bugs nobody wrote a test for. Scenarios only assert what their author imagined;
invariants run across every scenario regardless of what it tests, and can produce findings on
scenarios that pass.

## Scope

Generalise the reference-feed monitor from issue 03 into a monitor layer that any scenario
gets for free.

**Invariants:**

- attached reference clients equals live **Connection** count (from issue 03)
- reference feed rate, gap and chunk-size variance within bounds (from issue 03)
- no duplicate player transcript across **Talk Targets** — only the **Primary Target**
  delivers player-directed events, per ADR-0002
- no callback delivered after teardown or component destruction
- reference client attach and detach balance to zero at end of run
- no **Connection** outlives its **Lease** grace period
- no delegate fired on an unexpected thread
- no unexplained `OnFailure`
- no unknown server packet type — F8 would have been caught by this alone
- latency percentiles within declared budgets

**Hook policy.** Prefer polling public state. Where an invariant genuinely needs an
observation point the plugin does not expose, that hook is a design change justified on its
own merits, listed in this issue with its rationale — not a test back door (ADR-0005).

**Reporting.** An invariant violation is a finding with the same evidence chain as an
assertion failure, deduped by root cause, carrying its occurrence rate. Violations on
otherwise-passing scenarios are the point — surface them, do not suppress them.

## Done when

Every scenario runs with the monitor layer attached, violations appear in `report.json`
independent of scenario outcome, and the unknown-packet-type invariant reproduces F8.
