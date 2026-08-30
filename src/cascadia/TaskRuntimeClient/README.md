# Task Runtime client

This static library owns the process-neutral client-side state that will sit
between `TaskRuntimeTransport` and UI/CLI projection coordinators. It never
owns XAML, Task persistence, Git, provider processes, or terminal VT streams.

M2C4c1a intentionally contains no threads, Win32 waits, connector, callbacks,
or heartbeat loop. `RuntimeClientEpochState` issues monotonic connection epochs
and makes retirement first-writer-wins. `RuntimeClientRequestRegistry`
correlates one active wire attempt per logical operation, records conservative
write transmission state, classifies disconnect actions with the protocol
policy, and retains a bounded soft tombstone ring for duplicate/late replies.

Both classes are caller-serialized. The later client supervisor will own their
mutex, generate fresh canonical request IDs for every retry, and treat an
unknown current-epoch response as a protocol fault. A response from a stale
epoch cannot mutate the active epoch. Completed tombstones may be evicted; an
extremely late duplicate then fails closed as unknown instead of being applied.
Epoch tombstones are cleared only after the old reader has joined and its
connection has closed. C1b will retire the entire epoch when a transmitted
request is locally abandoned, so this slice does not yet need an unevictable
hard tombstone. Logical operation IDs remain lifetime-monotonic in the future
supervisor; this registry intentionally enforces only one active attempt per
operation and leaves retry-attempt generation to that higher layer.
