# 11 — JSON scenario front-end

Status: `ready-for-agent`
Depends on: 04, 05, 07, 08, 09, 10 — deliberately last

## Goal

Let a new feature test be a data file instead of a rebuild, using a vocabulary derived from
scenarios that already exist rather than one invented up front.

## Why last

Designing the step vocabulary before writing scenarios means inventing a language for work
not yet done — you get steps that do not compose and a rewrite once real scenarios show what
repeats. By this point issues 04 through 10 have produced ten or more scenarios against the
step library; the recurring shapes are visible and the vocabulary is extracted, not guessed.

Accepted cost, agreed at design time: low-friction authoring arrives late.

## Scope

A step executor interpreting JSON scenario files, calling the same C++ step library the
hand-written scenarios call. Both tiers register in one registry, share the event recorder,
and produce identical artifacts — a JSON scenario and a C++ scenario are indistinguishable in
the report.

Starting vocabulary, to be revised against what issues 04-10 actually used:

```
spawn_character, set_talk_targets, say_text, speak_wav, play_character_audio,
inject_echo{delay_ms, gain}, set_object, set_property, expect_event{kind, match, within_ms},
expect_no_event{kind, for_ms}, assert_metric{name, op, value}, wait, disconnect
```

**Hard constraint: no control flow.** No loops, no conditionals, no variables beyond
parameter substitution. Steps describe what a human did and what should be observable. The
moment a scenario wants logic it is a C++ scenario — that is the design, not a limitation to
work around.

Parameter matrices are the one exception, and they belong to the Python orchestrator, not the
file: the paired AEC run is one scenario file executed twice with different `AECType`.

## Done when

At least three existing C++ scenarios are expressible as JSON files with no loss, a new
scenario can be added with no rebuild, and the vocabulary is documented with the C++ escape
hatch stated plainly.
