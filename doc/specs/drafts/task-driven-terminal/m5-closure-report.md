---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M5 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M5 closure report

## Outcome

M5 completes the fixed Tasks page as a Runtime-backed product surface. The UI
does not open SQLite, launch Codex, run Git, or own Task process state. The
prototype source remains available only through the explicit
`RICHER_TASKS_USE_PROTOTYPE=1` fallback/test seam.

Task-owned terminal Surface attachment and close-origin policy remain M6.
`Save and Run` durably creates the Task and an idempotent queued TaskRun; M7
owns preparation scheduling and automatic orchestration beyond that queue.

## Runtime boundary

- Strict, versioned projection/query and UI-command JSON DTOs cover Task,
  ExecutionContext, terminal state, AgentSession, TaskRun, queued/deferred
  messages, structured events, result, reason, and event cursor.
- Terminal UI clients can query and submit Task commands. TaskCtl remains
  restricted to scoped `rtask` commands; cross-client command shapes fail
  closed.
- Create, Start, Stop, Close, Mark Done, and Reopen use canonical IDs, expected
  Task versions, transaction receipts, and replay-safe results.
- List queries omit event payloads; selected detail queries load a bounded tail.
  Oversized multi-Task responses are reduced before crossing the 1 MiB control
  frame boundary.
- A real child `TaskRuntime.exe` process test creates and reloads a Task over the
  secured control pipe.

## Projection and interaction

- `RuntimeTaskProjectionSource` owns the reconnecting client and background
  request queue. `TaskProjectionCoordinator` publishes immutable snapshots and
  rejects stale generations within one Runtime epoch while accepting a fresh
  epoch after helper restart.
- ViewModels mutate only on the UI dispatcher. Selection is preserved by Task
  ID across list/detail refresh, filtering, optimistic Save, and Runtime
  resnapshot.
- Active Tasks use one bounded two-second coalesced refresh; idle pages do not
  poll. Selected detail refreshes separately, preventing every list row from
  carrying large event history.
- The in-page draft binds the selected Workspace identity and effective source
  root. Save has no Run side effect; Save and Run queues exactly one Run.
- Execution log places active Runs first, then queued messages and newest
  history. Structured Run Details use the fixed 290-pixel wide aside and an
  inline compact layout, avoiding the earlier inconsistent right-column widths.
- Runtime M5 intentionally hides Open/Close terminal actions until M6 supplies
  a task-owned Surface.

## Accessibility and verification

- Tasks views inherit the application/system theme instead of forcing Dark,
  preserving high-contrast behavior.
- Existing theme brushes, text status/glyph redundancy, wrapping, visible
  focus, keyboard reachability, and Escape handling cover wide and narrow
  layouts. Escape closes the draft or narrow overlay pane and restores focus.
- New user strings are present in English and Simplified Chinese resources.
- Loading has a distinct progress state; an empty-result message is not shown
  while the first Runtime snapshot is pending.

Targeted verification covers strict DTO round trips and rejection, Runtime
command replay/stale versions/workspace filtering/event tails, product-router
client isolation, real-process create/query, epoch/generation races,
512-item capacity, high-volume event ordering, UI selection races, mapping of
Runs/messages/details, XAML construction, and existing prototype behavior.

Final acceptance uses the unified incremental Release/MSIX and native-test
gates. Manual Narrator, mixed-DPI, and visual high-contrast checks remain
explicit release acceptance rather than claims made by native unit tests.

The final unified gate passed, including Hook 246/246, every Task native
selection, TerminalApp/Settings selections, real Runtime process checks, and
Workspace CLI smoke. Portable packaging completed with `-SkipBuild -NoLaunch`.

- MSIX: 8,458,218 bytes; SHA256
  `030891BF454F4F774FF57EC5939651D411EA7DA971D9FED48C1D863DE639EB23`.
- Portable ZIP: 12,326,848 bytes; SHA256
  `28D9C7AACE7EAA093D6E40AE6B83BEF65251024C00386F5C1F046820F48B3B07`.
