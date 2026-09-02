# AFK time is enforced reactively, and only ever extends the server's deadline

The server drops a conversation after a fixed idle period (600s at time of
writing), warning at 150s intervals with the seconds remaining, and accepts a
`reset-idle-timer` message that restarts its countdown. It has no connect-time
parameter for the idle period, so the plugin cannot tell the server what a
project's AFK Time is.

We enforce AFK Time by answering those warnings instead of running a clock of our
own: when a warning arrives, renew iff `Elapsed + remaining_seconds < AfkTime`,
where `Elapsed` is time since the last Player Activity. Nothing is sent when
there is nothing to decide, the plugin never disconnects anyone, and reading the
server's own `remaining_seconds` rather than modelling its clock means we never
had to know whether the server counts player speech or bot speech as activity —
the predicate self-corrects either way.

## Considered options

- **Periodic `reset-idle-timer` heartbeat.** Rejected: it sends traffic on every
  connected session forever to solve a problem that only exists at the moments
  the server already tells us about.
- **Plugin disconnects at AFK expiry.** This is the only way to make AFK Time
  shorter than the server's own timeout. Rejected for v1: it puts the plugin in
  charge of tearing down sessions, and creates a plugin-initiated disconnect that
  auto-reconnect would then have to learn to ignore. Revisit if a project needs
  sub-600s AFK.
- **An idle-period parameter on `/connect`.** The correct fix, and the one to ask
  backend for if this ever needs to be exact. Not available today.

## Consequences

- AFK Time cannot be shorter than the server's timeout, so the setting clamps at
  600s and defaults to it. At the default the plugin sends no renewals at all and
  behaves exactly as it did before this change — the feature is opt-in, and
  existing projects see no behavioural difference on upgrade.
- The real disconnect lands on the first server deadline at or after AFK Time, so
  it overshoots by up to one warning interval. It is never early.
- A character speaking when the budget runs out is granted one further window so
  its line can finish. Once per budget, and only the player's return earns
  another — otherwise a character that never stops talking would hold a session
  open forever, which is the thing AFK Time exists to prevent.
- 600 is a backend constant now mirrored in a client clamp. If the server's idle
  period is ever raised above it, values between 600 and the new floor become
  settable but unachievable. It is derivable at runtime — at the first warning
  after activity, `Elapsed + remaining_seconds` is the server's whole budget — if
  that ever needs to stop being a hardcoded number.
