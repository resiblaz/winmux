# Task Runtime host foundation

This directory contains the Windows host boundary for the per-user Task
Runtime. The current M2C3 slice accepts multiple control clients, drives each
through Hello-first session state, routes typed requests through an injected
interface, and delivers targeted subscription notifications. It deliberately
stops before a reconnecting client implementation or real Task commands.

Implemented foundations:

- stable identity from normalized product channel, settings root, and current
  user SID;
- distinct protocol identity, Global instance/launch mutex names, and control
  pipe name per Runtime identity;
- protected, non-inheritable security descriptors granting access only to the
  owning user and Local System, with a medium-integrity no-write-up label;
- RAII single-instance and overlapped byte-mode named-pipe server handles;
- remote named-pipe clients rejected at the kernel pipe mode;
- bounded overlapped accept, connect, read, and write operations with
  cancellation, disconnect classification, and fail-closed frame decoding;
- client/server Hello exchange with Runtime identity, protocol, capability,
  and negotiated payload-limit validation;
- a bounded multi-client acceptor with independent reader/writer workers so a
  stalled Hello or slow response consumer cannot block another client;
- per-client queues that count pending and in-flight bytes, disconnect only the
  peer that exceeds its queue/write budget, and retain request IDs in flight
  until their response write completes;
- capability-gated typed RPC dispatch through a cancellation-aware,
  provider-neutral router seam;
- two-phase SnapshotAndSubscribe planning: the router allocates subscription
  identity, Runtime installs a bounded Pending reservation, then the router
  atomically registers its event source while producing the snapshot;
- events arriving during snapshot creation are buffered, validated against the
  negotiated payload limit, and batch-queued strictly after the snapshot
  response with no first-event race;
- per-subscription generation/cursor enforcement, targeted multi-window
  delivery, explicit Unsubscribe/SubscriptionClosed cleanup, and fail-closed
  duplicate-owner detection;
- explicit client/queue limits, connection outcome statistics, and cancellable
  shutdown of idle accept/read/write operations;
- `TaskRuntime.exe --bootstrap-check` composition/smoke entry point.

The default server accepts at most 32 clients (hard limit 64). Queue storage is
allocated only as responses are produced; each client has an 8 MiB default
outbound budget. The idle accept waits for up to 60 seconds and is cancelled
immediately during shutdown, avoiding a periodic pipe-recreation loop.
Each client may own at most 64 subscriptions by default. Pending snapshot
notifications reserve space for the response itself and share the client's
bounded queue budget; overflow disconnects only that client.

`--bootstrap-check` creates and validates the identity, security, mutex, and
first control-pipe instance, prints the protocol identity, and exits. It is not
the long-running Runtime mode. M2C4a now defines the client-side envelope,
Snapshot DTO, and disconnect replay/outcome policy. M2C4b now provides the
shared `TaskRuntimeTransport` library and a cancellable, single-deadline
connect-plus-Hello seam. M2C4c will add the reconnecting client supervisor,
request correlation, and subscription resnapshot policy execution.
