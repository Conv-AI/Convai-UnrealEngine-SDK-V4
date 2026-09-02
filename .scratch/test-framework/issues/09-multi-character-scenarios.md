# 09 — Multi-character scenarios

Status: `ready-for-agent`
Depends on: 04, 07, 08

## Goal

Cover the routing and lifetime rules that only exist with more than one **Connection** — the
newest and densest bug area in the plugin.

## Scope

Scenarios written in C++, since these need control flow the JSON front-end will not express.

**Routing** (ADR-0002):

- microphone reaches every **Talk Target** and nothing else
- player transcript and speech start/stop arrive only from the **Primary Target**, exactly
  once, regardless of target count
- attendee events fan from every target
- changing the **Talk Target** set mid-utterance takes effect within a stated bound
- dropping a target whose **Connection** went away does not deliver to a dead proxy

**Lifetime** (ADR-0001):

- releasing a **Lease** turns the **Connection** into an **Orphaned Connection**, reused by
  the next component wanting that character inside the grace period
- an **Orphaned Connection** past its grace period is gone
- a **Prepared Connection** becomes normally leased on first acquisition
- the **Advertised Action Contract** survives an orphan rebind to a component whose own
  configuration differs — per CONTEXT.md the contract belongs to the **Connection**
- **Connection** stays exclusive per character id

**Chaos**, the shape the DLL harness's `random_chaos` covers one layer down:

- kill one **Connection** mid-utterance while others stream
- connect and tear down repeatedly inside the grace period
- teardown during active speech, during handshake, during action dispatch
- PIE stop with connections live — the shape that produced F1

**Aggregate state** (ADR-0003): the process-global Blueprint nodes report state derived
across every live **Connection**, not the state of any one.

## Done when

Each scenario runs ten times with the monitor layer attached, and failures carry a replay
fixture. The chaos set runs last and longest.
