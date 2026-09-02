# 12 — Release stripping, and the harness that is not dead

Status: `ready-for-human`
Depends on: 02

*Scope 1 closed by deletion, 2026-08-28 on `refactor/one-test-runner`: the harness is gone —
its subsystem, cases and connection-test actor (22 files), `WITH_CONVAI_TESTS` and
`GetSessionProxyForTesting`; the three Blueprint debug utilities stay under `Public/Tests` as
developer tools — and its two checks with unique coverage are `ConvaiTests` scenarios
(`action_dispatch`, `knowledge_bank_lifecycle`). What remains under `Private/Tests` is the
module's `WITH_TESTS` unit tests, and `push_to_public_v4.bat` now deletes both `Tests` folders
from the public snapshot, fail-closed, instead of checking each file for its guard. F16's "what a
customer loses" no longer applies: nothing test-shaped ships in any configuration. Scope 4 and 5
are still the human's.*

*Scope 2 and 3 were done 2026-08-20 on `fix/strip-in-plugin-harness` — see F16 for the
measurements — and scope 1 was first resolved by **gating, not removal**: the maintainer's
decision then was to keep the harness, so all 46 files were wrapped in `#if WITH_TESTS`. Its two
open questions are answered in F16: construction registers commands and nothing else, and yes, a
command is harmful in a customer build — `Convai.Test.Run` takes a `CharacterID=` off the console
and opens billed sessions with the shipped title's credentials.*

*Left for a human: **scope 4**, packaging the stripped plugin into a clean project, and **scope
5**, confirming `Content/Submixes/AudioInput.uasset` reaches customers by the other route. The
release script's new steps were dry-run against a copy of the tree, but no clean-project package
was built.*

*Rewritten 2026-08-06 (session 2) against F16. The original issue was written on the belief
that `Source/Convai/{Public,Private}/Tests` had never compiled. It compiles, it runs, and it
ships. That changes what this issue is: a behavioural change to a shipping module, not a
deletion of dead source. The corrections are listed under "What the original issue got wrong"
so nobody re-derives them.*

## Goal

Ensure no test code reaches customers — including the test code that reaches them today.

## Current state, all re-verified 2026-08-06

**The in-plugin harness is live.** `UConvaiTestHarnessSubsystem` is an unguarded
`UCLASS()`, `CONVAI_API`, `UGameInstanceSubsystem` and `FTickableGameObject`
([ConvaiTestHarnessSubsystem.h:31-32](../../Source/Convai/Public/Tests/ConvaiTestHarnessSubsystem.h#L31-L32)).
It overrides no `ShouldCreateSubsystem`, so the engine constructs it on every game instance in
every configuration, and `Initialize` registers five console commands
([ConvaiTestHarnessSubsystem.cpp:256-284](../../Source/Convai/Private/Tests/ConvaiTestHarnessSubsystem.cpp#L256-L284)):

```
Convai.Test.Run    Convai.Test.RunAll    Convai.Test.Abort
Convai.Test.Report Convai.Test.List
```

Observed answering in a headless `-game` run with no test module loaded. `push_to_public_v4.bat`
does not strip the `Tests` folders, so customers receive this and can invoke it.

**`WITH_CONVAI_TESTS` guards almost nothing.** `PublicDefinitions.Add("WITH_CONVAI_TESTS=0")`
at [Convai.Build.cs:258](../../Source/Convai/Convai.Build.cs#L258) is unconditional, and
`ConvaiTestMacros.h:7-8` defines it to `0` if it is not already defined. Of the 46 files under
the two `Tests` directories, **three** reference it — `ConvaiTestMacros.h` itself and one block
each in `ConvaiTest_Audio.cpp:180` and `ConvaiTest_EndToEnd.cpp:230`. The subsystem is not among
them.

**`ConvAI.uplugin` already lists `ConvaiTests`** alongside `Convai`, `ConvaiEditor`,
`ConvaiAnimGraph`, `ConvaiVisionBase` and `ConvaiToolset`. Deleting the folder without editing
the descriptor leaves a module entry pointing at nothing, which fails packaging for the
customer rather than for us.

**The strip script's current behaviour.** It removes `.graphifyignore`, `graphify-out`,
`push_to_public_v4.bat`, `CLAUDE.md`, `.scratch`, `Docs`, `Content` (restoring
`Content/Skills`) and `Source/ThirdParty`, and fails closed on any surviving `*graphify*` path
([push_to_public_v4.bat:105-155](../../push_to_public_v4.bat)). Nothing test-shaped is checked.

## What the original issue got wrong

| Claim | Reality |
|---|---|
| "The harness … has never compiled" | It compiles, constructs, ticks and registers commands in every build |
| "`GetSessionProxyForTesting()` is public and not gated" | It **is** gated, by `#if WITH_CONVAI_TESTS` at [ConvaiPlayerComponent.h:236-241](../../Source/Convai/Public/ConvaiPlayerComponent.h#L236-L241), and the macro is `0`. It does not ship. Nothing to decide |
| (F16) "43 files, zero occurrences of the macro" | 46 files, three reference it |
| (F16) "registers `Run`, `RunAll` and `List`" | Five commands; `Abort` and `Report` too |

## Scope

### 1. Remove the live harness — as a behavioural change

Deleting `Source/Convai/Public/Tests` and `Source/Convai/Private/Tests` removes a
`UGameInstanceSubsystem` that currently constructs in every customer build. Treat it as the
removal it is:

- **Establish what is reachable before deleting.** The five console commands are known. What is
  not known, and F16 records as unestablished, is whether anything in that tree has side effects
  at subsystem construction beyond registration, and whether any command does something harmful
  when invoked in a customer build. Answer both before deleting, because the answers decide
  whether this is also a security note in the release.
- **Delete the `WITH_CONVAI_TESTS` definition and its three call sites together**, including the
  `#ifdef` block in `ConvaiPlayerComponent.h` — that one lives *outside* the `Tests` folders and
  removing the folders will not take it.
- Port anything worth keeping into `ConvaiTests` first. Issue 02 already took the parts it
  wanted.

### 2. Strip `ConvaiTests` at release

`push_to_public_v4.bat` must remove `Source/ConvaiTests` **and** drop the `ConvaiTests` entry
from `ConvAI.uplugin`. Edit the descriptor with the same `ConvertFrom-Json` PowerShell the
script already uses for `VersionName`, rather than by text substitution.

### 3. Fail closed on anything test-shaped

The `*graphify*` sweep at [:149-155](../../push_to_public_v4.bat) is the pattern. Add the same
for `Source/ConvaiTests`, `*Tests*` under `Source/Convai`, and `WITH_CONVAI_TESTS` appearing
anywhere in the snapshot. A strip that silently half-works is worse than one that stops.

### 4. Verify the stripped output builds

Package the stripped plugin into a clean project; confirm it compiles and connects. This is the
check that catches a descriptor edit gone wrong, and it is worth running on every release.

### 5. While in the release script: the `Content` strip

Out of scope to fix here, in scope to state. `Content` is removed as dependency-managed, and it
holds `Content/Submixes/AudioInput.uasset` and `MuteMic.uasset`. If those do not reliably reach
customers by the other route, `_FoundSubmix` is null, the plugin's own capture component is
unrouted, and every customer is in F19's position — microphone in the master mix and inside
**Reference Audio** — with only a `Warning` to say so. F6 raised this as a risk; F19 measures
what it costs. Confirm the delivery route, or file it.

## Out of scope

Whether an automation surface should ship as a product feature. ADR-0005 records that as
revisitable if customers ask — and F16 strengthens the case for the separate module, since the
shipping binary contains test surface today.

## Done when

A stripped package contains no test module, no `Tests` folders and no `WITH_CONVAI_TESTS`;
`ConvAI.uplugin` matches what is on disk; the script fails closed on any of the three surviving;
and the stripped plugin builds and connects in a clean project.
