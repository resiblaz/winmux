# Task Runtime client

This static library owns the process-neutral client between
`TaskRuntimeTransport` and future UI/CLI projection coordinators. It never owns
XAML, Task persistence, Git, provider processes, or terminal VT streams.

## M2C4c1 request client

The client has one supervisor, one reader per active connection epoch, and one
writer per active epoch. The supervisor exclusively connects, publishes and
retires monotonic epochs, applies reconnect backoff, joins both I/O workers,
and only then closes the connection. Stop closes request admission first,
signals every blocking operation, and completes every accepted logical
operation exactly once.

Requests are conservatively marked `WriteStarted` before transport `Send`.
Response correlation and epoch retirement share one state lock, so the first
one to acquire it wins. A read-only request interrupted after transmission may
retry with a fresh lifetime-unique canonical request ID. A transmitted mutation
never replays automatically and completes as `OutcomeUnknown`. Requests that
were not sent remain queued for the next epoch.

Unknown current-epoch responses and unsupported notifications fault the client;
duplicate or late responses are ignored through bounded tombstones. Permanent
connect failures fault without a hot retry. A successful Hello does not reset
backoff by itself: a correlated response first has to prove the epoch healthy.

The public completion is a `shared_future`; I/O threads only satisfy its
promise and never invoke a user callback. Supervisor completion, retry, and
retirement buffers are reserved before admission opens so retirement and Stop
do not lose accepted promises or transmission policy while staging results.
Each epoch serializes cancellation against Close and has a worker-count barrier;
Close cannot reset a connection until Reader, Writer, and concurrent cancellation
have returned. Injected connector, transport, request-ID, and backoff calls,
connection cancellation/close, and promise delivery all occur outside the
client state lock.

This slice deliberately rejects `SnapshotAndSubscribe` and `Unsubscribe` and
does not yet implement notifications or heartbeat liveness. M2C4c2 adds desired
subscription state, snapshot/cursor recovery, and resnapshot handling;
M2C4c3 adds negotiated heartbeat and virtual-time liveness tests.
