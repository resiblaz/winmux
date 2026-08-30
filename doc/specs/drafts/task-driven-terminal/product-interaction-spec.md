---
author: RicherLife / Codex collaboration
created on: 2026-08-29
last updated: 2026-08-30
status: accepted product and interaction baseline
---

# Task-driven Terminal: Product and Interaction Specification

## 1. Document status

This document records the accepted product and interaction contract for a
local-first, task-driven workflow inside this Windows Terminal fork. Staged
implementation is authorized through `implementation-plan.md`.

Requirement labels have the following meaning:

- **CONFIRMED**: explicitly agreed in the product discussion.
- **WORKING**: recommended working assumption that still needs explicit review.
- **OPEN**: unresolved product decision that blocks or materially changes the
  implementation.
- **DEFERRED**: intentionally outside the first implementation.

The browser prototype is non-normative. This document takes precedence when the
prototype and specification disagree.

Unclear interactions follow the repository's reference-first rule: inspect the
pinned Multica checkout, record its observed behavior and applicability in
`multica-interaction-reference.md`, then obtain explicit user confirmation
before changing an item to `CONFIRMED`.

## 2. Abstract

The feature adds a fixed Tasks page that turns the terminal into a local task
workspace. A Task keeps the goal, acceptance criteria, execution context,
terminal ownership, run history, and review state together. The system must
keep persistent Task state separate from the technical state of individual
agent runs.

The feature is inspired by Multica's separation of a persistent work item from
immutable execution attempts. It is not a port of Multica and must be
implemented independently.

## 3. Goals

- **CONFIRMED**: provide a fixed Tasks page alongside the existing Summary
  fixed page.
- **CONFIRMED**: keep the left Workspace Sidebar focused on Workspaces; do not
  add terminal counts or Task lists to it.
- **CONFIRMED**: allow Tasks to create and own terminal Surfaces without
  changing the lifecycle of user-created terminals.
- **CONFIRMED**: make terminal ownership and close authority predictable.
- **CONFIRMED**: keep one persistent Task goal across multiple immutable runs.
- **WORKING**: keep all code and credentials local in the first version.
- **WORKING**: let a human review successful agent output before a Task becomes
  Done.
- **CONFIRMED**: execute Codex through a background Runtime rather than hosting
  a stock Codex TUI inside the visible task-owned terminal.
- **WORKING**: leave clear adapter boundaries for providers other than Codex.
- **CONFIRMED**: preserve Microsoft Terminal upstream synchronization by
  isolating fork-specific logic and minimizing edits to upstream hotspots.

## 4. Non-goals for the first implementation

- Cloud coordination, accounts, RBAC, or multi-user collaboration.
- Squads, sub-agent orchestration, or distributed scheduling.
- Automatic pull-request merging or unattended release deployment.
- A full Kanban, Gantt, or project-management suite.
- Automatic conversion of ordinary terminals into task-owned terminals.
- Multiple providers in the first vertical slice.
- Copying Multica source code, UI components, or assets.

## 5. Terminology

### 5.1 Workspace

A user-visible project/session grouping already present in the fork. A
Workspace may contain user-owned Surfaces, task-owned Surfaces, and Tasks.

Workspace identity does not by itself determine a Task's execution directory.

### 5.2 Task

The persistent user-visible unit of intent. A Task contains:

- title, goal, background, and acceptance criteria;
- Workspace association;
- product status and priority;
- execution context;
- logical task-terminal identity;
- agent sessions and immutable TaskRun history;
- result and review history.

The UI calls this object a **Task**. Internal implementation names may use
`TaskRecord` or `WorkItemRecord`, but user-visible language must remain Task.

### 5.3 TaskRun

One concrete execution attempt by an agent. Retry and continued work create new
TaskRun records. A terminal TaskRun is never overwritten by a later attempt.

### 5.4 TaskTerminal

The logical terminal owned by a Task. A TaskTerminal may temporarily have no
physical Surface when it has never been opened, has been intentionally closed,
or is suspended across application restart.

Closing and reopening a TaskTerminal may create a new physical Surface ID while
retaining the same logical TaskTerminal and Task history.

### 5.5 Surface

A visible terminal Surface hosted by a Workspace. Every Surface has immutable
ownership assigned at creation:

- `user`: created and managed by the user;
- `task`: created and managed by one Task.

### 5.6 ExecutionContext

The persisted description of where and how a Task executes:

- selected Workspace;
- source/project root;
- effective runtime root;
- Direct or Worktree mode;
- branch and repository identity when applicable;
- terminal profile;
- last known task-terminal directory;
- directory lease and worktree metadata.

### 5.7 AgentSession

A provider-specific conversation/session identity such as a Codex thread. A
Task may use more than one AgentSession over its lifetime when an earlier
session cannot be resumed safely. Each TaskRun records which AgentSession it
used.

## 6. Core invariants

1. **CONFIRMED**: Surface ownership is assigned at creation and is not converted
   later.
2. **CONFIRMED**: a user-owned Surface is closed and otherwise managed by the
   user.
3. **CONFIRMED**: a task-owned Surface can be intentionally closed only through
   its owning Task on the Tasks page.
4. **CONFIRMED**: a user-owned Surface is never silently attached to a Task.
5. **CONFIRMED**: creating a Task from a user terminal copies context but
   creates a distinct task-owned terminal when the Task opens.
6. **CONFIRMED**: one Task has one logical TaskTerminal.
7. **CONFIRMED**: one Task can have many immutable TaskRuns, but at most one
   active TaskRun.
8. **WORKING**: one physical Surface belongs to at most one Task.
9. **WORKING**: one Task has at most one open physical task-owned Surface.
10. **CONFIRMED**: Task status and TaskRun status are separate contracts.
11. **CONFIRMED**: TaskRun terminal states are immutable; later metadata may only
    enrich the record without changing its historical outcome.
12. **CONFIRMED**: closing a Surface never deletes its owning Task or run
    history.
13. **CONFIRMED**: a Task cannot become Done while it has an active TaskRun.
14. **CONFIRMED**: Stop Task and Close Task are distinct. Stop Task sends the
    Agent the semantic equivalent of terminal `Esc`, interrupting the current
    work while retaining the Agent terminal/session. Close Task closes the
    task-owned Agent terminal and ends that terminal session.
15. **CONFIRMED**: Close Task preserves the Task record, its current product
    status, ExecutionContext, AgentSession metadata, TaskRuns, events, and
    results. The Task can create/reopen an Agent terminal later. Cancel Task and
    Delete Task are separate actions.
16. **CONFIRMED**: closing the last Terminal window does not stop, close, or
    cancel an active Task. The Runtime outlives all UI windows, continues the
    App Server turn/event stream, and allows a later window to reconnect.
17. **CONFIRMED**: the Runtime is an on-demand, per-user helper process. It is
    not installed as a Windows service and does not require elevation.
18. **CONFIRMED**: Runtime control/events and Agent-terminal VT traffic use
    separate named-pipe channels so terminal throughput cannot block Stop,
    Close, status, or recovery commands.
19. **CONFIRMED**: the Runtime is the sole writer of a separate
    `workspace-tasks.db`; Terminal UI processes access Task state through the
    Runtime protocol rather than mutating the database directly.
20. **CONFIRMED**: durable history follows Multica's boundary: persist
    structured results, Agent messages, tool calls/results, failures,
    disconnects, and recovery metadata. Raw VT is only a bounded live buffer
    and is not retained as authoritative Task history.
21. **CONFIRMED**: only transient infrastructure failures are automatically
    retried. User actions, authentication, quota, configuration, model,
    corrupted-session, and execution-environment failures require explicit
    correction or manual retry.
22. **CONFIRMED**: the first provider control surface is a TaskRun-scoped,
    capability-limited `rtask` CLI. MCP may later wrap the same Runtime command
    layer but does not define separate business behavior.
23. **CONFIRMED**: Agent-terminal keystrokes are immediate live input. Tasks-page
    follow-up messages are durable: fold them into a not-yet-started run, or
    consolidate them into one follow-up TaskRun after an active run ends.
24. **CONFIRMED**: Git Tasks default to one Task-owned Worktree reused by all of
    that Task's TaskRuns. Direct execution is explicit for Git or the fallback
    for non-Git directories and serializes managed runs by canonical path.
25. **CONFIRMED**: Worktree creation snapshots the user's complete visible Git
    state without modifying the source worktree/index/stash. It must faithfully
    restore tracked changes and bounded non-ignored untracked files or refuse
    to start.
26. **CONFIRMED**: align Worktree safety/finalization behavior with Multica but
    retain one intentional lifecycle adaptation: the Worktree belongs to the
    persistent Task. Each TaskRun checkpoints remaining changes; explicit Task
    cleanup performs final delivery/removal.
27. **CONFIRMED**: logical TaskTerminal identity persists, but an idle physical
    Agent-terminal projection/subscription automatically suspends after five
    minutes with no active TaskRun and no attached Surface client. Running work
    never auto-suspends.
28. **CONFIRMED**: Stop Task is immediate semantic `Esc` with no confirmation
    and never force-kills after timeout. Close Task confirms only when active
    work exists, issues App Server turn interruption, and enters a persisted
    manual hold. Escalation after five seconds is a remaining technical decision
    because App Server is shared across Tasks.
29. **CONFIRMED**: Direct TaskRuns hold an exclusive canonical-path lease for
    their full managed execution and wait without timeout when occupied.
    Worktree TaskRuns run in parallel but serialize short Git repository
    snapshot/worktree/checkpoint/finalization critical sections.
30. **CONFIRMED**: repository identity, source root, execution mode, Task
    Worktree, and Task branch become immutable after the first TaskRun. Changing
    them creates a derived Task with `forked_from_task_id`; later-run model,
    instructions, acceptance criteria, and relative workdir remain revisioned.
31. **CONFIRMED**: Cancel Task preserves the aggregate, Worktree, branch, and
    history under Cancelled/manual-hold state. Worktree finalization/cleanup is
    separate. Delete Task is irreversible, must safely checkpoint/finalize
    first, preserves result branches, and keeps only a minimal late-event
    tombstone.
32. **CONFIRMED**: Task creation uses one in-page draft with Cancel, Save, and
    Save and Run. Execution log lives in Task detail with active runs expanded,
    deferred-message visibility, collapsed newest-first history, and structured
    live Run Details.
33. **CONFIRMED**: InReview keeps the Agent terminal logically available;
    physical resources may idle-suspend. Done requires no active TaskRun or
    unclaimed follow-up messages and automatically executes Close Task while
    preserving Task/Worktree/branch/history.
34. **CONFIRMED**: completed/cancelled Tasks and bounded structured history are
    retained until explicit Delete Task. Raw VT is not retained. Delete
    tombstones retain only replay-protection fields for 30 days.

## 7. Fixed-page information architecture

The Workspace Surface bar contains fixed tool entries separate from terminal
Surfaces:

```text
[user and task Surface tabs ...] [+]                 [Tasks] [Summary]
```

- **CONFIRMED**: Tasks and Summary are sibling fixed pages.
- **CONFIRMED**: fixed pages are not Surfaces and do not count as terminals.
- **WORKING**: Tasks defaults to the current Workspace with an explicit
  "All Workspaces" scope.
- **WORKING**: the page uses a list/detail layout on normal desktop widths and
  a list-then-detail layout when narrow.
- **WORKING**: selection, filters, and scroll position persist while the
  application remains open.

### 7.1 Task list priority

Primary information:

- Task title and identifier;
- Task product status;
- active-run indicator when present.

Secondary information:

- Workspace;
- last update time;
- priority.

Terminal state, exact directory, branch, and historical run details belong in
the Task detail rather than every list row.

### 7.2 Task detail sections

The detail view contains, in reading order:

1. title, Task status, priority, and current actions;
2. active-run or blocking banner when applicable;
3. goal and background;
4. acceptance criteria;
5. execution log with active runs first and past runs collapsed;
6. result/review content;
7. Task properties and execution context;
8. explicit terminal lifecycle actions.

### 7.3 Execution log interaction

- Active TaskRuns are always expanded and placed before history.
- An active row shows status, elapsed time, trigger, Open Terminal, Stop Task,
  and View Run Details.
- Queued/deferred follow-up messages appear between active and historical runs
  with visible count and accessible View/Edit/Remove actions.
- Historical TaskRuns are newest-first and collapsed behind a count by default.
- History rows show outcome, timing, retry lineage, AgentSession/model,
  checkpoint/diff summary, normalized failure reason, View Details, and Retry
  when eligible.
- Wide windows open structured Run Details as a right-side panel; narrow
  windows navigate to a detail page with an explicit Back action.
- Active Run Details append committed structured events live.
- Raw/bounded diagnostics stay behind an explicit Technical Details affordance
  and are never hover-only.
- Completed copy states that one run ended normally and does not imply Task
  completion.

## 8. Creation flows

### 8.1 Create from the Tasks page

```text
New Task
  -> choose Workspace
  -> confirm project directory
  -> choose execution mode
  -> enter goal and acceptance criteria
  -> Save, or Save and Run
```

- Creation uses an in-page draft detail rather than a blocking modal.
- The draft exposes title, goal/background, acceptance criteria, Workspace,
  source and relative directory, Worktree/Direct choice, and initial Codex
  configuration.
- The footer contains Cancel, Save, and Save and Run. Save and Run is primary.
- `Save` assigns a Task ID and persists a Todo Task without creating a
  Worktree, Runtime AgentSession, physical Surface, or TaskRun.
- `Save and Run` persists the Task before any side effect, then validates the
  directory/runtime, snapshots state, prepares Worktree, creates Runtime
  AgentSession/TaskRun, and finally attaches its task-owned Surface.
- Preparation failure leaves a recoverable Todo Task and no unidentified
  terminal. Any partial Worktree follows persisted recovery/cleanup rules.
- The UI must disclose that an agent can read and modify files under the
  selected execution root.

### 8.2 Create from a user-owned terminal

The user terminal offers "Create Task for this directory".

```text
User Surface context
  -> copy Workspace and observed CWD
  -> open the Task creation flow
  -> confirm or change the directory
  -> create Task
  -> create a separate task-owned Surface when opened or run
```

- **CONFIRMED**: the originating user Surface remains user-owned.
- **CONFIRMED**: no process, shell state, or Surface ID is transferred to the
  Task.
- **WORKING**: the nearest Git worktree root is offered as the default when the
  observed CWD is inside a repository; the exact selected path remains visible
  before confirmation.

## 9. Execution-directory contract

### 9.1 Directory selection

The Task determines the directory; the terminal does not discover it after
launch.

Candidate order for the creation form:

1. explicit directory supplied by the invoking context;
2. active user Surface CWD when available and trustworthy;
3. Workspace default/last active project root;
4. explicit folder picker.

The selected directory must be normalized to an absolute canonical path and
persisted before the first TaskRun.

### 9.2 Direct execution

- `source_root` and `runtime_root` are the same canonical directory.
- Managed TaskRuns sharing the same resolved canonical root require one
  exclusive lease for the full TaskRun.
- Later runs enter WaitingForDirectory in FIFO order within one Runtime, have no
  independent timeout, and may be cancelled through Stop Task.
- App Server turn start occurs only after the lease is acquired.
- Ordinary user terminals cannot be reliably locked; the UI may warn about
  them but must not claim that all external edits are prevented.

### 9.3 Worktree execution

- `source_root` is the user repository.
- `runtime_root` is a Task-specific Git worktree.
- **CONFIRMED**: all TaskRuns for one Task reuse that Task's worktree.
- **CONFIRMED**: Worktree is the default for a valid Git repository; Direct
  requires an explicit user choice.
- Worktree creation must not silently change or stash the user's working tree.
- Worktree deletion is separate from Task completion and requires an explicit
  reviewed action.
- Close Task and Done preserve the Worktree. Failure/interruption never deletes
  a Worktree that may contain produced work.
- Worktree setup starts from the selected source HEAD, captures staged and
  unstaged tracked state without changing the source, and copies non-ignored
  untracked files subject to explicit count/byte limits.
- Ignored files are not copied implicitly.
- Partial replay is forbidden. Exceeding a bound, unreadable content, or a
  replay mismatch fails setup and never falls back to Direct or bare HEAD.
- After faithful replay, Runtime records a Task-internal baseline commit and
  snapshot digest, allowing later TaskRun output to be distinguished from the
  user's initial state.
- Each TaskRun records start/end commit and status. Agent-created commits remain
  untouched; dirty remainder is checkpointed on every terminal outcome,
  including failure, interruption, and Stop Task.
- Checkpoint/finalization bypasses interactive commit hooks but follows the
  repository's signing configuration. If a required commit cannot be created,
  Runtime preserves the Worktree and reports the failure rather than deleting
  unique work.
- Explicit cleanup mirrors Multica finalization: checkpoint remaining work,
  remove the managed Worktree only after successful delivery, keep the branch
  when work exists, and remove an empty branch.
- Runtime never pushes, merges, rebases, squashes, or rewrites Agent commits.
- Worktree TaskRuns do not hold a Direct execution lease. Operations touching
  repository-wide Git metadata briefly acquire a common-dir lock around source
  snapshot, worktree/branch create, checkpoint, finalization, and cleanup.

### 9.4 Directory mutation

- The execution root, repository, execution mode, initial branch choice, and
  Worktree configuration are editable until the first TaskRun.
- After the first TaskRun, repository common-dir identity, source root,
  Direct/Worktree mode, Task Worktree, and Task branch are immutable.
- To change an immutable field, create a derived Task. Copy title, description,
  acceptance criteria, and selected planning metadata; assign a new Task ID,
  ExecutionContext, terminal, Worktree/branch, AgentSession lineage, and run
  history. Record `forked_from_task_id` on the new Task without modifying the
  source Task.
- Model/provider parameters, Agent instructions, acceptance criteria, Task
  description, and a relative working subdirectory inside the same immutable
  runtime root may change for later TaskRuns. Each TaskRun snapshots the exact
  revision it receives.
- If a task-owned terminal changes CWD interactively, the next TaskRun must
  show the difference and use a deterministic policy rather than silently
  changing the Task root.

## 10. Terminal ownership and presentation

### 10.1 User-owned Surface

- Created from ordinary terminal commands or the new-Surface button.
- Shows the normal close affordance.
- Supports the existing close, split, move, reorder, and profile behaviors.
- Manual Codex sessions remain part of Summary/history and do not automatically
  create Tasks.

### 10.2 Task-owned Surface

- Created only by the owning Task workflow.
- Renders a terminal-style VT projection synthesized from Codex App Server
  structured thread/turn/item events; it does not embed or scrape the stock
  Codex TUI.
- Sends terminal input to the Runtime's provider adapter, which maps lines to
  `turn/start` or `turn/steer` and Esc to `turn/interrupt`; it is not a normal
  user Shell Surface.
- Displays a Task glyph and identifier in the Surface tab.
- Does not display a close button.
- Exposes "Open Task" from its context menu.
- Rejects direct Surface-close commands and routes the user to its Task.
- **CONFIRMED**: first-slice task-owned Surfaces contain exactly one Pane and
  cannot split or duplicate.
- **CONFIRMED**: task-owned Surfaces cannot move between Workspaces or windows
  in the first slice.
- Multiple windows may view Task state, but exactly one window holds the Agent
  terminal input-controller lease.
- Open Terminal in another window activates the existing owning window; only a
  detached/closed owning window permits the requester to acquire the lease and
  reattach a new Surface generation.

### 10.3 Summary/history ownership

- User-owned Surface Codex sessions are manual history and remain in
  `workspace-history.db`, Summary, and Sessions.
- Task-owned Codex threads/turns are Task data and remain in
  `workspace-tasks.db`, Tasks, and Run Details.
- TaskRun content is not duplicated into AgentHistoryStore.
- Task thread creation uses provider source/metadata plus Runtime's session
  registry to classify ownership and filter generic history discovery.
- Runtime receives task-scoped Hook/App Server events directly; ordinary
  user-session history continues through the existing history ingestion path.
- Summary list/read/resume uses Runtime RPC because UI processes no longer
  launch their own App Server, while existing history persistence/presentation
  may remain behind that interface.
- A future Summary Task projection may link to a Task without copying its full
  event history.

### 10.4 Attempted direct close

Direct close attempts include the tab close affordance, middle click, close
shortcut, command palette, close-other-tabs, Workspace close, and equivalent
automation commands.

For a task-owned Surface:

- the request must not remove the Surface;
- keyboard focus must not become trapped;
- a non-blocking explanation identifies the owning Task;
- the explanation offers an accessible "Open Task" action;
- repeated close attempts must not stack duplicate dialogs or notifications.

## 11. Close policy matrix

| Request source | User-owned Surface | Task-owned Surface |
|---|---|---|
| Tab close button | Allow | Button absent |
| Middle-click tab | Allow | Deny; show owning Task |
| Close shortcut | Allow | Deny; show owning Task |
| Command palette close | Allow | Deny; show owning Task |
| Close other/right tabs | Allow | Skip task-owned Surfaces and report retained count |
| Workspace close | Existing behavior | Deny while task-owned Surfaces remain; open filtered Tasks view |
| Task detail | Not applicable | Delegate to Task terminal close flow |
| Window/application exit | Existing behavior | Persist/suspend logical terminal; apply shutdown recovery |
| Crash/forced termination | Existing recovery | Reconcile as interrupted/suspended on next start |

All close entry points must call one centralized close-policy service. Hiding a
button is not sufficient enforcement.

## 12. Stopping and closing a Task

The Task detail exposes two distinct lifecycle actions:

- `Stop Task`: send the Agent the semantic equivalent of `Esc`; interrupt the
  current TaskRun/turn without closing its terminal or Agent session.
- `Close Task`: close the task-owned Agent terminal and end its Runtime-backed
  terminal session. This is not the same as Delete Task.

Stop Task interaction:

- Task-page Stop and physical Agent-terminal `Esc` converge on the same Runtime
  interrupt command.
- Do not show a confirmation dialog.
- Show Stopping while waiting for provider acknowledgement.
- After five seconds without acknowledgement, report that the Agent has not
  stopped and allow another Esc; do not automatically terminate the process.
- Queued/Preparing/WaitingForDirectory work has no live Agent to receive Esc,
  so Stop cancels that pending TaskRun directly.

Close Task interaction:

- With no active/pending TaskRun, unsubscribe/close the Runtime Agent-terminal
  projection and Surface without deleting the persisted Codex thread.
- With active/pending work, confirm that the run will terminate while Task,
  Worktree, checkpoints, messages, and history remain.
- Persist close intent and issue `turn/interrupt`; the exact timeout escalation
  under a shared App Server enters ClosePending because killing the shared
  process would affect unrelated Tasks.
- ClosePending disables new input and follow-up claiming for that Task, keeps
  Worktree/history safe, and continues bounded interrupt/reconciliation.
- If no other Task is active, Runtime may restart App Server as provider
  recovery. Otherwise only an explicit global Restart Provider action may do
  so, after disclosing the number/identity of affected active Tasks.
- Checkpoint Worktree state before completing close; checkpoint failure
  preserves the Worktree and appears as a recoverable close/finalization error.

Manual hold:

- Stop Task and Close Task set a persisted `manual_hold` flag.
- Unclaimed deferred messages remain editable and are not discarded.
- No deferred follow-up TaskRun starts while the hold is set.
- Explicit Open/Run/Resume Task clears the hold and may claim queued work.

### 12.1 Cancel, Worktree cleanup, and Delete

Cancel Task:

- if work is active, execute the accepted Close Task sequence first;
- set Task status to Cancelled and retain `manual_hold`;
- preserve Task, messages, AgentSession references, TaskRuns, structured
  history, Worktree, branch, and checkpoints;
- explicit Reopen changes status to Todo and allows a later terminal/run.

Finalize/Cleanup Worktree is independent of Task status:

- require no active TaskRun;
- checkpoint/finalize using the accepted Git delivery policy;
- remove the Worktree only after the branch safely carries all unique work;
- preserve non-empty result branches and delete only branches equal to the
  baseline;
- failure preserves the Worktree and does not change Task status;
- a later reopen may recreate a Worktree from the preserved Task branch.

Delete Task:

- show an irreversible confirmation and stop/close active Runtime resources;
- require successful safe checkpoint/finalization when a Worktree exists;
- refuse deletion when unique work cannot be delivered safely;
- delete Task, TaskRuns, durable messages, structured events/results, and
  Runtime/session bindings;
- retain a result branch that contains produced work;
- retain a minimal Task tombstone sufficient to reject late Runtime/provider
  events from recreating deleted content;
- deleting the result branch is a distinct higher-risk action and is not part
  of ordinary Delete Task.

| Terminal/run condition | Required behavior |
|---|---|
| No active TaskRun | Stop is disabled; Close ends the Agent terminal session and removes its Surface |
| Queued or waiting | Stop cancels the pending run but retains the Agent terminal; Close cancels/ends the session and removes the Surface |
| Starting or running | Stop delivers semantic `Esc` and retains the terminal; Close ends the Agent session and removes the Surface after confirmation |
| Failed or cancelled run | Terminal remains available; Close preserves outcome and diagnostics |
| Task in review | Terminal remains available; Close does not delete Task history |
| Task done | Terminal behavior remains an open product decision |
| App Server thread/turn enters terminal error | Retain an exited/error placeholder until Close Task or restart |

After Close Task, the record shows "Open task terminal". Reopening uses
the stored ExecutionContext and creates a new physical Surface generation.
Close Task does not automatically change the Task product status to Cancelled.

At the service boundary, semantic interrupt, Runtime session close, and Surface
removal remain separate idempotent operations. Stop Task invokes only semantic
interrupt. Close Task orchestrates Runtime session close and Surface removal so
partial failure can be recovered safely.

## 13. Process exit, application exit, and recovery

### 13.1 Child process exits

A task-owned Surface must not disappear solely because its primary process
exited or because a profile's ordinary `closeOnExit` setting would close it.

- Retain an exited placeholder Surface owned by the Task.
- Show whether the last TaskRun completed, failed, was cancelled, or could not
  be classified.
- Provide "Open Task" in the placeholder.
- Restart and intentional close remain Task-page actions.

### 13.2 Graceful application exit

Application exit is not an intentional TaskTerminal close.

- Persist the logical TaskTerminal and its physical Surface restore metadata.
- Detach UI Surfaces without issuing Stop Task or Close Task.
- If no run is active, mark the physical view detached/suspended for later
  restoration while preserving AgentSession metadata.
- If runs are active, the separate Runtime continues the App Server turn and
  structured event stream after the last Terminal window exits.
- A later Terminal window reconnects to the Runtime session and obtains missed
  output/events before accepting new input.
- The Runtime must be a separate process boundary rather than a worker owned by
  `WindowsTerminal.exe`.
- After the last UI client detaches, an active TaskRun continues. An idle Agent
  terminal starts the five-minute suspension grace period instead of remaining
  resident indefinitely.

### 13.3 Crash or forced termination

On next startup, recovery must not guess that an unfinished run succeeded.

- Reconcile non-terminal TaskRuns as Interrupted or recoverable.
- Preserve Task status and append a recovery event.
- Offer Resume when the provider session and execution context are safe.
- Otherwise offer a new TaskRun retry while preserving the interrupted record.
- A Terminal UI/Surface disconnect is not a TaskRun failure while the Runtime
  and provider remain alive.
- Runtime loss, provider transport loss, process exit, and user input interrupt
  use distinct reason codes so retry policy can classify them safely.

## 14. Product state machines

### 14.1 Task status

Proposed product statuses:

```text
Backlog -> Todo -> InProgress -> InReview -> Done
                 \-> Blocked
Any nonterminal -> Cancelled
Done/Cancelled -> Todo (explicit reopen)
```

Confirmed transition rules:

- Starting or completing a TaskRun does not itself change Task product status.
- The Agent explicitly changes Todo/Backlog to InProgress when it begins work
  on the Task deliverable.
- The Agent explicitly changes InProgress to InReview when it believes the
  deliverable is ready for human review.
- The Agent explicitly changes the Task to Blocked with a reason when it cannot
  continue without outside action.
- A failed, cancelled, or interrupted final active run moves InProgress to Todo
  only when no active run or automatic retry remains and the Task was not
  explicitly set Blocked.
- Done is a human-only confirmation; Agent-scoped status commands cannot set
  Done.
- Changing Task status does not silently terminate a running process; actions
  that conflict with an active run must explain and resolve that conflict.

### 14.2 TaskRun state

Proposed technical states:

```text
Queued
  -> Preparing
  -> WaitingForDirectory
  -> Starting
  -> Running
  -> Completed | Failed | Cancelled | Interrupted
```

Terminal TaskRun states describe process execution only. `Completed` does not
assert that the Task goal or acceptance criteria were satisfied.

### 14.3 TaskTerminal state

Proposed states:

```text
NotCreated | Starting | Ready | Busy | Exited | Suspended | Error | Closed
```

TaskTerminal state must not be collapsed into Task status or TaskRun state.

Suspension rules:

- Running/Starting/Queued/WaitingForDirectory work is never automatically
  suspended.
- An attached Surface keeps an idle Agent terminal live for direct interaction.
- With no active TaskRun and no attached Surface, begin a five-minute grace
  period.
- At expiry, persist AgentSession and recovery metadata, close the physical
  Agent-terminal projection/subscription, release bounded VT rendering state,
  and set TaskTerminal to Suspended. The persisted Codex thread remains
  resumable.
- Opening the Task or claiming a follow-up message automatically starts a new
  Runtime terminal generation in the same ExecutionContext and resumes the
  AgentSession when safe.
- Resume failure follows the recorded session-safety policy and may create a
  fresh AgentSession without changing the Task record or Worktree.
- Suspended is automatic/resource-oriented; Closed remains an explicit user
  action and is not auto-resumed without Open Task or new work.

## 15. TaskRun history and retry

- Active runs appear before terminal runs.
- Past runs are collapsed by default when active runs exist.
- Every run records trigger, provider, session, directory snapshot, start/end
  times, result, error classification, and ordered events.
- Recovery metadata records the prior TaskRun, provider/session identity,
  Runtime identity, ExecutionContext revision, last committed event sequence,
  last activity/heartbeat, termination reason, and resumability verdict.
- Retry targets one prior TaskRun but creates a new record.
- Retry reuses the TaskTerminal and ExecutionContext unless recovery rules
  require a new terminal generation or AgentSession.
- Raw diagnostics containing machine paths or secrets must not be surfaced as
  casual hover text; detailed diagnostics belong in an explicit run-details
  view.

### 15.1 Automatic retry policy

Every retry creates a new TaskRun linked through `retry_of_run_id`; automatic
retry never rewrites the prior terminal outcome.

| Failure/disconnect class | Policy |
|---|---|
| Terminal window/UI or terminal-stream client disconnect | Run continues; no retry |
| Runtime helper crash/restart | Record `Interrupted/runtime_recovery`; automatically retry once after recovery when safe |
| Transient provider network interruption | Automatically retry, up to three total executions |
| Agent semantic inactivity or configured timeout | Automatically retry once |
| User Stop Task (`Esc`) | Never automatically retry |
| User Close Task | Never automatically retry |
| Authentication, quota, configuration, model unavailable | Manual retry after correction |
| Context overflow or poisoned AgentSession | Manual retry with a fresh AgentSession over preserved files/context |
| Missing directory, invalid repository, or damaged Worktree | Block retry until the execution environment is repaired |
| Unclassified provider/process failure | Manual retry after reviewing diagnostics |

Automatic retry requires a recorded safe execution context and an allowlisted
normalized reason code. Hitting an attempt ceiling produces a terminal failed
or interrupted TaskRun and waits for user action.

### 15.2 Follow-up message queue

Agent-terminal input and Tasks-page follow-up messages are different channels:

| Input/state | Behavior |
|---|---|
| Keystroke in attached Agent terminal | Forward immediately to the live Agent TUI |
| Tasks-page message with no active/pending run | Persist message and create a new TaskRun |
| Tasks-page message while run is queued/preparing/waiting | Persist and fold into that pending TaskRun before Agent start |
| Tasks-page message while run is running | Persist as deferred; after the active run ends, consolidate deferred messages in order into one new TaskRun |

Requirements:

- messages are never dropped merely because a run already exists;
- multiple deferred messages create one follow-up TaskRun, not concurrent runs;
- the UI shows pending/deferred count and allows review, edit, and remove before
  they are claimed by a TaskRun;
- once a message is claimed by a TaskRun, its delivered content is immutable;
- TaskRun records store the IDs and canonical content revision of included
  messages;
- `Tab` remains graphical focus navigation and is not a queue shortcut;
- Stop/Close behavior for unclaimed deferred messages remains an explicit
  follow-on decision.

### 15.3 Retention

- Completed, Cancelled, and other preserved Tasks do not expire automatically.
- TaskRuns, bounded Agent messages, tool calls/results, normalized diagnostics,
  results, events, recovery metadata, and Git/checkpoint metadata remain until
  Delete Task.
- Payload limits apply at ingestion; oversized tool output is truncated with an
  explicit marker, original size when known, and content digest when feasible.
- Raw VT is not retained as durable history.
- Worktree and result-branch lifecycle remains independent from database
  retention.
- Delete Task removes content records and retains only the accepted minimal
  tombstone for 30 days.
- Runtime performs bounded tombstone cleanup and SQLite checkpoint/maintenance
  only while it does not interfere with active Task work.

## 16. Actions by state

| Condition | Primary action | Secondary actions |
|---|---|---|
| Saved, terminal not created | Open and run | Edit Task |
| Terminal ready, no active run | Run with Codex | Open terminal; close terminal |
| Run queued/waiting | Open terminal | Stop Task (semantic Esc, terminal retained); Close Task |
| Run running | Open terminal | Stop Task (semantic Esc, terminal retained); Close Task; view events |
| Run failed/cancelled/interrupted | Retry | View run; close terminal |
| Task in review | Confirm Done | Continue with new run; open terminal |
| Task blocked | Resolve blocker | Edit reason; open terminal |
| Task done | View result | Reopen Task; close terminal; clean worktree |

Destructive or process-terminating actions require explicit wording about what
will stop and what will be preserved.

## 17. Accessibility and input

Target assumptions:

- Windows desktop, resizable rectangular window, mixed DPI;
- existing Fluent/WinUI visual language;
- normal desk viewing distance;
- keyboard and mouse first, with touch and screen-reader compatibility;
- Chinese and English localization initially, with layouts tolerant of text
  expansion and future RTL mirroring.

Requirements:

- Every action must be keyboard accessible with visible focus.
- `Tab` remains focus navigation in graphical Tasks UI; provider-specific TUI
  input behavior must not leak into fixed-page keyboard handling.
- Escape closes dialogs and popovers without leaving a keyboard trap.
- State uses text and icon/shape in addition to color.
- Text, UI component, and focus contrast must meet WCAG 2.2 targets.
- Long AI work shows persistent progress/events rather than a blocking spinner.
- Closing or deleting has a clear recovery or preservation explanation.
- UI strings use resource localization rather than embedded text.

## 18. Security and privacy

- The user must see the effective execution root before an agent starts.
- The application must not concatenate Task prompts into a shell command
  without a provider-safe argument/stdin contract.
- Task identity passed to child processes must be treated as untrusted input at
  integration boundaries.
- Canonical paths, drive roots, reparse points, and symlinks require validation
  before locks, worktree creation, cleanup, or deletion.
- Worktree snapshot limits, ignored-file behavior, and the fact that untracked
  files may be copied must be visible before first execution.
- Task output and diagnostics may contain sensitive paths or code; casual UI
  summaries must avoid unnecessary exposure.
- Credentials remain owned by the local provider CLI/runtime.
- Raw VT output is not durable history. Any bounded reconnect buffer is
  process-local or short-lived and is discarded according to Runtime session
  policy.
- `rtask` receives TaskRun scope and a short-lived capability out of band from
  user-authored command arguments. Capabilities cannot set Done, delete Tasks,
  or address a different Task/TaskRun.
- Named-pipe endpoints must grant access only to the owning Windows user and
  reject incompatible protocol/runtime identities before accepting commands or
  terminal input.

## 19. Reliability requirements

- Persist intent before launching processes.
- Persist each state transition atomically with an ordered event.
- Repeated launch, stop, retry, close, and recovery requests must be
  idempotent.
- A UI crash must not rewrite terminal outcomes as successful.
- Missing directories, missing provider CLIs, invalid sessions, and stale
  Surface IDs must produce recoverable states rather than orphaned records.
- Task-owned Surface close enforcement must cover every existing close path.
- Control traffic must not share terminal-stream backpressure; Stop and Close
  remain deliverable even if a terminal client is slow or disconnected.
- UI clients never mutate `workspace-tasks.db`; the Runtime serializes writes
  and publishes committed projections/events.

## 20. Compatibility and upstream synchronization

- Ordinary Windows Terminal behavior remains unchanged for user-owned
  Surfaces.
- Task ownership must be represented through fork-specific services and
  metadata rather than broad changes throughout Tab, Pane, or renderer code.
- Changes to `TerminalPage`, Tab/Pane close routing, action maps, XAML, IDL, and
  project files are upstream-sync hotspots and must remain small and reviewed.
- New Task components should live in isolated files with explicit interfaces.
- Microsoft rendering, PTY, accessibility, and stability fixes must continue to
  merge independently of Task data and orchestration.

## 21. First-slice boundary

The first vertical slice should include only behavior accepted by the final
architecture. The current working boundary is:

- fixed Tasks page;
- local persistence;
- Task create/read/update and acceptance criteria;
- one local provider adapter for Codex;
- one single-Pane, single-controller task-owned terminal per Task;
- Task-owned Worktree execution for Git repositories;
- Direct execution for non-Git directories and explicit Git opt-in;
- one active TaskRun per Task;
- stop, retry, review, Done, intentional terminal close, and restart recovery;
- user-owned Surface behavior unchanged.

Multiple providers, multiple panes, and window transfer remain candidates for
later slices unless architecture review proves they must be foundational.

## 22. Open product decisions

The following items require explicit product decisions before native
implementation:

1. **CONFIRMED**: Codex executes through a background Runtime that owns the
   App Server process and thread/turn lifecycle. The visible task-owned Surface
   renders a Runtime-generated VT projection from structured events rather than
   the stock Codex TUI.
2. **CONFIRMED**: task-owned Surfaces are single-Pane and cannot split or
   duplicate in the first version.
3. **CONFIRMED**: task-owned Surfaces cannot move between windows/Workspaces;
   only one window owns input. Other windows activate it or reattach after its
   lease is released.
4. **CONFIRMED**: Worktree is foundational and default for Git Tasks. One
   Worktree belongs to the persistent Task and is reused across TaskRuns.
   Direct is explicit for Git and the fallback for non-Git directories.
5. **CONFIRMED**: execution identity is immutable after the first TaskRun;
   changing repository/root/mode creates a derived Task.
6. **CONFIRMED**: graceful window/application exit detaches UI and active Tasks
   continue in the per-user Runtime.
7. **CONFIRMED**: Agent-terminal input is live; Tasks-page follow-up messages
   are durable and use the Multica-style fold/defer rules.
8. **CONFIRMED**: InReview retains the logical Agent terminal; Done requires no
   active run/unclaimed follow-up and automatically executes Close Task.
9. **CONFIRMED**: bounded structured Task history is retained until Delete;
   raw VT is excluded and minimal deletion tombstones expire after 30 days.
10. **CONFIRMED**: follow-up messages merge into a pending run before start;
    while Running, ordered messages consolidate into one later TaskRun.
11. **CONFIRMED**: the background Runtime is a separate process that continues
    active Tasks after every Terminal window closes. Window exit detaches the
    Surface view and is not Stop Task or Close Task.
12. **CONFIRMED**: Stop Task is semantic Agent `Esc` and retains the task
    terminal/session; the Codex adapter maps it to `turn/interrupt`. Close Task
    unsubscribes/closes the terminal projection while preserving the Codex
    thread for later resume. Delete Task remains separate.
13. **CONFIRMED**: normal InReview does not close the terminal; user-confirmed
    Done closes it through the accepted Close Task workflow.
14. **CONFIRMED**: explicit Close Task preserves the Task and its status and
    allows the Agent terminal to be opened again. Cancel Task and Delete Task
    retain distinct semantics.
15. **CONFIRMED**: the separate Runtime is an on-demand per-user helper, not an
    installed Windows service. It uses the current user's provider credentials,
    PATH, settings root, and security identity.
16. **CONFIRMED**: raw VT is a bounded live/reconnect buffer only. Durable
    history contains structured Agent messages, tool calls/results, final
    results, failure/disconnect events, and recovery metadata.
17. **CONFIRMED**: with no active TaskRun and no Surface client, suspend the
    physical Agent-terminal projection/subscription after five minutes while
    preserving the logical TaskTerminal, Codex thread, Worktree, AgentSession
    metadata, and history. Running work is never auto-suspended.

35. **CONFIRMED**: the Codex provider uses one Runtime-owned App Server process;
    persistent Task maps to thread, TaskRun maps to turn, and structured item/
    tool/file events are the durable source. The task Surface is a synthesized
    VT projection with capability/version gating and no TUI-scraping fallback.
36. **CONFIRMED**: if Close Task interruption is not acknowledged after five
    seconds, enter ClosePending and do not kill the shared App Server for one
    Task. Restarting the shared Provider is a separate global recovery action
    that discloses all affected active Tasks.
37. **CONFIRMED**: Summary/history and Tasks have strict ownership boundaries.
    User-started Codex sessions remain in `workspace-history.db`; Task-owned
    threads/turns exist only in `workspace-tasks.db` and Tasks UI. Runtime is the
    only App Server transport and does not duplicate TaskRun content into
    AgentHistoryStore.
18. **CONFIRMED**: use a control/event named pipe and a separate framed binary
    terminal-stream pipe. Runtime control remains responsive under heavy VT
    output.
19. **CONFIRMED**: use a separate `workspace-tasks.db` with the Runtime as its
    sole writer. UI clients issue commands and subscribe to projections/events
    through the Runtime.
20. **CONFIRMED**: failure and disconnect records must contain enough session,
    execution-context, lineage, and reason metadata for a later retry to decide
    between safe resume and a fresh AgentSession without rewriting the prior
    TaskRun.
21. **CONFIRMED**: adopt the Multica-style retry boundary above: only
    allowlisted transient infrastructure failures retry automatically; user,
    credential, quota, configuration, model, session-poisoning, and execution
    environment failures require manual action.
22. **CONFIRMED**: TaskRun start/completion never infers product progress. The
    Agent explicitly sets InProgress, InReview, or Blocked through a structured,
    task-scoped command; Done is user-only. The system retains only the
    last-active-run failure rollback to Todo.
23. **CONFIRMED**: the initial Agent command surface is a restricted local
    `rtask` CLI. Long text uses stdin; endpoint, TaskRun identity, and capability
    are injected by the Runtime. A future MCP adapter calls the same internal
    Runtime commands.
24. **CONFIRMED**: Task Worktree setup snapshots current staged/unstaged tracked
    changes and bounded non-ignored untracked files without mutating the source.
    If the state cannot be reproduced exactly, setup fails without partial copy
    or fallback.
25. **CONFIRMED**: use the adapted Multica Git lifecycle: persistent per-Task
    Worktree, per-TaskRun checkpoint commits on every outcome, failure-safe
    preservation, and Multica-style explicit finalization at cleanup. No
    automatic push, merge, rebase, or source-branch mutation.
26. **CONFIRMED**: a persistent Task aggregates one logical TaskTerminal and
    ExecutionContext, zero or more AgentSessions, and multiple immutable
    TaskRuns. Retry/follow-up creates a TaskRun, never a replacement Task, and
    only Delete Task removes the aggregate.
27. **CONFIRMED**: Stop sends semantic Esc immediately without confirmation or
    automatic force-kill. Close confirms only for active/pending work, issues
    structured turn interruption, and preserves queued messages under
    `manual_hold` until explicit Open/Run/Resume. Shared-App-Server escalation
    after five seconds remains a technical compatibility decision.
28. **CONFIRMED**: Direct uses a full-run canonical-path lease with cancelable,
    no-timeout waiting; Worktree execution is parallel and holds repository
    locks only for short Git critical sections. Current-user Named Mutexes
    coordinate separate Runtime identities using the same path/repository.
29. **CONFIRMED**: after the first TaskRun, immutable execution identity cannot
    be revised in place. "Create Task from this Task" creates a new aggregate
    linked by `forked_from_task_id`; safe per-run configuration remains
    revisioned and snapshotted.
30. **CONFIRMED**: Cancel preserves all Task/worktree/history data and prevents
    automatic work; Cleanup is a separate safe-finalization operation; Delete
    removes records only after safe delivery, preserves result branches, and
    leaves a minimal replay-protection tombstone.
31. **CONFIRMED**: creation CTA contract uses Cancel, Save, and Save and Run;
    only Save and Run prepares Worktree/Runtime/Surface after persistence.
32. **CONFIRMED**: execution-log presentation uses expanded active runs,
    visible follow-up queue, collapsed newest-first history, and responsive
    structured live Run Details.
33. **CONFIRMED**: InReview keeps the logical terminal available; Done blocks on
    active/unclaimed work and closes the Agent terminal while preserving the
    aggregate and Git artifacts.
34. **CONFIRMED**: retain bounded structured content until Delete Task; retain
    only replay-protection tombstones for 30 days after deletion.

## 23. Product acceptance gate

Before implementation begins, reviewers must be able to answer all of the
following without inspecting code:

- Who owns every Surface and who may close it?
- Which directory and repository does every TaskRun modify?
- What survives closing a Surface, closing the application, or crashing?
- What distinguishes Task, TaskRun, TaskTerminal, Surface, and AgentSession?
- Which action creates a new run and which action resumes an existing session?
- What happens for every close entry point?
- What happens when a provider process exits normally or abnormally?
- What does the user review before a Task becomes Done?
- Which behaviors remain exactly compatible with ordinary Windows Terminal?

## 24. Resources

- Local ignored Multica reference: `.references/multica`
- Browser prototype: `doc/prototypes/task-page`
- Microsoft Terminal upstream sync policy: `UPSTREAM.md`
