# Convai Unreal Engine SDK

The plugin that lets an Unreal project hold a live conversation with a Convai
character. This glossary fixes the language used across the SDK so the same word
means the same thing in code, docs, and design discussion.

## Language

### Player presence

**Player Activity**:
Something the player themselves does to engage the conversation — speaking, or
submitting text.
_Avoid_: interaction, input, user activity

**AFK Time**:
The longest stretch without **Player Activity** that a **Session** is allowed to
survive.
_Avoid_: idle timeout, auto-reset timeout, session timeout

**Idle Warning**:
Notice that a **Session** is going to end in an **Idle Disconnect**, carrying how
long is left. Only raised to the game for a warning that will be allowed to come
true — one the plugin answers with an **Idle Renewal** is not a warning of
anything.

**Idle Renewal**:
A request from the plugin that the server restart its countdown, sent to keep a
**Session** alive when the **AFK Time** budget has not yet run out.
_Avoid_: keepalive, heartbeat, ping

### Connection lifetime

**Session**:
One live conversation between a player and a character, from connect to
disconnect.

**Connection Grace**:
The window during which a **Session** whose owner has let go is held open, so a
new owner claiming the same character reuses it instead of reconnecting.
_Avoid_: idle time, TTL, timeout

**Prepared Connection**:
A **Session** opened ahead of any owner so the first claim skips the handshake.
_Avoid_: warm connection, pre-connect

**Reprieve**:
The single **Idle Renewal** granted past the end of **AFK Time**, so a character
already mid-sentence is not cut off in the middle of a word. Earned back only by
**Player Activity**.

**Explicit Disconnect**:
A **Session** ended because the game or the player asked for it.

**Idle Disconnect**:
A **Session** ended by the server because its **AFK Time** budget ran out.

**Unexpected Disconnect**:
A **Session** ended by network or server fault — neither asked for nor budgeted.

## Relationships

- Every **Session** ends in exactly one of **Explicit Disconnect**, **Idle
  Disconnect**, or **Unexpected Disconnect**
- **Player Activity** restarts the **AFK Time** budget; a character speaking
  does not
- An **Idle Warning** is answered with an **Idle Renewal** only while **AFK
  Time** remains, save for one **Reprieve**
- A **Reprieve** is available once per **AFK Time** budget and is spent only to
  let a character finish a line already in progress
- **AFK Time** governs player presence; **Connection Grace** governs component
  ownership — they are unrelated clocks

## Example dialogue

> **Dev:** "The character just talked for four minutes straight. Does that count
> against **AFK Time**?"
> **Domain expert:** "No. **AFK Time** measures the player's absence, not the
> conversation's silence. Only **Player Activity** restarts it — the character
> holding forth to an empty chair is exactly the case we want to time out."
>
> **Dev:** "So we cut them off mid-word?"
> **Domain expert:** "Once, they get a **Reprieve** to finish the line. Only
> once. A character still talking after that is talking to nobody, and no amount
> of talking earns another — only the player coming back does."
>
> **Dev:** "And if the player walks away and the component is destroyed, is that
> **AFK Time** running out?"
> **Domain expert:** "That's **Connection Grace** — a different clock, about
> whether a new owner can reclaim the **Session**. Don't conflate them."

## Flagged ambiguities

- "idle time" was used for both **AFK Time** and **Connection Grace** — resolved:
  distinct concepts, distinct names. **Connection Grace** has nothing to do with
  the player.
- "disconnect" was used for all three endings of a **Session** — resolved: the
  three are named separately, because only an **Unexpected Disconnect** is ever
  a candidate for reconnection.
- "auto-reset timeout" was used to mean the server's disconnection of an idle
  **Session** — resolved: that behaviour is now named **AFK Time**, and the
  message that defers it is an **Idle Renewal**.
