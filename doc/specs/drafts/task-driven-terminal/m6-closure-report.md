---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M6 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M6 closure report

## Outcome

M6 completes the task-owned Agent Surface while preserving the ownership
boundary accepted in the product specification. A Surface opened from Tasks is
owned by its Task and can be closed only through the Task lifecycle. A terminal
created normally remains user-owned and keeps the existing close, split, move,
bulk-close, and connection-exit behavior.

## Runtime terminal path

- `RuntimeTaskTerminalService`, its pipe server, and its backend own the
  per-Task terminal session, bounded in-memory VT replay, current-user secured
  endpoint, client attach/detach, and generation checks.
- One Surface owner and one expiring, revisioned input-controller lease prevent
  two windows from driving the same Task terminal. Disconnect releases the
  owner so a later window can reattach without creating another Task terminal.
- Exact Esc input and the Tasks Stop command converge on provider interrupt.
  Ordinary input is steered to a running turn or persisted as a follow-up when
  no turn is currently accepting live input.
- Runtime end-of-stream carries a generation-bound, single-use authorization.
  Stale generations and replayed authorization cannot close a Surface.

## Terminal application integration

- `TaskRuntimeConnection` adapts the Runtime binary terminal stream to the
  Terminal connection surface and reconnects using the durable Task/session
  identity.
- `TaskSurfacePolicy` routes tab close, shortcuts, middle click, bulk close,
  pane close, move, connection exit, window exit, and Runtime close through one
  policy. User close origins are denied or skipped for Task surfaces; window
  exit detaches; connection exit retains a placeholder.
- Task surfaces are single-pane and cannot be split or moved. Open activates an
  existing owner when possible and reattaches after ownership is released.
- Close Task terminal requests the Runtime lifecycle operation. Closing an app
  window never translates into Stop or Close, so active work continues in the
  per-user Runtime.

## Verification

- Surface policy: 4/4 TerminalApp tests passed, including every close origin,
  ordinary-user compatibility, detach/placeholder actions, and one-time
  generation authorization.
- Terminal service: 2/2 real named-pipe tests passed, covering snapshot, live
  input, Esc, end-of-stream, owner exclusion, detach/reopen, and stale
  generation refusal.
- Projection and protocol tests passed for single-generation open, cross-owner
  activation, capability negotiation, and strict terminal DTO validation.
- The final unified incremental Release/MSIX gate passed on 2026-09-01 without
  cleaning build outputs.
