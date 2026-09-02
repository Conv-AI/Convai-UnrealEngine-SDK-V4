# Convai Debug Overlay

An in-viewport debugger for Convai's actions and dynamic context. It answers,
at a glance: which objects does the system see, what does the selected
character currently know, can it actually reach things, and what is it doing?

## Opening it

- **Hotkey**: `Ctrl+Alt+K` during PIE or in a Development build. The key (the
  `K`) is configurable under **Project Settings ▸ Plugins ▸ Convai ▸ Debug
  Overlay ▸ Toggle Key**.
- **Console**: `Convai.DebugOverlay` toggles it for that world's game instance.
- **Shipping/Test builds**: refused unless **Allow In Shipping Builds**
  is enabled in the same settings section. Editor and Development builds need
  no opt-in.

The hotkey is ignored while a text field has keyboard focus (Ctrl+Alt is AltGr
on many keyboard layouts). In single-process multi-client PIE the hotkey is
application-wide (the first client to register handles it) — use the
`Convai.DebugOverlay` console command to toggle a specific client's overlay.

## Selection

- **PgUp / PgDn** — cycle the selected *object* (its marker turns cyan, and its
  context-state rows highlight in the panel).
- **Shift+PgUp / Shift+PgDn** — cycle the selected *character* (the panel and
  the over-head action queue follow it).

## What you see

**Object markers** — a name + `◆` glyph for every registered Convai object,
with no first-N display cap. A base marker sits just above the resolved
rendered component's bounds (or the whole actor's rendered bounds), rather
than at its pivot. Disabled object components are simply absent — they are not
registered, so nothing marks them. Every enabled movement point with **Create
Separate Destination** ticked gets its own smaller `◇` marker labeled with the
destination name, floating on the exact spot it targets. Markers remain
visible through walls but dim when occluded. To keep that complete marker set
bounded, the overlay reprojects at most 64 markers and refreshes occlusion for
at most 12 visible markers per frame in independent round-robin passes. Widgets
retain their last screen position and occlusion result between updates; a full
position pass takes `ceil(marker count / 64)` frames, so later registry entries
are delayed proportionally rather than dropped.

**Character panel** (right side) —
- Header: character name, `TALKING`/`idle`, and what it is looking at.
- **CONTEXT STATES**: every dynamic-context state key/value the character
  holds. A state change pulses the row's dot, tinted by the state's *Should
  Respond* setting: grey = Never, amber = Auto, red = Always. The selected
  object's own states highlight cyan — that list *is* its tracked properties
  exactly as the AI sees them. A row briefly reads **"(pending flush)"**
  while the value sits in the debounce batch, i.e. before it is actually
  online in the chatbot's context backend.
- **FACTS** (hidden when empty): declarative
  facts set through *Set Context Fact*, verbatim, with the same "(pending
  flush)" marker. Spatial facts are excluded — they render under
  SURROUNDINGS with reachability.
- **SURROUNDINGS**: one row per published spatial fact — objects, other
  characters, the player, and any separate destinations authored on movement
  points ("Door Other Side"). The bold subject label carries the nav
  verdict: plain name = reachable, cyan "— reached" = the character is
  already there, red "— no path" = unreachable. A subject with no row is
  one the AI is currently told nothing about (out of sight, opted out, or
  disabled by the observer/project settings). Each label also shows the fact
  sentence the pass last published, *verbatim* and dimmed — e.g. "You are
  already at the PressurePlate, which is underneath you." — with the
  "(pending flush)" marker until it leaves the debounce batch. Nothing is
  recomputed, so the row can never drift from the prompt.

- **EVENTS** (hidden when empty): the last six committed context events in
  order, preceded by a dim "(+K earlier)" when truncated, then what the next
  flush will add — staged events "(pending flush)", ephemeral events
  "(one-shot)" (the AI hears them exactly once, then they vanish), and any
  pending attention text.

**Action queue** — the selected character's pending actions float over its
head, current action first; their pointer and order are unchanged by result
animation. Each terminal result is appended beneath the live queue in
completion order: green **done**, red **failed**, or amber **aborted**. Rapid
results remain stacked instead of replacing one another. The eight most recent
rows are retained. Each drifts downward and fades from a 2.5-second baseline;
newer burst results receive a small fixed lifetime extension, so they remain
legible without retiming or jumping older rows.

## Notes

- The overlay is snapshot-first: panels re-pull live registry state at the
  object poll cadence, so it can be opened at any time — the event feed only
  drives the moment-of-change animations.
- Proximity reachability reads the spatial pass's per-observer nav cache; it
  requires spatial awareness to be running (i.e. a chatbot in the level).
  Without a navmesh, published rows still render: non-reached subjects read
  red "no path", while the pure-proximity "reached" state can still appear.
