# Convai Objects & Tracked Properties

The **Convai Object Component** makes any Actor visible to every chatbot in the
level: drop it on a door, crate, lever, or room and every chatbot pulls it into
its known-objects list at session start — no per-chatbot authoring. This guide
covers the four live systems the component feeds: tracked properties, movement
awareness, gaze & attention, and spatial awareness. (For navigation movement —
access points, reachability, the reusable `Convai Move To` node, and the
lower-level `Resolve Goal Location` resolver — see
[ActionsV2.md](./ActionsV2.md).)

## The object itself

**Enabled** is the master switch (on by default). Untick it — or call
**Set Convai Object Enabled** at runtime — and the object is invisible to the
whole system: no environment entry, no tracked-property broadcasts, no spatial
awareness, no gaze. Disabling mid-session cleans the object out of every
chatbot (states, environment entry, attention slot) exactly like destroying
the component would; re-enabling runs the full registration again.

Fill in **Object Entry**:

- **Name** — what every chatbot calls the object. Must be unique in the level;
  the subsystem auto-suffixes duplicates (or merges them — see below).
  Whitespace is tidied at registration (trimmed, doubles collapsed); casing
  is kept — names are speech text the AI says out loud.
- **Description** — what the object *is*, in plain language for the AI.
  Identical descriptions are sent once: when several objects register the
  same non-empty text, later ones reach the AI as `Same as <first>.` (three
  coins sharing one long description cost one description, not three).
- **Object Is / Component Name / Socket Or Bone Name / Acceptance
  Radius / Movement Points** — how AI movement targets the object
  ([ActionsV2.md](./ActionsV2.md)). Tick **Create Separate Destination** on a
  movement point and it becomes its own AI-addressable target
  (`"Door Other Side"`) with independent proximity and reachability — see the
  separate-destinations section there.

**Merged sets**: tick *Merge With Same-Named Objects* on a pile of identical
props (same Name + same Merge Group Index) and the AI perceives ONE logical
object — centroid position, shared description, group highlight on gaze, and
movement resolves at action time to the nearest reachable live member,
independent of spatial-awareness polling. The centroid is context only, never
the movement destination. In spatial wording, **already at** is the hard
arrival fact; **close by** is only a distance band and does not skip movement.

The stock `Escort {character: ref} to {destination: ref}` action row is disabled
by default. A custom exact named `Escort` handler can pass these same entries to
the reusable **Convai Escort** async node. Despite its general `Reference` wire
type, `character`
resolves semantically and case-insensitively from `Environment.Characters`;
`destination` can resolve to the object, a merged member selected by policy, or
a separately named movement-point destination. Connect the chatbot owner's
AI-controlled pawn to **Escorting Actor** and the two resolved entries to
**Escorted Character** and **Destination**. Convai Escort composes the reusable
movement task: whole-actor targets stay tracked as actors, while retained
component/socket or movement-point locations refresh during the move. The
resolver—not the raw engine result—decides whether the destination was reached.
Companion lag/catch-up uses full 3D distance, and task teardown withdraws any
still-held follow prompt. See
[ActionsV2 → Escort](./ActionsV2.md#escort).

## Tracked Properties

Each entry in **Tracked Properties** picks a UPROPERTY on the owning Actor the
AI should stay aware of — a "door is locked" bool, a health float, an enum
state. Property paths may hop through object references into sub-actors,
components, and instanced objects (e.g. `Turret.Health`); every hop is
re-validated each poll, so a null or destroyed reference pauses tracking
instead of breaking it. Pure functions on hopped-to objects are pickable too.

- **Alias** — optional; the chatbot then sees `<ObjectName>.<Alias>` instead of
  the full dotted path.
- **Description / State Value Descriptions** — tell the AI what the property
  and its values mean.
- **Should Respond** — whether a change updates the AI silently (`Never`) or
  also nudges it to react (`Auto` / `Always`).
- **Delivery** — when a nudging change reaches the chatbots: `Send Normally`
  (next scheduled send) or `Wait Until Conversation Is Idle` (delivered once
  the conversation has stayed quiet for the chatbot's `Quiet Time Before
  Delivery` seconds — no upper bound on the wait, so it never interrupts an
  ongoing exchange). Available when Should Respond is `Auto` or `Always`.
- **Flush Immediately** bypasses the normal context debounce for a genuine
  change. With idle delivery it still waits for silence, then sends at the
  first idle instant. This row is intentionally visible rather than hidden
  under Advanced because nested Advanced rows are unreliable in some supported
  Unreal Details panels.
- Choosing **Should Respond = Never** resets Delivery to `Send Normally`, resets
  Flush Immediately to false, and disables both controls. A one-shot `Watch
  Property` can still upgrade one matching change to `Always`, but a property
  authored as Never uses normal, non-flushed delivery for that watch
  ([ActionsV2 → built-in experimental actions](./ActionsV2.md#built-in-experimental-actions)).
- Paths survive Blueprint variable renames: each segment latches the variable's
  GUID and re-latches in-editor when the name stops resolving (best effort).
- Runtime mutators: `Add Tracked Property`, `Remove Tracked Property`,
  `Update Tracked Property`, `Get Tracked Properties`.

## Movement awareness

Every object included in spatial awareness is sampled on the same shared clock.
While it is genuinely moving, its existing spatial fact gains a short,
observer-relative clause such as `moving upward slowly`, `moving toward you`,
`moving to your right quickly`, or `rotating`. Ordinary speed has no adjective;
ordinary stationary objects have no extra spatial wording. **Enable Movement
Awareness** is on by default; turning it off removes movement wording, movement
state publication, and responses for this object while ordinary position,
relation, and reachability awareness keep working. A movement watch cannot fire
while the state is disabled; an already-armed watch remains dormant.

**Sensitivity** decides how much coherent translation or rotation counts as
movement. **Medium** is the balanced default and preserves the original
detector behavior. **Very Low** recognizes only clear movement and suits noisy
physics objects; **Low** ignores more drift and collision bumps; **High** detects
slow or subtle motion; and **Very High** detects extremely small motion but may
also notice physics noise. A prospective direction must keep coherent raw and
rolling evidence and make sensitivity-scaled progress before replacing an
established trip direction. At **Low**, a direction-churning terminal shake
within about 60 cm is treated as settling and remains latched until it leaves a
70 cm release margin; use a more sensitive profile when a
small intentional shuttle must remain `Moving`. The same detector drives both
spatial wording and the optional state below, so there is no parallel movement
inference for the declarative fact.
Changing sensitivity during play silently establishes a fresh sample baseline;
the setting change itself cannot request a response or consume a watch.

Start/stop hysteresis and confirmation windows suppress easing tails and
physics jitter. Detection uses a short rolling net-transform trend plus one
prospective-direction candidate. Bounded back-and-forth collision bumps retain
the completed trip direction and may settle after the normal stop window;
coherent travel that exits the profile's envelope becomes the new direction.
If that travelled segment later changes into a different endpoint-bounce
direction, the bounce gets a fresh local envelope instead of inheriting the
trip's earlier escape.
Rotation remains strict so a spinner cannot become `Stopped` merely because its
centroid stays put. The selected **Object Entry** target is sampled (whole actor,
or the resolved specific component), so a moving child mesh should be selected
as the object reference.

For gameplay that must react to an edge, enable **Add Movement State** on the
Convai Object Component. This adds one synthetic state:

```
<ObjectName>.Movement = Stopped | Moving | Upward | Downward | Toward You | Away | Left | Right | Rotating
```

The initial value is seeded silently. **When Movement Starts** and **When
Movement Stops** independently choose `Never`, `Auto`, or `Always`; these are
the notification controls, not tracking switches. The state keeps updating when
either is `Never`. Delivery and Flush Immediately apply only when at least one
transition can request a response; with both set to `Never`, they return to Send
Normally and off. The state is also discoverable by `Watch Property`, and the
same opt-in makes the passive surroundings fact say `stopped` whenever that is
the current state. Turning **Add Movement State** off removes that state,
explicit stopped wording, watchability, and transition responses, but active
movement wording continues while Movement Awareness remains enabled. Disabled dependent
rows keep their authored choices when either feature switch is temporarily
turned off; re-enabling establishes a silent current baseline rather than
inventing a transition.

Idle delivery coalesces to the latest truth; it is not an every-edge event
queue. If a responsive stop is waiting for conversation idle and the object
starts again first, the newer moving state supersedes that held stop. For a
time-critical platform stop that must reach the character even while it is
speaking, use **Delivery = Send Normally** and **Flush Immediately = true**.
With **When Movement Starts = Never**, only the responsive stop flushes.

For a platform, the AI can arm `Platform.Movement = Stopped` so the preceding
start does not consume the watch. Enable **Watch Property Action** on the
companion/chatbot for that flow. Every confirmed edge preserves its short
transition history: `Platform.Movement is now Upward (was Stopped)` and
`Platform.Movement is now Stopped (was Downward)`. This remains accurate when a
fast round trip is coalesced into one context batch or held until conversation
is idle. A confirmed start or stop supersedes older pending or held motion data
and its response request in both the state and surroundings fact. Direction-only
relabels are not detector edges and may still coalesce with the active edge. If
a silent direction relabel follows a queued start, the final direction is
updated without erasing its `was Stopped` origin; a direction watch that actually
fires instead reports the direction it followed. Spatial/relation refreshes that
occur while a responsive Movement value waits for conversation idle remain in
that same held lane, so the durable state and surroundings sentence cannot
publish contradictory motion.

The key already says `Movement`, so directional values stay concise instead of
repeating `Moving`: a physical turn can read `Movement is now Upward (was Left)`.
`Moving` is only the fallback when no reliable single direction can be resolved;
it is not a wildcard for every active direction.
Vertical direction is world-relative; horizontal direction is relative to the
observing character. Discrete labels stay current if that relationship changes,
including when the object passes the character or the character turns. Those
perspective-only corrections stay silent and do not consume a watch. A physical
change in the object's motion direction is also silent by default, but can
satisfy a one-shot `Watch Property`; use the stable `= Stopped` target when only
the physical stop matters. Low-confidence settling motion retains the last
useful direction until the confirmed stop rather than publishing collision
jitter as a new direction. Speed remains fact-only to avoid spending state
updates on ordinary acceleration. Only confirmed start and stop edges use the
configured response policy.

Movement tells the AI whether and how an object is moving, not which authored
stop it reached. If an elevator or platform needs exact level knowledge, expose
the controller's own enum as an ordinary tracked property, for example
`MovingPlatform.CurrentLevel = Lower | Upper | Between`. That small structured
state is more reliable and cheaper than trying to infer named floors from
arbitrary geometry or continuous vision.

`<ObjectName>.Movement` is reserved while **Add Movement State** is enabled.
If an authored tracked property or Alias collides with it, the synthetic state
is suppressed and the component logs a warning. For merged same-named objects,
each enabled member uses its own sensitivity; any qualifying member starts the
logical object, while every enabled member must be quiet before it stops. The
strongest start/stop response wins across members; idle delivery and immediate
flushing also win when any contributing responsive member requests them.

## How a change reaches the chatbot

All objects sample on one shared clock (default 0.25 s — the
`ObjectPollIntervalSeconds` custom param). Each tick the component formats
every tracked property's current value and compares it with the last snapshot;
on change it pushes a **context state** (`<ObjectName>.<Alias-or-Path>` =
value) to every chatbot. States land in the chatbot's canonical context block.
The complete canonical order is states, persistent facts (including spatial
facts), events, then the chatbot's derived `Instructions` and active `Current
action plan` blocks. The last two replace in place and are omitted when empty;
they are not tracked-property history. A changed value also emits a `Key is now
Y (was X)` delta line at the prompt tail. Chatbots that connect later are
seeded from the cache; changes during the first ~1 s after BeginPlay never
trigger a response (startup grace). Whitespace in keys is Pascal-cased away,
and multi-word values are quoted in the prompt. See
[ActionsAndEnvironment → Dynamic context](./ActionsAndEnvironment.md#dynamic-context)
for the managed action sections.

Within one debounce or idle-delivery window, an ordinary state that goes
`A -> B -> A` has no net change, so the pending update and its response request
are cancelled. Synthetic movement uses the same state pipeline but marks its
confirmed detector edge as transition-significant: a brief
`Stopped -> Upward -> Stopped` trip still arrives as
`Movement is now Stopped (was Upward)`.

## Gaze & attention

While *Gazeable* is on (default), the player's gaze (line trace + dot-product
fallback from the Convai Player Component) can land on the object:

- **Highlight** — `On Gazed In` / `On Gazed Out` fire immediately; merged sets
  highlight together unless *Highlight Only This Object When Gazed* is set.
- **Attention** — dwell long enough and the object is promoted to every
  chatbot's "object in attention" slot (`On Attention Gained` / `On Attention
  Lost`); gaze never tramples a slot set explicitly from BP/C++. The player
  component's `Gaze Delivery` setting can hold the promotion cue until the
  conversation pauses (`Wait Until Conversation Is Idle`) so a glance can't
  make a character interrupt itself; if the player looks away before the cue
  lands, it's cancelled.
- Turn *Gazeable* off for props that should exist for actions/state tracking
  but never react to being looked at.

## Spatial awareness

With *Include In Spatial Awareness* on (and the system enabled in Project
Settings ▸ Plugins ▸ Convai ▸ Spatial Awareness), the context subsystem
composes one fact per included subject for every chatbot, e.g.:

> The Crate is close by, in front of you and to your left, reachable by walking.
> You are already at the PressurePlate, which is underneath you and User.
> The MovingPlatform is underneath you, stopped, and is reachable by walking.

- **Distance & direction** are egocentric to the observing chatbot. The current
  close by / some distance away / far away bands use straight-line world
  distance, with the Nearby and Moderate thresholds configured in Project
  Settings (defaults: 1,000 cm and 4,000 cm). They do not measure the walking
  route; `reachable by walking` means that a valid path exists, not that it is
  short. `you`, `your left`, and similar wording always mean the character
  receiving the fact, not the player; explicitly named people use their own
  frame.
- Per-chatbot **Surroundings / Relations Response** decide whether changes nudge
  the AI (`Never` default), and the matching **Surroundings / Relations
  Delivery** settings can hold a nudging change until the conversation pauses
  so the character doesn't interrupt itself to comment on someone walking by.
  The first spatial snapshot is always seeded with `Never`, even when either
  response setting is `Auto` or `Always`; those settings apply only to later
  changes. This keeps the initial spatial facts from prompting an unsolicited
  opening line.
- **Reachability** comes from real navmesh pathfinds — for objects, other
  characters, and the player alike — cached until either end moves ~100 uu
  (plus a recompute the moment sub-threshold motion stops), and corridors
  with un-walkable vertical jumps are rejected. Navmesh changes refresh
  verdicts with nothing moving: when a door opens, every cached verdict
  re-checks about a third of a second after the mesh settles. Two triggers
  feed this — the engine's rebuild-completed event, and (faster in busy
  scenes) any registered object with navigation-affecting geometry moving.
  Every current verdict is explicit: an arrived observer gets `You are already
  at the <object>` (or `already with <person>`) with no walking verdict. If a
  person is merely close across a wall or on another floor, that does not count
  as `already with`; the short gap must also have a valid nav path. If a
  relation matters it stays in the arrival sentence (`You are already at the
  Platform, which is underneath you and stopped`), keeping the destination
  explicit without repeating its name. Otherwise,
  a valid route says `reachable by walking`, while a blocked route says `with no
  walking path there`. A door opening thus replaces the old negative belief
  with a positive one instead of merely dropping the negative phrase.
- **Relations**: subjects in tight contact classify from their pawn-blocking
  bounds — "on top of" / "underneath" / "next to" — and a support relation
  lists every entity in contact ("underneath you and User", capped at three).
  Looser pairs get a single directional relation ("behind and to the left of
  the Crate"). A sentence keeps the subject once — for example, `The Platform
  is underneath you and is reachable by walking`, not `...and the Platform is
  reachable...`. Facts only re-send when their sentence changes.
- Line of sight gates each subject fact: a subject the chatbot can't see is withheld.
  Separate destinations authored on movement points are the one exception in
  where the trace aims: they check visibility of their *base object* — a
  door's far side is occluded by the very door it belongs to, but its
  existence is authored knowledge, known whenever the door is seen.
