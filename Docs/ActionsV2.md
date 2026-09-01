# Actions V2 — Typed Templates & Unified Result

This is the overhauled action system that replaces the legacy
`RelatedObjectOrCharacter` + `ConvaiExtraParams` shape on `FConvaiResultAction`
and the bare `TArray<FString>` action templates. New code should use this guide;
the legacy API is still mirrored for back-compat (see the migration table at the
end).

> **Looking for a walkthrough rather than a reference?** The
> [GitBook/](./GitBook/) folder has a step-by-step guide split into
> [Overview](./GitBook/Actions_Overview.md) →
> [Phase 1: Default Actions](./GitBook/Actions_Phase1_DefaultActions.md) →
> [Phase 2: Custom Actions](./GitBook/Actions_Phase2_CustomActions.md) →
> [Phase 3: Parameterized Actions](./GitBook/Actions_Phase3_ParameterizedActions.md).

For Environment / DynamicContext / scene-metadata semantics that are *not*
specific to actions, see [ActionsAndEnvironment.md](./ActionsAndEnvironment.md).

---

## TL;DR

1. **Author actions as structured `FConvaiAction` templates** in the Details panel.
   Each action has an `Enabled` switch, a `Name`, optional `Description`, and an
   ordered list of `FConvaiActionParam` entries.
2. **Each parameter has a `Type`** (`Auto`/`Reference`/`String`/`Number`/`Bool`/`Enum`),
   optional `Description`, optional `Connector` (joining text), optional `Choices`
   (or an `EnumType` for `Enum`-typed params).
3. **You author actions purely through the structured fields.** The wire-format
   string is generated for you (kept in sync internally) — you don't edit it.
4. **Action results land in `FConvaiResultAction.Parameters`** — a
   `TMap<FString, FConvaiResultParam>` keyed by placeholder name. Each
   `FConvaiResultParam` always populates every value field (`StringValue`,
   `NumberValue`, `BoolValue`, `RefValue`) best-effort; `Type` indicates the
   intended slot.
5. **BP accessors** (`Get First Param`, `Get Param`, `Get Param As Ref/Number/...`)
   give clean reads. Legacy `RelatedObjectOrCharacter` / `ConvaiExtraParams` are
   kept as deprecated mirror fields populated from the new `Parameters` map.

## The template: `FConvaiAction`

```cpp
USTRUCT(BlueprintType)
struct FConvaiAction
{
    bool bEnabled = true;                      // disabled templates stay authored but are not advertised
    FString Name;                              // canonical, no placeholders ("Parse")
    FString Description;                       // optional, surfaced to the LLM
    TArray<FConvaiActionParam> Parameters;     // ordered, positional
    FString RenderedString;                    // generated wire format, kept in sync; hidden from editor/BP

    // Speech timing — control WHEN the action runs relative to the bot's speech.
    bool  bWaitForBotSpeech = false;           // defer the (first) action until the
                                               // character has spoken (OnStarted/FinishedTalking)
    float DelayAfterBotSpeechSec = 0.0f;       // extra delay after the speech condition resolves
                                               // (EditCondition: bWaitForBotSpeech)
};
```

Action identity is case-insensitive for replacement, contract shadowing, and
dispatch. Only **enabled**, non-empty designer actions enter the connect-time
contract and frozen parser snapshot. Every authored non-empty name still
reserves its identity, however, so a disabled designer row suppresses a
same-named native built-in rather than silently activating it. Toggle/name edits
apply on the next fresh connection.

`bWaitForBotSpeech` / `DelayAfterBotSpeechSec` let an action hold until the
character actually speaks — useful for gestures/animations that should line up
with dialogue rather than firing the instant the sequence arrives. Both are
copied onto the parsed `FConvaiResultAction` (below) so your handler and the
action queue can honour them.

## The parameter: `FConvaiActionParam`

```cpp
USTRUCT(BlueprintType)
struct FConvaiActionParam
{
    FString Name;                              // placeholder, e.g. "email"
    FString Description;                       // optional human-language description
    EConvaiActionParamType Type = Auto;        // see matrix below
    FString Connector;                         // optional join word ("on", "to")
    TArray<FString> Choices;                   // optional fixed-choice constraint
    TObjectPtr<UEnum> EnumType;                // required when Type == Enum
};
```

## Type matrix

| Type        | Wire hint     | Parser behavior                                                                  | When to use |
|-------------|---------------|----------------------------------------------------------------------------------|-------------|
| `Auto`      | (no hint)     | Infer at parse time: try Reference, then Number, then Bool; fall back to String. | Default. Most flexible. Pairs well with the always-best-effort coercion below. |
| `Reference` | `: ref`       | Resolve against `Environment.Objects` then `.Characters`.                        | Game actions whose target is an actor/object in the scene. |
| `String`    | `: string`    | Treat as string. (Other fields still coerced best-effort.)                       | Free-form text (names, descriptions, sentences). |
| `Number`    | `: number`    | `Atof(value)`.                                                                   | Durations, counts, scores. |
| `Bool`      | `: bool`      | `true` for "true"/"yes"/"1"; `false` otherwise.                                  | On/off toggles. |
| `Enum`      | `: enum`      | Render `EnumType`'s values as the `Choices` block; logs warning if `EnumType` is unset. | Constrained to a fixed set you've already declared as a `UENUM`. |

> **All value fields are populated best-effort regardless of declared `Type`.**
> A `String`-typed param whose value happens to match an Object in the env will
> still have `RefValue` set. Designers can read whichever interpretation suits.

## Wire format

Sent in `action_config.actions[]` as a single string per template. Build rules:

1. `<Name>`
2. For each param:
   - If `Connector` non-empty, append ` <Connector>`.
   - Append ` {Param.Name`.
   - **Choices/Enum**: when `Type == Enum`, derive choices from `EnumType`
     (skipping `_MAX` and `Hidden` values). Otherwise use the manual `Choices`
     array. If non-empty, append ` [c1|c2|...]`.
   - When `Type == Enum && EnumType == nullptr`, append ` [ERROR: EnumType not set]`.
   - If `Type != Auto`, append `: <type>` (the words `ref`/`string`/`number`/`bool`/`enum`). Only `Auto` elides.
   - Append `}`.
3. Description tail: ` — ` + action description + per-param sentences (`<param>: <desc>`).
   Empty descriptions are skipped — no dangling em-dash, no trailing period.

### Examples

| Template (struct form)                                                         | Generated string |
|---------------------------------------------------------------------------------|------------------|
| `Move To` (no params)                                                           | `Move To — Move the character to a target location.` |
| `Wait For` + `[{time in seconds, Number}]`                                      | `Wait For {time in seconds: number} — Wait for a duration. time in seconds: How long to wait, in seconds.` |
| `Pick Up` + `[{item, Reference}]`                                               | `Pick Up {item: ref} — Pick up an item.` |
| `Put` + `[{ball, Reference}, {table, Reference, Connector="on"}]`               | `Put {ball: ref} on {table: ref} — Place one object on another.` |
| `Set Mood` + `[{mood, String, Choices=[happy,sad,angry]}]`                      | `Set Mood {mood [happy\|sad\|angry]: string} — Set the bot's mood.` |
| `Set Mood` + `[{mood, Enum, EnumType=EBotMood{Happy,Sad,Angry}}]`               | `Set Mood {mood [Happy\|Sad\|Angry]: enum} — Set the bot's mood.` |
| `Activate` + `[{thing, Auto}]`                                                  | `Activate {thing} — Activate something.` |

The `{placeholder}` syntax (curly braces — the universal format-string convention
across Python, Jinja, and Mustache) gives the LLM the strongest possible "fill
this slot" signal and teaches it to wrap response values in braces too —
`Parse {MMA2} {537}` — so the parser can split unambiguously even when values
contain spaces. The parser also accepts legacy `"quoted"` responses and a
whitespace fallback when the LLM doesn't wrap at all. For multi-parameter
actions, an object-like named response such as
`{character: "User", destination: "Gallery"}` is also accepted; the outer brace
pair is a map wrapper, not the first positional value.

## The rendered wire-format string

`FConvaiAction.RenderedString` holds the wire format actually sent to the LLM. It
is kept in sync with the structured fields (Name / Description / Parameters) via
the chatbot's `PostEditChangeProperty` / `PostLoad`, but it is a plain
`UPROPERTY()` — **hidden from the editor and Blueprint**. Author actions through
the structured fields; the string is an implementation detail you don't edit.
(`FConvaiAction::ToActionConfigString` renders it; `ParseFromActionConfigString`
exists for round-tripping but isn't a designer-facing editable text box.)

## The result: `FConvaiResultAction`

```cpp
USTRUCT(BlueprintType)
struct FConvaiResultAction
{
    FString Action;                                    // canonical name post-template-match
    FString ActionString;                              // raw incoming
    TMap<FString, FConvaiResultParam> Parameters;      // by placeholder name

    bool  bWaitForBotSpeech = false;                   // copied from the matched template
    float DelayAfterBotSpeechSec = 0.0f;               // copied from the matched template

    // DEPRECATED — back-compat mirrors. Don't use in new code.
    FConvaiObjectEntry RelatedObjectOrCharacter;
    FConvaiExtraParams ConvaiExtraParams;
};

USTRUCT(BlueprintType)
struct FConvaiResultParam
{
    EConvaiActionParamType Type;        // declared / inferred
    FString StringValue;                // raw value, always set
    float NumberValue;                  // Atof, always attempted
    bool BoolValue;                     // true/yes/1, always attempted
    FConvaiObjectEntry RefValue;        // env lookup, always attempted
    uint8 ByteValue;                    // matched enum index (Type==Enum + EnumType set); 0 otherwise
};
```

### Parser flow

For each incoming `{name, target?}`:

1. **Match a template** from the frozen connect-time action contract — enabled
   designer actions plus unshadowed built-ins only — via a Levenshtein-scored
   prefix match. Disabled actions are not parser candidates.
2. **If matched**:
   - Set `Action` to the template's canonical `Name`.
   - Combine `name leftover + target` (LLM can put values in either).
   - For each declared param: `CoerceParam(value, declaredType, env)` and store
     under the placeholder name.
   - Validate against `Choices` (logs warning on miss; doesn't drop the value).
3. **If matched but template has no declared params** + a value is present →
   surface under `Parameters["target"]` so handlers always see *something*.
4. **If no template matched** + a target is present → same fallback under
   `Parameters["target"]`. `Action` stays as the raw incoming name so handlers
   can still string-dispatch.
5. **Populate deprecated mirrors** (`RelatedObjectOrCharacter`,
   `ConvaiExtraParams.Number`, `.Text`, `.NamedParams`) from `Parameters` so
   pre-existing handlers keep working.

## BP accessors

```
Get First Param(ResultAction) → FConvaiResultParam
Get Param(ResultAction, Name) → FConvaiResultParam
Get Param Type(ResultAction, Name) → EConvaiActionParamType
Get Param As String(ResultAction, Name) → FString
Get Param As Number(ResultAction, Name) → float
Get Param As Bool(ResultAction, Name) → bool
Get Param As Ref(ResultAction, Name) → FConvaiObjectEntry
Get Param As Byte(ResultAction, Name) → uint8   // enum index; convert with Byte-to-Enum<YourType>
Has Param(ResultAction, Name) → bool
```

For an `Enum`-typed param, `Get Param As Byte` returns the matched enum index
(the parser fuzzy-matches the LLM's value against the enum's display names, so
minor spelling drift still resolves). Feed it into a **Byte to Enum** node for
your `UENUM`. `Get Param As String` still returns the raw display name if you'd
rather switch on text.

## Built-in experimental actions

`Remind Self` and `Watch Property` are two optional utility actions, off by
default. Enable them per
chatbot in the Details panel under **Convai ▸ Actions ▸ Experimental**; they
take effect on the **next session start** (like every action mutation) and
require `Enable Actions`. They never appear in the Environment's `Actions`
array — the plugin injects them into the connect-time `action_config` and
handles them in C++ inside the chatbot component. Action-name identity is
case-insensitive. Any same-named designer row reserves and suppresses the
built-in contract entry, even while disabled. A Blueprint function or custom event
matching an advertised built-in (on the owning actor or chatbot subclass) still
overrides its C++ handler.

When either built-in is advertised, the chatbot also receives one concise,
silent declarative hint explaining when to use it. Developers should not repeat
those instructions in the character's backstory.

### `Remind Self {reminder}`

Lets the AI queue a note to itself **after other actions** — the action queue
is sequential, so the reminder fires exactly when the previous action
completes. That turns "speak when you arrive" into a plan the AI can make
itself:

```
User: "Show me the Mona Lisa."
AI emits:  [Move To MonaLisa, Remind Self "describe the painting to the visitor"]
```

When `Move To` finishes (wherever your movement handler calls
`Handle Action Completion`), the reminder delivers a one-shot context event —
`Reminder to yourself: <reminder>` — with `ShouldRespond = Always` and
`Delivery = Wait Until Conversation Is Idle`, so the character responds on
arrival but waits out anyone mid-conversation, however long that takes (the
next genuine pause, per the chatbot's `Quiet Time Before Delivery`). Composes
with `Wait For` as a timed alarm:
`[Wait For 30, Remind Self "check on the visitor"]`.

Caveat: a **new** action sequence arriving mid-execution replaces the queued
tail (standard replanning semantics), silently dropping a not-yet-fired
reminder — the drop is logged.

### `Watch Property {property}`

The same one-parameter action also supports an exact target value: pass
`Platform.Movement = Stopped` to wait until a later change reaches that value.
A bare key remains fully backward compatible and means the next genuine change.

Lets the AI arm a **one-shot watch** on a tracked property so it is notified on
the next matching genuine change — even when the property's `ShouldRespond` is
`Never`/`Auto`. For puzzles and reactive scenes: a platform arriving, a door
unlocking.

- The parameter is the property's **context key** exactly as the AI already
  sees it in its context states (e.g. `FrontDoor.DoorState`). Close-but-not-
  exact names resolve via a conservative fuzzy match; ambiguous or unknown
  names FAIL the action with a report so the AI can retry with the exact key.
- Append ` = exact value` when only that value should satisfy the watch. On
  the next matching genuine value change, that one update is upgraded to
  `ShouldRespond = Always` and the watch disarms. If that value is already
  current, the watch does not fire immediately: it waits for a genuine
  departure and a later return to the target. Re-arming is idempotent.
- Watches obey the property's batching policy. A property authored as `Never`
  is canonicalized to `Delivery = Send Normally` and `Flush Immediately =
  false`, so a watched change on that property uses normal debounced delivery.
  `Auto`/`Always` properties retain their configured Delivery/Flush behavior.
- Watches clear on `Reset Dynamic Context` and session stop, and survive
  transient reconnects.

## Reusable movement: `Convai Move To`

The asynchronous **Convai Move To** Blueprint node gives custom actions the
same movement policy used by Escort without copying that policy into another
graph. Its primary inputs are:

- **Moving Actor** — the actor that should move. It must resolve to a Pawn with
  the required AI movement setup.
- **Destination** — the `FConvaiObjectEntry` to resolve and follow.
- **Lock AI Logic** — an advanced opt-in; it defaults to `false` so unrelated
  AI logic is not unexpectedly paused.

The node exposes a **Move Request** proxy and two terminal pins: `Succeeded`
and `Failed`. Both return a result code and concise, player-safe
`Additional Note`; developer diagnostics stay in native logs.

Move Request and Escort Request share one cancellable-request lifecycle. Their
inherited `Cancel` function returns **Cancellation Succeeded**, while each node
retains its own typed results and movement policy.

Whole-actor targets remain actor-tracked. Component, socket, authored movement
point, and other retained-location targets are periodically re-resolved so a
moving anchor does not leave the request heading toward stale coordinates. One
bounded retry is allowed only for eligible movement failures. Unexpected task
termination reaches `Failed`, so the surrounding action cannot remain waiting.

The node deliberately does **not** call `Handle Action Completion` or
`Handle Action Cancellation`. The action handler owns the terminal
acknowledgment:

- On `Succeeded` or `Failed`, call `Handle Action Completion` once.
- To support cancellation, save **Move Request**. In the separate
  `Cancel <ActionName>` handler, call its `Cancel` function.
- If **Cancellation Succeeded** is `true`, the request stopped silently; clear
  the saved reference and call `Handle Action Cancellation`.
- If it is `false`, natural completion already finished or is queued. Do
  nothing and let the normal terminal pin acknowledge the action.

Repeated `Cancel` calls are safe; only the first winning call returns `true`.
The existing stock Blueprint `Move To` and `Follow` graphs remain unchanged.

## `Escort`

Guides another character to an object or named destination while keeping that
character nearby. `Escort` is a stock entry in `Environment.Actions`, disabled
by default. Its **Enabled** checkbox controls whether it joins the next
connection's advertised and frozen action contract. The stock row enables
**Wait for Bot Speech**, so an Escort arriving as the first action of a fresh
sequence waits for the character's speech gate before movement begins. Keep its
canonical template with two `Reference` parameters in this order:

- **`character`** — the character who should follow the guide. Its wire type
  remains the general `Reference` type for compatibility. Resolve it
  semantically and case-insensitively from `Environment.Characters`, so a
  same-named object cannot win.
- **`destination`** — the world object or separately named movement-point
  destination to reach. Its connector is **`to`**, producing the rendered form
  `Escort {character: ref} to {destination: ref}`.

Keep the action description grounded in the current surroundings. If the
destination fact says the character is **already at** it, address the
destination directly without follow-me or future-arrival wording. If it is
**close by**, use only a brief transition. Reserve an invitation to follow for
a destination that genuinely requires travel. This is generic movement
guidance rather than tour-guide-specific dialogue.

The `character` parameter is the escortee; it is **not** the pawn that moves.
On **Convai Escort**, connect that pawn to **Escorting Actor** and the resolved
`character` entry to **Escorted Character**. Escorting Actor must own a Convai
Chatbot Component and be controlled by an AI Controller. Pass the complete
escorted-character and destination entries; the node preserves movement points,
acceptance radius, merge resolution, reachability, and named destinations.

Before starting or resuming movement, the Escort task composes
`UConvaiMoveToTask`. The shared task refreshes `Resolve Goal Location` and
treats that resolver's reachability and arrival result as authoritative. For a
retained-location goal it consumes the resolver's **Destination** and
**Acceptance Radius**, matching the convenience Blueprint macro. Being inside
Convai's arrival policy counts as success even if the engine reported failure,
while an engine success outside that policy does not.

If the escortee remains too far behind, the guide pauses its owned movement,
faces the escortee, and queues one ephemeral “please follow” prompt using `Wait
Until Conversation Is Idle`, so it does not interrupt an ongoing exchange. It
waits rather than starting another plan, restores its previous focus, and
resumes toward the freshly resolved destination after the escortee catches up.
Lag/catch-up thresholds use full **3D** guide-to-escortee distance. A distant
escortee who is clearly ahead inside the remaining ordered navigation corridor
does not trigger that pause, while proximity to the route alone cannot make an
escortee behind on the same long segment count as ahead. Arrival remains strict:
it completes only when the guide is at the destination and the escortee is
nearby.
If the task ends while its idle-delivered follow prompt is still held, teardown
withdraws that undelivered ephemeral prompt so it cannot speak after the escort
has completed, failed, or been cancelled.

The task returns `Succeeded` or `Failed` with a result code and player-safe
`Additional Note`; internal setup details remain in logs. A successful request
preserves whether movement was necessary:

- **Reached** means Escort travelled to the destination. A typical handler
  completes with auto-reporting enabled, **Should Respond = Always**,
  **Delivery = Wait Until Conversation Is Idle**, **Ephemeral = true**, and
  **Flush Immediately = true**, so the character acknowledges arrival at the
  first quiet moment.
- **Already At Destination** means the shared resolver found the guide there
  before Escort began. Complete without forcing another response (normally
  **Should Respond = Never**) because the action description already tells the
  model to address that destination in its current line.

The node does not dispatch, complete, or cancel the Convai action. Create an
exact named `Escort` Blueprint handler, invoke **Convai Escort**, and choose the
policy above when calling `Handle Action Completion`. Escort adds no separate
arrival event, so the Blueprint completion remains the sole post-action
acknowledgment.

For cooperative cancellation, save **Escort Request**. An exact `Cancel Escort`
handler calls its `Cancel` function and calls `Handle Action Cancellation` only
when **Cancellation Succeeded** is `true`. A `false` result means natural
completion owns acknowledgment. Explicit cancellation emits no arrival or
failure pin.

`Cancel Action Plan` is independent. Enabling `Escort` does not enable or
advertise that experimental control. The Environment row only advertises the
action schema; it does not install a native action fallback.

## `Cancel Action Plan`

This experimental built-in is **off by default**. Opt in per chatbot with
**Convai > Actions > Experimental > Enable Cancel Action Plan**; like other
action-contract changes, it takes effect on the next session start and still
requires `Enable Actions`.

It lets the AI abandon an in-progress sequential plan before replacing it. When
the plugin built-in is advertised (not reserved by any same-named designer row,
matched case-insensitively), this is a reserved control
action: it has no parameters, never enters the ordinary action queue, and is not
delivered as an ordinary action event. Enable it when players can redirect a
character while a long-running action is still active. An enabled project-owned
`Cancel Action Plan` action retains ordinary custom-action semantics instead; a
same-named handler by itself does not change the frozen contract, while a
disabled same-named row suppresses the control without advertising a replacement.

The marker partitions each received sequence. Everything through the **last**
`Cancel Action Plan` marker is discarded and only the actions after it become
the replacement plan. Using the last marker makes malformed sequences with
more than one marker deterministic.

- If nothing has been dispatched yet, the plugin invalidates any speech/delay
  wait and starts the replacement immediately.
- If an action is already running, that active head remains as a private
  tombstone. The replacement is held separately and cannot start until the old
  action reports that it is fully finished.
- A second cancel while the same action is stopping does not call its cancel
  hook twice. It only replaces the held plan; the latest replacement wins.

There is deliberately no public execution ID. Cancellation stays compatible
with existing Blueprint handlers through an optional naming convention:

```
Action handler:        <ActionName>          // for example, Move To
Cancellation handler: Cancel <ActionName>   // for example, Cancel Move To
```

The cancellation handler receives the original `FConvaiResultAction` and is
looked up on the same object that handled the action. It should request cleanup
and then let the original terminal callback run, or call **Handle Action
Cancellation** after the operation is completely quiescent. Call exactly one
terminal API. In particular, if stopping an async operation causes its normal
completion callback to fire, do not also call `Handle Action Cancellation`.

While cancellation is pending:

- `Handle Action Cancellation` silently acknowledges an aborted action and
  releases the replacement plan.
- A successful `Handle Action Completion` keeps any auto-report/additional note
  in context because success can represent an important state change, but it
  cannot force a response or delay the replacement.
- A failed `Handle Action Completion` is treated as cancellation rather than a
  gameplay failure. Generated retry/failure prose is suppressed; an explicit
  note may still be retained silently.

If no cancellation handler exists, the plugin still waits for the action's
ordinary terminal callback. A diagnostic timeout may report the stuck state,
but it never starts the replacement speculatively: without an execution ID, a
late callback could otherwise complete the wrong action.

## Action-plan lifecycle and terminal APIs

Sequential actions run one at a time. Every dispatched custom handler must
eventually make exactly one terminal call:

| API | Meaning |
|-----|---------|
| `Handle Action Completion` | The current action finished normally. Success advances; an ordinary failure abandons the queued tail and reports according to the action's response settings. During cancellation, the special reporting rules above apply. |
| `Handle Action Cancellation` | The current action has stopped and cannot emit any more completion callbacks. This is the preferred silent acknowledgment for a cooperative cancel. |
| `Cancel Current Action Plan` | Request cancellation through the same barrier used by the built-in marker, with an empty replacement plan. It invokes an available `Cancel <ActionName>` handler once and waits for a terminal acknowledgment. |
| `Abort Action Sequence` | Fail the active plan immediately from inside gameplay logic, invoke its cancel hook once, stop plugin-owned action work, clear both its queued tail and any held replacement, and optionally report only the supplied reason. Use for an unrecoverable action failure, not a user redirect. |
| `Clear Action Queue` | Administrative queue pruning. It removes the queued tail and held replacement (and cancels a pending start when nothing is running), but it does not pretend a dispatched action stopped: the active head remains until its terminal callback. It is not a conversational cancellation request or gameplay outcome. |

A transient server disconnect cancels component-owned action tasks and clears
the active/held plan without reporting an outcome, but it does **not**
permanently shut down the executor; a reconnected session can accept new work.
An explicit `Stop Session` is terminal for that session: it also retires armed
watches, cancels component-owned work, invokes an available custom cancel hook,
and rejects callbacks already posted across the session boundary. A custom
latent operation must still quiesce itself when its `Cancel <ActionName>` hook
runs; without a public execution ID, a non-cooperative callback arriving after a
new action has started is fundamentally indistinguishable. A later explicit
`Start Session` opens a new lifecycle, whereas `End Play`/destruction is terminal
for the component.

The chatbot also maintains two derived context sections; applications do not
append these as events or facts:

```
Instructions:
- After an action that needs speech or follow-up, queue Remind Self immediately after it.
- Use Watch Property when the next change to a tracked property should wake you.
- If the user redirects or abandons a plan, place Cancel Action Plan before the replacement actions and acknowledge the change in the same response.

Current action plan:
- Currently doing action "Move To Platform".
- Next action: "Remind Self describe the platform".
- Additional queued actions: 1.
```

Only enabled built-ins contribute instructions. `Current action plan` is
omitted while idle and remains compact (generic current status, immediate next
action, and a remaining count rather than the full queue). Its invocation is
rendered from the canonical action template and ordered parameter values, so
custom verbs require no plugin-specific wording. These blocks replace in place
on authoritative queue transitions, so they do not grow the prompt over time.
Resetting dynamic context clears tracked world data but silently republishes
these session-derived blocks when they still apply.

## Migration from the legacy fields

| Old usage in handlers                                  | New replacement |
|--------------------------------------------------------|-----------------|
| `Action.RelatedObjectOrCharacter.Ref`                  | `Get Param As Ref(Action, "destination").Ref` (or whatever your placeholder is named — first Reference param via `Get First Param` works too). |
| `Action.RelatedObjectOrCharacter.Name`                 | Same as above; `.Name` field. |
| `Action.ConvaiExtraParams.Number`                      | `Get Param As Number(Action, "<param-name>")`. |
| `Action.ConvaiExtraParams.Text`                        | `Get Param As String(Action, "<param-name>")`. |
| `Get Action Param(ExtraParams, Name)` — string lookup  | `Get Param As String(Action, Name)`. |
| `Get Action Param As Number(ExtraParams, Name)`        | `Get Param As Number(Action, Name)`. |
| `Has Action Param(ExtraParams, Name)`                  | `Has Param(Action, Name)`. |

The deprecated fields are still populated so existing handlers keep working —
they just emit deprecation warnings at BP compile time.

## Compound / connector example

Modeling `Put ball on table`:

```cpp
FConvaiAction Put(TEXT("Put"), TEXT("Place one object on another"), {
    FConvaiActionParam(TEXT("ball"),  TEXT("What to pick up"),       EConvaiActionParamType::Reference),
    FConvaiActionParam(TEXT("table"), TEXT("Where to place it"),     EConvaiActionParamType::Reference),
});
Put.Parameters[1].Connector = TEXT("on");
```

Renders to: `Put {ball: ref} on {table: ref} — Place one object on another. ball: What to pick up. table: Where to place it.`

Server returns `{name: "Put {ball} on {table}"}` → parser populates
`Parameters["ball"].RefValue` (the cube actor) and `Parameters["table"].RefValue`
(the table actor).

## Enum example

```cpp
UENUM(BlueprintType)
enum class EBotMood : uint8 { Happy, Sad, Angry };

FConvaiActionParam Mood(TEXT("mood"), TEXT("How the bot should feel"));
Mood.Type = EConvaiActionParamType::Enum;
Mood.EnumType = StaticEnum<EBotMood>();
```

Renders to: `Set Mood {mood [Happy|Sad|Angry]: enum} — ...`

In the Details panel, the `Choices` field auto-hides (it's derived from
`EnumType`); the `EnumType` picker shows up. Misconfiguring (Type=Enum but no
EnumType) embeds an `[ERROR: EnumType not set]` token in the generated wire
string, so the broken contract is caught in logs / when inspecting what's sent
rather than silently shipping a bad prompt.

## The object entry: `FConvaiObjectEntry`

A `Reference`-typed param resolves to an `FConvaiObjectEntry` — the same struct
used for `Environment.Objects` / `.Characters` and the attention slot. Beyond
`Name` / `Description` / `Ref`, it carries fields that control both **what the
object is** (gaze / attention / vision scope) and **where a character stands**
when moving to it:

| Field | Type | Meaning |
|-------|------|---------|
| `ObjectReference` (shown as **Object Is**) | `EConvaiObjectReference` | What the object *is*: `WholeActor` or `SpecificComponent`. Drives gaze/attention scope and the movement fallback when no Movement Points are authored. |
| `MovementPoints` | `TArray<FConvaiMovementPoint>` | Designer-authored **standing spots** — where a character stands when walking to this object (e.g. one per side of a door). The resolver picks the reachable point with the shortest walking path. Empty = walk to the object reference itself. |
| `bFallbackToObjectWhenPointsUnreachable` (shown as **Use Object as Fallback**) | `bool` = false | *Advanced.* When points are authored but **all** are unreachable, fall back to targeting the object reference instead of reporting it unreachable. Off = an authored-but-blocked object reads as unreachable. |
| `AcceptanceRadius` | `float` = 150 | How close (cm) counts as arrived. Floored at 150 for movement-point arrival/reachability (smaller values only tighten the engine's AI Move To stop distance); object-body arrival tests roughly twice this radius. Larger values suit vehicles and wide objects. |
| `ComponentName` | `FString` | *Specific Component.* Case-insensitive substring match scoping the object to a sub-component of `Ref` (e.g. `"muzzle"`). Empty = Ref's origin. |
| `SocketOrBoneName` | `FName` | *Specific Component.* Socket/bone on the matched component; falls back to the component origin if not found. |
| `ResolvedComponent` | `USceneComponent*` | *Output only.* The component Convai resolved from `ComponentName` at receive time; read it from BP for component-specific work. |

### Movement Points — `FConvaiMovementPoint`

Each authored point is pure data. While the owning actor is selected (or the
component, in the Blueprint editor), every point draws as a grab handle in the
viewport — a pin with its acceptance-radius ring and an `[index] Name` label,
so you can tell which array element is which while editing in Details — click
it and move it with the transform gizmo (undo-aware; Blueprint-preview edits
propagate to instances); relative points ride their anchor live. Flipping
`Attachment` keeps the point where it is: the stored transform is rebased into
the new space automatically.

| Field | Type | Meaning |
|-------|------|---------|
| `Transform` | `FTransform` | Where the character stands when this point is chosen. |
| `Attachment` | `EConvaiMovementPointAttachment` | `RelativeToObject` (default — travels with the object) or `KeepWorldPosition` (nailed to the world — right for an elevator's floor landings). Switching converts the stored position, so the point never jumps. |
| `bEnabled` | `bool` = true | Untick to take the point out of play without deleting it. |
| `bCreatesSeparateDestination` | `bool` = false | *Create Separate Destination.* Off: the point is one of the places to stand for the object itself. On: the point becomes its own AI destination, named below. |
| `Name` | `FString` | *Destination Name* — what the separate destination is called; disabled until the checkbox above is on. |

### Separate destinations

By default every point simply belongs to the object: `go to Door` walks to the
nearest reachable one. Tick **Create Separate Destination** on a point and it
splits off as its own AI-addressable target, named
`"<Object> <Destination Name>"`. The motivating case is a door between two
rooms: author a point on each side, make the far one a destination named
`Other Side`, and the AI can now be told — and can decide — to *"go to Door
Other Side"* instead of stopping at the near side. Each destination gets its
own spatial fact, distance/direction, and navmesh reachability (a reachable
side says `reachable by walking`; a blocked far side says there is no walking
path; one already reached says the character is already there instead of
calling it reachable), and appears in the debug overlay like any other object.

Rules:

- The checkbox is an explicit opt-in: **a ticked, named point always becomes a
  destination; everything else always stays with the object.** No naming
  heuristics — what you see in the panel is what the AI gets.
- The elevator pattern: the point ON the platform stays unticked (it IS the
  platform — `go to the platform` resolves there), while the landings become
  world-fixed destinations named `Lower Landing` and `Upper Landing` —
  addressable as `Moving Platform Lower Landing` and `Moving Platform Upper
  Landing`. “Landing” makes each one sound like a place beside the platform,
  not another platform or a movable part of it. Generated destination metadata
  reinforces that distinction: world-fixed points are described as fixed
  standing locations for accessing the base object, while relative points are
  described as standing locations that move with it; both explicitly say they
  are not the base object itself. Configure the deck point as **Relative To
  Object** with **Create Separate Destination** off, and configure both landing
  points as **Keep World Position** with **Create Separate Destination** on.
- When *every* point is a destination, the base object keeps the object body
  as its movement target — `go to Door` means the door, `go to Door Other
  Side` means the side.
- A destination never falls back to the object body when its points are
  unreachable — that would defeat the point of separating it.
- Destination names are whitespace-normalized and case-insensitive for
  identity (`other side` and `Other  Side` are the same destination; the first
  occurrence's casing is displayed). Keep them speakable — the AI must say
  them back.
- Merged sets group destinations by name across the whole set. At action
  dispatch, goal resolution evaluates the live concrete members and chooses
  the nearest reachable member independently of spatial-awareness polling.
  The logical centroid is descriptive context, never the movement goal.
- Destinations are **authoring-time identity**. A runtime rename applies in
  stages: prompt facts pick it up on the next poll, the action-addressable
  entry updates when the object re-registers (disable/enable or respawn),
  and names that were sent in the connect-time action config refresh on the
  next reconnect.
- If a generated name collides with a real object's name (`Door` + destination
  `Key` vs an object named `Door Key`), the destination is skipped with a
  warning — rename one of them.

### `Resolve Goal Location`

Rather than reading these fields by hand, call the BP node **`Resolve Goal
Location`** — it turns an entry into the exact inputs an `AI Move To` node needs,
plus optional navmesh-reachability data:

```
Resolve Goal Location(Entry, SourceActor)
  → Target Actor, Object Actor*, OutGoalComponent, Destination,
    OutAcceptanceRadius, Uses Destination*, bOutSuccess, bOutAlreadyThere,
    bOutReachable, OutPathEndPoint, OutPathPoints,
    OutGoalTravelDistance, OutMovementPointIndex
```

- **Wire `Target Actor` + `Destination` straight into one AI Move To — no
  branch needed.** On successful resolution, `Target Actor` is null exactly
  when the goal is a fixed location (a Movement Point won, or the entry
  references a component), and AI Move To falls through to its Destination
  pin whenever its Target Actor pin is null. When `Target Actor` IS set,
  following the actor is the right behavior — the goal tracks a moving
  object. A failed resolution (`bOutSuccess` false) also returns null.
- **`Uses Destination`** (advanced) exposes the same fact as a bool, for graphs
  that prefer an explicit branch. **`Object Actor`** (advanced) is always the
  entry's actor while alive — for non-movement uses (attach an effect, query
  it), regardless of what won the goal.
- Gate on **`bOutSuccess`** before consuming outputs — `false` means `Ref` is
  null/destroyed.
- **`bOutAlreadyThere`** (cheap, no nav query) is true when `SourceActor` is
  already at the goal — check it to avoid issuing no-op moves. With Movement
  Points the arrival ring is `max(Acceptance Radius, 150)` around the point,
  measured from the pawn's center (object fallback instead measures to the
  object's bounding-box footprint with `max(Acceptance Radius × 2, 150)`).
  Mind the interplay with AI Move To: with *Stop On Overlap* (the default),
  path following stops when the pawn's *body edge* reaches the acceptance
  radius — a large radius can park the pawn's center up to one body radius
  outside the arrival ring, and arrival will read false. Pass a small/zero
  AI Move To Acceptance Radius so a normal-sized pawn's successful stop
  remains inside the resolver's arrival ring.
- **`bOutReachable`** is true when a navmesh path from `SourceActor` lands within
  tolerance of the goal; `OutPathPoints` is the full path (for debug draw), and
  `OutGoalTravelDistance` is its length. Only populated when `SourceActor` is
  provided. Must be called on the game thread.
- **`OutMovementPointIndex`** is the index of the winning Movement Point, or `-1`
  when the object reference resolved the goal — including when *Use Object as
  Fallback* re-runs the object fallback because every enabled point was
  unreachable; the outputs then describe the object goal, so `Uses Destination`
  follows the object's reference mode again. Otherwise, with points enabled,
  `Uses Destination` is always true and the shortest-path reachable point wins
  (exact ties keep the lower array index).
