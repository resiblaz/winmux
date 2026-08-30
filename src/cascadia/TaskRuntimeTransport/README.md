# Task Runtime transport

This static library is the shared Win32 transport boundary used by the
per-user Task Runtime host and its clients. It owns Runtime identity derivation,
the current-user named-pipe security contract, framed control channels,
session-specific binary terminal channels, Hello handshakes, and the injectable
named-pipe connector. It depends only on `TaskRuntimeProtocol` and Win32.

`RuntimeNamedPipeControlConnector` performs one synchronous connection attempt.
Its timeout is a single deadline covering pipe discovery, open, Hello, and
application of the negotiated control-frame limit. A successful result owns an
`IRuntimeControlConnection`; failed, cancelled, malformed, or rejected attempts
never expose a connection.

The default discovery policy retains the original bounded wait for a missing
pipe. Activation coordinators may explicitly request immediate absence
detection: only an initial `ERROR_FILE_NOT_FOUND` becomes `EndpointAbsent`.
An observed busy/available endpoint, a connect race, and every Hello-stage
failure remain distinct stable reasons and can never authorize process launch.

Each terminal endpoint is deterministically bound to the validated Runtime
identity, one canonical Runtime terminal-session ID, and one nonzero Surface
generation. Its separate byte-mode pipe uses the same protected current-user
and Local System ACL, remote-client rejection, non-inheritable handles, and
first-instance guard as the control pipe. Endpoint drift fails before pipe
creation or connection.

`RuntimeTerminalChannel` carries validated `TerminalFrame` values directly.
It enforces independent strictly increasing send/receive sequences, the exact
endpoint generation for session frames, a negotiated payload ceiling, bounded
fragment/coalescing decode, and one-reader/one-writer cancellation semantics.
Terminal Hello reuses the strict control Hello JSON inside sequence-1 binary
Hello frames, requires `TerminalStream`, binds the Runtime identity to the
endpoint, matches the Runtime process-instance ID observed on the control
epoch, and verifies that the negotiated terminal payload ceiling equals the
channel bound. A rejected peer receives a bounded terminal Error frame.

Control and terminal channels share only the generic overlapped-I/O primitive;
they never share a pipe handle, decoder, lock, queue, or worker. Tests hold a
maximum-size terminal write under backpressure while completing a control
request/response, proving terminal throughput cannot head-of-line block Stop,
Close, status, or recovery traffic.

Channel operations accept a manual-reset cancellation event. A signalled
event or `CancelPendingIo` returns `RuntimeTransportStatus::Cancelled`.
Cancellation is idempotent, and broad `CancelPendingIo` makes the connection
epoch terminal without destroying the object. It must not be reused because a
cancelled write may have left a partial length-prefixed frame; wait for active
operations, close it, then reconnect. An internal
cancellation generation and I/O-start lock prevent an operation that already
entered the connection from slipping past `CancelPendingIo`. Lifecycle stops
must additionally signal their persistent event before calling
`CancelPendingIo`, so work not yet admitted also fails closed. One reader and
one writer may run concurrently; callers must wait for both operations to
finish before calling `Close` or destroying the connection.

The connector owns no worker, retry, backoff, heartbeat, request correlation,
or subscription state. Those policies belong to `TaskRuntimeClient`.
