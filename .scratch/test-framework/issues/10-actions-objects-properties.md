# 10 — Actions, Objects, Tracked Properties

Status: `ready-for-agent`
Depends on: 07, 08

## Goal

Cover action dispatch and scene context, split so plugin bugs gate hard and model behaviour
does not.

## Two tiers, always both

**Tier A — the plugin handles the payload.** Deterministic, offline, no backend. Synthesise
the server payload, feed it through `HandleDataPacketReceived`, assert the whole chain:
parse → **Object** resolution → **Tracked Property** update → Blueprint dispatch.

This is where plugin bugs live. `ConvaiActionParsingTest.cpp` already proves the seam works.

Cases: well-formed actions; unknown action names; an action naming an **Object** that does
not exist; an object whose actor was destroyed between advertisement and dispatch; parameter
type mismatches; empty and oversized action arrays; actions arriving for an **Orphaned
Connection** rebound to a component with a different configuration — per CONTEXT.md the
**Advertised Action Contract** belongs to the **Connection**, so the new owner must dispatch
against what the server was told, not against its own config.

**Tier B — the server produces the payload.** Live, tolerance-based. Speak or send an
instruction, assert the returned action name is in an expected set rather than exactly equal.
Reported as `model-dependent`; never a hard failure.

Test characters need no special setup: the **Advertised Action Contract** is sent per
**Connection** in `action_config` at connect, not stored on the character. Provision
characters with `tests/harness/scripts/create_characters.py` from the DLL repo — it pins a
backstory and constrains response length.

## Tracked Properties

- a **Tracked Property** change reaches the character as context
- changes coalesce rather than flooding
- an **Object** leaving scope stops reporting
- property updates during a **Connection** handoff between components

## Done when

Tier A runs offline in the deterministic set and its failures are hard findings. Tier B runs
live and its results are reported with `model-dependent` attribution and occurrence rates.
