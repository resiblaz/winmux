# Tasks

Tasks is a fixed page beside Summary. It coordinates durable Codex work without
turning user-created terminals into managed tasks.

## Ownership

- A terminal opened from Tasks belongs to that Task. Close it from the Task
  detail; tab close, middle-click, close shortcuts, bulk close, split, and move
  commands do not remove it.
- A terminal opened normally remains user-owned and behaves exactly like an
  ordinary Windows Terminal tab.
- Closing a Terminal window only detaches its Task surfaces. The per-user Task
  Runtime continues active work and a later window can reattach.
- `Stop (Esc)` interrupts the active Codex turn and keeps the Task terminal.
  `Close task terminal` ends the Task terminal session and preserves the Task,
  structured history, Worktree, checkpoints, and result branch.

## Creating and running

`Save` creates only a Todo Task. `Save and run` also creates an immutable
TaskRun and lets the background Runtime prepare its directory and Codex
session. Git repositories default to a Task-specific Worktree; Direct mode is
explicit and holds an exclusive canonical-directory lease for the run.

The Runtime is a current-user helper started on demand. It is not a Windows
service and does not require elevation. `TaskRuntime.exe` and `rtask.exe` must
remain beside `WindowsTerminal.exe` in registered and portable distributions.

## Follow-ups, retries, and completion

- Messages entered on the Tasks page are durable. A message joins a run that
  has not started yet, or waits for the active run to finish. Multiple waiting
  messages become one ordered follow-up TaskRun.
- Unclaimed messages can be selected, edited, or removed. Claimed content is
  immutable history.
- Only allowlisted infrastructure failures retry automatically, with bounded
  exponential backoff. Manual Retry always creates another TaskRun.
- Codex can report In progress, In review, Blocked, and a final result through
  its scoped `rtask` capability. Only the user confirms Done.
- Done closes the Task terminal but retains files and history. Cancel preserves
  the Task under a manual hold. Reopen clears that hold.

## Cleanup and deletion

`Finalize worktree` checkpoints remaining changes, retains a branch containing
produced work, and removes only the validated managed Worktree. `Delete task`
first stops active work and performs the same safe delivery. If checkpoint or
delivery cannot be proven, deletion is refused and the Worktree path is
preserved. Successful deletion retains only a bounded replay-protection
tombstone for 30 days.

## Storage and privacy

Task records live in `workspace-tasks.db` under the Terminal settings root. The
Runtime is its sole writer. Structured messages, tool calls/results, failures,
recovery events, and Git metadata remain until Delete; raw terminal VT is only
held in a bounded in-memory replay buffer and is never durable history.

Runtime control and terminal pipes are restricted to the current Windows user.
Provider diagnostics are bounded and redacted before persistence. Task-scoped
Codex threads are excluded from Summary so that Tasks and ordinary manual
sessions remain separate.
