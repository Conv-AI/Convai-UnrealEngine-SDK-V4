# Actions, Environment, and the Dynamic Context Pipeline

> **Heads up:** the Actions surface has been overhauled. New work should use the
> typed `FConvaiAction` / `FConvaiResultAction.Parameters` flow documented in
> [ActionsV2.md](./ActionsV2.md). This page describes the broader
> Environment / Dynamic Context pipeline (Objects, Characters, attention,
> conversation partner, batching). For **Convai Object Components, Tracked
> Properties, gaze, and spatial awareness** — how the *world* feeds the chatbot
> — see [ConvaiObjectsAndTrackedProperties.md](./ConvaiObjectsAndTrackedProperties.md).

This guide is for UE engineers and BP designers working with the Convai chatbot
component's action / environment / context APIs. It documents the runtime
contract, the four prompt lanes the server tracks, and the BP surface for
mutating each.

## TL;DR

- **Set up at edit time:** populate `Environment` (Actions, Objects, Characters)
  in the chatbot's Details panel. Toggle `bEnableActions` if the bot should
  perform physical actions in addition to talking.
- **Mutate at runtime via methods, never directly.** `Environment` is hidden from
  BP runtime read/write — designers can only edit it before play. At runtime use
  `AddObject`, `RemoveObject`, `SetObjectInAttention`, `SetConversationPartner`, etc.
- **Updates batch.** Calls within a debounce window coalesce into one WebRTC
  message. Pass `bFlushImmediately = true` only for time-critical updates.
- **Updates can wait for a pause.** Every context/attention method takes a
  `Delivery` option: `Send Normally` (default) or
  `Wait Until Conversation Is Idle`, which holds the update while anyone is
  talking so the character can't interrupt itself (see
  [Delivery](#delivery--dont-let-the-character-interrupt-itself)).
- **Reads are pull-based:** override the BP virtual `GatherEnvironmentExtras` to
  inject world-derived affordances right before `/connect`.

## The four server prompt lanes

The bot's system prompt is built from four independent lanes, each with its own
RTVI message and its own update semantics:

| Lane                      | Server field                                | Set by                                      | Replace style                |
|---------------------------|---------------------------------------------|---------------------------------------------|------------------------------|
| Action affordances        | `action_config.{actions,objects,characters}`| `/connect` only                             | **Connect-time only**        |
| Descriptive scene state   | `bot_connect_config.scene_description`      | `update-scene-metadata`                     | Full replace each send       |
| Dynamic info text         | `bot_connect_config.dynamic_info.text`      | `context-update` (text field)               | Append / Replace per `mode`  |
| Attention object          | `action_config.current_attention_object`    | `context-update.current_attention_object`   | Replace                      |

Lanes are NOT merged. Adding an object to scene metadata at runtime does NOT
extend the action set — that's frozen at connect. If you need new action
targets, reconnect with an updated `Environment`.

## `bEnableActions`

This toggle now lives **inside** the `EnvironmentData` struct
(`FConvaiEnvironmentData.bEnableActions`) and shows at the top of the
**Environment** section in the Details panel. It **defaults to `true`**. When
true:

- `Environment` is serialized as `action_config` and sent at `/connect`.
- `SetObjectInAttention` actually emits messages.
- The bot is permitted to emit `action-response` payloads.

When false the chatbot is conversational-only. `SetObjectInAttention` becomes a
no-op (logs a Warning explaining why) since the server can't resolve attention
without an `action_config`.

## Environment mutation

### Edit time (Details panel)

Populate `Environment.Actions`, `Environment.Objects`, `Environment.Characters`
in the chatbot's Details panel. These are sent in `action_config` at the next
`/connect`.

`Environment.Actions` is `TArray<FConvaiAction>` — each entry is a structured
template with four fields:

- `Enabled` — keeps the definition in the project while controlling whether it
  is advertised on the next fresh connection. Disabled actions are excluded
  from both the sent contract and its frozen parser snapshot.
- `Name` — canonical action name (no placeholders), e.g. `"Parse"`.
- `Description` — optional human-language explanation surfaced to the LLM.
- `Parameters` — ordered `TArray<FConvaiActionParam>`; each param has a `Name`
  (the placeholder) and an optional `Description`.

Defaults shipped on a fresh chatbot:

| Name          | Description                | Parameters |
|---------------|----------------------------|------------|
| `Move To`     | (none)                     | `destination` — type `Reference` |
| `Follow`      | Follow a character         | `character` — type `Reference` |
| `Stop Moving` | (none)                     | (none) |
| `Wait For`    | (none)                     | `time in seconds` — type `Number` |

(These are the literal defaults seeded on `FConvaiEnvironmentData.Actions`. You
can edit descriptions, prune the list, or add your own in the Details panel.)

Collapsed array rows use the action name as their label. Disable an action when
you want to preserve its setup without offering it to the AI. Enablement is part
of the connect-time contract: changing it does not alter an active session and
takes effect when a fresh connection is created. Action identity is
case-insensitive. Only enabled designer actions enter the contract, but every
authored action name reserves its identity: a disabled same-named row also
suppresses the native built-in, so turning a project action off cannot silently
activate a different implementation.

#### Wire format sent in `action_config.actions[]`

Each `FConvaiAction` is rendered to a single string per these rules:

1. `<Name>`
2. ` {Param.Name}` for each parameter (always — the LLM needs to see the placeholder).
3. ` — ` if any description (action's or any param's) is non-empty.
4. `<Description>.` when the action description is non-empty.
5. `<Param.Name>: <Param.Description>.` for each param with a non-empty description, joined by `. `.

Worked examples:

| Scenario                         | Generated string |
|----------------------------------|------------------|
| Both descs                       | `Parse {email} {password} — Parses login credentials. email: user's email. password: user's password.` |
| Only action description          | `Parse {email} {password} — Parses login credentials.` |
| Only some param descriptions     | `Parse {email} {password} — email: user's email.` |
| No descriptions at all           | `Parse {email} {password}` |
| Single-param, both descs         | `Wait For {time in seconds} — Wait for a duration. time in seconds: How long to wait, in seconds.` |
| No params, with description      | `Move To — Move the character to a target location.` |
| No params, no description        | `Move To` |

The `{placeholder}` syntax (curly braces — the universal format-string convention
across Python, Jinja, and Mustache) hints to the LLM to wrap response values in
braces too — e.g. `Parse {MMA2} {537}` — which lets the parser split
unambiguously even when values contain spaces. The parser also accepts legacy
`"quoted"` responses and degrades to whitespace splitting if the LLM doesn't
wrap at all.

### Runtime — Objects and Characters

```
Add Object         → write-through + schedule update-scene-metadata
Add Objects        → batch write-through + one update-scene-metadata
Remove Object      → write-through + schedule
Remove Objects     → batch
Clear Objects      → write-through + schedule

(Same set for Characters)
```

Same-name entries replace in place. Removing or re-describing an entry that was
in the connect-time `action_config` only takes effect on reconnect — the server's
`action_config.objects` / `.characters` is connect-time-immutable.

### Runtime — Actions

```
Add Action(FConvaiAction)             → write-through; takes effect on next /connect
Add Actions(TArray<FConvaiAction>)
Add Action By Name(FString)           → convenience for no-description / no-params
Remove Action(FString Name)           → keyed by FConvaiAction.Name
Remove Actions(TArray<FString> Names)
Clear Actions
```

Actions have **no runtime delivery channel** on the server. These methods only
mutate the local `Environment.Actions`; the change takes effect on the next
`/connect`. Use them to prepare the next session, not to alter the live one.

`Add Action` / `Add Actions` replace any same-name entry in place
case-insensitively (Enabled / Description / Parameters get overwritten with the
new values).

### Attention object

The "object in attention" is the antecedent the AI resolves "this"/"that"/"it"
against.

```
Set Object In Attention(AttentionObject, Text, ShouldRespond = Auto,
                        Delivery = Send Normally,
                        bAddAttentionEvent = true, bFlushImmediately = false)
```

Behavior:

1. Mirror the entry into `EnvironmentData.CurrentAttentionObject` (so reconnect
   carries it through) and stamp `AttentionSource = Explicit` — this **locks**
   the slot against gaze overwrites (see below).
2. Auto-add the entry into `Environment.Objects` if missing — the server only
   resolves attention against `action_config.objects`, never characters
   (character-only entries are auto-promoted).
3. Stage the attention slot in the dynamic-context batch. Last-wins within the
   debounce window — multiple calls collapse to one message. With
   `Delivery = Wait Until Conversation Is Idle`, this staging (and the announce
   event below) waits for a pause in the conversation — steps 1-2 and the
   ownership lock still happen immediately.
4. **By default (`bAddAttentionEvent = true`)** emit a **one-flush ephemeral**
   event announcing the change (e.g. *"Bob is paying attention to FrontDoor"*),
   with the optional `Text` appended. The AI sees it once on the next update and
   it's then gone — so changing attention nudges the AI without a lingering
   state piling up. The event only fires when `ShouldRespond` is `Auto`/`Always`
   (`Never` sets the slot silently).
   - Turn `bAddAttentionEvent` **off** to set the slot silently; then the
     optional `Text` falls back to a normal *persistent* event instead.

Server / diagnostics:

- Empty `AttentionObject.Name` clears attention.
- Has no effect when `EnvironmentData.bEnableActions` is false.
- Logs a **Warning** explaining why a call had no effect: actions disabled, the
  object wasn't in this chatbot's object list, or — while connected — the object
  wasn't in the connect-time `action_config` (so it only becomes a valid
  attention target after the next reconnect).
- Not gated on being connected; calls staged while disconnected fire on the next
  flush after reconnect.

#### Gaze-driven attention (ownership model)

The chatbot tracks **who owns** the attention slot via `AttentionSource`
(`EConvaiAttentionSource`: `None` / `Explicit` / `Gaze`). `Set Object In
Attention` is an **Explicit** set and locks the slot. Gaze uses separate
gaze-gated setters that respect that lock:

```
Try Set Object In Attention From Gaze(...)        → bool   // accepts only if slot is
                                                            // None or already Gaze
Try Clear Object In Attention From Gaze(Expected) → bool   // clears only if gaze still
                                                            // owns it AND object matches
```

These are normally driven by the `UConvaiObjectComponent` gaze pipeline, not
called by hand. Full details — gaze events, highlighting, merged sets — are in
[ConvaiObjectsAndTrackedProperties.md → Gaze & attention](./ConvaiObjectsAndTrackedProperties.md#gaze--attention).

### Conversation partner

```
Set Conversation Partner(Partner, bFlushImmediately)
```

Sets the character the bot is currently talking to (the player or another NPC).
This is a client-side concept — the server treats the partner as a normal scene
character, not a distinct lane.

- Auto-adds Partner to `Environment.Characters` if missing.
- Schedules an `update-scene-metadata` send when the partner is brand-new (not
  in the connect snapshot).
- Pass an empty entry to clear without removing anyone from the character list.

#### `bAutoFillConversationPartnerFromPlayer`

Edit-time toggle. When true, before `/connect`:

1. Look up the first registered `UConvaiPlayerComponent` in the world.
2. If found: `Partner.Name = PlayerComponent.PlayerName`, `Partner.Ref = its owner`.
3. Else: `Partner.Name = "User"`, `Partner.Ref = player pawn 0`.

Runs before `GatherEnvironmentExtras` so a BP override can inspect or replace
the auto-filled value.

### `LookAtTarget` and `PointAtTarget`

```
LookAtTarget  : TObjectPtr<AActor>   // BlueprintReadWrite, Replicated
PointAtTarget : TObjectPtr<AActor>   // BlueprintReadWrite, Replicated
```

Pure animation hooks. AnimBP reads `LookAtTarget` for gaze / IK and
`PointAtTarget` for arm IK / gesture (when the rig supports it). Neither touches
the conversation or RTVI pipeline, and they're independent of each other. Both
are **replicated**. Set them from gameplay (line trace, NPC AI, story beats, the
conversation partner's position, whatever) without worrying about server side
effects.

## Dynamic context

Dynamic context is how you keep the character's understanding of the world
current *between* connect and reconnect — the running feed of "what's true now"
and "what just happened" that shapes its replies. Use it whenever gameplay
changes something the character should know: the player's health dropped, a quest
advanced, a light turned on, an NPC left the room.

**Which one do I use?**

- **State** — a named value that has a current reading and keeps changing:
  `Health`, `Ammo`, `Quest Stage`. Overwrites in place by key.
- **Event** — a moment in time: *"the player fired a shot."* Use `bEphemeral` for
  a one-off nudge that shouldn't linger.
- **Fact** — a lasting truth phrased as a sentence: *"the drawbridge is down."*
  Stays until you remove it.

(Tracked Properties on Convai Object Components are just State updates the engine
emits for you — see [ConvaiObjectsAndTrackedProperties.md](./ConvaiObjectsAndTrackedProperties.md).)

BP-callable mutators, all batched (each takes advanced `Delivery` +
`bFlushImmediately` pins — see
[Delivery](#delivery--dont-let-the-character-interrupt-itself)):

```
Set Context State(Name, Value, ShouldRespond = Never, Delivery, bFlushImmediately)
Set Context States(States, ShouldRespond = Never, Delivery, bFlushImmediately)
Add Context Event(Text, ShouldRespond = Auto, Delivery, bEphemeral = false, bFlushImmediately)
Remove Context State(Name, bFlushImmediately)
Set Context Fact(Sentence, Key = "", ShouldRespond = Never, Delivery, bFlushImmediately)
Remove Context Fact(KeyOrSentence, bFlushImmediately)
Reset Dynamic Context()
Get Context State Value(Name) → (bool Found, out Value)   // BlueprintPure
```

There are three kinds of dynamic context, all flushed together as one
`context-update` carrying the full canonical context plus a delta summary:

- **State** — canonical key/value (`"Health" → "80"`). Replaced in place by key.
  Keys are whitespace-sanitized on receipt (Pascal-cased: `"door state"` →
  `"DoorState"`) so they read as one token in the prompt — set/remove/get all
  apply the same rule, so spaced keys keep working from Blueprint. Values are
  stored raw; multi-word values are quoted when rendered
  (`DoorState is "half open"`, single words stay bare). This is the same channel
  **Tracked Properties** write to automatically (see
  [ConvaiObjectsAndTrackedProperties.md](./ConvaiObjectsAndTrackedProperties.md#how-a-change-reaches-the-chatbot)).
  Ordinary changes that return to their original value inside one batch are
  cancelled as a net no-op. Internal producers can preserve a meaningful
  excursion—movement uses this so a brief start-and-stop still reports the
  final `Stopped (was Upward)` transition through this same state channel. The
  synthetic Movement property also records compact physical direction changes,
  such as `Upward (was Left)`, while leaving speed in the spatial fact. Response
  emphasis remains per state key: a `Never` change stays in canonical context
  and is neither deferred nor echoed in the prompt-tail delta just because
  another item in the same batch requests a response.
- **Events** — chronological "this just happened" lines. `bEphemeral = true`
  shows the event to the AI **exactly once** on the next update and never
  persists it — for transient cues that shouldn't pile up. `false` (default)
  keeps it in the running context.
- **Context facts** (see below) — persistent plain-language sentences the AI
  treats as currently true.

When actions are enabled, the chatbot may append two **managed, derived**
sections after that world context:

- **Instructions** — short usage guidance for built-in actions that are both
  enabled and advertised in the current session contract. This is where the AI
  learns when to use controls such as `Remind Self`, `Watch Property`, and
  `Cancel Action Plan`; action descriptions remain focused on what
  each action does and what parameters it accepts.
- **Current action plan** — only while work is active. It reports the current
  action invocation with generic running, waiting, or cancelling wording, the
  immediate next action, and a count of anything after that. It never needs to
  understand or conjugate an action's verb and deliberately does not echo the
  full queue.

These sections are not events or user-authored facts. They replace in place on
authoritative action transitions and are omitted when empty, so repeated action
changes do not grow the prompt. Canonical ordering is states, persistent facts
(including spatial facts), events, `Instructions`, then `Current action plan`.
`Reset Dynamic Context` clears the tracked world context and immediately
restores any session-derived instructions or still-live action status.

### Context facts

```
Set Context Fact(Sentence, Key = "", ShouldRespond = Never, Delivery, bFlushImmediately)
Remove Context Fact(KeyOrSentence, bFlushImmediately)
```

A **fact** is the current truth about the world, e.g. *"the treasure chest is
locked"* — unlike an event (a one-off "this happened"), it stays in context
until removed. It reads to the AI as a bare sentence, with no `key: value`
wrapper.

- `Sentence` is the exact wording the AI reads — write a complete sentence; it's
  the only thing the AI ever sees from the call.
- `Key` is optional bookkeeping **never shown to the AI**. Leave it empty for a
  one-off fact (the sentence becomes its own handle). Give a stable `Key` (e.g.
  `"chest_locked"`) when the wording will *change* over time and you want each
  new `Sentence` to **replace** the previous one instead of piling up.
- `Remove Context Fact` takes the `Key` you gave it, or — if you set it without a
  Key — the exact `Sentence`.

The **spatial-awareness** pass emits its surroundings/relations descriptions as
context facts (see
[ConvaiObjectsAndTrackedProperties.md → Spatial awareness](./ConvaiObjectsAndTrackedProperties.md#spatial-awareness)).
Positive and negative nav verdicts are both explicit (`reachable by walking` /
`no walking path`) so a newly opened route cleanly replaces a stale blocked belief;
when the character has already arrived, that path wording is replaced with
`You are already at <object>` (or `already with <person>`).
Confirmed motion is also current-state wording: moving objects include a short
direction clause, and an object with **Add Movement State** enabled
continues to say `stopped` after arrival. Ordinary stationary props remain
unlabelled. The declarative surroundings sentence and synthetic Movement state
are two views of the same confirmed movement detector, not independent
classifiers, so each confirmed start or stop edge updates both consistently.
The initial spatial snapshot is seeded with
`ShouldRespond = Never`; authored `Auto` / `Always` spatial response settings
begin applying only after that baseline has been delivered.

### Debounce tunables

Two chatbot properties (advanced) control the batch window:

- `ContextDebounceWindow` (default `0.5s`) — how long to wait after the most
  recent staged update before flushing. Each new update resets the timer so
  bursts coalesce into one send.
- `ContextMaxDebounceWindow` (default `3.0s`) — safety cap on how long the first
  update in a burst can be delayed, so an unbroken stream can't postpone the
  flush forever. Set `>= ContextDebounceWindow`.

## `Delivery` — don't let the character interrupt itself

The dynamic-context methods (`Add Context Event`, `Set Context State(s)`,
`Set Context Fact`, `Set Object In Attention`, gaze attention, and each tracked
property) take a `Delivery` option controlling **when** the update reaches the
character:

- **Send Normally** (default) — batched into the next scheduled send. Exactly
  the behavior before this option existed.
- **Wait Until Conversation Is Idle** — held back until nobody is speaking and
  the character isn't preparing or giving a reply, so an Auto/Always update
  can't make the character interrupt itself (or talk over the user)
  mid-sentence. Classic use: a tour-guide character walking to an exhibit while
  the visitor is still chatting — the "you have arrived" cue waits for the
  pause.

Details worth knowing:

- Waiting applies only when `ShouldRespond` is Auto/Always. A silent (`Never`)
  update has nothing to interrupt with — it always sends normally.
- On a Convai Object's **Tracked Properties** array, selecting `Never` also
  canonicalizes the stored settings: Delivery returns to `Send Normally`,
  Flush Immediately returns to false, and both Details rows are disabled.
  A one-shot Watch Property promotion on that entry therefore uses normal,
  non-flushed delivery.
- **`Quiet Time Before Delivery (s)`** (chatbot property, advanced, default 2)
  is how long the conversation must stay continuously quiet before held updates
  are delivered — it covers the user merely pausing between sentences. Any
  activity restarts the wait, and there is deliberately **no upper bound** on
  the total wait: a waiting update never interrupts an ongoing conversation.
  Set it to 0 to deliver the moment idle is detected.
- Held events land at the **end** of the event history when released, so they
  read as the most recent thing that happened — even if newer events were sent
  while they waited.
- A newer write to the same state / fact / attention **supersedes** a held
  older one (values never roll backwards). If a newer state or keyed fact is
  routed to idle delivery while an older same-key value is still pending in
  the normal debounce batch, that older value is withdrawn so it cannot flush
  first. `Reset Dynamic Context` cancels held work outright, and held updates
  survive transient disconnects.
- Idle delivery is latest-state coalescing, not an every-transition queue. For
  example, a held `Platform.Movement = Stopped` is superseded if the platform
  starts again before any idle window. Use `Send Normally` plus
  `Flush Immediately` when that stop must be delivered before motion resumes.
- A held gaze-attention cue is cancelled if the player looks away before it
  ever landed.

## `bFlushImmediately` (advanced)

Every batched method has an advanced-display `bFlushImmediately` parameter
(default false). When true, the call bypasses debounce coalescing and emits the
WebRTC message in the same frame. Combined with
`Wait Until Conversation Is Idle`, the wait wins first — the held update then
releases at the first idle instant, skipping the quiet time.

**Use sparingly.** High-frequency immediate flushes spam the WebRTC channel and
will degrade conversation latency. Reserve for genuinely time-critical updates
(e.g. boss-fight phase change, an event the bot must register before the next
user utterance).

## `GatherEnvironmentExtras` (BP virtual)

```
Gather Environment Extras(out ExtraActions, out ExtraObjects, out ExtraCharacters)
```

Override in the chatbot's BP to dynamically populate the Environment right
before `/connect`. The defaults from the Details panel are still sent — this
hook only **appends**. Use it to reflect dynamic world state (nearby NPCs,
quest-conditional objects, party members) without baking the values into edit
defaults.

Order at session start:

1. Auto-fill conversation partner (if enabled)
2. `GatherEnvironmentExtras` (BP can see and override the auto-fill)
3. Build `action_config` from `Environment`
4. `/connect`

## Action responses

When `bEnableActions == true` the bot can emit action sequences. These arrive
as RTVI `action-response` messages with the wire shape:

```json
{
  "actions": [
    { "name": "Move To", "target": "cube" },
    { "name": "Parse \"MMA2\" \"537\"" },
    { "name": "Wait For", "target": "5 seconds" }
  ]
}
```

Empty array (`actions: []`) is the no-op signal. There is no `"None"` sentinel —
legacy code that treated `"None"` specially has been removed.

#### Template matching

For each entry, the parser:

1. Looks up the chatbot's `Environment.Actions` and finds the `FConvaiAction` whose
   `Name` best matches the prefix of the incoming `name` (Levenshtein-scored, so
   minor LLM noise is forgiven).
2. Sets `Result.Action` to the matched template's canonical `Name` (no
   placeholders) so handlers can `switch` on it cleanly.
3. Falls back to the raw incoming name if no template matched, so unknown
   actions still surface to handlers.

#### Param extraction

Once a template is matched, the parser pulls per-placeholder values out of the
*combined* name leftover + target field (the LLM may put values in either),
coerces each to its declared `Type`, and stores them in
**`FConvaiResultAction.Parameters`** (`TMap<placeholder, FConvaiResultParam>`).
Read them with the typed accessors — `Get Param As Ref/Number/Bool/String/Byte` —
documented in [ActionsV2.md](./ActionsV2.md). That is the current API.

The deprecated `ExtraParams.NamedParams` / `RelatedObjectOrCharacter` /
`ExtraParams.Number` / `.Text` mirrors are still populated *from* that map so
older handlers keep compiling — but new graphs should not use
`Get Action Param(...)` or read `RelatedObjectOrCharacter` directly. See the
migration table in
[ActionsV2.md → Migration](./ActionsV2.md#migration-from-the-legacy-fields).

### Reusable movement: `Convai Move To`

Use the asynchronous **Convai Move To** Blueprint node when a custom action
needs Convai's standard object-resolution and movement policy without copying
the stock movement graph. It accepts **Moving Actor** and an
`FConvaiObjectEntry`, returns `Succeeded` or `Failed`, and exposes a
**Move Request** proxy.

The node keeps whole-actor goals tracked while they move. For component,
socket, movement-point, and other retained-location goals, it refreshes the
resolved destination during the request. Both outputs return a result code and
concise, player-safe `Additional Note`; developer diagnostics stay in native
logs.

This is a movement primitive, not an action executor: it never calls
`Handle Action Completion` or `Handle Action Cancellation`. The custom action
handler that started it still owns exactly one terminal acknowledgment. For
cancellation, save **Move Request**, call `Cancel` from the separate
`Cancel <ActionName>` handler, and call `Handle Action Cancellation` only when
**Cancellation Succeeded** is true. False means natural completion already owns
acknowledgment. The existing stock Blueprint `Move To` and `Follow`
implementations remain unchanged.

### `Escort` action

`Escort` is a stock row in `Environment.Actions`, disabled by default. Enable
that row to advertise it at the next connection, exactly like any other action.
Its stock timing policy enables **Wait for Bot Speech**, so a first action in a
fresh sequence does not begin moving before the character's speech gate.
Keep its canonical template as:

```
Escort {character: ref} to {destination: ref}
```

Keep the row's description spatially grounded: when the surroundings say the
character is already at the destination, discuss it now without follow-me or
future-arrival wording; when it is close by, use only a brief transition;
otherwise invite the escorted character to follow.

Both wire values use the compatible `Reference` type and the second parameter
uses the connector `to`. `character` is nevertheless resolved semantically and
case-insensitively from `Environment.Characters`, so a same-named object cannot
become the companion. Connect the chatbot owner's AI-controlled pawn to
**Escorting Actor**, the resolved `character` entry to **Escorted Character**,
and the destination entry to **Destination**. Escorting Actor must own the
Convai Chatbot Component. `destination` may be a normal Convai object or a
separately named movement-point destination.

The **Convai Escort** async node composes the reusable `Convai Move To` task. The shared
resolver remains authoritative for destination choice, authored arrival, and
reachability instead of trusting the raw engine movement result alone. For
retained-location goals, movement consumes the resolver's **Destination** and
**Acceptance Radius**, matching the convenience Blueprint macro. If the
companion lags in full 3D distance, the guide pauses, faces them, queues a
one-shot follow prompt for the next quiet conversational moment, and resumes
after they catch up. A distant companion who is clearly ahead inside the
remaining ordered navigation corridor does not trigger that pause; being near a
sparse route segment behind the guide does not count as ahead. Task teardown
withdraws that prompt if it is still held and undelivered.

The node returns `Succeeded` or `Failed` with a result code and player-safe
`Additional Note`; engine diagnostics stay in native logs. Success preserves
whether movement occurred: **Reached** follows actual travel, while **Already
At Destination** identifies a no-op arrival. It never changes the Convai action
state. An exact named `Escort` Blueprint handler invokes the node and calls
`Handle Action Completion` on its terminal pins. For **Reached**, normally use
auto-reporting with **Always**, **Wait Until Conversation Is Idle**, ephemeral
delivery, and immediate flush. For **Already At Destination**, normally use
**Never** so the direct discussion prompted before the action is not repeated.
No internal arrival event is created, so the handler alone owns response
delivery.

For cancellation, save **Escort Request**. The exact `Cancel Escort` handler
calls `Cancel`, then calls `Handle Action Cancellation` only when
**Cancellation Succeeded** is true. False means natural completion is already
responsible. Explicit cancellation is silent and cannot race a second terminal
acknowledgment.

`Cancel Action Plan` is a separate experimental control. Enabling `Escort` does
not enable or advertise cancellation; opt in independently when the experience
must redirect a moving or waiting escort. The row advertises the action schema
but does not install a native fallback. See
[ActionsV2 → Escort](./ActionsV2.md#escort).

### Cancelling and replacing a plan

The optional built-in **`Cancel Action Plan`** is a reserved control marker for
user redirection while that built-in is advertised and unshadowed. Shadow
identity is case-insensitive and every authored action name participates, even
when that row is disabled. It
never reaches an ordinary Blueprint action event and never joins the sequential
queue. An enabled project-owned same-name action retains ordinary custom-action
semantics; a same-named handler by itself does not change the frozen contract,
while a disabled same-named row suppresses the control without advertising a
replacement. If a built-in
control response contains multiple markers, the
**last** marker wins: actions through it are discarded and the suffix after it
is the new plan.

An already-running action remains at the queue head as a tombstone while the
replacement is held separately. The replacement cannot start until the old
action sends one terminal acknowledgment. Repeated cancel responses do not
invoke cleanup repeatedly; the active action's optional cancel hook runs at
most once, while the newest replacement suffix replaces the previously held
one.

For a custom action named `Move To`, the plugin looks for an optional Blueprint
handler named exactly `Cancel Move To` on the same object that handled `Move
To`. It receives the original `FConvaiResultAction`. The hook requests cleanup;
once no old callback can still arrive, finish through exactly one of:

- `Handle Action Cancellation` for a silent, fully-quiescent cancellation; or
- the action's existing `Handle Action Completion` callback if stopping the
  underlying operation naturally causes it to fire.

Do not call both. A cancellation-time success preserves auto-report text and an
explicit note as silent context because success can represent a real state
change. A cancellation-time failure suppresses generated failure/retry prose;
an explicit note may remain as silent context. Neither case forces a response
or honors a post-action delay, and neither releases the held replacement before
the terminal callback.

There is no public execution ID. Therefore, a missing or non-terminating cancel
handler is diagnosed but cannot be bypassed safely: starting the replacement
before the old action is quiescent would let a late callback complete the wrong
action.

### Cancel, abort, and clear are different operations

| Operation | Use it for | Queue/reporting behavior |
|-----------|------------|--------------------------|
| `Cancel Current Action Plan` (or the AI's `Cancel Action Plan` marker) | The user redirected or abandoned valid work. | Cooperatively cancels the dispatched head once, waits for its terminal acknowledgment, then starts the held replacement (empty for the direct API). |
| `Abort Action Sequence` | Gameplay determined the current plan cannot continue. | Invokes the active cancel hook once, stops plugin-owned action work, clears the active sequence and any held replacement, then may publish only the supplied reason so the AI can replan. |
| `Clear Action Queue` | Administratively prune pending work without inventing an outcome. | Removes the queued tail and held replacement; if an action was dispatched, its active head remains until its real terminal callback. It neither cancels that operation nor stands in for lifecycle teardown. |

Connection loss and lifecycle teardown are intentionally different. A
transient server disconnect cancels owned action tasks, invokes an available
custom cancel hook, and clears the current and held plans without publishing a
success/failure, then leaves the executor available for fresh work after
reconnect. An explicit `Stop Session` is terminal for that session and also
retires armed property watches. Work already posted across the session boundary
is rejected, but a custom latent operation must still quiesce its own callbacks
from `Cancel <ActionName>`; without a public execution ID, a late
non-cooperative callback cannot be identified after a new action has begun. A
later explicit `Start Session` begins a fresh lifecycle. `End Play` and
component destruction remain terminal.

See [ActionsV2 → Cancel Action Plan](./ActionsV2.md#cancel-action-plan) for the
full lifecycle and Blueprint convention.

## Replication

- `EnvironmentData` (which contains `bEnableActions`), `ConversationPartner` — replicated.
- `LookAtTarget`, `PointAtTarget` — replicated.
- `AttentionSource` — runtime-only, not persisted, not replicated.
- Dynamic-context tracker / pending batches — server-only state on the chatbot;
  flushes to WebRTC, not to clients.

## Player speaking lifecycle

On a Convai Player component, `Is Speaking` and the inherited `On Started Talking` /
`On Finished Talking` delegates expose normalized utterance edges rather than depending
on one backend signal:

- server voice-activity start/stop events remain the primary source;
- the first non-empty partial transcription opens the bracket if the start event was
  missing;
- a non-empty final transcription closes it if the stop event was missing; and
- a server stop waits 250 ms of real time before closing, so a continuing partial can
  cancel a premature or duplicated stop. World pause and time dilation do not affect
  this grace.

Repeated starts and stops do not rebroadcast the same edge. The final transcription is
broadcast before `On Finished Talking`, so Blueprint listeners see the complete text
before the utterance closes. The backend's empty final sentinel remains a transcription
event but does not bypass the stop grace.

Do not use the WebRTC active-speaker snapshot as a speech lifecycle event. It may repeat
the same participant set, may contain both user and character, and does not emit a
reliable empty/silence edge.

## Migrating from the legacy `UConvaiEnvironment` API

The old `UConvaiEnvironment` UObject (with `Actions`, `Objects`, `Characters`,
`MainCharacter`, `AttentionObject` UPROPERTYs and Add/Remove/Clear/Set BP methods)
has been replaced by an `FConvaiEnvironment` USTRUCT field on the chatbot, mutated
through granular methods.

A deprecated `UConvaiEnvironment` migration shim is kept so existing BP graphs
that did `Chatbot->Environment->XxxMethod(...)` and `Chatbot->Environment->MainCharacter`
keep compiling and emit clear deprecation warnings pointing users at the new API. Each
shim method delegates to the chatbot, and each shim property reads from the chatbot's
new state via `BlueprintGetter`.

The C++ struct field on the chatbot is now named `EnvironmentData` (type
`FConvaiEnvironmentData`) — the BP-visible `Environment` property name is reserved for
the legacy `UConvaiEnvironment*` pointer (lazily allocated, deprecated). New C++ code
should access `EnvironmentData` directly; new BP code should call the granular methods
on the chatbot.

Concrete mappings:

| Legacy call                            | New API                                     |
|----------------------------------------|---------------------------------------------|
| `Environment->AddObject(Entry)`        | `Chatbot->AddObject(Entry)`                 |
| `Environment->AddCharacter(Entry)`     | `Chatbot->AddCharacter(Entry)`              |
| `Environment->AddAction(Action)`       | `Chatbot->AddAction(Action)`                |
| `Environment->SetAttentionObject(E)`   | `Chatbot->SetObjectInAttention(E)`          |
| `Environment->SetMainCharacter(E)`     | Split — see below                           |
| `Environment->ClearAttentionObject()`  | `Chatbot->SetObjectInAttention(default)`    |

`MainCharacter` was a single field that served two distinct purposes:

| Old usage                                          | New replacement              |
|----------------------------------------------------|------------------------------|
| "Who is the bot currently talking to?"             | `SetConversationPartner`     |
| "Where should the character look?" (anim gaze)     | `LookAtTarget`               |

The shim's `SetMainCharacter` / `GetMainCharacter` only redirect to `LookAtTarget`.
Graphs that relied on the conversation-partner aspect must migrate to
`SetConversationPartner` explicitly.

`UConvaiActionContext` is also kept as a deprecated alias of the shim for the
same reason.

The chatbot exposes `GetEnvironment()` (deprecated) which lazily allocates a
shim instance bound to the chatbot — this keeps the `Chatbot->Environment->...`
call shape working even though the underlying `Environment` field is now a
struct. New code should call the granular methods on the chatbot directly.

## Quick reference

| Need to…                                           | Use this                                     |
|----------------------------------------------------|----------------------------------------------|
| Tell the bot a new prop appeared                   | `Add Object`                                 |
| Tell the bot a prop was removed from the scene     | `Remove Object`                              |
| Tell the bot the player is looking at something    | `Set Object In Attention`                    |
| Tell the bot it's now talking to a specific NPC    | `Set Conversation Partner`                   |
| Make the character's eyes follow something         | Assign `LookAtTarget`                        |
| Make the character point at something              | Assign `PointAtTarget`                       |
| Update bot context with state changes              | `Set Context State` / `Set Context States`   |
| Tell the bot something happened (once)             | `Add Context Event` (`bEphemeral` for one-shot) |
| State a lasting truth about the world              | `Set Context Fact` / `Remove Context Fact`   |
| Auto-track a live actor value                      | Add a Tracked Property on a Convai Object Component ([guide](./ConvaiObjectsAndTrackedProperties.md)) |
| Bake dynamic affordances at connect time           | Override `Gather Environment Extras` in BP   |
| Author a new action template at edit time          | Add an entry to `Environment.Actions` (FConvaiAction) |
| Read an action's param in a handler                | `Get Param As Ref/Number/Bool/String/Byte(Action, "name")` ([V2](./ActionsV2.md#bp-accessors)) |
| Finish / cancel an active action                   | `Handle Action Completion` / `Handle Action Cancellation` |
| Redirect or abandon the current plan               | `Cancel Current Action Plan` (or enable the AI's `Cancel Action Plan` built-in) |
| Report an unrecoverable plan failure               | `Abort Action Sequence`                      |
| Inspect or clear the action queue                  | `Is Actions Queue Empty` / `Clear Action Queue` / `Fetch First Action` |
| Stop the bot interrupting itself with an update    | Set `Delivery = Wait Until Conversation Is Idle` |
| Check if a conversation is happening               | `Is In Conversation` / `Is Listening` / `Is Thinking` / `Is Talking` (chatbot), `Is Speaking` (player) |
| React once when the player starts or stops speaking | Player `On Started Talking` / `On Finished Talking` |
| Let the AI speak after arriving somewhere          | Enable the `Remind Self` action ([V2](./ActionsV2.md#built-in-experimental-actions)) |
| Let the AI react to a silent property change or value | Enable `Watch Property`; use `Key = exact value` for a target ([V2](./ActionsV2.md#built-in-experimental-actions)) |
| Guide another character without leaving them behind | Enable the `Escort` action ([V2](./ActionsV2.md#escort)) |
| React when a moving object stops                   | Enable Movement Awareness, add its Movement state, and watch `<Object>.Movement = Stopped` ([object guide](./ConvaiObjectsAndTrackedProperties.md#movement-awareness)) |
| Force-immediate transmit                           | Set `bFlushImmediately = true` (rarely)      |
