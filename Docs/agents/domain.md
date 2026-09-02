# Domain Docs

How the engineering skills should consume this repo's domain documentation when
exploring the codebase. This repo is **single-context**: one glossary and one ADR
directory cover all modules under `Source/`.

## Before exploring, read these

- **`CONTEXT.md`** at the repo root — the domain glossary.
- **`Docs/adr/`** — read ADRs that touch the area you're about to work in.

If these don't exist, **proceed silently**. Don't flag their absence; don't suggest
creating them upfront. The producer skill (`/grill-with-docs`) creates them lazily
when terms or decisions actually get resolved.

The existing `Context/` directory at the repo root is **not** the glossary — it is
raw working notes and captures (`rtvi.md`, `handoff.md`, hang dumps). Read it for
background if relevant, but don't treat it as `CONTEXT.md` and don't write glossary
entries into it.

## File structure

```
/
├── CONTEXT.md
├── Docs/
│   ├── adr/
│   │   ├── 0001-....md
│   │   └── 0002-....md
│   └── agents/
└── Source/
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a
hypothesis, a test name), use the term as defined in `CONTEXT.md`. Don't drift to
synonyms the glossary explicitly avoids.

If the concept you need isn't in the glossary yet, that's a signal — either you're
inventing language the project doesn't use (reconsider) or there's a real gap (note
it for `/grill-with-docs`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than
silently overriding:

> _Contradicts ADR-0007 (event-sourced orders) — but worth reopening because…_
