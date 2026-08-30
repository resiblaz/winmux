---
author: RicherLife / Codex collaboration
created on: 2026-08-30
last updated: 2026-08-31
status: active execution queue; M2E1 complete, terminal session semantics next
---

# Task-driven Terminal: M2 and M3 Execution Queue

This document freezes the implementation and verification queue used to close
M2 and M3. It refines, but does not replace, `implementation-plan.md` or the
accepted ADRs. Every row is an independently reviewable commit. `main` remains
untouched until the feature branch has passed both milestone closure gates.

## 1. Completed baseline

- M0 design baseline and M1 TaskCore are complete.
- M2A through M2C4b are complete.
- M2C4c1a/c1b provide monotonic client epochs, bounded correlation, a
  reconnecting request client, conservative mutation outcomes, and safe Stop.
- The Runtime host libraries provide secure multi-client control transport,
  atomic server-side subscriptions, the long-running `TaskRuntime.exe --serve`
  product foundation, and connect-first on-demand activation that remains
  independent from every client process and parent Job.

## 2. M2 queue

| Stage | Deliverable | Required verification |
|---|---|---|
| M2C4c2a (complete) | Pure bounded desired-subscription registry | snapshot/cursor, duplicate, unknown kind, gap, generation drift, remove-in-flight, epoch retire, ABA/capacity |
| M2C4c2b1 (complete) | Bounded pull streams and request-failure transitions | FIFO/bounds, terminal under pressure, cancellation, callback reentry, RPC failure, overflow reset |
| M2C4c2b2 (complete) | `TaskRuntimeClient` threaded subscription integration | buffered first event, reconnect/resnapshot, multi-subscription, stale epoch, bounded slow consumer, Stop/destruction |
| M2C4c3a (complete) | Virtual client scheduler replacing real backoff waits | exact virtual deadlines, Stop wake, no busy loop, request regression |
| M2C4c3b (complete) | Negotiated heartbeat/liveness | due/ack/miss, old ACK, full user queue, Stop race, backoff health reset |
| M2D1 (complete) | Long-running Runtime foundation host and concrete Ping/RuntimeStatus router | real child process, Hello, reconnect, second instance, malformed peer isolation, graceful stop |
| M2D2 (complete) | On-demand launch coordination and parent independence | connect-first, one launch under concurrent clients, launch failure/cancel, crash restart, short-lived parent test |
| M2E1 (complete) | Secure binary terminal transport | framing fragmentation/coalescing, ACL/identity/generation, payload bounds, cancellation, control/data HOL |
| M2E2 | Terminal session, input lease, bounded VT ring, deterministic Fake VT | observers/controller, stale input, resize, ack/reset/catch-up, slow peer, Esc/Stop convergence |
| M2F1 | Scoped `rtask` DTOs and capability registry | missing/expired/wrong-run/wrong-method, method bounds, redaction, idempotency |
| M2F2 | `rtask.exe`, TaskCore command router, package integration | four ADR commands, stdin/UTF-8/exit codes, duplicate command, cross-task refusal, CLI/package smoke |
| M2G1 | Five-minute terminal idle suspend and idempotent wake | active-run exclusion, attach race, new generation, Closed never auto-wakes |
| M2G2 | Helper idle exit and final parent-exit recovery | active work survives all UI clients, reconnect catch-up, idle-exit cancellation, eventual exit |
| M2 closure | Product composition and milestone evidence | subscriptions, heartbeat, real host, terminal, `rtask`, HOL, lease, parent-exit, full Release/portable gates |

### 2.1 Frozen M2 decisions

- Subscription delivery is a bounded serialized pull stream. Reader/Writer
  threads never invoke arbitrary user callbacks.
- Each logical subscription survives connection epochs. Every new epoch uses a
  new SnapshotAndSubscribe request and wire subscription identity.
- A duplicate notification is ignored. Unknown optional kinds advance the
  cursor without projection mutation. Gap, generation drift, or
  SubscriptionClosed requests a clean resnapshot.
- The first client implementation retires the whole epoch on a live gap, so no
  old reader can race a replacement snapshot.
- The negotiated heartbeat is the maximum permitted inbound silence. The
  client schedules Ping at half the interval and retires the epoch if no
  correlated response or other valid inbound message arrives by the full
  interval. Bounds remain the protocol's 250-60,000 ms.
- Internal subscription and heartbeat work has reserved capacity and cannot be
  starved by the public request limit.
- Per-subscription pull queues default to 512 updates and 4 MiB. Overflow
  retires the epoch and records resnapshot-required; it does not grow memory.
- Agent-terminal live output defaults to a 4 MiB / 4,096-frame reconnect ring.
  Durable history remains structured and never stores raw VT.
- Terminal projection suspension remains five minutes. After all runs are
  inactive and all terminal sessions are Suspended/Closed, the helper uses an
  additional two-minute idle-exit grace. New activity cancels either timer.
- `rtask` capabilities are random, method/run-bound, renewable per active turn,
  expire after ten minutes at most, and are invalidated immediately when the
  run becomes terminal.
- Update/drain across executable versions remains an M8 concern; M2 fails a
  mismatched protocol closed and supports clean restart.

## 3. M3 component boundary

```text
TaskCore                         typed records, transactions, SQLite, recovery
TaskExecution -> TaskCore       canonical paths, Direct leases, Git lock/runner
TaskGit -> TaskExecution+Core   snapshot, Worktree, checkpoint, finalize
TaskRuntimeService              Store/Execution/Git orchestration only
TaskRuntime.exe                 composition root
```

TaskCore never runs Git or owns Named Mutexes. TaskExecution and TaskGit never
depend on Runtime transport, XAML, TerminalApp, or provider code.

## 4. M3 queue

| Stage | Deliverable | Required verification |
|---|---|---|
| M3A | Typed Git/worktree records, SQLite v4, identity freeze and delete gate | v3 migration, rollback, replay/conflict, first-run freeze, recovery, undelivered-work refusal |
| M3B | Canonical directory identity, Direct FIFO lease, Git common-dir mutex | Unicode/alias/reparse, cancellation, abandoned owner, two Runtime identities/processes |
| M3C | Structured Windows Git runner and repository inspector | argv/env injection, timeout/cancel/tree kill, output/UTF-8 bounds, linked/detached/SHA-1/SHA-256 |
| M3D | Read-only faithful source-state capture | clean/staged/unstaged/untracked/ignored, 2,000/200 MiB bounds, symlink/vanish, digest race, source unchanged |
| M3E | Worktree provision, replay, verification, baseline and setup recovery | deterministic collisions, every crash phase, hook/sign/identity/lock/copy/persist failure, no fallback |
| M3F | Per-TaskRun checkpoint and fake-run integration | preserve Agent commits, add-A, no empty commit, all terminal outcomes, exactly-once recovery, Worktree reuse |
| M3G | Explicit finalize/cleanup, Delete guard and recovery | exact path/ref proof, empty/produced branch, failure preservation, crash resume, late-event tombstone |
| M3H | Cross-process concurrency, soak and milestone gate | Direct FIFO, same-repo Git serialization, different-repo parallelism, abandoned mutex, repeated restart/finalize |

### 4.1 Frozen M3 decisions

- Use new `TaskExecution` and `TaskGit` static targets. Do not move M3 code into
  TerminalApp's existing Git helpers or grow TaskCore into a process library.
- Registered and portable Runtime identities receive separate managed data
  roots. Worktrees live under `<runtime-data>/task-worktrees/v1/<task-id>`.
- A versioned marker records TaskId, normalized common-dir digest, deterministic
  branch, baseline, and a random ownership nonce. Cleanup requires every field
  plus `git worktree list --porcelain` to agree.
- Existing directories use a handle-resolved final path plus volume/file
  identity. Drive/system roots, unsafe reparse traversal, and unresolved path
  aliases fail closed. First-slice Worktree mode rejects UNC roots.
- Direct locks and Git common-dir locks use current-user Windows Named Mutexes
  keyed by a versioned SHA-256 canonical identity and are independent of
  product channel. A missing cross-process lock never degrades to process-only.
- Direct waits have no timeout but are cancellable and FIFO within one Runtime.
  Git critical sections use a cancellable 30-second deadline and bounded
  diagnostics.
- Tracked dirty state uses `git stash create` without changing the source stash,
  index, or worktree. The Task Worktree applies that object with index state;
  non-ignored untracked files are copied from a NUL-delimited manifest.
- The manifest limit is 2,000 files / 200 MiB. Entries record relative path,
  type, size, and SHA-256. Symlinks/reparse entries, disappearing files, and any
  source/destination digest mismatch fail setup.
- Clean source state uses source HEAD as baseline. Dirty reconstructed state
  creates a Task-internal baseline commit. Empty commits are forbidden.
- Branches use `refs/heads/wt-task/<canonical-task-id>`. An existing branch or
  directory is adopted only with persisted ownership proof; otherwise setup
  fails without a timestamp suffix.
- Worktree mode fails closed for bare/unborn repositories, sparse checkout,
  submodules, and required Git LFS/filter/network material. It never silently
  falls back to Direct.
- Managed commits use `git add -A`, `--no-verify`, and a command-local disabled
  hooks path, while preserving configured signing. Fallback identity is
  `Richer Task Runtime <task-runtime@localhost>` only when repository and user
  identity are both absent. Signing failures preserve the Worktree.
- Direct mode holds its directory lease but never stages, commits, baselines,
  or rewrites the user's Git state.
- Broad `git worktree prune`, `RemoveAll`, force deletion outside an exact
  managed marker, reset, checkout, stash push/pop, amend, rebase, merge, push,
  or source-branch mutation are prohibited.
- Product cleanup removes only versioned Runtime sidecars. Files such as
  `AGENTS.md`, `CLAUDE.md`, and Agent edits are legitimate work and are never
  treated as sidecars.
- Minimum supported Git is 2.38. Git runs with terminal prompts disabled,
  sanitized repository-selection environment, bounded output, and no shell.
- All non-ignored untracked files within the accepted bounds are captured to
  satisfy faithful visible-state reconstruction. A later UI may preview the
  manifest; M3 does not silently omit files based on content heuristics.

## 5. Verification discipline

For every stage:

1. deterministic component tests and negative/recovery cases;
2. CRLF-aware `diff --check`, XML/project graph checks, and independent review;
3. focused unified native Release gate;
4. stage-specific race or temporary-repository matrix in repeated rounds;
5. an independent commit with a clean worktree.

M2 and M3 closure each additionally require the full unified Release/MSIX,
Hook, native, CLI, and portable packaging gates. Build outputs are preserved in
accordance with `AGENT.md`; no clean build is part of this queue.
