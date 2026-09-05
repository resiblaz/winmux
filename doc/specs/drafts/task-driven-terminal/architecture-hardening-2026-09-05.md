# Task architecture hardening

Scope: the requested architecture findings 1, 4, and 5.

## Business revision and telemetry

`AppendTaskProviderEvent` commits ordered events and receipts without changing
`Task.Version` or `Task.UpdatedAt`. Business commands retain optimistic version
checks. Delayed telemetry validates ownership and creation timestamps instead
of comparing the latest business revision. Existing event cursors continue to
drive log refresh and replay; no schema migration is required.

## Independent Task operations

The provider registry uses short state locks and per-Task operation gates.
External adapter calls release the registry lock, and adapter identity fences
reject completions from a replaced provider. Capacity reservations include
in-flight starts. Events for a busy Task are queued while other Tasks continue
receiving events; provider close IO runs outside the event pump.
New events cannot overtake older deferred events for the same Task when its
operation gate becomes available during an event read. Capacity reclamation
still permits successive Runs of the same Task.

JSON-RPC correlates concurrent requests by ID with bounded pending/notification
queues. One cooperative reader dispatches replies and notifications. Timed-out
and cancelled request IDs tolerate late replies. Shutdown signals IO before
disposing handles, and transport generations isolate requests across restart.
Stopped adapters cannot initialize again through a late call from an old
coordinator operation; restart creates a replacement adapter.

The scheduler dispatches lifecycle IO to a bounded worker pool, serializes it
per Task, and reserves one slot for Stop/Close/Delete. Run preparation and
checkpointing finish before worktree cleanup proceeds. Shutdown cancels and
joins lifecycle workers. Idle suspension rechecks eligibility after provider IO.

## Committed Workspace topology

WorkspaceCollection batches mutations behind a nested update scope. The
committed callback runs after tabs, view items, MRU entries, active surfaces,
and selection have been reconciled. Native drag completion explicitly commits
the reordered topology. It is a publication boundary, not a rollback mechanism
or suppression of XAML's own collection notifications.

AgentHistoryCoordinator uses explicit Workspace view, selection, retention,
history, and session-opening ports instead of TerminalPage private fields.
TerminalPage no longer exposes TaskPage's implementation header transitively.

## Verification

Regression coverage includes telemetry/business concurrency and receipt replay;
out-of-order RPC replies, cancellation, late replies, and restart; a blocked
Task start alongside another Task's commands/events; stale provider completion;
slow close alongside another Task's Stop; shutdown worker joining; and nested
Workspace commits with add/reorder/remove invariants.

- The full Release gate passed **831 native cases**. After the final adapter
  stop and event-order corrections, the unified incremental graph rebuilt the
  package/tests, CLI smoke passed again, and **27 Provider + 13 orchestrator
  cases** passed. The latest results cover **832 distinct passing cases**,
  including 10 added in this architecture pass; see
  [case results and log hashes](architecture-hardening-2026-09-05/validation-results.json).
- **246 Hook assertions** passed. Build caches were preserved. The first
  compile exposed missing declarations after removing a transitive TaskPage
  include; explicit declarations/includes corrected the boundary.
- The isolated portable deployment matches the built GUI, Runtime, and
  TerminalApp DLL hashes; see
  [environment](architecture-hardening-2026-09-05/environment.json).
- Real Codex execution printed eight progress lines, wrote
  `architecture-smoke.txt`, and verified the exact content `architecture-ok`.
  [Telemetry sampling](architecture-hardening-2026-09-05/real-telemetry.json)
  captured 49 successive observations where the event cursor advanced while
  the business version and update timestamp stayed unchanged.
- Closing the completed Task retained the
  [Tasks page](architecture-hardening-2026-09-05/04-closed-stays-in-tasks.png).
  After terminating only the isolated Runtime, the same GUI survived and
  [reconnected](architecture-hardening-2026-09-05/05-runtime-reconnected.png)
  to a new Runtime process; see
  [process evidence](architecture-hardening-2026-09-05/runtime-reconnect.json).
- The [final state](architecture-hardening-2026-09-05/final-state.json) has no
  active Runs. The previously recorded bare-`rtask` PATH issue was observed
  again and remains tracked in the [earlier report](fix-verification-2026-09-05.md).
  This pass did not add long-duration stress or a Narrator/DPI test matrix.
