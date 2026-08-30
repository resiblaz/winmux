# Task Runtime host foundation

This directory contains the Windows host boundary for the per-user Task
Runtime. The current M2C2b slice accepts multiple control clients, drives each
through Hello-first session state, and routes typed requests through an
injected interface. It deliberately stops before Task subscriptions or real
Task commands.

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
- explicit client/queue limits, connection outcome statistics, and cancellable
  shutdown of idle accept/read/write operations;
- `TaskRuntime.exe --bootstrap-check` composition/smoke entry point.

The default server accepts at most 32 clients (hard limit 64). Queue storage is
allocated only as responses are produced; each client has an 8 MiB default
outbound budget. The idle accept waits for up to 60 seconds and is cancelled
immediately during shutdown, avoiding a periodic pipe-recreation loop.

`--bootstrap-check` creates and validates the identity, security, mutex, and
first control-pipe instance, prints the protocol identity, and exits. It is not
the long-running Runtime mode. M2C3 will add targeted control notifications,
subscription ownership, and reconnect delivery over these per-client queues
without changing the identity, ACL, channel, or router contracts.
