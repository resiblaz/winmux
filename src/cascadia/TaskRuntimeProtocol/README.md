# Task Runtime protocol v1

This library is the process-neutral wire contract shared by the Terminal UI,
`rtask`, and the per-user Task Runtime. It has no XAML, SQLite, Git, process,
provider, or named-pipe ownership code.

## Control/event framing

Each frame is a four-byte unsigned little-endian payload length followed by
that many bytes of strict UTF-8 JSON. Empty payloads, invalid UTF-8, payloads
above the negotiated limit, oversized transport feeds, and excessive frame
bursts fail the decoder closed. A failed decoder must be explicitly reset for
a new connection.

The first control frame is a typed `hello` request. Protocol major versions
must match and minor-version ranges must overlap. Required capabilities must be
known and supported; unknown optional capabilities are ignored for forward
compatibility. The client also supplies the expected Runtime identity so a
portable or Dev client cannot attach to another product channel accidentally.

## Agent-terminal framing

The terminal stream uses this fixed 32-byte little-endian header:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `TTR1` |
| 4 | 2 | header version (`1`) |
| 6 | 2 | frame kind |
| 8 | 4 | flags |
| 12 | 4 | payload length |
| 16 | 8 | strictly increasing frame sequence |
| 24 | 8 | TaskTerminal generation (`0` for connection-level frames) |

Kinds cover Hello, Attach, Reset, Output, Input, Resize, Acknowledge, Ping,
Pong, and Error. Output and input bytes are binary. Hello, Attach metadata, and
Error payloads are UTF-8. Resize carries two little-endian 32-bit dimensions.
Acknowledgement carries the acknowledged 64-bit output sequence in its
payload; its header sequence remains the independent sequence of the
acknowledgement direction.

The framing layer validates sizes, kinds, flags, generation shape, dimensions,
acknowledgements, and monotonic sequence. Runtime session code remains
responsible for capability authorization, current-generation/input-lease
ownership, catch-up/reset decisions, queue budgets, and slow-client policy.
