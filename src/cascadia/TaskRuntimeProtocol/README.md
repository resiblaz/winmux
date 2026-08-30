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

After Hello, control requests, responses, and notifications use strict typed
envelopes with canonical UUID correlation IDs and object-valued bodies.
Methods declare required negotiated capabilities. A control-session state
machine enforces Hello-first ordering, bounded in-flight requests, duplicate
in-flight ID rejection, and close-state refusal before Runtime dispatch.

Clients use one discriminated decoder for response/notification payloads so
each JSON document is parsed once. Known notifications and responses return a
typed variant. An unknown optional notification returns the same typed
notification value with `UnknownNotification` status; clients must still feed
it to their subscription cursor so the committed sequence advances.

`SnapshotAndSubscribe` consumers seed a subscription ID, generation, and
committed cursor atomically. Known next notifications advance exactly by one;
duplicates are ignored and gaps or identity/generation drift require a clean
resnapshot. Unknown optional notifications retain and advance their cursor but
their payload is ignored.

The typed SnapshotAndSubscribe result contains exactly `subscriptionId`,
`generation`, `cursor`, and an object-valued `snapshot`. Unsubscribe parameters
contain exactly the matching subscription ID and generation. Provider/domain
snapshot fields remain inside the bounded snapshot object instead of leaking
into transport ownership types. Generation, cursor, and notification sequence
values use unsigned 64-bit integer-form JSON numbers; decimal and exponent
spellings are rejected rather than normalized by the JSON library.

Client disconnect policy is method- and transmission-aware. Ping, QueryTask,
and RuntimeStatus may retry with a fresh request ID. SnapshotAndSubscribe
resnapshots; Unsubscribe is never replayed across a connection. Mutating
commands may retry only before writing starts—after that their result is
`OutcomeUnknown` until a durable idempotency receipt can resolve it.

## Agent-scoped rtask commands

`SubmitCommand` carries an exact `rtask` params object for one of four methods:
`status_in_progress`, `status_in_review`, `status_blocked`, or `result`. The
object always contains canonical `commandId` and `runId` values, a 64-byte
lowercase hexadecimal capability, and the method. Only blocked/result methods
contain `text`; status methods reject it. There is deliberately no caller-
supplied Task ID.

The Runtime capability record, not the DTO, resolves the bound Task ID. A
TaskCtl Hello must negotiate `task_commands`; TerminalUi peers cannot use this
Agent-scoped SubmitCommand route even when connected to the same control pipe.

Blocked reasons are limited to 16 KiB and 4,096 Unicode scalar values. Results
are limited to 256 KiB and 65,536 scalar values. Both require nonempty strict
UTF-8 without embedded NUL. Result DTOs return the original command ID,
`applied` or `replayed`, and nonzero task/event sequences so the durable
command receipt remains the idempotency authority.

Diagnostic summaries include only validated correlation IDs, method, text byte
count, and a literal `<redacted>` capability marker. They never include the
capability or long text, and malformed JSON errors do not echo parser context.

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

The transport handshake places the existing strict control Hello request and
response JSON inside the first terminal `Hello` frame in each direction. Both
frames use sequence `1` and generation `0`; the request requires
`TerminalStream`, its Runtime identity must match the session-specific pipe
endpoint, its Runtime process-instance ID must match the active control epoch,
and the response's terminal payload limit must match the channel's
pre-negotiated bound. Session frames begin after sequence `1` and must carry
the endpoint's exact TaskTerminal generation.
