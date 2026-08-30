# Task Runtime transport

This static library is the shared Win32 transport boundary used by the
per-user Task Runtime host and its clients. It owns Runtime identity derivation,
the current-user named-pipe security contract, framed control channels, Hello
handshakes, and the injectable named-pipe connector. It depends only on
`TaskRuntimeProtocol` and Win32.

`RuntimeNamedPipeControlConnector` performs one synchronous connection attempt.
Its timeout is a single deadline covering pipe discovery, open, Hello, and
application of the negotiated control-frame limit. A successful result owns an
`IRuntimeControlConnection`; failed, cancelled, malformed, or rejected attempts
never expose a connection.

Connection operations accept a manual-reset cancellation event. A signalled
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
