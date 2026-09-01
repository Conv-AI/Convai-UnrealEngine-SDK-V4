# Convai Actions — Overview

Convai characters can do more than talk: they can **perform physical actions**
in your scene. The action system lets the LLM that drives a Convai character
emit structured commands (move to a target, pick up an object, wait, run a
custom animation, etc.) which your gameplay code then executes.

This guide is split into **three phases**, each one introducing the next layer
of capability:

| Phase | Goal | What you'll learn |
|-------|------|-------------------|
| **[Phase 1 — Default Actions](./Actions_Phase1_DefaultActions.md)** | Get the bot moving in the level with zero scripting. | Enable actions on the chatbot, register objects, set up locomotion, run on a NavMesh. |
| **[Phase 2 — Custom Actions](./Actions_Phase2_CustomActions.md)** | Add a new action with no parameters and wire it to a Blueprint event. | The `FConvaiAction` template, `OnActionReceived` event, `HandleActionCompletion`. |
| **[Phase 3 — Parameterized Actions](./Actions_Phase3_ParameterizedActions.md)** | Add typed parameters, connectors, and choices. | Parameter types (`Auto` / `Reference` / `String` / `Number` / `Bool` / `Enum`), `Get Param As X` accessors, the generated wire format, abort vs. completion. |

## Mental model

The action system has three moving parts you'll see throughout the guide:

1. **The chatbot's `Environment`** — a struct on the chatbot component that
   declares the world the bot can act on:
   - `Actions` — the list of `FConvaiAction` templates the bot is allowed to use.
   - `Objects` — props in the scene the bot can target (cube, lever, door, …).
   - `Characters` — NPCs / the player.
   Only enabled actions are sent to the server at `/connect` time as the bot's
   "action contract"; the parser freezes that same enabled-only set for the
   connection, so disabled actions cannot be emitted or matched. Their authored
   names still reserve identity and suppress same-named native built-ins.
2. **`bEnableActions`** — a per-chatbot toggle that decides whether the
   `Environment` is sent at all. Off = conversational-only. On = the bot can
   emit action sequences.
3. **The action-response pipeline** — when the bot decides to act, the server
   sends back a list of `FConvaiResultAction`s. The plugin matches each one
   against your declared templates, parses out parameter values, resolves any
   actor references against `Environment.Objects` / `.Characters`, and fires
   `OnActionReceivedEvent_V2` with the parsed sequence. Your handler runs the
   action and calls back into `HandleActionCompletion` or `AbortActionSequence`
   to advance / retry / abort.

## Reusable movement for custom handlers

The asynchronous Blueprint node **`Convai Move To`** is the shared movement
primitive used by **Convai Escort**. Give **Moving Actor** the pawn that should
move and provide an
`FConvaiObjectEntry`; it handles Convai goal selection, reachability, arrival,
moving-target updates, one bounded retry, and cancellation.

The node exposes only **Succeeded** and **Failed**. Each returns a result code
and player-safe `Additional Note`; developer diagnostics stay in native logs.

The node moves the pawn; it does not finish the current Convai action. Your
handler must call `Handle Action Completion` from a terminal pin. For
cancellation, save **Move Request** and call its `Cancel` function from the
separate `Cancel <ActionName>` event. Call `Handle Action Cancellation` only
when **Cancellation Succeeded** is true; false means natural completion owns
the terminal acknowledgment. The bundled stock Blueprint `Move To` and
`Follow` graphs remain unchanged.

## Guided movement: `Escort`

`Escort` is a stock row under **Convai → Actions → Environment → Actions**,
disabled by default. Enable that row when the chatbot should guide another
character without leaving them behind. It then joins the advertised contract at
the next connection like any ordinary action. Keep its canonical template as:

```
Escort {character: ref} to {destination: ref}
```

`character` keeps the general `Reference` wire type but is resolved semantically
and case-insensitively from `Environment.Characters`; `destination` is the
Convai object or named destination. Connect the chatbot's owning AI-controlled
pawn to **Escorting Actor**, the character entry to **Escorted Character**, and
the destination entry to **Destination**. The Convai Escort node composes the
shared movement task, so
the normal Convai resolver remains authoritative for destination choice,
reachability, and arrival. The guide pauses and faces a companion lagging by
full 3D distance, asks them to follow only at a quiet conversational moment,
then resumes when they catch up. If teardown happens first, any still-held
follow prompt is withdrawn.

The stock description also uses the current surroundings to keep speech
natural: already at means discuss the destination now, close by gets only a
brief transition, and follow-me wording is reserved for real travel.

The node exposes `Succeeded` and `Failed` with a result code and player-safe
note. An exact named Blueprint handler invokes it and owns response policy plus
`Handle Action Completion`; the Environment row advertises the schema but does
not install a native fallback. **Reached** means movement occurred and normally
uses an idle-delivered, immediately flushed **Always** response. **Already At
Destination** is a distinct successful no-op and normally completes with
**Never** to avoid repeating the destination discussion. Escort emits no
separate response event, so the handler's completion policy is not duplicated.

To cancel, save **Escort Request**. `Cancel Escort` calls its `Cancel` function
and acknowledges cancellation only when **Cancellation Succeeded** is true.
Cancellation is silent, and a false return leaves acknowledgment to the normal
terminal pin.

`Cancel Action Plan` is a separate experimental control. Enable it independently
when the user must be able to redirect an escort that is moving or waiting.
See [ActionsV2](../ActionsV2.md#escort) for
the resolver and cancellation details.

## Sequential plans and redirection

The queue dispatches one action at a time. A running handler owns its operation
until it produces one terminal acknowledgment; the next action never starts
merely because a new model response arrived.

For experiences where the player can change their mind mid-action, enable the
reserved **`Cancel Action Plan`** built-in. The AI puts that marker before the
replacement actions. While the built-in is advertised and unshadowed, the
marker itself never reaches gameplay code. (An enabled project action matched
case-insensitively keeps ordinary custom-action behavior; a same-named handler
alone does not change the frozen contract, while a disabled same-named row
suppresses the native control.) If a control response
contains more than one marker, the last one wins and only its suffix is
retained.

When an action is already running, it stays as the active queue-head tombstone
while the new suffix is held separately. An optional Blueprint event named
exactly `Cancel <ActionName>` can request cleanup. It is called at most once for
that active action, even if another cancel response arrives; repeated cancels
only replace the held suffix, so the latest user intent wins. The old handler
then finishes through its normal `Handle Action Completion` callback or the
silent `Handle Action Cancellation` acknowledgment. Only then can the
replacement begin.

This cooperative barrier intentionally has no public execution ID. It prevents
a late callback from an abandoned action from completing one of the new
actions. Cancellation-time successes retain meaningful state-change notes as
silent context; cancellation-time failures suppress generated retry/apology
prose.

Keep these controls distinct:

- **Cancel Current Action Plan** / **Cancel Action Plan** means “valid work was
  redirected”; request cleanup and wait before replacing it.
- **Abort Action Sequence** means “gameplay says this plan failed”; clear it and
  optionally tell the AI why.
- **Clear Action Queue** administratively drops pending/held work, not a
  conversational outcome. It does not fake completion of a dispatched action;
  that active head remains until its terminal callback.

The chatbot derives compact `Instructions` and `Current action plan` context
sections for enabled built-ins and active work. The current-plan block describes
the rendered invocation with generic waiting/running/cancelling wording, shows
only the immediate next action and remaining count, and disappears while idle;
neither block accumulates as event history.

A transient server disconnect cancels owned action work and clears the current
plan without reporting it, but leaves the executor reusable after reconnect.
Explicit `Stop Session` is terminal for that session (and clears armed property
watches); a later `Start Session` opens a fresh lifecycle. `End Play` or
destruction is terminal for the component and stale callbacks are ignored.

## What's new in V2

If you've used Convai actions before this overhaul, here's what changed at a glance:

- Action templates moved from raw `FString` (`"Wait For <time in seconds>"`) to
  a structured `FConvaiAction` with typed `Parameters`, optional `Connector`,
  optional `Choices` / `EnumType`.
- The **wire format** sent to the LLM is generated from the structured fields
  (with typed hints, connectors, and choice blocks) instead of being hand-written.
- Action results unify everything into `Parameters: TMap<Name, FConvaiResultParam>`
  with always-best-effort string / number / bool / actor-ref coercion. Legacy
  `RelatedObjectOrCharacter` and `ConvaiExtraParams` are still populated as
  deprecated mirrors so existing handlers keep working.

If you have legacy graphs, follow the migration notes in [Phase 3](./Actions_Phase3_ParameterizedActions.md#migrating-from-the-legacy-fields).

## Related systems

Actions are how the bot *acts on* the world. Two neighbouring systems feed the
bot *knowledge of* the world — worth knowing about as you build:

- **Convai Object Components & Tracked Properties** — drop a component on an
  actor to make it visible to every chatbot, expose live values (a lock state, a
  health count) that stream into dynamic context, and drive gaze-based attention.
  See [ConvaiObjectsAndTrackedProperties.md](../ConvaiObjectsAndTrackedProperties.md).
- **Dynamic context & spatial awareness** — state, events, facts, and an
  automatic "where things are" pass, all delivered to the LLM between turns. See
  [ActionsAndEnvironment.md](../ActionsAndEnvironment.md).

Actions can also be timed to the character's speech: an `FConvaiAction` can set
`bWaitForBotSpeech` / `DelayAfterBotSpeechSec` so a gesture lines up with the
dialogue rather than firing the instant the sequence arrives (see
[ActionsV2.md](../ActionsV2.md#the-template-fconvaiaction)).
