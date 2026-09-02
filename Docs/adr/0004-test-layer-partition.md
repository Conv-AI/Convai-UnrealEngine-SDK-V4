# Echo cancellation is measured in the client library, not in Unreal

Status: accepted

`convai_client` and this plugin each carry their own test framework, and they test different
things about the same feature. The client library owns AEC *quality* — its
`IAudioEchoCanceller::ProcessMicrophoneStream` cancels in place, so an offline test can feed a
known far-end and near-end pair and read the residual back as an ERLE figure in milliseconds,
with no network and no engine. The plugin owns AEC *plumbing* — that **Reference Audio** is
captured at the right rate, without gaps, and fanned to every live **Connection**, and that the
microphone reaches exactly the **Talk Target** set. Neither repo tries to cover the other's half.

The same line runs through the rest of the suite: the deep protocol matrix (error codes,
reconnect permutations, chaos) is a library concern and stays in `convai_client`'s harness. The
plugin keeps a small, broad product-acceptance suite instead, because the plugin is what ships to
customers and "the library passed" is not evidence that the deliverable works.

## Considered options

**One Unreal framework tests everything end to end.** Attractive because there is one place to
look and one report to read. Rejected: every AEC assertion would then route through a live
backend and server-side ASR, which can only answer "did echo leak, yes or no". A ten-decibel
regression passes that test. Measuring cancellation requires the residual signal, and the
residual only exists inside the library.

**Test AEC only in the library and trust the plugin's wiring.** Rejected: the failure we
actually expect is a feed bug — a submix recorder that stopped, a client never registered for
reference audio, a rate mismatch — and none of those are visible from inside the library. It
also leaves `SetStreamDelay` unexercised, which the plugin currently never calls at all.

## Consequences

- An Unreal AEC test asserting "no player transcript during character speech" is meaningless
  alone: a silent fixture passes it. Every such test runs paired, once with cancellation on and
  once off, and the off-run must fail. This generalises — any assertion that something did *not*
  happen needs a control proving it can.
- The plugin ships against a prebuilt `convai_client` binary while that repo's tests run at its
  HEAD. Green library tests say nothing about the shipped plugin unless the versions are pinned
  together.
- Reading a failure may mean opening two repos. Accepted deliberately: the alternative is a
  single suite where the fast deterministic checks are hostage to backend availability.
