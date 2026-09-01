---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M7 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M7 closure report

## Outcome

M7 completes Runtime-owned task orchestration. The UI submits durable commands
and renders projections; it does not run Git, launch Codex, hold directory
locks, or infer completion from terminal/process state.

## Scheduling and execution

- One scheduler owns a bounded set of at most four Run workers. Direct runs
  acquire and release their canonical directory lease on the same worker
  thread for the entire Run. Worktree runs reuse the Task Worktree and retain
  repository-locked prepare, checkpoint, finalize, and recovery phases.
- The executor starts/resumes the Task Codex session, injects the scoped
  `rtask` endpoint/run/capability context, synthesizes terminal output, and
  persists structured provider events, status, result, and failure evidence.
- Unstarted messages fold into the pending prompt. Deferred messages remain
  durable and are claimed in canonical order into one later TaskRun. Claimed
  message content is immutable; unclaimed messages support edit and removal.
- Only allowlisted infrastructure reasons retry automatically, with bounded
  attempts and exponential backoff. Manual Retry creates a distinct linked
  TaskRun. Poisoned or interrupted sessions use the accepted resume/fresh
  fallback policy.

## Lifecycle and resource policy

- Stop, Close, Cancel, Finalize Worktree, Delete, Done/Reopen, derived Task, and
  global provider restart are represented by strict idempotent Runtime command
  contracts. Stop preserves manual hold; Close ends the Task terminal; Delete
  refuses unproven delivery and retains a 30-day replay tombstone.
- Terminal sessions with no active Run or attached Surface suspend after five
  minutes. The per-user Runtime exits only after all control clients, active
  Runs, pending intents/messages/Git work, and live terminals are quiescent.
- Window exit only detaches UI. A persisted active Run keeps the Runtime alive
  and accepts a later client reconnect with the same Runtime instance.
- `TaskAgentSessionRegistry` filters only current Task-owned Codex sessions from
  Summary; manual sessions and unrelated history remain unchanged.

## Verification

- Orchestrator policy/end-to-end tests: 5/5 passed for ordered follow-ups,
  retry limits/backoff, a Direct Queued-to-Completed Run, resource release, and
  Stop intent during directory wait. A Ready terminal whose Provider session
  is already absent also suspends instead of indefinitely blocking Runtime
  drain.
- Runtime projection tests: 6/6 passed for durable messages, revision/removal,
  manual Retry, derived Task, terminal ownership, and lifecycle commands.
- TerminalApp tests passed for Task page mapping/actions and exact Summary
  exclusion; the full Runtime suite passed 216/216 after updating the legacy
  idle fixture to model an unmanaged active phase instead of an M7-consumable
  Queued Run.
- The final unified incremental Release/MSIX gate passed on 2026-09-01.
