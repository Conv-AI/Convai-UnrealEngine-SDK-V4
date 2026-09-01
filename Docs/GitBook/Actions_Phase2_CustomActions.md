# Phase 2 — Custom Actions

Phase 1 used the four shipped default actions. Now you'll add your **own
action** with no parameters and wire it to a Blueprint event. By the end of
this phase, asking the bot *"print hello"* will run your Blueprint code.

## What you'll need

- The Phase 1 setup working — chatbot in a level, `Enable Actions` ticked,
  NavMesh in place, character locomotion set up.
- The character Blueprint asset open (or accessible).

## Step 1 — Declare the action template

1. Select the Convai character actor in the level.
2. In the Details panel, find the **Convai Chatbot** component →
   **Convai → Actions → Environment → Actions**.
3. Click **`+`** to add a new entry. UE expands an `FConvaiAction` struct.
4. Fill in:
   - **Enabled**: `true`. Disabled definitions remain authored but are excluded
     from the connect-time contract and frozen parser set.
   - **Name**: `Print` (this is the canonical action name your handler will
     dispatch on).
   - **Description**: `Print a debug message to the screen`.
   - Leave **`Parameters`** empty.

The **`Rendered String`** field below auto-populates with:

```
Print — Print a debug message to the screen.
```

That's the wire-format string sent to the LLM. As you edit the Name or
Description, the rendered string updates live.

Action-name identity is case-insensitive. Only enabled definitions enter the
frozen contract and parser set, but every authored name reserves its identity;
a disabled same-named action therefore suppresses a native built-in without
being advertised itself.

## Step 2 — Bind a handler in Blueprint

The chatbot fires `OnActionReceivedEvent_V2` whenever the bot decides to act.
You react to it in the character's Blueprint.

1. Open your character Blueprint.
2. In the **Components** tab, click the **Convai Chatbot** component to
   select it.
3. In the **Details** panel for that component, find the **Events** category.
   Click the **`+`** next to **`On Action Received Event V2`**. UE drops a
   bound event into the Event Graph.
4. The event delivers a **`Sequence Of Actions`** array (each entry is an
   `FConvaiResultAction`) plus references to the Chatbot Component and the
   interacting Player Component.

## Step 3 — Dispatch by action name

Inside the bound event, **for each** action in the array:

1. **Get** `Action` (the canonical name string).
2. **Switch on String** with cases for each action you've declared:
   - `Print` → call your custom logic, then `Handle Action Completion(Is Successful=true)`.
   - **Default** → call `Handle Action Completion(Is Successful=true)` to keep the queue moving (or leave un-handled actions to retry — that's an `Is Successful=false` case).

For the `Print` case:

1. Drag a **`Print String`** node off the execution pin and feed it a literal
   `"Hello from Convai!"` (or read from the Action — but for this phase the
   action has no params).
2. After the print, drag in **`Handle Action Completion`** off the chatbot
   reference. Set:
   - **Is Successful**: `true` (the action ran) → dequeues and starts the next action.
   - Leave **b Auto Report** at its default `true` — the bot gets a sensible
     default outcome message (*"you were able to Print successfully"*).
   - Leave **Should Respond** at `Never` (the outcome lands in the LLM's view on
     the next user turn instead of interrupting the bot to narrate its own action).
   - Leave **Additional Note** and **Delay** (advanced) empty / `0`.
   - Leave **Delivery**, **b Ephemeral**, and **Flush Immediately** at their
     defaults for this ordinary silent completion.

That's the full handler. The bot now knows how to execute `Print`.

## Step 4 — Play test

Hit **Play** and ask the character: *"Print hello."*

In the editor's viewport you should see `"Hello from Convai!"` (or whatever
literal you used) appear via Print String. The bot will also speak its
acknowledgment.

## How the pipeline runs end-to-end

When the LLM emits an action:

1. The server sends `{name: "Print"}` (no target, no params).
2. The plugin's parser finds the `Print` template by name in your
   `Environment.Actions`.
3. Since the template has no declared parameters, `Parameters` stays empty.
4. `OnActionReceivedEvent_V2` fires on the chatbot with the parsed sequence.
5. Your Blueprint dispatches by `Action == "Print"` and runs the print.
6. `Handle Action Completion(Is Successful=true)` advances the queue. If there
   were more actions in the sequence, the next would now run.

## Why the queue exists

The bot can emit **a sequence** of actions in one response, e.g.
*"Wait 2 seconds, then go to the cube, then print done."* The queue makes
sure each action completes before the next one starts. Your handler is
responsible for telling the queue when the current action is done — that's
what `Handle Action Completion` does.

| Call | Effect |
|------|--------|
| `Handle Action Completion(Is Successful=true)` | Mark current action successful, run the next one immediately. |
| `Handle Action Completion(Is Successful=true, Delay=1.5)` | Successful, but wait 1.5s before the next action. |
| `Handle Action Completion(Is Successful=false)` | Failed — clears the rest of the queue. |
| `Handle Action Cancellation` | Confirm that a cooperatively cancelled action is fully stopped; silently release a held replacement plan. |

The full pin list is `Is Successful`, `b Auto Report`, `Should Respond`,
`Additional Note`, `Delay`, `Delivery`, `b Ephemeral`, and `Flush Immediately`.
The trailing delivery pins are advanced and default to the legacy behavior.
`Wait Until Conversation Is Idle` prevents the outcome from interrupting active
speech; pairing it with `Flush Immediately` releases at the first idle moment
instead of waiting through the normal quiet-settle window. You can push an outcome into the bot's context in the
same call — useful for narration. Either let `b Auto Report` generate a default
message and add a note, or turn it off and supply your own text:

```
Handle Action Completion(Is Successful=true, Additional Note="picked the red one", Should Respond=Auto)
  → sends "you were able to Print successfully, note: picked the red one"
```

## Custom movement without duplicating the resolver

For a custom action whose parameter is an `FConvaiObjectEntry`, call the
asynchronous **`Convai Move To`** node with the pawn to move and that entry. It
selects the Convai goal, validates reachability and arrival, tracks a moving
whole actor, refreshes component/socket and movement-point location targets,
and performs one bounded retry after an eligible engine failure when a fresh
resolve still says the destination is reachable. Cancellation or pre-emption by
another movement owner ends the request instead of competing with that owner.

The node exposes a **Move Request** proxy. Save it on your Blueprint if
`Cancel <ActionName>` should stop the move, then call **Cancel** on that proxy.
The node has only **Succeeded** and **Failed** terminal pins. Each returns a
result code and player-safe `Additional Note`; developer diagnostics stay in
native logs.

`Convai Move To` intentionally does not own the action queue. After your
gameplay cleanup, call `Handle Action Completion` from the terminal pin. In the
separate cancellation handler, call `Handle Action Cancellation` only when
**Cancellation Succeeded** is true. False means a natural terminal pin already
owns acknowledgment. Repeated `Cancel` calls are safe and return false after
the first winning call.

## Optional cancellation handler for long-running actions

If `Print` were a long-running operation, you could let the user redirect the
character without waiting for it to finish. Enable the experimental **Cancel
Action Plan** built-in on the chatbot, then add a second Custom Event with this
exact name:

```
Cancel Print
```

The convention is always **`Cancel <ActionName>`**, preserving spaces and the
canonical action name exactly. The event receives the original
`FConvaiResultAction` and is invoked on the same Blueprint object that handled
the original action. It is optional: short or non-cancellable actions need no
extra event.

Use this event only to request cleanup—cancel a timer, stop an async operation,
or signal a Gameplay Task. Then acknowledge the old action through exactly one
terminal path:

- If stopping the operation triggers its existing completion callback, let
  that callback call `Handle Action Completion`; **do not** also acknowledge
  cancellation.
- If cleanup finishes without the normal callback, call `Handle Action
  Cancellation` only after the operation is quiescent and cannot call back
  later.

The replacement plan waits behind the old active action until that terminal
acknowledgment arrives. Repeated player redirects do not call `Cancel Print`
again for the same execution; they only replace the pending plan, and the most
recent replacement wins. There is no execution-ID pin to wire.

If normal completion wins the race, a successful auto-report or additional
note is retained in context silently because it may describe a real state
change. Cancellation-related failure prose is suppressed so the character does
not apologize, retry the abandoned plan, or fight the user's redirection.

The stock `Escort {character: ref} to {destination: ref}` row uses this
contract. It is disabled by default; its ordinary **Enabled** checkbox controls
whether it is advertised at the next connection. The row only advertises the
schema. An exact named `Escort` Blueprint handler resolves the `character`
reference from `Environment.Characters`, calls **Convai Escort**, and handles
its `Succeeded` or `Failed` pin. The reusable Escort task uses full 3D catch-up
distance, composes Convai Move To, treats the resolver as the arrival authority,
and withdraws an undelivered follow prompt during teardown.

Keep Escort's description grounded in the current surroundings: discuss an
already-reached destination directly, use only a brief transition when it is
close by, and reserve follow-me language for real travel. On `Succeeded`,
**Reached** normally completes with an idle-delivered, immediately flushed
`Always` response; **Already At Destination** normally completes with `Never`
so the current discussion is not repeated.

For cooperative cancellation, save **Escort Request** and implement the exact
event **`Cancel Escort`**. Call its `Cancel` function, then call
`Handle Action Cancellation` only when **Cancellation Succeeded** is true.
There is no native action fallback and no `Cancelled` terminal pin.

Enabling the Escort row does not enable `Cancel Action Plan`; that experimental
control remains a separate opt-in.

A transient server disconnect is lifecycle cleanup rather than a model-issued
cancel: the plugin silently retires owned action work and clears the plan, then
allows new work after reconnect. `Stop Session` is terminal for that session;
`End Play`/destruction is terminal for the component. These paths suppress stale
callbacks instead of feeding cancellation prose back to the character.

## When things go wrong

If your handler can't recover (the target is gone, preconditions failed),
don't retry forever — **abort the whole sequence** and let the LLM replan:

```
Abort Action Sequence(EventText="Couldn't print — screen is off", ShouldRespond=Always)
```

This clears the rest of the queued actions and fires a context event so the
LLM acknowledges and likely emits a new action plan on its next turn.

`Abort Action Sequence` is for an unrecoverable gameplay failure. Do not use it
for an otherwise-valid action that the user merely redirected; use `Cancel
Action Plan` / `Cancel Current Action Plan` so cleanup completes before any
replacement starts. `Clear Action Queue` is lower-level administrative pruning:
it drops pending/held work but deliberately leaves a dispatched head waiting for
its real terminal callback; it does not replace either semantic outcome.

## Recap

You added one action template, wrote one handler, and got the bot doing
custom things. The next step is **typed parameters** — telling the LLM that
an action takes a number, an actor reference, or one-of-N choices. Onward to
[Phase 3 — Parameterized Actions](./Actions_Phase3_ParameterizedActions.md).
