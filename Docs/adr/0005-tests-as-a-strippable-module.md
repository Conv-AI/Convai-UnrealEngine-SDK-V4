# The test framework is a separate module that plugs in the way a customer would

Status: accepted, amended 2026-08-20 and 2026-08-28

**Amendment (2026-08-28).** The in-plugin harness the 2026-08-20 amendment kept is deleted, on
`refactor/one-test-runner`: its subsystem, base class, logger, six cases and connection-test
actor (22 files), the `WITH_CONVAI_TESTS` definition and
`UConvaiPlayerComponent::GetSessionProxyForTesting`. Three Blueprint debug utilities that lived
beside it — `UConvaiReplayComponent`, `UConvaiAudioChunkTestProxy`, `UConvaiTestDebugLibrary` —
stay under `Source/Convai/{Public,Private}/Tests` on the maintainer's call: developer tools, not
tests, `#if WITH_TESTS` and stripped with the folder at release. Nothing invoked it, two of its six cases
duplicated `ConvaiTests` scenarios and two could never pass. The two live checks only it carried
are `ConvaiTests` scenarios now — `action_dispatch` and `knowledge_bank_lifecycle` — and they
reach the plugin the way this ADR asks: through `OnActionReceivedEvent_V2` and the public
Knowledge Bank REST proxies.

What stays under `Source/Convai/Private/Tests` is the module's UE Automation unit tests, 111
`Convai.*` names, and they stay inside the `Convai` module deliberately. Seven of them need what
no separate module could reach: `ConvaiContextFormatTest`,
`ConvaiHeldContextLaneTest`, `ConvaiMergedObjectNavigationTest`, `ConvaiSpatialMotionTest` and
`ConvaiUtf8StringTest` include `Source/Convai/Private` headers; `ConvaiActionParsingTest` calls
`UConvaiActions`, which has no `CONVAI_API`; `ConvaiMergedObjectProximityTest` forward-declares
the unexported `ConvaiContextPrivate::ComputeNearestDistance`. Exporting internals so a test can
see them is what this ADR resists, so the `#if WITH_TESTS` gate stays in-repo and
`push_to_public_v4.bat` deletes both `Tests` folders from the public snapshot, fail-closed. The
binary-identity argument therefore holds for customers — no test code ships in any
configuration — and the divergence is confined to our own Development builds. `run.py --tier
unit` drives them, so one runner covers all three tiers.

**Amendment (2026-08-20, F16).** This decision governs the `ConvaiTests` framework and is
unchanged for it: it still reaches the plugin only through public extension points, and it is now
a `DeveloperTool` module the release script strips, so no test module reaches customers.

What is amended is the older in-plugin harness under `Source/Convai/{Public,Private}/Tests`, which
this ADR described as never having compiled. It compiled, constructed a `UGameInstanceSubsystem`
on every game instance and shipped — see F16. On the maintainer's decision it is **kept, not
deleted**, and gated with `#if WITH_TESTS` so it compiles out of Shipping and Test builds.

That is the compile-time gating this ADR rejected, and the rejection's reason still applies to
it: the `Convai` binary the suite runs against is no longer byte-identical to the shipped one.
The divergence is passive surface only. The alternative — moving that harness to its own module —
would require five Convai-private headers to become public and `CONVAI_API`-exported, which this
ADR resists under "Consequences". The trade was made knowingly; F16 records both sides.

Test code lives in its own `ConvaiTests` module, listed in the `.uplugin` and deleted by the
public-release script, so nothing test-shaped reaches customers. It reaches the plugin only
through the extension points the plugin already offers third parties: a **Virtual Mic** is a
component implementing `IConvaiAudioCaptureInterface` that `FindFirstAudioCaptureComponent`
discovers on the owning actor, and synthetic server traffic goes in through the already-public
`UConvaiConnectionSessionProxy::HandleDataPacketReceived`. No `#if`, no test-only branches, no
new public API in the shipping module.

The consequence worth having: the `Convai` binary under test is byte-identical to the one that
ships. A preprocessor-gated harness tests a different program than the customer runs, and in an
autonomous fix loop that difference is expensive — an agent can spend cycles on a bug that only
exists at `WITH_CONVAI_TESTS=1`, or miss one the test build's macro state happened to mask.

## Considered options

**Compile-time gating via `WITH_CONVAI_TESTS`**, the existing approach. Rejected for the binary
divergence above. Note the flag is currently `PublicDefinitions.Add("WITH_CONVAI_TESTS=0")`
unconditionally, so the harness under `Source/Convai/{Public,Private}/Tests` has never compiled;
that source is also not stripped by `push_to_public_v4.bat` and ships to customers today as dead
code. Both need fixing regardless of this decision.

**Runtime-switchable seams compiled into the shipping module.** Also keeps the binary identical
and would give customers an automation surface, which is a genuine SDK feature. Rejected here
because it is public API to support and version forever, and the interface-discovery route
already delivers the same property for free. Revisit only if customers ask for scripted-audio
automation as a product.

## Consequences

- The test module can only use `CONVAI_API`-exported surface. Anything it needs that is private
  today must become properly public on its own merits — not widened "for tests".
- Some invariant monitors want observation points the plugin does not expose yet (balance of
  reference-audio client attach/detach, thread of delegate delivery). Where polling public state
  cannot answer it, the hook is a design change to justify on its own, not a test back door.
- Deleting the module must not break packaging: the `.uplugin` entry and any references go with
  it, so the release script needs to edit the descriptor rather than only remove a folder.
