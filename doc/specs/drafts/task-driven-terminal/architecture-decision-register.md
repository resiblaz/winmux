---
author: RicherLife / Codex collaboration
created on: 2026-08-29
last updated: 2026-08-30
status: accepted architecture baseline
---

# Task-driven Terminal: Architecture Decision Register

## 1. Purpose

This register records the technical decisions accepted before native
development and the contracts that each implementation milestone must keep.

Decision status:

- `BLOCKING`: implementation must not start without an accepted decision.
- `FOUNDATIONAL`: later replacement would require data or process migration.
- `REVERSIBLE`: can be changed behind an interface after the first slice.
- `DEFERRED`: not required for the first slice.

Every accepted decision must record:

1. the selected option and rejected alternatives;
2. the product requirements it satisfies;
3. failure and recovery behavior;
4. security and privacy implications;
5. upstream Microsoft Terminal conflict surface;
6. tests required to keep the decision true.

For interaction-sensitive decisions, the evidence must first include the
corresponding entry in `multica-interaction-reference.md` and the user's
confirmation of any adaptation.

## 2. Architecture principles already fixed

- Product state, run state, and terminal state are separate.
- User-owned Surface behavior must remain compatible with Windows Terminal.
- Surface ownership is immutable after creation.
- Task-owned Surface close requests are centrally authorized.
- Persistence precedes process launch and destructive cleanup.
- TaskRun history is append-oriented and retry-safe.
- Provider integration sits behind an adapter.
- Fork-specific components should avoid renderer, PTY, and core Tab/Pane edits
  unless a narrow integration point is unavoidable.
- The first implementation is local-first.

## 3. Candidate component boundaries

The names are provisional; responsibilities are the important part.

```text
TaskPage / TaskViewModels
          |
          v
TaskApplicationService (commands, invariants, transactions)
          |
          +-- TaskStore
          +-- TaskEventStore
          +-- ExecutionContextResolver
          +-- TaskTerminalService
          +-- SurfaceClosePolicy
          +-- DirectoryLeaseManager
          +-- WorktreeManager
          +-- AgentRuntimeAdapter
          +-- TaskRecoveryService
          |
          v
Existing Workspace / Surface / History integration ports
```

The Tasks UI must not call SQLite, process-launch APIs, Git, or
`TerminalPage` mutation methods directly.

## 4. Decision sequence

Decisions should be discussed in this order because later choices depend on
earlier ones:

1. execution and process-ownership model;
2. TaskTerminal and physical Surface relationship;
3. application/window lifetime and recovery authority;
4. persistence boundary and transaction model;
5. close-policy integration;
6. provider/runtime protocol;
7. directory lease and Worktree lifecycle;
8. event model and history integration;
9. UI/view-model threading;
10. multi-window and future daemon boundary.

## ADR-TASK-001: Agent execution model

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: Codex executes through a background Runtime-owned App Server. The
visible TaskTerminal Surface renders a VT projection synthesized from
structured App Server events and forwards input through the provider adapter;
it does not host or scrape the stock Codex TUI.

### Option A: terminal-hosted agent

The task-owned terminal's PTY directly hosts the provider process or a stable
task host that launches it.

Benefits:

- execution is visibly and conceptually inside the Task terminal;
- user input and raw provider output share the terminal;
- aligns with this product's terminal-first identity.

Risks:

- reliable structured launch, cancellation, completion, and recovery are
  harder;
- direct command injection into an interactive shell is unsafe and fragile;
- closing or losing the PTY necessarily affects the run;
- resuming across application restart may require a separate host process.

### Option B: background Runtime with projected Agent terminal

A local background Runtime owns Codex App Server thread/turn state. The
task-owned Surface renders an adapter-generated VT projection of structured
events and forwards terminal input as provider operations.

Benefits:

- structured process lifecycle, messages, and recovery;
- the terminal Surface can potentially detach and reattach without making the
  UI process the Agent process owner;
- closer to Multica's local runtime model.

Risks:

- requires a versioned terminal-stream/projection and control protocol;
- must define buffering, backpressure, reconnect, input ownership, and resize;
- depends on an experimental App Server protocol and strict capability/version
  compatibility checks.

### Decision rationale

The user selected Option B after reviewing Multica's daemon model. Separating
the Runtime from the Surface makes run lifecycle, structured events, recovery,
and future remote/background operation explicit. The later Stop/Close
clarification requires the Surface to be an attached Agent terminal view, not
an unrelated shell. Task ownership still governs who may intentionally close
that view and Runtime session; it does not make the Surface the process owner.

### Accepted consequences

- App Server structured events are persisted and rendered into the attached
  task-owned terminal's synthesized VT stream.
- user line input maps to `turn/start` or `turn/steer`; Esc maps to
  `turn/interrupt`.
- Stop Task sends the semantic equivalent of `Esc` through the Runtime adapter
  and retains the terminal/session.
- Close Task interrupts/unsubscribes the projected Agent-terminal session and
  then removes its Surface while preserving the Codex thread; Delete Task
  remains separate.
- Semantic interrupt, Runtime session close, and Surface removal are separate
  idempotent service operations.
- Runtime lifetime after all Terminal windows close remains a separate
  blocking decision in ADR-TASK-003.

### Required implementation evidence

- a concrete launch/stop/resume sequence;
- behavior when the App Server thread/turn errors or the application crashes;
- how user follow-up input reaches the running agent;
- how prompt content is passed without shell injection;
- how output is attributed to one TaskRun.

## ADR-TASK-002: TaskTerminal identity and Surface generations

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: TaskTerminal is a persisted logical entity separate from Runtime
Agent-terminal sessions and physical UI Surfaces.

Identity model:

```text
TaskTerminal
  logical ID: stable for Task lifetime
  current_runtime_session_id: nullable
  current_surface_id: nullable
  surface_generation: monotonic
  state: NotCreated/Starting/Ready/Busy/Exited/Suspended/Error/Closed
```

Ownership boundaries:

- Runtime Agent-terminal session owns App Server thread/turn subscriptions,
  provider-session attachment, and synthesized live VT projection state;
- physical Surface owns one UI rendering/input attachment and controller lease;
- TaskRun owns one immutable execution attempt and its structured events;
- TaskTerminal links them without making any ephemeral ID the Task identity.

Rules:

- one TaskTerminal has at most one Runtime session and one physical Surface;
- Close clears session/Surface bindings but preserves logical identity;
- Suspend clears subscriptions/projection buffers after recovery metadata is
  saved while retaining the persisted Codex thread;
- a terminal App Server error may leave a placeholder Surface until
  Close/restart;
- reattach/reopen increments generation; stale generations cannot reacquire
  input or mutate state;
- snapshot restore validates Task ID, logical terminal ID, generation, and
  Runtime lease rather than trusting a saved Surface ID;
- Runtime/UI crash recovery may replace ephemeral resources without changing
  TaskTerminal identity.

## ADR-TASK-003: Application/window lifetime authority

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: a Runtime process separate from `WindowsTerminal.exe` owns the App
Server process, Codex threads/turns, and active TaskRuns. Closing any or all
Terminal windows only detaches their Surface clients. Active Tasks continue,
and later windows can reconnect.

Rationale: this matches the selected background-Runtime model and Multica's
separation between UI and daemon execution. It also makes window exit distinct
from the explicit Stop Task and Close Task commands.

Accepted consequences:

- the Runtime, not a Terminal window, is authoritative for process and run
  liveness;
- UI disconnect never writes Completed, Cancelled, or Interrupted by itself;
- the Runtime continuously drains App Server notifications even with no Surface
  attached, so the provider/control stream cannot block;
- reconnect requires sequenced catch-up followed by live output/events;
- input is accepted from at most one authoritative controller at a time;
- Runtime crash, user logoff, operating-system shutdown, and application update
  require explicit recovery classifications;
- per-user helper deployment is fixed by ADR-TASK-015; idle lifetime, output
  buffering, and update handshake remain follow-on blocking decisions.

## ADR-TASK-004: Persistence boundary

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: store Task, TaskRun, ordered events, TaskTerminal identity,
ExecutionContext, and recovery intents in a separate `workspace-tasks.db`.
The per-user Runtime helper is the database's sole writer. Terminal UI clients
must use Runtime commands, queries, and subscriptions and must not mutate the
database directly.

Rationale: a Task-specific database avoids coupling new orchestration
migrations to the mature AgentHistoryStore schema. Runtime single-writer
ownership also prevents multiple Terminal windows from racing state
transitions.

Accepted consequences:

- Runtime commands commit state changes and ordered events before notifying UI
  clients;
- UI queries return Runtime projections rather than raw SQLite access;
- stable provider/session/turn IDs link TaskRun records to existing history;
- cross-database history enrichment is eventually consistent and cannot be
  part of the same SQLite transaction;
- schema versioning, WAL policy, backup/corruption handling, and transaction
  details remain in ADR-TASK-005;
- registered and portable channels use settings-root-relative Task databases
  and distinct Runtime identities.

## ADR-TASK-005: State and transaction model

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: use normalized current-state tables plus append-only ordered events
and durable command receipts. Do not use pure event sourcing and do not use
state-only CRUD without an audit/recovery log.

Core schema domains:

```text
task
execution_context
task_terminal
agent_session
task_run
task_message
task_run_message
task_event
git_checkpoint
command_receipt
delete_tombstone
```

Command transaction:

```text
BEGIN IMMEDIATE
  -> find command_receipt by idempotency key
  -> if found, return stored result without repeating effects
  -> load Task aggregate and compare expected version
  -> validate command and state transition
  -> update normalized current-state rows
  -> append event(s) with monotonic aggregate/run sequence
  -> store command receipt and bounded result
COMMIT
  -> publish committed projections/events to clients
```

Constraints:

- one active TaskRun per Task, enforced by transaction plus a partial unique
  index or equivalent database invariant;
- terminal TaskRun outcomes cannot transition to another outcome;
- Task aggregate version increments on every committed mutation;
- message claim and follow-up TaskRun creation are atomic;
- current AgentSession/Surface bindings are unique per logical TaskTerminal;
- foreign keys and cascading content deletion follow ADR-TASK-027, while
  tombstones remain independent.

SQLite policy:

- Runtime is the only process that opens the database for mutation;
- enable foreign keys, WAL, bounded busy timeout, and explicit synchronous
  durability appropriate for Task state;
- publish only after commit; clients reconnect/catch up by committed sequence;
- transactional schema migrations with monotonic schema version;
- reject a database created by a newer unsupported schema;
- create a controlled backup before a destructive migration;
- corruption or failed migration preserves the original file and starts a
  read-only/recovery mode rather than silently recreating an empty database.

Recovery queries:

- nonterminal TaskRuns;
- pending Stop/Close/Delete/Worktree intents;
- stale terminal/controller leases;
- unclaimed/deferred messages;
- incomplete automatic-retry commands;
- tombstones and bounded maintenance work.

Implementation rule: typed/queryable state uses normalized columns and
constraints. Bounded provider-specific detail may use versioned JSON payloads,
but opaque JSON cannot replace core state-machine columns.

## ADR-TASK-006: Central Surface close policy

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: every Surface-close path calls one synchronous
`SurfaceClosePolicy::Evaluate` using a cached ownership projection. No caller
implements its own `task_id` branch, and UI close paths never wait on Runtime
IPC or SQLite.

Inputs:

```text
surface_id
surface_generation
close_origin
cached owner kind and Task identity
```

Close origins include tab button, middle click, shortcut, command palette,
automation, Close Other/Right, Pane/Workspace close, process `closeOnExit`,
window/application exit, Task command, and recovery cleanup.

Policy results:

```text
AllowUserClose
DenyAndOpenTask
SkipProtected
DetachForWindowExit
RetainExitedPlaceholder
DelegateToTaskClose
```

Behavior:

- user-owned Surface returns Allow and preserves existing Windows Terminal
  behavior;
- ordinary close attempts on task-owned Surface return DenyAndOpenTask;
- Close Other/Right skips task-owned Surfaces and reports retained count;
- Workspace close is denied while task-owned Surfaces remain and opens the
  filtered Tasks view;
- window/application exit detaches the client and never closes the Task;
- provider/process exit retains the accepted exited placeholder;
- Task Close delegates to Runtime orchestration and removes the Surface only
  with a generation-bound one-time authorization.

Implementation constraints:

- Runtime publishes owner/Task/generation projections to each UI client;
- ownership uncertainty for an existing task-marked Surface fails closed;
- stale generations cannot close or send input to replacement Surfaces;
- XAML close-button visibility is presentation only, never enforcement;
- one narrow TerminalPage/action-handler integration routes existing close
  requests through policy to minimize Microsoft upstream conflicts;
- Task-specific rules remain in fork-owned services/files rather than Tab,
  Pane, renderer, or PTY internals.

Verification requires a parameterized matrix of every close origin against
user-owned, task-owned, stale-generation, detached, exited, and recovering
Surface states.

## ADR-TASK-007: Provider runtime protocol

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: implement a provider-neutral Runtime adapter and use Codex App Server
as the initial provider backend. One App Server child process belongs to the
per-user Runtime helper; persistent Tasks map to Codex threads and TaskRuns map
to Codex turns.

Provider-neutral interface:

```text
ProbeCapabilities
StartSession
ResumeSession
StartRun
SteerRun
InterruptRun
SubscribeEvents
RespondToApproval
SuspendSession
CloseSessionProjection
ClassifyFailureAndResumeSafety
```

Codex mapping verified against the locally installed Codex CLI 0.150.1
generated experimental schema:

```text
StartSession        -> thread/start
ResumeSession       -> thread/resume
StartRun            -> turn/start
SteerRun            -> turn/steer
InterruptRun        -> turn/interrupt
Suspend/Close view  -> thread/unsubscribe plus Runtime projection cleanup
```

Structured notifications include thread/turn lifecycle, Agent message deltas,
command execution, file changes, MCP tool progress, plan/diff updates, errors,
and token usage. Runtime converts these into bounded provider-neutral
`TaskProviderEvent` records before persistence/publication.

Agent-terminal projection:

- no stock Codex TUI is embedded or screen-scraped;
- Runtime renders structured events into a synthesized VT stream for the
  task-owned Surface;
- line input with no active turn starts a turn;
- line input during a steerable active turn uses turn/steer;
- rejected steering is preserved as a deferred follow-up with an explicit
  terminal notice rather than dropped;
- Esc and Stop Task use turn/interrupt;
- approvals are typed Runtime events and responses, not parsed terminal text.

Process/lifetime:

- Runtime launches App Server directly with structured argv and a controlled
  environment, never through shell concatenation;
- the App Server child is contained and monitored by Runtime, but its lifetime
  is independent of UI windows;
- one shared App Server may host multiple Task threads;
- App Server crash classifies active turns as Interrupted/runtime_recovery and
  applies ADR-TASK-018;
- idle Runtime may stop/restart App Server after thread/session recovery state
  is persisted.

Compatibility:

- App Server is experimental and is not treated as a stable public protocol;
- Runtime maintains a tested protocol major/minimum Codex CLI version plus
  capability map;
- initialize/version/capability checks occur before Task execution;
- unknown optional notifications are bounded and ignored/logged safely;
- missing or incompatible required methods produce
  `runtime_version_unsupported` and never fall back to TUI scraping;
- CI compares the supported minimal method/event contract against generated
  schema from the pinned/tested CLI fixture;
- existing UI-side `CodexAppServerClient` responsibilities move behind the
  Runtime adapter; UI no longer starts independent App Server processes.

Security/reliability:

- provider thread/turn IDs are opaque and bound to Task/TaskRun rows;
- approval and tool payloads have strict size/type limits and redaction;
- responses are correlated by request ID and notifications by thread/turn/item
  identity;
- malformed/reordered events fail the affected run safely without poisoning
  unrelated threads;
- Task-scoped `rtask` capability/instructions are injected through supported
  thread/turn configuration, not prompt-parsed authority.

Close timeout behavior is resolved by ADR-TASK-030: use per-Task ClosePending
and never kill a shared App Server merely to close one Task.

## ADR-TASK-008: Directory lease model

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: Direct TaskRuns hold an exclusive real-path lease for their full
managed execution. Worktree TaskRuns execute concurrently but serialize short
operations that mutate/read shared Git repository metadata.

Direct lease rules:

- normalize an absolute path, resolve symlink/reparse aliases where safe, and
  use the resulting canonical identity as the lease key;
- acquire before Agent process creation and release after TaskRun finalization;
- later runs enter WaitingForDirectory with no independent timeout;
- Stop Task cancels a waiting run without modifying the directory;
- one Runtime uses FIFO ordering; separate Runtime identities guarantee mutual
  exclusion but not global FIFO fairness;
- ordinary user terminals and external tools remain unmanaged writers and are
  disclosed as a Direct-mode risk.

Git common-dir critical-section rules:

- Worktree Agent execution does not hold a long Direct lease;
- source snapshot, `git worktree add`, branch create/delete, checkpoint,
  finalization, and cleanup acquire a short lock keyed by canonical Git
  common-dir identity;
- the lock covers only operations requiring a consistent shared repository
  view and is released before Agent execution.

Cross-process mechanism:

- use current-user Windows Named Mutexes keyed by a versioned hash of canonical
  path/common-dir identity;
- keep names independent of portable/registered channel so separate Runtime
  identities still coordinate the same directory;
- OS process loss releases the actual Mutex; SQLite waiting state is reconciled
  separately and is never treated as the filesystem lock;
- persist waiting/holder metadata for UI and recovery without trusting it for
  mutual exclusion.

Security/reliability requirements:

- reject drive/system roots and unsafe canonicalization failures according to
  ExecutionContext validation;
- bound lock acquisition diagnostics and avoid leaking unrelated paths in
  global notifications;
- idempotent cancellation cannot release a lease owned by another TaskRun;
- Runtime restart reconciles stale WaitingForDirectory records before
  scheduling new work.

## ADR-TASK-009: Worktree lifecycle

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: Worktree support is part of the first architecture slice and is the
default for valid Git repositories. One Worktree is owned by the persistent
Task and reused by all TaskRuns for that Task. Direct mode is an explicit Git
choice or the fallback for non-Git directories.

Rationale: per-Task Worktrees preserve files, AgentSession continuity, queued
follow-up context, and retry state across multiple TaskRuns while isolating
different Tasks for concurrency. This adapts Multica's per-execution Worktree
to this product's persistent Task/terminal model.

Accepted lifecycle:

```text
Create/save Git Task
  -> record source root and requested Worktree mode

First open/run
  -> create Task Worktree and branch
  -> persist runtime root and baseline metadata

Later TaskRuns/retries/follow-ups
  -> reuse the same Worktree and branch

Close Task / InReview / Done / Failed / Interrupted
  -> preserve Worktree

Explicit reviewed cleanup
  -> verify state and managed path
  -> preserve/commit or refuse on unsafe work
  -> remove Worktree only after confirmation
```

Invariants:

- selecting Worktree never silently falls back to Direct;
- source working tree, index, and stash list are not modified by setup;
- every TaskRun snapshots source/runtime root, branch, base revision, and
  ExecutionContext revision;
- a Worktree with uncommitted or otherwise undelivered work is preserved;
- deletion and cleanup resolve and validate exact paths inside the managed
  Worktree root;
- Direct mode uses canonical-path leases to serialize managed TaskRuns;
- ordinary user terminals remain unmanaged external writers.

Required decisions:

- default worktree root for MSIX and portable distributions;
- branch naming and collision handling;
- dirty source checkout policy;
- untracked file policy;
- recovery of preserved work after failure;
- cleanup confirmation and proof that deletion stays inside the managed root;
- relation between Task completion, terminal close, and worktree cleanup.

Resolved above: completion and terminal close preserve the Worktree. Still open
are dirty-source snapshot policy, branch naming, managed root, delivery/commit
policy, cleanup UI, and exact recovery bounds.

## ADR-TASK-010: History integration

Status: **ACCEPTED, REVERSIBLE behind an interface** (2026-08-29)

Decision: keep strict source ownership instead of duplicating TaskRun content
into AgentHistoryStore.

```text
User-owned Surface / manually started Codex
  -> workspace-history.db
  -> Summary / Sessions

Task-owned App Server thread/turn
  -> workspace-tasks.db
  -> Tasks / Run Details
```

Source-of-truth rules:

- TaskStore owns Task thread/turn messages, tool events/results, outcome,
  recovery, and Git metadata;
- AgentHistoryStore owns manual user-session summaries/turns;
- TaskRun stores opaque provider thread/turn IDs but no SQLite foreign key into
  `workspace-history.db`;
- no background projection writes TaskRun content into history tables;
- Delete Task has no duplicate history payload to clean up transactionally.

Runtime/App Server integration:

- Runtime is the only process that starts/controls Codex App Server;
- Summary list/read/resume requests use Runtime RPC rather than UI-launched App
  Server clients;
- existing `CodexAppServerClient` protocol code moves/reuses behind Runtime
  adapter boundaries;
- provider `threadSource`/metadata and Runtime session registry classify
  Task-owned threads so generic manual-session discovery can exclude them;
- a later Summary projection may show a lightweight link to Task without
  copying Task events.

Hook/late-event handling:

- task-scoped Runtime environment routes Hook events to Task ingestion;
- ordinary user-terminal Hook events retain existing history ingestion;
- session registry rejects cross-owner routing;
- Task tombstones acknowledge/drop late task events without recreating content;
- event IDs and provider thread/turn/item IDs remain idempotent.

Migration/compatibility:

- existing history databases remain valid and do not migrate into TaskStore;
- a task thread accidentally ingested by an older build is not automatically
  deleted; reconciliation flags it and suppresses duplicate UI presentation;
- registered/portable data roots remain separate;
- retention/redaction follows each owning store's accepted policy.

## ADR-TASK-011: Multi-window ownership

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: Tasks belong to a Workspace/data domain rather than one UI window,
so multiple windows may view Task state. A TaskTerminal has at most one attached
single-Pane Surface and one authoritative input-controller lease.

First-slice restrictions:

- task-owned Surface contains exactly one Pane;
- split, duplicate, move-to-Workspace, tab tear-off, and window transfer are
  rejected for task-owned Surfaces;
- user-owned Surfaces retain all existing behavior;
- if another window opens a Task whose Surface is attached, activate/focus the
  owning window instead of creating a duplicate;
- when the owning window disconnects, Runtime releases its controller lease and
  a later requester may attach a new Surface generation;
- multiple windows may subscribe to Task projections/events, but non-owner
  views do not send terminal input.

Lease requirements:

- Runtime is authoritative for lease owner, generation, and expiry;
- acquire/release commands are idempotent and versioned;
- client heartbeat/disconnect releases stale UI ownership without stopping the
  TaskRun;
- an old Surface generation cannot send input after a new lease is issued;
- Stop Task and Close Task remain Runtime control commands and do not depend on
  which UI window currently owns terminal input;
- all windows receive the committed ownership-change event.

## ADR-TASK-012: UI threading and projection

Status: **ACCEPTED, REVERSIBLE** (2026-08-29)

Decision: UI observes immutable Runtime projections through a background
client/coordinator and applies ViewModel mutations only on the XAML UI thread.
XAML/ViewModels never own orchestration, SQLite, Git, provider, or process
lifecycle.

Component boundary:

```text
TaskRuntimeProtocol
TaskRuntimeClient             background pipe IO / parsing / reconnect
TaskProjectionCoordinator     immutable per-window projection and sequencing
TaskPageViewModel
TaskItemViewModel
TaskRunViewModel              UI-thread observable state
TaskRuntimeConnection         separate Agent-terminal data channel
TaskPage.xaml
```

Snapshot/subscription contract:

- Runtime exposes an atomic SnapshotAndSubscribe operation;
- response includes a consistent snapshot plus committed event cursor;
- later notifications carry strictly newer committed sequence numbers;
- duplicates are ignored; gap/generation mismatch triggers a clean resnapshot;
- no UI refresh races between "load list" and "begin subscription".

Threading rules:

- named-pipe IO, framing, JSON parsing, Git, SQLite, and Provider work stay off
  UI thread;
- DTO/projection records are immutable across threads;
- only UI dispatcher code mutates observable vectors/properties;
- high-frequency structured deltas are batched/throttled before XAML
  projection; token-level notifications cannot force layout per token;
- Task selection and Workspace changes use generation/cancellation tokens so
  late results cannot replace a newer view;
- Task terminal VT stream bypasses TaskPage ViewModels and uses
  TaskRuntimeConnection.

Command UX:

- drafts may update optimistically because they are UI-local until Save;
- persisted commands show Pending immediately but do not speculate a committed
  Task/Run status;
- final state/error comes from Runtime response plus committed event;
- window shutdown cancels subscriptions/releases input lease without stopping
  Task work.

Verification:

- fake Runtime protocol tests for snapshot/subscribe race, duplicate/gap,
  reordering, reconnect, and stale selection;
- assertions that observable state is only mutated from UI dispatcher;
- batching/performance tests for high-volume structured events;
- keyboard/focus/UIA tests for all commands, panels, and error states;
- ordinary Surface rendering/input paths remain unaffected.

## ADR-TASK-013: First-slice feature boundary

Status: **ACCEPTED, REVERSIBLE** (2026-08-29)

Decision: accept the first-slice boundary, milestone order, component layout,
upstream-hotspot budget, and verification gates in `implementation-plan.md`.

The first slice includes the complete local Codex workflow required by accepted
foundational contracts:

- TaskCore persistence/state/recovery;
- per-user Runtime, pipes, `rtask`, and App Server adapter;
- Git Worktree/Direct execution and checkpoints;
- fixed Tasks UI, structured execution log, and Agent-terminal projection;
- task-owned Surface/close policy/multi-window input lease;
- follow-up, retry, status, suspend, lifecycle, deletion, and history
  separation;
- registered/portable packaging, tests, accessibility, security, and upstream
  compatibility.

The first slice excludes alternate providers, cloud collaboration, multiple
task panes/writable mirrors, Surface moves, automated Git delivery, full raw VT
history, MCP control, and advanced planning/retention features.

Implementation remains gated on an updated browser prototype and explicit
post-prototype authorization; accepting this ADR does not itself authorize
native code changes.

## ADR-TASK-014: Stop and Close Task semantics

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: Stop Task and Close Task are not synonyms.

- Stop Task sends the Agent the semantic equivalent of terminal `Esc`, ending
  or interrupting the current turn while retaining the Runtime session and
  task-owned Surface.
- Close Task requests termination of the Runtime-backed Agent terminal session
  and removes its task-owned Surface. It does not delete the Task record or run
  history.
- Close Task preserves the Task product status and persisted ExecutionContext,
  so a later Open Task can create a new Runtime session/Surface generation.
- Cancel Task and Delete Task are separate application commands.

Required orchestration shape:

```text
StopTask command
  -> persist interrupt intent
  -> request Runtime semantic Interrupt (Esc-equivalent)
  -> classify the current TaskRun/turn outcome
  -> keep Runtime terminal session and Surface attached

CloseTask command
  -> persist close intent
  -> request Runtime Agent-terminal session close
  -> wait for acknowledgement or apply bounded force-close policy
  -> remove task-owned Surface through SurfaceClosePolicy
  -> clear physical Surface/session bindings
  -> append final close/recovery events
```

Consequences:

- both commands must be idempotent when the run, session, Surface, or some
  combination is already absent;
- failure to deliver semantic Esc must not report the turn stopped;
- failure to close the Runtime session must not silently remove the only Agent
  terminal view unless an explicit force-close policy applies;
- a Surface-removal failure after successful session close leaves a recoverable
  pending-close intent;
- restart recovery resumes any partially completed close orchestration;
- ordinary user-owned Surfaces are never included in this workflow;
- successful close clears ephemeral Runtime/Surface bindings but retains the
  Task, its status, context, AgentSession references, and TaskRun history;
- whether normal InReview/Done transitions also close the terminal remains a
  separate product decision.

Accepted interaction timing:

- Stop Task and physical terminal Esc invoke the same semantic interrupt;
- Stop has no confirmation and never escalates automatically to process kill;
- after five seconds without acknowledgement, publish a timeout/warning event
  and permit another Esc;
- pending pre-Agent work is cancelled directly because no active turn exists;
- Close without active/pending work needs no destructive confirmation;
- Close with work confirms and sends `turn/interrupt`; force escalation uses
  ADR-TASK-030 ClosePending and never kills a shared App Server for one Task;
- process exit is followed by the accepted per-TaskRun Worktree checkpoint;
- Stop and Close persist `manual_hold`; deferred messages remain stored but
  cannot auto-create a follow-up TaskRun until explicit Open/Run/Resume clears
  the hold.

Recovery requirements:

- close intent, grace deadline, force-termination result, checkpoint outcome,
  Surface removal, and hold state are persisted/reconcilable phases;
- Runtime restart resumes incomplete Close orchestration idempotently;
- failure in one phase cannot report later phases complete;
- manual hold survives Runtime/UI restart.

## ADR-TASK-015: Runtime deployment identity

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: deploy the Runtime as an on-demand, non-elevated helper process for
the current Windows user. Do not install a Windows service.

Runtime identity is scoped to the current user plus product channel/settings
root so portable and registered builds cannot accidentally control each
other's tasks or databases.

Required lifecycle:

```text
Terminal client needs Runtime
  -> connect to the channel-specific named endpoint
  -> if absent, acquire a launch lock and start matching helper executable
  -> perform protocol/version/security handshake
  -> attach or issue commands

Last Terminal client exits
  -> Runtime continues while active TaskRuns or live Agent sessions require it
  -> idle policy may later suspend sessions and exit
```

Consequences:

- the helper runs with the same user identity, provider credentials, PATH, and
  filesystem permissions as the Terminal user;
- installation, startup, and normal use require no administrator rights;
- the helper must survive the parent UI process and must not be placed in a
  UI-owned kill-on-close job object;
- one single-instance mutex and one named endpoint are required per Runtime
  identity;
- named-pipe ACLs must restrict access to the owning user, and clients must
  validate Runtime identity and protocol version;
- service-account credential routing, session-zero desktop limitations, and
  service installation/upgrade are avoided;
- idle exit, executable update/drain, stale-helper recovery, and crash restart
  remain explicit follow-on decisions.

## ADR-TASK-016: Control and terminal transport separation

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: use two local named-pipe transport classes between each Terminal
client and the per-user Runtime.

```text
Control/event pipe
  length-framed UTF-8 JSON requests, responses, and typed notifications

Agent-terminal pipe
  framed binary VT output, terminal input, resize, attach/reset, and sequence
  acknowledgement messages
```

The control/event pipe owns Task commands, Task queries, subscriptions,
Interrupt/Close, errors, capabilities, and protocol/version handshake. The
terminal pipe is session-specific and carries high-volume terminal traffic.

Rationale: a single stream would allow a slow client or high-volume VT output
to head-of-line block Stop Task, Close Task, recovery, and liveness messages.

Required protocol properties:

- length framing with strict payload limits;
- protocol major/minor and capability negotiation before use;
- current-user-only named-pipe ACL and Runtime identity validation;
- opaque request IDs and idempotency keys for mutating commands;
- monotonic per-subscription event and terminal-output sequence numbers;
- bounded queues and explicit slow-client disconnect behavior;
- exactly one authoritative input-controller lease per Agent terminal session;
- read-only observers may subscribe without acquiring input control;
- Stop Task through the control pipe and physical terminal `Esc` through the
  terminal pipe converge on the same Runtime interrupt operation;
- Close Task remains a control command and cannot be blocked by terminal data
  backpressure.

Open follow-ons:

- exact JSON method schema and binary frame header;
- reconnect/catch-up and terminal reset contract;
- VT spool/ring limits and persistence;
- heartbeat cadence and stale-client lease recovery.

## ADR-TASK-017: Durable history and recovery record

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: use a Multica-like structured history as the durable record. Do not
persist the full raw VT stream as authoritative history.

Persist for each TaskRun:

- immutable run identity, attempt number, trigger, and retry lineage;
- Task and ExecutionContext revision snapshot;
- Runtime, provider, executable/configuration, model, and AgentSession IDs;
- ordered Agent messages, tool calls, bounded tool results, and state events;
- start, activity, heartbeat, stop, and completion timestamps;
- final result/summary when available;
- normalized failure/disconnect reason plus bounded diagnostic detail;
- last committed event sequence and resumability verdict;
- Worktree/branch or Direct-directory recovery reference.

Do not treat the following as durable Task history:

- complete raw VT byte streams;
- UI-local terminal scrollback;
- hover-only error text;
- unbounded provider stderr or tool payloads.

Disconnect classification:

```text
Terminal UI disconnect
  -> view detached; TaskRun continues; not a failure

Terminal stream transport disconnect
  -> client detached; Runtime continues; emit client event only

Provider transport/process disconnect
  -> classify TaskRun failure/interruption and persist recovery record

Runtime helper loss
  -> on restart reconcile nonterminal runs as Interrupted/runtime_recovery
```

Retry consequences:

- retry always creates a new TaskRun linked to the prior run;
- the prior run's terminal outcome never changes;
- retry uses the recorded ExecutionContext and preserves existing file work;
- provider/session resume occurs only when the adapter returns a safe
  resumability verdict;
- poisoned or missing sessions start a fresh AgentSession over the preserved
  execution context;
- raw VT loss never prevents retry because it is not the recovery source of
  truth.

Open follow-ons:

- exact event/message schemas and payload limits;
- failure taxonomy and automatic-retry allowlist;
- retention policy and redaction;
- interaction between TaskRun records and existing AgentHistoryStore rows.

## ADR-TASK-018: Automatic retry boundary

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: automatically retry only normalized, allowlisted transient
infrastructure failures. All retries create a new TaskRun and retain the prior
TaskRun's terminal outcome.

Allowlist and ceilings:

| Reason class | Automatic ceiling |
|---|---|
| `runtime_recovery` after helper crash/restart | 2 total executions |
| transient provider network interruption | 3 total executions |
| semantic inactivity or configured execution timeout | 2 total executions |

Never automatically retry:

- Terminal UI or terminal-stream disconnect, because the run continues;
- user Stop Task/semantic Esc or Close Task;
- authentication, access, quota, configuration, or model errors;
- context overflow, invalid request, or another poisoned-session verdict;
- missing/invalid directories, repositories, Worktrees, or provider
  executables;
- unclassified process/provider failures.

Retry requirements:

- persist `retry_of_run_id`, attempt, normalized reason, prior Runtime/provider
  and AgentSession IDs, ExecutionContext revision, and resumability verdict;
- preserve existing file/worktree state before creating the new run;
- resume the prior AgentSession only when the provider adapter returns Safe;
- create a fresh AgentSession for a manual retry after a poisoned-session
  failure;
- apply bounded backoff and idempotency so Runtime restart cannot create
  duplicate retries;
- when the ceiling is reached, stop automatically and surface the reason and
  recovery action to the user.

Open follow-ons:

- exact backoff schedule;
- whether platform update/download failures join the allowlist;
- reason-code compatibility across Runtime versions.

## ADR-TASK-019: Product-status authority

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: TaskRun process state never implicitly advances Task product status.
The Agent or user must issue an explicit structured status command.

Agent-scoped transitions:

- set InProgress when beginning work on the Task deliverable;
- set InReview when the deliverable is ready for human review;
- set Blocked with a bounded human-readable reason when outside action is
  required;
- cannot set Done.

System transition:

- when the last active TaskRun fails, is cancelled, or is interrupted and no
  automatic retry remains, move InProgress to Todo unless the Task was
  explicitly set Blocked.

User authority:

- the user may confirm Done;
- reopening Done/Cancelled is explicit;
- status changes do not implicitly Stop Task or Close Task.

Runtime requirements:

- expose a structured, task-scoped status-update command to the Agent;
- authorize it against the active TaskRun and Task identity rather than trusting
  IDs supplied in Agent text;
- persist status transition and event atomically before publishing it;
- reject Agent attempts to set Done or mutate another Task;
- never infer status from terminal text, process exit code, or final-message
  wording.

The Agent invokes these commands through the scoped CLI accepted in
ADR-TASK-020.

## ADR-TASK-020: Agent-scoped local control CLI

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: expose the initial Agent-to-Runtime command surface through a small
local `rtask` CLI. Scope it to one active TaskRun with a short-lived capability.
MCP support may later adapt to the same internal Runtime command handlers.

Initial command family:

```text
rtask status in-progress
rtask status in-review
rtask status blocked --reason-stdin
rtask result --stdin
```

Runtime-injected context:

```text
RICHER_TASK_ENDPOINT
RICHER_TASK_RUN_ID
RICHER_TASK_CAPABILITY
```

Security and protocol requirements:

- the CLI does not accept an arbitrary Task ID for scoped mutations;
- the Runtime resolves Task identity from the authenticated active TaskRun;
- the capability is random, short-lived, method-limited, and invalidated when
  the TaskRun reaches a terminal state;
- capability material is not passed as a command-line argument;
- long text is read from stdin with strict byte and character limits;
- the CLI connects only to the Runtime control pipe and never opens SQLite;
- Agent scope cannot set Done, Cancel/Delete Task, Close Task, or affect another
  TaskRun;
- Runtime status/result mutation and event append commit in one transaction;
- repeated command IDs are idempotent;
- logs redact capability values and bound sensitive payloads.

Packaging consequences:

- `rtask.exe` must ship beside the matching Runtime for registered and portable
  distributions;
- the Runtime injects an absolute executable path/instruction so behavior does
  not depend on a conflicting global PATH entry;
- CLI/runtime protocol major versions must match;
- future MCP tools call the same application-service methods and authorization
  checks rather than reimplementing Task rules.

## ADR-TASK-021: Live input and durable follow-up queue

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: keep two explicit user-input channels.

- Agent-terminal keystrokes are immediate Runtime terminal input.
- Tasks-page follow-up messages are durable records governed by fold/defer
  scheduling.

Scheduling rules:

```text
No active or pending TaskRun
  -> persist message -> create TaskRun

Queued / Preparing / WaitingForDirectory
  -> persist message -> fold into pending TaskRun input before Agent start

Running
  -> persist message as deferred
  -> after active run is terminal, claim all ordered deferred messages
  -> create exactly one follow-up TaskRun
```

Data requirements:

- message ID, Task ID, author, creation/update timestamps, canonical content,
  revision, disposition, and claimed TaskRun ID;
- dispositions distinguish pending, folded, deferred, claimed, removed, and
  delivered;
- claim and new TaskRun creation occur in one Runtime database transaction;
- repeated enqueue commands use idempotency keys;
- a claimed message revision is immutable and recorded on the TaskRun;
- UI may edit/remove only unclaimed messages;
- exactly one follow-up TaskRun is produced for one ordered deferred batch.

Interaction requirements:

- show queued-message count and disposition without requiring hover;
- explain that terminal typing is immediate while Tasks-page messages may wait;
- keep `Tab` for focus traversal in the graphical Tasks page;
- expose accessible View/Edit/Remove actions for unclaimed messages.

Open follow-ons:

- disposition of deferred messages when Stop Task or Close Task occurs;
- maximum message count/payload and batching prompt format;
- whether an automatic retry claims messages added after the failed attempt;
- how provider adapters render the ordered batch into a new prompt.

## ADR-TASK-022: Faithful source-state snapshot

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: initialize a Task Worktree from the user's visible Git state, not
only from HEAD, without mutating the source working tree, index, or stash list.
If Runtime cannot prove a faithful reconstruction, fail setup without partial
copy, bare-HEAD fallback, or Direct fallback.

Required sequence:

```text
Resolve source worktree and canonical repository identity
  -> record source HEAD/ref and status inventory
  -> capture staged/unstaged tracked state without source mutation
  -> enumerate non-ignored untracked files with count/byte bounds
  -> create managed Task branch and Worktree
  -> replay tracked state and copy the complete accepted untracked set
  -> verify inventory/content digests
  -> create Task-internal baseline commit using the accepted commit policy
  -> persist baseline commit, snapshot digest, and ExecutionContext revision
```

Safety requirements:

- ignored files are not copied implicitly;
- untracked enumeration uses a default ceiling modeled after Multica (2,000
  files and 200 MiB total), subject to a later settings decision;
- symlinks/reparse points and paths escaping the selected root are rejected or
  handled by an explicitly documented safe rule;
- no source-side reset, checkout, stash push/pop, index rewrite, or file move;
- commits bypass interactive hooks but do not silently override repository
  signing policy; a signing failure fails setup/checkpoint and preserves any
  unique work;
- all Git subprocess arguments use structured Windows quoting and bounded
  output capture;
- partial Worktree cleanup validates the exact managed path before removal;
- any failure after user state has been reconstructed preserves data unless the
  Runtime proves the Worktree contains no unique work.

Recovery metadata:

- source HEAD and common-dir identity;
- source and Runtime roots;
- tracked snapshot object/reference and digest;
- untracked manifest with sizes/digests;
- baseline commit;
- Task branch and Worktree registration state;
- setup phase and cleanup eligibility.

Open follow-ons:

- exact Git plumbing sequence;
- branch naming and collision handling;
- treatment of submodules and Git LFS;
- sensitive but non-ignored untracked-file confirmation UX;
- exact checkpoint commit messages and author fallback.

## ADR-TASK-023: Adapted Multica Git finalization

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: align Git safety and delivery semantics with Multica while retaining
the accepted persistent per-Task Worktree lifetime.

Per-TaskRun checkpoint:

```text
Start TaskRun
  -> record start HEAD and dirty-state digest

TaskRun reaches Completed / Failed / Cancelled / Interrupted
  -> preserve Agent-authored commits without rewriting
  -> inspect dirty state
  -> if dirty, stage all and create Runtime checkpoint commit
  -> record end HEAD, changed paths, diff summary, and checkpoint outcome
  -> keep Worktree registered for the next TaskRun
```

Explicit Task cleanup/finalization:

```text
Verify no active TaskRun and exact managed Worktree identity
  -> clean Runtime sidecars without discarding legitimate Agent edits
  -> checkpoint any remaining dirty state
  -> if checkpoint/finalization fails, preserve Worktree and report its path
  -> remove Worktree only after branch safely carries all produced work
  -> keep branch when Task produced work
  -> delete branch when it remains equal to baseline
```

Commit policy aligned with Multica:

- use `git add -A` for managed checkpoint/finalization commits;
- use `--no-verify` so interactive/user hooks cannot make preservation depend
  on UI or unrelated test automation;
- retain repository user identity when configured and provide a bounded Runtime
  fallback identity only when absent;
- do not silently disable configured commit signing; failure to sign preserves
  the Worktree and reports checkpoint/finalization failure;
- never rewrite, squash, amend, rebase, push, or merge Agent commits;
- do not create empty checkpoint commits.

TaskRun delivery metadata:

- start/end commit IDs;
- whether Runtime auto-checkpointed;
- changed path summary and bounded diff statistics;
- branch and Worktree path;
- preserved-path reason when commit/removal failed.

This ADR intentionally differs from Multica at one point only: Multica removes
the disposable per-execution Worktree during finalization, while this product
keeps one Worktree across TaskRuns and finalizes only through explicit Task
cleanup.

## ADR-TASK-024: Idle Agent-terminal suspension

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: preserve logical TaskTerminal identity but automatically suspend its
physical event subscription and VT projection after five minutes when both
conditions hold:

1. no TaskRun is Queued, Preparing, WaitingForDirectory, Starting, or Running;
2. no Surface client is attached to that Agent terminal session.

State transition:

```text
Ready, zero clients, zero active runs
  -> IdleGrace(started_at)
  -> five minutes without new work/client
  -> persist session/recovery checkpoint
  -> unsubscribe/release physical projection state
  -> Suspended

Suspended + Open Task or claimed follow-up work
  -> create new Runtime terminal generation
  -> resume safe AgentSession in existing ExecutionContext/Worktree
  -> Ready or Running
```

Invariants:

- active/pending App Server turn work cancels the idle timer and is never
  auto-suspended;
- attaching a Surface cancels the timer;
- suspension is not Stop Task, Close Task, Cancel Task, or a Task status change;
- logical TaskTerminal, Task status, Worktree, messages, TaskRuns, and structured
  history remain unchanged;
- Closed never auto-resumes merely because a timer or UI reconnect occurred;
- one idempotent wake command arbitrates simultaneous UI attach and follow-up
  scheduling;
- resume failure records its reason and applies AgentSession safety rules;
- after every session is Suspended/Closed and no clients or work remain, the
  per-user Runtime helper may enter its own idle-exit grace period.

Configuration:

- first-slice default: five minutes;
- expose a later setting with bounded values and an optional "never while this
  Task is pinned" policy;
- the Runtime, not UI timers, owns the authoritative deadline.

## ADR-TASK-025: Persistent Task aggregate

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: the user-visible Task is the persistent aggregate root. It owns one
logical TaskTerminal, one current ExecutionContext lineage, zero or more
AgentSessions, durable follow-up messages, and multiple immutable TaskRuns.

Cardinality and invariants:

```text
Workspace 1 -> many Tasks
Task 1 -> 1 logical TaskTerminal
Task 1 -> 1 current ExecutionContext lineage
Task 1 -> many AgentSessions (at most one current)
Task 1 -> many TaskRuns (at most one active)
Task 1 -> many durable follow-up messages
TaskRun 1 -> 1 ExecutionContext revision snapshot
TaskRun 1 -> 1 AgentSession reference
```

Command consequences:

- first execution, retry, automatic retry, and follow-up each create a new
  TaskRun;
- no command replaces or resets the Task to represent a new attempt;
- a terminal TaskRun outcome is immutable; enrichment cannot change its
  outcome or lineage;
- Close Task ends the current terminal session but preserves the aggregate;
- Suspend/Wake changes physical Runtime resources but not Task identity;
- Cancel Task preserves the aggregate and history;
- Delete Task is the only command that removes the aggregate, subject to the
  accepted destructive-cleanup policy;
- Done is forbidden while an active TaskRun exists.

Concurrency requirements:

- Task commands use aggregate version/expected revision or equivalent
  transactional guards;
- the Runtime rejects a second active TaskRun for one Task;
- retry/follow-up claiming and active-run creation are atomic;
- multi-window clients observe one committed aggregate projection from the
  Runtime single writer.

## ADR-TASK-026: Immutable execution identity after first run

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: allow full ExecutionContext editing until the first TaskRun is
created. Afterwards, freeze repository common-dir identity, source root,
Direct/Worktree mode, Task Worktree, and Task branch for the lifetime of the
Task aggregate.

Immutable-change workflow:

```text
User requests different repository/root/mode/base branch
  -> do not mutate existing Task
  -> create derived Task with new identity and ExecutionContext
  -> copy selected product context and acceptance criteria
  -> record forked_from_task_id
  -> leave original Task, Worktree, AgentSessions, and TaskRuns unchanged
```

Revisioned fields allowed for later runs:

- provider/model/configuration supported by the Runtime adapter;
- Agent instructions;
- Task description and acceptance criteria;
- relative working subdirectory inside the immutable runtime root;
- other non-identity planning metadata.

Each TaskRun snapshots the exact configuration and relative workdir revision it
received. History never resolves these values through the Task's current row.

Rationale:

- one persistent Worktree and AgentSession lineage cannot safely span unrelated
  repositories or execution modes;
- derived Tasks make lineage visible without rewriting historical runs;
- immutable path identity simplifies locks, recovery, cleanup, and security
  validation;
- the design adapts Multica's "resource changes affect later task records"
  principle without mixing multiple repositories inside one persistent Task.

Requirements:

- derived Task creation is transactional and idempotent;
- `forked_from_task_id` does not grant access to old Runtime capabilities;
- queued/deferred messages are not copied unless explicitly selected and
  reviewed;
- an active TaskRun prevents derivation from claiming mutable live state until
  a safe snapshot point exists;
- UI clearly distinguishes "edit next run settings" from "create Task in a new
  execution context".

## ADR-TASK-027: Cancel, cleanup, and deletion boundaries

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: Cancel Task, Worktree Finalize/Cleanup, Delete Task, and Delete Result
Branch are separate commands with different preservation guarantees.

Cancel Task:

- closes active Runtime resources through the accepted close workflow;
- sets Cancelled plus persisted manual hold;
- preserves Task aggregate, messages, runs/events/results, Worktree, branch,
  checkpoints, and recovery references;
- explicit Reopen moves to Todo and may recreate/resume terminal resources.

Finalize/Cleanup Worktree:

- requires no active TaskRun;
- follows ADR-TASK-023 finalization and exact managed-path validation;
- removes Worktree only after branch delivery succeeds;
- keeps a non-empty branch, deletes a baseline-equal branch, and leaves Task
  status/history unchanged;
- on failure preserves Worktree and publishes a recoverable cleanup error.

Delete Task:

```text
persist delete intent
  -> stop/close Runtime resources
  -> checkpoint/finalize Worktree if present
  -> refuse if unique work is not safely delivered
  -> preserve result branch
  -> delete aggregate content in one database transaction
  -> retain minimal replay-protection tombstone
```

Tombstone contains only bounded identity/replay fields such as Task ID,
deletion time, and late-event identity digests; it does not retain Task content,
prompts, results, or tool payloads.

Delete Result Branch:

- is not part of ordinary Task deletion;
- requires an additional explicit destructive action and exact ref validation;
- remains outside the first slice unless later approved.

Recovery requirements:

- every phase is idempotent and persisted;
- Runtime restart continues an incomplete delete without repeating unsafe Git
  operations;
- late events acknowledge/ignore the tombstoned identity and never recreate the
  aggregate;
- deletion cannot remove user-owned Surfaces, source working tree content, or
  paths outside the managed Runtime root.

## ADR-TASK-028: Creation and execution-log UX contract

Status: **ACCEPTED, REVERSIBLE** (2026-08-29)

Decision: Task creation is an in-page draft with Cancel, Save, and primary Save
and Run. Execution log is embedded in Task detail and opens structured Run
Details without creating another fixed application page.

Creation command boundary:

```text
Save
  -> validate draft fields
  -> create Todo Task transaction
  -> no Worktree/AgentSession/Surface/TaskRun side effects

Save and Run
  -> create Todo Task transaction
  -> submit idempotent Start Task command
  -> prepare execution resources with persisted phases
  -> attach Surface only after Runtime Agent session exists
```

Execution-log projection:

- active TaskRuns expanded first;
- pending/deferred message queue visible next;
- historical runs newest-first and collapsed with count;
- Run Details projects ordered structured events, messages/tools/results,
  failure/recovery, and Git checkpoint metadata;
- wide UI uses side panel, narrow UI uses navigable detail page;
- UI observes Runtime projections and does not query SQLite directly;
- every action is keyboard reachable, has visible focus, and does not rely on
  hover-only disclosure.

Reliability requirements:

- failed preparation leaves a valid Todo Task plus actionable recovery state;
- repeated Save and Run uses idempotency keys and cannot create duplicate
  Tasks/Runs/Worktrees;
- Surface attach failure does not stop the background TaskRun and remains
  recoverable from the Task detail;
- active structured updates use committed sequence numbers and tolerate client
  reconnect without refreshing the entire Task page.

## ADR-TASK-029: Completion and retention lifecycle

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: InReview preserves logical Agent-terminal availability; user-confirmed
Done closes Runtime terminal resources. Structured content is retained until
explicit Delete Task, with a 30-day minimal deletion tombstone.

Completion rules:

- InReview does not invoke Close Task;
- an unobserved idle InReview terminal may suspend under ADR-TASK-024;
- Done is rejected while a TaskRun is active or follow-up messages remain
  unclaimed;
- successful Done transition persists intent/status, executes Close Task, and
  records close outcome without deleting Task, Worktree, branch, checkpoints,
  or structured history;
- if Close cannot finish safely, Done remains persisted with a visible
  terminal-close recovery condition rather than rewriting the status;
- Reopen Done explicitly returns to Todo before new Agent work.

Retention rules:

- no automatic age-based deletion for Tasks in any status;
- retain bounded TaskRuns, messages, tool calls/results, events, results,
  diagnostics, recovery, and Git metadata until Delete Task;
- enforce per-payload limits and explicit truncation metadata at ingestion;
- raw VT never enters durable Task history;
- Worktree/branch cleanup is independent from record retention;
- Delete Task leaves a minimal replay tombstone for 30 days, then Runtime may
  remove it in bounded idle cleanup;
- database checkpoint/maintenance runs in bounded idle work and yields to task
  scheduling.

Open technical follow-ons:

- exact payload byte limits and per-Task storage reporting;
- optional user-configurable retention/export in later versions;
- tombstone cleanup batch size and SQLite maintenance thresholds.

## ADR-TASK-030: Shared App Server ClosePending

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: when `turn/interrupt` for Close Task is not acknowledged within five
seconds, transition that TaskTerminal to ClosePending. Do not terminate the
shared App Server process to close one Task.

ClosePending behavior:

- retain persisted close intent and manual hold;
- reject new terminal input, TaskRun starts, retries, and follow-up claiming for
  the affected Task;
- preserve Worktree, checkpoints, Task/thread IDs, and structured history;
- continue bounded interrupt/status reconciliation;
- close the projection/Surface only after the turn/thread reaches a safe state;
- show a recoverable "waiting for Provider to stop" condition in Task detail.

Provider recovery:

- with no other active Task, Runtime may restart App Server after the normal
  provider-health threshold and reconcile the affected run as Interrupted;
- with other active Tasks, Runtime does not automatically restart App Server;
- a global Restart Provider action lists/discloses affected active Tasks and
  requires explicit confirmation;
- global restart marks affected nonterminal turns Interrupted/runtime_recovery
  and applies the accepted retry allowlist;
- repeated Close/Restart commands are idempotent.

This decision supersedes the earlier per-Task process-tree force-kill wording;
that operation is not safely available with one shared App Server.

## ADR-TASK-031: Modular source and dependency boundaries

Status: **ACCEPTED, FOUNDATIONAL** (2026-08-29)

Decision: implement the feature as focused domain/protocol/runtime/provider/Git/
UI components with one-way dependencies and explicit test seams. Do not
accumulate Task functionality in TerminalPage, one Coordinator/Manager, one
Runtime file, or one TaskPage code-behind.

Required dependency direction:

- TaskCore and TaskRuntimeProtocol depend on no XAML/TerminalApp layer;
- TaskRuntime depends on TaskCore/Protocol plus adapter interfaces;
- TaskCtl depends on Protocol, not Runtime internals or SQLite;
- TerminalApp Tasks UI depends on protocol projections/client interfaces, not
  TaskStore, Git, App Server, or process-launch implementations;
- Microsoft Terminal hotspot files integrate through narrow fork-owned ports.

Required separation:

- model/records and validation;
- state-transition policy;
- commands/application orchestration;
- SQLite schema/migrations/queries;
- control/event/terminal framing and transport;
- Runtime scheduling/recovery;
- Codex App Server translation;
- directory locks and Worktree operations;
- Surface ownership/close policy;
- projection coordinator/ViewModels;
- XAML views and responsive panels;
- diagnostics/telemetry/redaction.

Maintainability checks:

- a hand-written file nearing roughly 500 lines receives an explicit
  responsibility/split review; justified cohesive tables/fixtures may exceed
  it;
- no generic Utils dumping ground or cross-layer singleton;
- RAII and compile firewalls isolate Win32/SQLite/transport lifetimes;
- provider-specific branching remains within Provider adapters;
- business rules are independently testable without XAML, real Git repos,
  network, or installed Codex unless the test is explicitly integration-level;
- each implementation change names the ADR/component it realizes and includes
  negative/recovery tests in the same change set.

## 5. Architecture acceptance gate

Architecture is ready for an implementation plan only when:

- every `BLOCKING` ADR has an accepted answer;
- database tables and state transitions can be tested without XAML;
- process ownership and shutdown behavior are explicit;
- every Surface close path has one authorization result;
- no prompt is injected through an unspecified shell quoting path;
- multi-window scope is either designed or explicitly disabled;
- recovery can distinguish Completed, Failed, Cancelled, and Interrupted;
- ordinary user-owned terminal behavior remains unchanged;
- Microsoft upstream hotspot edits are listed and bounded.
