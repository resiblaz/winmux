---
author: RicherLife / Codex collaboration
created on: 2026-08-29
last updated: 2026-09-01
status: M0-M8 complete on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal: Proposed Implementation Plan

## 1. Gate and scope

This plan is the accepted architecture baseline and staged implementation is
authorized. The original gate was satisfied on 2026-08-30 after:

- the browser prototype is updated to the accepted product interactions and
  reviewed;
- design documents pass consistency review;
- existing unrelated working-tree changes are preserved or committed under
  their own scope.

The first release is local-first and Codex-only. It includes all accepted
foundational contracts required to avoid disposable data/process designs.

### 1.1 Current progress

- M0-M3 are complete: the accepted product/architecture baseline, TaskCore,
  secure per-user Runtime/protocol, execution contexts, locks, and recoverable
  Git Worktree lifecycle are implemented and verified.
- M4-M5 are complete: one Runtime-owned Codex App Server provider and durable
  structured event sink back the fixed Tasks page, immutable projection
  coordinator, in-page draft, execution log, queued messages, and Run Details.
- M6 is complete: task-owned terminal pipes, generation-bound Surface ownership,
  one input controller, reconnect, semantic Esc, Runtime-authorized close, and
  window-exit detach are integrated without changing ordinary user terminals.
- M7 is complete: the bounded background orchestrator executes Direct and
  Worktree runs, folds/defer messages, retries allowlisted failures, processes
  lifecycle intents, suspends idle terminal sessions, and excludes Task Codex
  sessions from Summary.
- M8 is complete: protocol 1.2 drain/update behavior, MSIX/portable payload
  validation, prototype updates, user documentation, migration/rollback,
  security/privacy, storage, and upstream-sync reviews are recorded.
- Closure evidence is in the M2 through M8 closure reports in this directory.
- Microsoft upstream synchronization work is isolated from Task milestones and
  is not merged into this branch.
- The executable M2/M3 commit queue and frozen technical defaults are tracked
  in [`m2-m3-execution-plan.md`](./m2-m3-execution-plan.md).

## 2. First-slice capabilities

### Included

- fixed Tasks page beside Summary;
- persistent Task aggregate and immutable TaskRuns;
- per-user on-demand Runtime helper that outlives UI windows;
- current-user-secured control/event and terminal named pipes;
- Runtime-only `workspace-tasks.db` writer;
- scoped `rtask.exe` control CLI;
- one shared, capability-gated Codex App Server adapter;
- synthesized Agent-terminal VT projection;
- single-Pane/single-controller task-owned Surface;
- centralized SurfaceClosePolicy;
- Git-default per-Task Worktree and explicit Direct mode;
- faithful source snapshot, baseline, per-run checkpoints, and safe finalization;
- Direct path leases and Git common-dir locks;
- durable follow-up queue and immediate Agent-terminal steering;
- Stop, ClosePending, Close, Suspend/Wake, Cancel, Done, Reopen, Delete;
- structured execution log, failures, recovery, and transient retry policy;
- strict Task versus Summary/history ownership;
- multi-window read projection and one input-controller lease;
- crash/restart/window-exit recovery;
- localization, keyboard, screen-reader, high-contrast, and large-text support.

### Explicitly deferred

- providers other than Codex;
- cloud/server synchronization and collaboration;
- squads/sub-agents owned by this product;
- Kanban/Gantt/project-planning suite;
- multiple panes or writable mirror Surfaces for one Task;
- moving task-owned Surfaces between windows/Workspaces;
- automatic push, merge, rebase, PR creation, or deployment;
- deleting delivered result branches from ordinary Delete Task;
- full raw-VT history;
- MCP control surface in addition to `rtask`;
- customizable retention/export and advanced Worktree policies.

## 3. Proposed native component layout

Names are provisional; boundaries are required.

```text
src/cascadia/TaskCore/
  task records, state machines, commands, events, SQLite store,
  recovery, directory leases, Worktree lifecycle

src/cascadia/TaskRuntimeProtocol/
  versioned DTOs, control/event framing, terminal framing,
  capabilities, client/server validation

src/cascadia/TaskRuntime/
  per-user helper executable, scheduler, App Server adapter,
  named-pipe server, session/turn ownership, projections

src/cascadia/TaskCtl/
  scoped rtask.exe client

src/cascadia/TerminalApp/Tasks/
  TaskPage, ViewModels, projection coordinator, dialogs/panels,
  fixed-page integration and SurfaceClosePolicy adapter

src/cascadia/TerminalConnection/
  TaskRuntimeConnection or an equivalent fork-owned connection adapter

src/cascadia/LocalTests_TaskCore/
src/cascadia/LocalTests_TaskRuntime/
  deterministic native tests and fake Provider/pipe peers
```

Shared code must not depend on XAML. Runtime code must not depend on
`TerminalPage`, Tab, Pane, renderer, or UI ViewModels.

## 3.1 Modularity and maintainability constraints

Dependency direction is one-way:

```text
TaskRuntimeProtocol      TaskCore
         \                /
          \              /
             TaskRuntime
             /         \
        TaskCtl      TerminalApp Tasks UI
```

No lower layer imports a UI or Microsoft Terminal application-layer type.

Required seams:

- `ITaskStore` / transaction boundary;
- `IAgentProviderAdapter`;
- `IWorktreeManager`;
- `IDirectoryLeaseManager`;
- `IRuntimeTransport` and terminal transport;
- `ITaskProjectionSource`;
- `ISurfaceClosePolicy` integration port;
- clock, process launcher, filesystem/Git runner, and ID generation test seams.

Source organization rules:

- one file/class has one primary responsibility;
- separate records/DTOs, validation, state transitions, SQL queries, migrations,
  orchestration, transport, provider translation, Git operations, ViewModels,
  and XAML controls;
- no `TaskManager.cpp`, `TaskRuntime.cpp`, `TaskPage.cpp`, or equivalent file
  becomes a catch-all for unrelated behavior;
- `TaskApplicationService` coordinates interfaces but does not contain SQL,
  JSON framing, Git commands, Codex method parsing, or XAML state;
- XAML code-behind handles view interaction only; product rules live in tested
  ViewModels/application services;
- Provider-specific `if (codex)` branches stay inside the Codex adapter;
- generated protocol/schema artifacts, if used, live in an isolated generated
  directory and are never hand-edited;
- reusable helpers are named for a domain concept, not accumulated in a generic
  `Utils` dumping ground;
- Win32 handles, SQLite statements, pipe state, and App Server transport use
  RAII/Pimpl or equivalent compile firewalls to limit include and lifetime
  coupling;
- public interfaces expose bounded typed values rather than raw JSON, SQL rows,
  process handles, or XAML controls.

File-size review heuristic:

- a hand-written implementation approaching roughly 500 lines triggers a
  responsibility review;
- exceeding the heuristic requires a documented reason such as a cohesive
  state table, generated binding, or protocol fixture;
- line count is a review signal, not permission to create artificial fragments.

Change-set rules:

- each milestone/commit has one architectural purpose and its tests;
- new behavior extends an interface or adds a focused component instead of
  growing central switches;
- no feature commit combines protocol, storage, Git, and XAML rewrites unless
  it is an explicit narrow integration step;
- tests mirror production component boundaries and use fakes at interfaces;
- reviewers reject circular dependencies, hidden singletons, cross-layer
  database access, and business logic in Microsoft hotspot files.

## 4. Upstream-hotspot budget

Fork-owned code is added in new files/targets. Expected Microsoft hotspot edits
must remain narrow:

- `TerminalPage.xaml`: add fixed Tasks entry and hosted TaskPage;
- `TerminalPage.h/.cpp/Workspaces.cpp`: replace boolean tool-page state with
  enum/ID selection and route one centralized close request;
- project/solution/filter/package manifests: include new targets/files;
- action dispatch: route existing close origins through one adapter;
- connection factory: recognize TaskRuntimeConnection;
- resources: localized Tasks strings and automation names.

Do not place Task state machines, SQLite, Git, Runtime IPC, or Codex protocol
logic in Microsoft hotspot files.

## 5. Milestone order

### M0: design baseline

Deliverables:

- accepted product/interaction specification;
- accepted ADR register with no Blocking entries;
- reviewed browser prototype;
- committed design-only change set.

Gate: Markdown/link consistency and user sign-off. No native build required.

### M1: TaskCore state and persistence

Status: **implementation complete on the feature branch (2026-08-30)**.

Deliverables:

- versioned schema with the v1 base and transactional forward migrations;
- Task/TaskRun/TaskTerminal/ExecutionContext/Message records;
- state transitions, command receipts, event sequencing, tombstones;
- crash-recovery query/intents;
- no process, Git, App Server, pipe, or XAML integration.

Tests:

- transition table and forbidden transitions;
- terminal-outcome immutability;
- one-active-run invariant;
- idempotency and stale expected-version conflict;
- transaction rollback and event ordering;
- migration, future-version rejection, corruption/recovery behavior;
- Delete/tombstone/30-day cleanup.

### M2: Runtime process and protocol

Status: **implementation and closure gates complete on the feature branch
(2026-08-31)**. Detailed component, test, MSIX, and portable evidence is in
[`m2-closure-report.md`](./m2-closure-report.md).

Deliverables:

- per-user Runtime executable and single-instance identity;
- secure named-pipe control/event server;
- terminal framing server with fake VT provider;
- TaskRuntimeClient and `rtask.exe`;
- version/capability handshake, reconnect, SnapshotAndSubscribe;
- helper idle lifecycle without real Codex.

Tests:

- ACL/identity/version refusal;
- frame/payload limits and malformed peers;
- duplicate commands and reconnect catch-up;
- slow-client isolation and control/data head-of-line protection;
- multi-window subscriptions and input lease generation;
- parent UI exit while Runtime remains alive.

### M3: execution context, locks, and Git

Status: **complete on `feature/task-runtime-foundation`**; closure evidence is
recorded in `m2-m3-execution-plan.md` and `m3-closure-report.md`. The branch is
intentionally not merged pending review.

Deliverables:

- canonical directory resolver;
- Direct Named Mutex lease and waiting queue;
- Git common-dir lock;
- Worktree snapshot/baseline/checkpoint/finalize/recovery;
- per-Task Worktree persistence across TaskRuns;
- fake-run checkpoint integration.

Tests use temporary repositories and cover:

- clean/dirty/staged/untracked snapshots;
- ignored files, symlinks/reparse points, disappearing files, limits;
- source index/worktree/stash non-mutation;
- signing/hook/identity/lock/commit failures;
- failure-safe preservation and path-validated cleanup;
- empty versus produced-work branches;
- Direct mutual exclusion and cancellation;
- two Runtime identities contending for the same repository.

### M4: Codex App Server adapter

Status: **complete on `feature/task-runtime-foundation`**; see
[`m4-closure-report.md`](./m4-closure-report.md).

Deliverables:

- provider-neutral adapter interface;
- one Runtime-owned Codex App Server child;
- thread/start/resume and turn/start/steer/interrupt;
- structured item/tool/file/approval event translation;
- synthesized VT Agent-terminal projection;
- `rtask` capability injection;
- provider failure/resume classification and transient retry;
- ClosePending/shared-provider recovery.

Tests:

- deterministic fake App Server for every method/event/error/approval path;
- schema/method capability compatibility fixture;
- unknown optional event and missing required capability behavior;
- one Provider crash with multiple active Task threads;
- steering rejection to durable deferred message;
- Stop, ClosePending, Suspend/Wake, Resume/fresh-session fallback;
- opt-in local smoke against an installed supported Codex CLI.

Real Codex/network access is never required for deterministic native CI.

### M5: Tasks UI without Task Surface

Status: **complete on `feature/task-runtime-foundation`**; see
[`m5-closure-report.md`](./m5-closure-report.md).

Deliverables:

- fixed Tasks page and tool-page selection enum;
- in-page draft, Task list/detail, filters, scope, properties;
- execution log and responsive Run Details;
- immutable projection coordinator and UI-thread ViewModels;
- Save/Save and Run commands against fake/real local Runtime;
- accessibility/localization foundation.

Tests:

- ViewModel snapshot/event race and stale selection;
- keyboard focus/order and Escape behavior;
- high-volume event batching;
- narrow/wide layouts and text expansion;
- UIA names, status cues, high contrast, large system text;
- no regression to Summary or user Surface tabs.

### M6: task-owned Agent Surface and close policy

Status: **complete on `feature/task-runtime-foundation`**; see
[`m6-closure-report.md`](./m6-closure-report.md).

Deliverables:

- TaskRuntimeConnection and synthesized terminal stream;
- single-Pane/single-controller task Surface;
- every CloseOrigin routed through SurfaceClosePolicy;
- Open/activate/reattach, exited placeholder, detach-on-window-exit;
- Stop/Esc, Close, ClosePending, global Provider restart;
- Surface generation and one-time close authorization.

Tests:

- complete CloseOrigin x Surface-state matrix;
- stale generation/input lease refusal;
- multiple windows and owner activation/release;
- closeOnExit placeholder behavior;
- window exit with active turn and later reattach;
- ordinary user Surface behavior byte-for-byte/interaction compatible where
  observable.

### M7: orchestration completion

Status: **complete on `feature/task-runtime-foundation`**; see
[`m7-closure-report.md`](./m7-closure-report.md).

Deliverables:

- follow-up fold/defer queue and manual hold;
- auto retry ceilings/backoff;
- explicit Agent status via `rtask`;
- InReview/Done/Cancel/Reopen/Delete/Finalize/derived Task;
- idle suspension and Runtime idle exit;
- strict Summary/history source separation and Runtime App Server proxy;
- storage maintenance/tombstones.

Tests:

- end-to-end lifecycle and all interruption/recovery points;
- Task deletion with late Hook/App Server events;
- checkpoint failure blocking Delete;
- Done with active/unclaimed work refusal;
- Runtime/UI/App Server crashes at every persisted orchestration phase;
- Summary excludes Task threads and manual sessions remain unchanged.

### M8: release hardening

Status: **complete on `feature/task-runtime-foundation`**; see
[`m8-closure-report.md`](./m8-closure-report.md) and
[`m8-release-notes.md`](./m8-release-notes.md).

Deliverables:

- updated interactive browser prototype matching native behavior;
- optional migration/rollback notes;
- packaged Runtime/rtask in registered and portable distributions;
- version/update/drain behavior;
- security, privacy, storage, and upstream-sync review;
- user documentation.

Gate:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1
pwsh -NoLogo -NoProfile -File .\tools\deploy\build-portable-release.ps1 -SkipBuild -NoLaunch
```

Build outputs remain incremental under `AGENT.md`; no clean build unless its
documented exceptions apply.

## 6. Cross-cutting test matrix

Every milestone adds deterministic tests for:

- success, validation failure, cancellation, timeout, crash, and restart;
- zero, one, and multiple UI clients;
- user-owned versus task-owned Surface behavior;
- Direct versus Worktree;
- clean versus dirty repositories;
- manual, automatic, and poisoned-session retries;
- current versus stale command/event/Surface generations;
- registered versus portable Runtime identity/data root;
- light, dark, high-contrast, keyboard-only, and large-text UI.

## 7. Change-set discipline

- Each milestone is independently reviewable and keeps the feature disabled or
  hidden until its safety contracts are complete.
- Do not mix Microsoft upstream synchronization into a Task feature milestone.
- Do not commit or rewrite unrelated existing changes.
- No schema/process/protocol change lands without its recovery and negative
  tests in the same change set.
- Native implementation commits reference the accepted ADRs they realize.

## 8. Final acceptance scenarios

Before enabling Tasks by default, all scenarios must pass:

1. Save a Task without creating execution resources.
2. Save and Run from dirty Git state without modifying the source checkout.
3. Close every window while work continues, then reconnect.
4. Stop via terminal Esc and Tasks Stop with identical provider behavior.
5. Close one hung Task without harming other App Server threads.
6. Queue follow-ups during a run and create exactly one later TaskRun.
7. Fail/retry with safe session resume and poisoned-session replacement.
8. Preserve failed work through checkpoint and Worktree recovery.
9. Mark InReview, inspect, then Done and close terminal without deleting Git
   artifacts/history.
10. Cancel/Reopen, Finalize Worktree, and Delete with tombstone/late-event
    protection.
11. Ordinary user terminals retain all existing close, split, move, profile,
    Summary, and history behavior.
12. Microsoft upstream sync reports Task hotspots but leaves renderer/PTY core
    changes independently mergeable.
