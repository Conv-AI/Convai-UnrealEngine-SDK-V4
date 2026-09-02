# 07 — Record and replay fixtures

Status: `ready-for-agent`
Depends on: 02

## Goal

Turn every live failure into something that reproduces on demand, so the fix agent can prove
a fix rather than observe a coincidence.

## Why it matters here

Against a live backend the same scenario never runs twice identically. Without replay, an
agent patches, re-runs, sees green, and cannot distinguish "fixed" from "the model answered
differently". Replay is the loop's verification instrument.

## Scope

**Recording.** Capture the server-to-plugin direction: every `JsonData` delivered to
`UConvaiConnectionSessionProxy::HandleDataPacketReceived`, plus `OnAudioDataReceived` frames,
with arrival timestamps and the **Session Proxy** each belongs to. Packets alone replay half
the plugin — audio drives the streamer, lip sync and blendshapes.

**Replay on the arrival thread.** `HandleDataPacketReceived` is called off the game thread
today ([ConvaiActionUtils.cpp:1267](../../Source/Convai/Private/ConvaiActionUtils.cpp#L1267)
comments on exactly that). Replaying conveniently on the game thread would hide every race
the real path can hit. Reproduce the arrival thread and the inter-packet timing.

**Automatic minting.** A live failure writes its fixture without being asked — the packet and
audio window around the failure, the scenario, and the assertion that tripped. The fixture
path goes in the finding's evidence chain.

**Mutation, for edge cases live traffic will not produce.** Truncate mid-array, reorder,
duplicate, drop, inject an unknown `type`, oversize an action array, malform UTF-8. F8 shows
the plugin already meets packet types it does not handle; mutation finds the rest before the
server does.

## Out of scope

Replaying the plugin-to-server direction. Recording is one-way for now.

## Done when

A failing live scenario leaves behind a fixture; replaying that fixture reproduces the same
finding with no network; and the mutation set runs as its own offline scenario.
