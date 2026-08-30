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

The public request API deliberately rejects direct `SnapshotAndSubscribe` and
`Unsubscribe`; callers use `Subscribe` and its bounded pull stream instead.
Logical subscriptions survive connection epochs while every epoch receives a
fresh wire identity and snapshot. Reader delivery is snapshot-first, advances
unknown optional events without exposing them, ignores duplicates, and retires
the whole epoch on a cursor gap, generation drift, stream overflow, or remote
closure. Writer work reserves request bytes and concurrency independently from
public requests, correlates internal responses without callbacks, and sends
unsubscribe before new snapshots.

Each stream and the whole client have retained-memory limits. Permanent local
delivery failure ends at the configured snapshot-attempt ceiling. Removal is
allocation-free at the stream integration seam, terminal delivery remains
available when the ordinary queue is full, and Stop/destruction wins over a
simultaneous retry ceiling by closing remaining streams with `ClientStopped`.
An item that exceeds a negotiated epoch frame limit faults only that logical
subscription; it does not fault unrelated requests or subscriptions.

Reconnect backoff now uses an injected monotonic scheduler with absolute
deadlines and explicit wake-source results. Production uses bounded Win32
waits; deterministic tests advance manual time. Stop wins an exact-deadline
race, and invalid zero-delay policies, clock overflow, or wait failure fault
the client instead of entering a reconnect loop. Transport I/O timeouts,
epoch lifecycle waits, and public pull deadlines remain independently owned.

The negotiated heartbeat uses the same scheduler without adding a timer
thread. Supervisor owns half-interval and full-silence deadlines, Reader only
rearms after current-epoch correlation or valid subscription delivery, and the
single Writer sends reserved-capacity Ping work ahead of ordinary requests.
Other valid inbound traffic supersedes an outstanding Ping; its late ACK is
ignored by a client-namespaced monotonic ID high-water and cannot refresh
liveness. Stop and epoch retirement remain higher priority than Ping or
timeout, while a true silence timeout reconnects through the normal backoff.

M2D next composes these client/runtime foundations into the long-running
product helper and on-demand activation path.
