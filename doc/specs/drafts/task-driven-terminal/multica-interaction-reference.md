---
author: RicherLife / Codex collaboration
created on: 2026-08-29
last updated: 2026-08-30
multica commit: 64ec7f54163d918d5d7fd4dcae857f241b7842d0
status: accepted reference evidence; confirmation queue complete
---

# Multica Interaction Reference for the Task-driven Terminal

## 1. Purpose and limits

This document answers unclear interaction questions by inspecting the pinned,
locally ignored Multica checkout at `.references/multica`.

Multica is used only as behavioral and architectural evidence:

- its source, UI components, and assets must not be copied;
- its custom license is not treated as compatible implementation material;
- behavior that depends on Multica's server/daemon model is not automatically
  suitable for a terminal-owned process;
- every proposed adaptation remains `OPEN` until the user confirms it.

## 2. Evidence sources

Primary local sources inspected for this revision:

- `apps/docs/content/docs/tasks.zh.mdx`
- `apps/docs/content/docs/issues.zh.mdx`
- `apps/docs/content/docs/assigning-issues.zh.mdx`
- `apps/docs/content/docs/mentioning-agents.zh.mdx`
- `apps/docs/content/docs/comments.zh.mdx`
- `apps/docs/content/docs/project-resources.zh.mdx`
- `apps/docs/content/docs/daemon-runtimes.zh.mdx`
- `packages/views/issues/components/execution-log-section.tsx`
- `packages/views/issues/components/terminate-task-confirm-dialog.tsx`
- `packages/views/locales/zh-Hans/issues.json`
- `server/internal/service/builtin_skills/multica-mentioning/SKILL.md`
- `server/internal/daemon/execenv/local_worktree.go`
- `server/internal/daemon/daemon.go`

## 3. Interaction reference matrix

### M-01: persistent work versus one execution

**Multica behavior**

- Issue holds the durable goal, discussion, assignee, and product status.
- A task is one execution attempt.
- Retry creates a new task; earlier attempts remain unchanged.
- `completed` only means the execution ended normally and does not mean the
  Issue is Done.

**Applicability**

Directly applicable after terminology translation: this product's user-visible
Task corresponds to Multica's Issue, while this product's TaskRun corresponds
to Multica's task.

**Proposed adaptation**

Keep one persistent Task with immutable TaskRuns and reserve Done for human
confirmation.

**User decision**: `CONFIRMED-Q1-PERSISTENT-TASK` on 2026-08-29. One persistent
Task owns the logical terminal and ExecutionContext and accumulates immutable
TaskRuns. Retry/follow-up creates a new TaskRun; only Delete Task removes the
aggregate.

### M-02: save ownership without starting

**Multica behavior**

- Assigning an Issue to an agent normally starts a run.
- The confirmation flow offers "Don't start yet", which saves ownership but
  creates no task.
- Backlog assignment also does not start until the Issue leaves Backlog or a
  later comment triggers work.

**Applicability**

Directly applicable to Task creation.

**Proposed adaptation**

The creation dialog has `Save` and `Save and Run`; `Save` creates no terminal
process or TaskRun.

**User decision**: `CONFIRMED-Q2-SAVE-RUN` on 2026-08-29. Use one in-page draft
with Cancel, Save, and primary Save and Run. Save persists only a Todo Task;
Save and Run persists before Worktree/Runtime/Surface side effects.

### M-03: product status is not derived from process state

**Multica behavior**

- The server does not automatically move an Issue to InProgress when a task
  starts or to InReview when a task completes.
- The agent explicitly updates Issue status as its work progresses.
- A system exception moves an InProgress Issue back to Todo when its last active
  task fails and no retry remains.
- Done is normally human confirmation or an explicit integration such as a
  merged closing PR.

**Applicability**

The separation is directly applicable. The mechanism is not: this product must
decide whether its Codex integration can reliably issue structured Task-status
commands.

**Options to confirm**

- A: follow Multica; only explicit agent/user commands change Task status.
- B: application starts set InProgress and successful process exit sets
  InReview automatically; Done remains human-only.

**User decision**: `CONFIRMED-Q3-EXPLICIT-STATUS` on 2026-08-29. Follow
Multica: Agent/user commands explicitly change Task product status; TaskRun
start/completion does not. Agent scope may set InProgress, InReview, or Blocked
but not Done. Preserve the last-active-run failure rollback to Todo.

### M-04: execution-log hierarchy

**Multica behavior**

- Execution log lives in the Issue detail rather than a separate task page.
- Active runs remain visible at the top.
- Past terminal runs collapse under "Show past runs (N)".
- A live transcript dialog appends messages and tool events while open.
- A live agent chip also exposes active work without navigating away.

**Applicability**

Directly applicable to the Task detail. The fixed Tasks page remains this
product's own navigation model.

**Proposed adaptation**

Keep active runs expanded, collapse past runs, and expose the active-run state
both in the Task row and task-owned Surface tab.

**User decision**: `CONFIRMED-Q4-EXECUTION-LOG` on 2026-08-29. Embed the log in
Task detail, keep active runs expanded, show queued follow-ups, collapse
newest-first history, and open structured live Run Details in a responsive
panel/page.

### M-05: stop is explicit and irreversible

**Multica behavior**

- Deferred, queued, starting, directory-waiting, and running tasks can be
  stopped.
- Stop requires an explicit destructive confirmation.
- Copy explains that the task cannot be resumed; running work may take a few
  seconds to stop.
- Changing Issue status or assignee does not stop an active task.

**Applicability**

The confirmation pattern applies. "Cannot resume" may not apply if this
product preserves an AgentSession, but the cancelled TaskRun itself must remain
terminal and immutable.

**Proposed adaptation**

Stop cancels only the current TaskRun. Continue/retry always creates a new
TaskRun. Closing a busy task-owned terminal combines stop and close in one
explicitly worded confirmation.

**User decision**: `CONFIRMED-Q5-STOP-CLOSE-TIMING` on 2026-08-29. Diverge from
Multica where terminal semantics require it: Stop is immediate semantic Esc
without confirmation and never auto-kills; Close confirms when work is active,
waits five seconds, and persists a manual hold so deferred messages do not
restart immediately. The later shared App Server decision supersedes direct
per-Task process-tree kill; timeout escalation remains a technical follow-on.

### M-06: manual retry and session reuse

**Multica behavior**

- Retry targets the agent used by the selected historical run, not necessarily
  the Issue's current assignee.
- Existing file work is preserved when possible.
- The prior session is resumed only when safe and claimed by the same runtime.
- Context overflow or invalid requests trigger a fresh session over the
  preserved working directory.
- If the prior directory is gone, retry uses a fresh working directory.

**Applicability**

Directly applicable to TaskRun history and AgentSession recovery, except this
product currently has one Codex provider.

**Proposed adaptation**

Retry snapshots the original run's provider/configuration, reuses the Task
execution root, and resumes the session only after a provider capability/safety
check.

**User decision**: `CONFIRMED-Q6-RETRY-RECOVERY` on 2026-08-29. Preserve the
execution context/files, resume only when the provider reports the prior
session safe, and create a fresh AgentSession for poisoned-session manual
retry.

### M-07: follow-up messages while work is pending or running

**Multica behavior**

- A new comment for an agent with a waiting task is folded into that pending
  task.
- If the agent is already executing, later comments wait until the active task
  ends and are consolidated into one follow-up execution.
- The comment is not dropped and does not start a concurrent duplicate run.
- Trigger outcomes distinguish coalesced, deferred, blocked, and newly queued.

**Applicability**

Highly applicable to the earlier "queue message" question. This product has an
additional direct terminal-input channel, so queued Task messages must be
visually distinct from keystrokes sent to the terminal process.

**Options to confirm**

- A: adopt Multica's rule: merge before start; while running, queue one
  consolidated follow-up TaskRun.
- B: forward messages live to Codex when supported, queue only when live input
  is unsupported.
- C: disable Task-page messages during Running and require direct terminal
  input.

**Preliminary recommendation**: A. It is deterministic, avoids concurrent runs,
and preserves a clear TaskRun boundary.

**User decision**: `CONFIRMED-Q7-FOLD-DEFER` on 2026-08-29. Agent-terminal
keystrokes remain immediate live input. Tasks-page follow-up messages are
durable: fold into queued/preparing/waiting work, or consolidate into one new
TaskRun after a running TaskRun ends.

### M-08: directory locking

**Multica behavior**

- `in_place` local directories run one task at a time.
- Later tasks enter `waiting_local_directory` and may be cancelled while
  waiting.
- Paths are canonicalized so symlink aliases share the same lock.
- Waiting has no independent timeout.

**Applicability**

Directly applicable to managed Direct TaskRuns. It cannot prevent ordinary
user terminals or external programs from modifying the directory.

**Proposed adaptation**

Canonicalize the runtime root, serialize managed Direct TaskRuns, expose a
cancelable WaitingForDirectory state, and clearly label unmanaged-edit risk.

**User decision**: `CONFIRMED-Q8-DIRECT-LEASES` on 2026-08-29. Direct holds a
canonical real-path lease for the entire managed TaskRun, waits without timeout,
and may be cancelled. Worktree runs are parallel; repository snapshot/create/
checkpoint/finalization critical sections use a short common-dir lock.

### M-09: Worktree isolation

**Multica behavior**

- For a local Git directory, `worktree` mode creates a separate worktree for
  each execution task, enabling concurrency.
- It snapshots tracked dirty state without modifying the user's working tree
  and replays bounded untracked files.
- Results are committed to an `agent/<agent>/<task>` branch.
- Failure still preserves produced work; unsafe finalization preserves the
  worktree instead of deleting it.
- A run with no changes leaves no result branch.
- Selecting worktree never silently falls back to in-place execution.

**Applicability and mismatch**

Isolation and safe preservation apply. The lifecycle does not map directly:
Multica creates one worktree per execution task, whereas this product currently
proposes one logical terminal and potentially one worktree per persistent Task.

**Options to confirm**

- A: one worktree per persistent Task, reused by all TaskRuns.
- B: one worktree per TaskRun, matching Multica, with a new task-owned terminal
  generation for each run.
- C: Direct-only first slice; defer Worktree after the terminal/run model is
  proven.

**User decision**: `CONFIRMED-Q9-PER-TASK-WORKTREE` on 2026-08-29. For a Git
repository, default to one Worktree owned by the persistent Task and reuse it
across all TaskRuns. Direct is an explicit Git choice or non-Git fallback.
Close/Done preserve the Worktree, and failure never deletes produced work.

**User decision**: `CONFIRMED-Q23-FAITHFUL-SNAPSHOT` on 2026-08-29. Follow
Multica's visible-state principle: capture tracked state without modifying the
source/index/stash, copy bounded non-ignored untracked files, record a baseline,
and reject setup rather than partially replay or fall back when fidelity cannot
be proven.

**User decision**: `CONFIRMED-Q24-ADAPTED-GIT-LIFECYCLE` on 2026-08-29. Align
Multica's Git snapshot, baseline, auto-commit, failure preservation, empty-branch
cleanup, and no-push/no-merge safety behavior. Deliberately adapt Worktree
ownership to the persistent Task: checkpoint each TaskRun and defer Worktree
removal/final delivery to explicit Task cleanup.

### M-10: resource changes and historical runs

**Multica behavior**

- Changing project resources or execution mode affects future tasks.
- Finished execution records are not rewritten.

**Applicability**

Directly applicable as a history invariant.

**Proposed adaptation**

Every TaskRun snapshots its ExecutionContext. Changing a Task root/mode, if
allowed, creates a new context revision and affects only later TaskRuns.

**User decision**: `CONFIRMED-Q10-IMMUTABLE-EXECUTION-IDENTITY` on 2026-08-29.
Before first run, execution identity is editable. Afterwards, repository/root/
mode/Worktree/branch are immutable; major changes create a derived Task linked
by `forked_from_task_id`. Safe instructions/model/relative-workdir changes are
revisioned and snapshotted by later TaskRuns.

### M-11: deletion versus cancellation

**Multica behavior**

- Deleting an Issue is irreversible and removes comments, attachments, and
  linked records.
- Unfinished tasks are cancelled with deletion.
- When work should merely stop, Cancelled status is recommended so discussion
  and results remain.

**Applicability**

Directly applicable, with additional TaskTerminal and Worktree cleanup choices.

**Proposed adaptation**

Prefer Cancel Task for normal abandonment. Delete Task is destructive, stops
active runs, closes its task-owned terminal through the Task workflow, and
handles Worktree cleanup as an explicit separate choice.

**User decision**: `CONFIRMED-Q11-CANCEL-CLEANUP-DELETE` on 2026-08-29. Cancel
preserves the aggregate/Worktree/history under manual hold; Worktree cleanup is
independent; Delete requires safe finalization, preserves produced branches,
removes content records, and retains only a minimal late-event tombstone.

### M-12: runtime and application lifetime

**Multica behavior**

- A separate daemon owns execution and normally runs in the background.
- Desktop starts the daemon, but the execution model is not tied to an Issue
  page or browser tab.
- Runtime restart reclaims tasks that did not end cleanly; active tasks may
  fail and qualify for automatic retry.

**Applicability and mismatch**

This is architectural evidence for a background runtime, not an answer for the
confirmed task-terminal close-authority rule. Multica has no visible
task-owned terminal whose Surface lifecycle must be enforced.

**Options to confirm**

- A: terminal-owned execution; closing/stopping the TaskTerminal controls the
  provider process/PTY.
- B: daemon/background execution; the product must independently decide whether
  a TaskTerminal is absent, a companion shell, or an attached view of the
  Runtime-owned Agent PTY.

**User decision**: `CONFIRMED-Q12-B` on 2026-08-29. Use background Runtime
execution. Clarified subsequently: one Runtime-owned Codex App Server manages
Task threads/turns, and the task-owned Surface renders a synthesized VT
projection from structured events rather than stock TUI/PTY output.

**Still open**

- `CONFIRMED-Q14-OUTLIVES-WINDOWS` on 2026-08-29: the Runtime is a separate
  process boundary; active Tasks continue after every Terminal window exits,
  and a later window reconnects. This follows Multica's UI-independent daemon
  lifetime at the product level.
- `CONFIRMED-Q15-STOP-CLOSE` on 2026-08-29 supersedes the earlier cascade
  interpretation: Stop Task sends the Agent semantic `Esc` and keeps the Agent
  terminal/session open; Close Task closes the Runtime-backed Agent terminal
  and its Surface. Multica has no terminal equivalent; this is a direct product
  decision for this fork.
- `CONFIRMED-Q17-CLOSE-PRESERVES` on 2026-08-29: Close Task preserves the Task
  record, product status, context, and complete history and allows reopening a
  new Agent-terminal generation. Cancel and Delete remain separate. Multica
  does not provide a terminal equivalent; its broader preference for
  cancellation over destructive deletion is consistent with preserving work.

### M-13: task-owned terminal close, panes, and window moves

**Multica behavior**

No equivalent. Multica's runs are daemon-owned and its Issue UI does not own a
Windows Terminal Surface.

**Applicability**

None. These behaviors must be designed from this fork's ownership contract and
then confirmed directly with the user.

**User decision**: `CONFIRMED-Q13-SINGLE-SURFACE` on 2026-08-29. First slice:
one Task owns one single-Pane Surface and one input-controller window. No split,
duplicate, Workspace move, or window transfer. Other windows may view the Task
and activate the owner; they reattach only after the owner lease is released.

### M-14: Agent-to-coordinator control surface

**Multica behavior**

Multica gives running agents its CLI and runtime instructions. Agents explicitly
write Issue status, comments, and results through structured CLI operations
rather than requiring the server to infer intent from prose or process exit.

**Applicability**

Directly applicable. This fork needs a smaller, local-only, TaskRun-scoped
surface rather than the full Multica CLI.

**User decision**: `CONFIRMED-Q22-SCOPED-CLI` on 2026-08-29. Provide an
`rtask` CLI whose injected capability authorizes only the active TaskRun.
Status/result text uses stdin where appropriate. MCP may later wrap the same
Runtime commands but cannot create a second behavior path.

## 4. Confirmation queue

No product-interaction confirmations remain.

`CONFIRMED-Q16-DONE-CLOSES` on 2026-08-29: InReview retains the logical Agent
terminal and may idle-suspend; Done requires no active TaskRun or unclaimed
follow-up and automatically performs Close Task while preserving Task/Git
artifacts.

`CONFIRMED-Q25-RETENTION` on 2026-08-29: retain Tasks and bounded structured
history until explicit Delete, never retain raw VT as durable history, and keep
minimal deletion tombstones for 30 days.

`CONFIRMED-Q18-PER-USER-HELPER` on 2026-08-29: use an on-demand,
non-elevated per-user Runtime helper rather than a Windows service. This aligns
with Multica's user-launched daemon model while preserving portable and packaged
application scenarios.

`CONFIRMED-Q19-STRUCTURED-HISTORY` on 2026-08-29: follow Multica's durable
history boundary. Persist structured results, messages, tool calls/results,
failure/disconnect reason codes, and recovery metadata; retain raw VT only as a
bounded live buffer. Recovery records must support a later retry/resume verdict.

`CONFIRMED-Q20-IDLE-SUSPEND` on 2026-08-29: unlike Multica, this product has a
persistent logical Agent terminal. To bound resources, suspend its physical
event subscription/VT projection after five minutes with no active TaskRun and
no attached UI; preserve the Codex thread/context/history and auto-resume on
Open Task or new work.

`CONFIRMED-Q21-TRANSIENT-RETRY` on 2026-08-29: automatically retry only
allowlisted transient infrastructure failures. UI disconnect is not a failure;
user Stop/Close and auth, quota, configuration, model, poisoned-session, and
execution-environment failures wait for explicit user action.

No item in this queue becomes a product requirement merely because Multica uses
it. User confirmation is required.
