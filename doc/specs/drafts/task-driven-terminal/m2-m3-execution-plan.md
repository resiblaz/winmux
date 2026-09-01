---
author: RicherLife / Codex collaboration
created on: 2026-08-30
last updated: 2026-09-01
status: active execution queue; M2 closure and M3A-M3G complete, M3H next
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
- The terminal foundation provides secure binary transport, a bounded live VT
  ring, per-client queues, snapshot/replay/reset, one versioned controller
  lease, deterministic Backend injection, and targeted slow-peer isolation.

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
| M2E2 (complete) | Terminal session, input lease, bounded VT ring, deterministic Fake VT | observers/controller, stale input, resize, ack/reset/catch-up, slow peer, Esc/Stop convergence |
| M2F1 (complete) | Scoped `rtask` DTOs and capability registry | missing/expired/wrong-run/wrong-method, method bounds, redaction, idempotency |
| M2F2 (complete) | `rtask.exe`, TaskCore command router, package integration | four ADR commands, stdin/UTF-8/exit codes, duplicate command, cross-task refusal, CLI/package smoke |
| M2G1 (complete) | Five-minute terminal idle suspend and idempotent wake | active-run exclusion, attach race, new generation, Closed never auto-wakes |
| M2G2 (complete) | Helper idle exit and final parent-exit recovery | active work survives all UI clients, reconnect catch-up, idle-exit cancellation, eventual exit |
| M2 closure (complete) | Product composition and milestone evidence | subscriptions, heartbeat, real host, terminal, `rtask`, HOL, lease, parent-exit, full Release/portable gates |

### 2.1 Frozen M2 decisions

Closure evidence is recorded in
[`m2-closure-report.md`](./m2-closure-report.md).

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
- Terminal Input/Resize dispatcher work and Release carry the lease revision
  captured at receipt. The revision is sideband session state, not injected
  into raw provider input bytes; stale queued work fails closed after lease
  expiry, disconnect, or reacquisition.
- Terminal projection suspension remains five minutes. After all runs are
  inactive and all terminal sessions are Suspended/Closed, the helper uses an
  additional two-minute idle-exit grace. New activity cancels either timer.
- `rtask` capabilities are random, method/run-bound, renewable per active turn,
  expire after ten minutes at most, and are invalidated immediately when the
  run becomes terminal.
- Capability material is 32 system-random bytes encoded as 64 lowercase hex
  bytes. The bounded registry defaults to 1,024 active Runs; same-Run,
  same-method-set issuance renews idempotently without creating another token.
- Blocked reasons are bounded to 16 KiB / 4,096 Unicode scalars and results to
  256 KiB / 65,536 scalars. Both are strict nonempty UTF-8 with no embedded
  NUL, and neither capability nor text is included in diagnostics.
- Capability authorization resolves both Task and TaskRun; the wire DTO never
  accepts Task identity. Runtime issues only for Starting/Running Runs and the
  TaskCore transaction rechecks the binding and active state before mutation.
- Agent status/result, event append, Task version, and command receipt commit in
  one SQLite transaction. Receipt replay compares Task and semantic request
  digest but ignores the server-selected CAS version, so a duplicate command
  remains replayable after the first commit or Runtime restart.
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
| M3A (complete) | Typed Git/worktree records, SQLite v4, identity freeze and delete gate | v3 migration, rollback, replay/conflict, first-run freeze, recovery, undelivered-work refusal |
| M3B (complete) | Canonical directory identity, Direct FIFO lease, Git common-dir mutex | Unicode/alias/reparse, cancellation, abandoned owner, two Runtime identities/processes |
| M3C (complete) | Structured Windows Git runner and repository inspector | argv/env injection, timeout/cancel/tree kill, output/UTF-8 bounds, linked/detached/SHA-1/SHA-256 |
| M3D (complete) | Read-only faithful source-state capture | clean/staged/unstaged/untracked/ignored, 2,000/200 MiB bounds, symlink/vanish, digest race, source unchanged |
| M3E (complete) | Worktree provision, replay, verification, baseline and setup recovery | deterministic collisions, every crash phase, hook/sign/identity/lock/copy/persist failure, no fallback |
| M3F (complete) | Per-TaskRun checkpoint and fake-run integration | preserve Agent commits, add-A, no empty commit, all terminal outcomes, exactly-once recovery, Worktree reuse |
| M3G (complete) | Explicit finalize/cleanup, Delete guard and recovery | exact path/ref proof, empty/produced branch, failure preservation, crash resume, late-event tombstone |
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

### 4.2 M3A closure evidence

- TaskCore owns bounded typed Worktree/checkpoint records and SQLite schema v4.
  The v3 migration preserves multiple valid legacy checkpoints for one Run,
  and an injected migration failure proves schema changes and `user_version`
  roll back together.
- Every TaskRun insertion freezes its ExecutionContext identity in the same
  transaction. Both direct Run creation and message-claim Run creation use the
  shared persistence entry point; event failure rolls back the Run, message
  claims, version, and freeze bit. A Task cannot be created pre-frozen.
- Worktree initialization is one CAS/receipt transaction bound to a frozen
  Worktree ExecutionContext. Exact replay succeeds, request/command reuse and
  stale versions fail closed, and Direct, unfrozen, or cross-Task contexts are
  rejected.
- Aggregate load, bounded recovery, and Runtime activity expose typed Git
  state. Delete refuses every Worktree that is not Finalized, still has
  undelivered work, or is not cleanup-eligible.
- The focused unified Release gate passed with TaskCore 69/69,
  TaskRuntimeProtocol 71/71, TaskRuntimeActivation 66/66,
  TaskRuntimeClient 132/132, and TaskRuntime 182/182. The complete TaskCore
  suite then passed 100/100 repeated rounds (6,900/6,900 test executions).
- The split Release/MSIX plus CLI gate and portable repackaging passed. The
  resulting MSIX is 8,112,516 bytes with SHA256
  `691419342283F75863C719FD9B36F949226316F41F3B97C9D81049529FDD5207`;
  the portable ZIP is 11,995,735 bytes with SHA256
  `237933253BB4E8EC38EC771D093D082E7729E5DAE5A7AE2B51E368A4323CBCB0`.
- `git diff --check`, LF/final-newline inspection, and both modified MSBuild
  project XML parses passed without errors.

### 4.3 M3B closure evidence

- A separate `TaskExecution` static target now owns directory identity,
  digest/security helpers, the Direct FIFO lease, and the Git common-directory
  lock. `TaskRuntimeHost` references the target, while TaskCore remains free of
  process, filesystem-handle, and Named Mutex ownership.
- Existing directories are opened by handle, resolved with
  `GetFinalPathNameByHandleW`, identified by volume serial plus 128-bit file ID,
  and reopened to prove the resolved alias still names the same object. Unicode,
  case and extended-path aliases converge; missing/file/relative/oversized/NUL,
  drive root, Windows system subtree, device, and reparse attributes fail
  closed. Worktree policy rejects UNC before filesystem access.
- Direct leases use one process-wide FIFO registry in front of a mandatory
  cross-process Mutex. Waiting has no timeout, cancellation removes any queued
  waiter without reordering successors, reentrant owner acquisition is
  rejected, and the RAII lease remains owner-thread affine.
- Git common-directory locks use a cancellable, bounded deadline of at most 30
  seconds. Different directory identities remain parallel. Direct and Git use
  separate lock domains while both remain independent of product channel or
  Runtime identity.
- Mutex names are versioned SHA-256 identities over current-user SID, lock
  domain, and canonical directory digest. Creation uses a protected
  current-user/System DACL plus exact Medium/NoWriteUp label; every opened
  object is revalidated. Broad-ACL precreation fails closed, and both Direct
  and Git report abandoned owners while retaining safe acquisition semantics.
- `TaskExecution.LocalTests` passes 13/13 tests, including two child-process
  cases proving that distinct registered/portable Runtime labels serialize on
  the same Direct or Git lock. The final suite passed 100/100 repeated rounds
  (1,300/1,300 tests and approximately 400 child processes).
- The unified Release/MSIX, complete native-test graph, and CLI smoke gate
  passed with TaskCore 69/69, TaskExecution 13/13, TaskRuntimeProtocol 71/71,
  TaskRuntimeActivation 66/66, TaskRuntimeClient 132/132, and TaskRuntime
  182/182. Portable repackaging also passed.
- The resulting MSIX is 8,112,513 bytes with SHA256
  `B480F69134F1B0376ACD2ABC617B1CDFCE3DBAC8FF826CA69F5C43B8E283ACA8`;
  the portable ZIP is 11,995,719 bytes with SHA256
  `0EAFCD60D724899FDBF5059E274844FEB9619B15C0E7B3D517F3E596AB5C2547`.

### 4.4 M3C closure evidence

- `TaskExecution` now provides a structured `GitCommandRunner` with explicit
  executable, canonical working-directory identity, argv vector, allowlisted
  author/committer environment overrides, cancellation handle, deadline, and
  independent stdout/stderr bounds. It never invokes a shell.
- Windows command-line encoding follows the CRT quote/backslash rules for every
  argument. The child environment removes all inherited `GIT_*` selectors and
  `GCM_INTERACTIVE`, then installs fixed non-interactive prompt/config policy.
  Metacharacter, quote, trailing-backslash, Unicode, embedded-NUL, duplicate,
  reserved-variable, and replaced-directory cases fail or round-trip exactly.
- `git.exe` must be a local, handle-resolved regular file below a canonical
  non-reparse directory. Each process starts suspended with an exact inherited
  handle list, enters a kill-on-close Job before resume, and uses two bounded
  drain threads. Timeout, cancellation, output overflow, invalid UTF-8, launch,
  pipe, Job termination, and nonzero Git exit remain distinct outcomes.
- The runner deadline covers preparation and execution. Cancellation is polled
  before launch and wins at the process wait; Job termination is verified before
  returning. A native fake `git.exe` proves descendant tree kill and 50 repeated
  process/pipe/Job cycles prove handle stability.
- `GitRepositoryInspector` requires Git 2.38 or newer, resolves the common-dir,
  acquires its Git lock, and rechecks the exact common-dir identity under lock
  before reading worktree/git-dir, bare/inside state, object format, HEAD, and
  symbolic ref. It preserves UTF-8 branch refs and rejects bare, unborn, plain,
  stale-directory, malformed-output, timeout, and cancellation cases.
- Real Git 2.38.1 tests cover nested/main/linked/detached Unicode worktrees,
  SHA-1 and SHA-256 repositories, common-dir contention, and different git-dir
  versus common-dir identities. `TaskExecution.LocalTests` passes 27/27.
- The final M3C Git matrix passed 100/100 rounds (1,700/1,700 test executions).
  The unified Release/MSIX, complete native-test graph, CLI smoke, and portable
  repackaging gates passed with TaskCore 69/69, TaskExecution 27/27,
  TaskRuntimeProtocol 71/71, TaskRuntimeActivation 66/66,
  TaskRuntimeClient 132/132, and TaskRuntime 182/182.
- The resulting MSIX is 8,112,520 bytes with SHA256
  `AF29D5405F7538AA322C674C32D45315AC0BF7737D71657AED6460AD5F800E92`;
  the portable ZIP is 11,995,727 bytes with SHA256
  `5958C7A2916CBFBFEB1AE1CC95F5B68C35720CBA79A983A173841E64BC6A6C84`.

### 4.5 M3D closure evidence

- The new `TaskGit` static target owns source inventory, safe untracked-file
  hashing, tracked-state proof, and canonical snapshot digests. Runtime only
  references this target as a composition boundary; TaskCore, transport, XAML,
  TerminalApp, and provider code remain outside the implementation.
- Capture verifies repository identity, acquires the Git common-directory
  Mutex, and verifies identity again under lock. It compares three tracked
  worktree/index tree proofs, three porcelain-v2/NUL inventories, two hashes of
  every untracked file, the stash ref, and a final repository inspection before
  returning a snapshot.
- Dirty tracked state uses `git stash create` with command-local fallback
  identity. The transient commit is retained for replay but excluded from the
  canonical digest because commit timestamps are nondeterministic; equal trees,
  status, manifest, repository identity, and stash ref therefore produce the
  same digest across captures.
- Untracked manifest records explicitly include relative UTF-8 path, regular
  file type, size, and SHA-256. Handle-resolved final paths, parent traversal,
  file identity/basic metadata, exclusive read sharing, and a second digest
  reject escapes, reparse/device/directory entries, disappearance, replacement,
  or content races. The fixed bounds are exactly 2,000 files and 200 MiB.
- Real SHA-1/SHA-256 temporary repositories cover clean, staged plus unstaged,
  nested Unicode untracked, ignored, and pre-existing stash states. Tests prove
  HEAD, branch, logical index entries, worktree contents, status, and refs/stash
  remain unchanged. Deterministic injections cover tracked/stash races, vanish,
  second-hash drift, cancellation, malformed hasher records, and resource
  failures; a real Windows symlink/reparse case fails closed on this machine.
- `TaskGit.LocalTests` passes 16/16. The stage matrix passed 20/20 capture/race
  rounds (140/140), 100/100 path/status rounds (600/600), and 5/5 limit rounds
  (15/15), for 755/755 repeated test executions in addition to focused and full
  gates.
- The unified Release/MSIX, Hook, complete native-test graph, CLI smoke, and
  portable `-SkipBuild -NoLaunch` gates passed. The resulting MSIX is 8,112,504
  bytes with SHA256
  `CB8F02B1126B289A92D0E0F0215927CC546E7D27EA629C8B4D913E624844E75C`;
  the portable ZIP is 11,995,698 bytes with SHA256
  `622D4BD59C4C4DC8F5CFA0312BA44622CBB4BF77C9077B14927CBD1B6FAA7ECC`.

### 4.6 M3E closure evidence

- TaskCore persists the monotonic setup phases `Planning -> CapturingSource ->
  Provisioning -> RestoringSource -> CreatingBaseline -> Ready`, plus explicit
  Failed evidence. Runtime advances at most one durable phase per call, so a
  restart repeats only the idempotent external operation for the recorded phase.
- Deterministic layouts use
  `<runtime-data>/task-worktrees/v1/<task-id>` and versioned `.state` files. The
  ownership marker and immutable source journal are checksum-protected and bind
  TaskId, ownership nonce, repository identity, source HEAD/ref, tracked trees,
  stash object, status/snapshot digests, and the bounded untracked manifest.
  Journal publication precedes marker publication; recovery can therefore
  restore a missing marker without recapturing changed source state, while an
  orphan marker fails closed.
- Preflight holds the common-directory Mutex and rejects sparse checkout,
  submodules in any captured tree, LFS/required clean-smudge filters, promisor
  or partial-clone material, missing objects, unsafe paths, and deterministic
  branch/path collisions. Existing resources are reused only when persisted
  ownership proof and exact detached/branch registration agree; no timestamp
  branch, broad prune, force cleanup, or Direct fallback exists.
- Provision creates a detached Worktree first, verifies its exact registration,
  then attaches `refs/heads/wt-task/<task-id>`. A crash or injected branch-switch
  failure preserves the detached Worktree and resumes it on the next call.
  User checkout hooks are proven active in tests and bypassed command-locally
  for managed setup.
- Tracked state is restored with `stash apply --index`; non-ignored untracked
  files use handle-verified source/destination paths, exclusive source reads,
  SHA-256 before and after copy, a versioned Runtime staging file outside the
  Worktree, atomic no-replace move, and idempotent identical-file reuse. Real
  source, destination-parent, staging, and runtime-root reparse cases fail
  closed. Final tracked tree, index tree, manifest, ownership, and every file
  digest are verified twice.
- Clean snapshots use source HEAD directly and cannot create an empty baseline
  commit. Dirty snapshots use `git add -A`, disabled hooks and `--no-verify`,
  preserve configured signing, and use the Runtime identity only when both
  configured name and email are absent. Incomplete identity and signing
  failures preserve the staged candidate. A nonce-bound subject, exact parent,
  tree diff, clean status, and marker CAS allow recovery after commit succeeds
  but marker replacement or SQLite persistence fails.
- The Runtime integration test injects a transactional event-write rollback
  after each successful external phase (capture, provision, replay, baseline),
  proves the database remains at the prior phase, then reaches Ready through
  idempotent recovery. Cancellation is durably classified as Failed and
  requires an explicit retry transition.
- `TaskGit.LocalTests` passes 55/55 and the complete Runtime suite passes with
  the two new setup-recovery cases. Hook tests pass 246 assertions. The unified
  Release/MSIX build and complete native gate passed; the initial CLI check met
  a transient live-instance exit code 8, then the formal CLI-only gate passed
  without terminating or restarting the user's Terminal process.
- The resulting MSIX is 8,116,550 bytes with SHA256
  `F8009895498892BAC42D3DB1918D3AD9149F7D8F349C87DE605BB60E3FBF058C`;
  portable `-SkipBuild -NoLaunch` produced an 11,998,934-byte ZIP with SHA256
  `0363D6B7D770EC98CB34421AB31BD8605A78D30E219D44E4FE538BB25C51ADAE`.

### 4.7 M3F closure evidence

- TaskCore now owns idempotent begin/advance commands for one checkpoint per
  TaskRun. Checkpoint and Worktree revisions advance in the same SQLite
  transaction, and event/receipt failure rolls both records and the Task
  version back together. Pending, Inspecting, Committing, Succeeded,
  NotNeeded, and Failed evidence is bounded and included in recovery/activity
  queries; a failed or incomplete prior checkpoint blocks the next TaskRun.
- Start capture records the managed branch HEAD and a SHA-256 dirty-state
  digest and requires the persistent Worktree to be clean. End inspection
  verifies that start history remains an ancestor, uses `git add -A`, captures
  bounded UTF-8 changed paths and numeric diff statistics, and creates no
  commit when the index already equals HEAD.
- Runtime checkpoint commits use a deterministic Run-bound subject, disabled
  hooks plus `--no-verify`, configured identity when complete, bounded fallback
  identity only when both fields are absent, and unchanged signing policy.
  Exact parent, branch ref, non-empty tree diff, and clean status prove the
  commit without amending, rebasing, squashing, merging, pushing, or rewriting
  Agent commits.
- Commit recovery recognizes the exact already-created checkpoint when Git
  succeeds before SQLite publication. Tests inject rollback after start
  capture, staged inspection, and commit publication; every retry converges
  without a duplicate commit. Signing failure preserves the staged Worktree,
  blocks a new Run, and resumes only through explicit retry.
- Fake-run integration covers Completed, Failed, Cancelled, and Interrupted,
  preserves an Agent-authored commit beneath the Runtime checkpoint, and runs
  a second no-change TaskRun against the same Worktree path and branch. SHA-1,
  SHA-256, Unicode paths, active failing hooks, missing identity, signing
  failure, and post-commit observation failure are covered.
- Focused suites pass with TaskGit 60/60 and TaskRuntime 188/188; the four new
  TaskCore checkpoint transaction/policy tests also pass. The complete
  Task-only unified incremental Release gate passed with all TaskCore,
  TaskExecution, TaskGit, protocol, activation, client, and Runtime tests plus
  Runtime/rtask command-line checks. No build cache or user process was
  cleaned, stopped, or restarted.

### 4.8 M3G closure evidence

- TaskCore now persists the explicit `Ready -> Finalizing -> Finalized` path
  and the recoverable `Finalizing -> Preserved -> Finalizing` path. Each edge
  is an idempotent CAS/receipt transaction; active TaskRuns and Pending,
  Inspecting, Committing, or Failed checkpoints block cleanup. A non-Ready
  managed Worktree also blocks creation of another TaskRun.
- Finalization revalidates the versioned marker, source journal, normalized
  common-directory identity, exact managed path, deterministic branch ref,
  Worktree registration, branch HEAD, and baseline ancestry while holding the
  repository Mutex. It never uses broad prune, reset, checkout, force removal,
  force branch deletion, merge, rebase, push, or source-branch mutation.
- Remaining non-ignored dirty state is preserved with `git add -A`, disabled
  hooks, `--no-verify`, the accepted identity fallback, and unchanged signing
  policy. A nonce-bound subject, exact single parent, non-empty tree diff,
  clean status, and ref equality recover a commit that completed before the
  next durable phase without duplicating or rewriting Agent commits. Ignored
  files fail closed because ordinary Git staging cannot prove their delivery.
- Git removes the exact clean Worktree without `--force` only after the Task
  branch carries the final commit. A produced branch remains; a branch equal
  to baseline is deleted only through `update-ref -d <ref> <expected-old>`.
  Only the three deterministic Runtime sidecar files are removed, individually
  and marker-last; shared directories and Agent files are never recursively
  deleted.
- Recovery covers commit-before-observation, Worktree/branch removal before
  SQLite publication, partial marker-last sidecar cleanup, and repeated calls
  after complete removal. Failures before removal persist `Preserved` plus the
  exact Worktree path. Once the branch is safe and the Worktree is gone,
  remaining sidecar/store failures leave `Finalizing` and resume without
  repeating Git removal.
- Delete remains a separate intent. TaskCore refuses it before Finalized plus
  cleanup-eligible evidence, deletes aggregate content transactionally after
  safe finalization, retains the bounded tombstone, and rejects a late
  finalization command as Tombstoned. Finalization leaves Task status/history
  unchanged.
- Focused finalization suites pass with TaskCore 3/3, TaskGit 6/6, and Runtime
  5/5. The complete Task-only unified incremental Release gate passes with
  TaskGit 66/66 and TaskRuntime 193/193 plus all TaskCore, TaskExecution,
  protocol, activation, client, and command-line checks. No cache or user
  process was cleaned, stopped, or restarted.

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
