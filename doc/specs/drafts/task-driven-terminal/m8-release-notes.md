# M8 release, migration, and rollback notes

## Distribution contract

Registered MSIX and portable ZIP layouts must contain matching non-empty
`WindowsTerminal.exe`, `TaskRuntime.exe`, and `rtask.exe` files. The release
gate and portable packager now reject an archive or deployment missing either
Task executable.

Task Runtime protocol 1.1 adds terminal surfaces, orchestration, and graceful
drain capabilities while retaining the 1.0 request shapes for existing
operations. A newer UI that connects to an older Runtime supporting
`runtime_drain` requests a drain. The helper stops only after durable Task work
is quiescent; the reconnecting client then activates the matching packaged
Runtime. An older helper without drain support remains usable for its negotiated
capabilities and exits through the existing idle grace.

## Data migration

M6-M8 require no destructive database migration. Existing schema-v1 Task,
TaskRun, message, intent, terminal, Git, event, receipt, and tombstone records
remain authoritative. Runtime recovery treats unfinished provider work as
Interrupted only when persisted Agent-session evidence identifies a real
managed execution; synthetic M2 activity fixtures remain untouched.

Rollback to a build that negotiates protocol 1.0 is safe for the database:
newer command names are not stored as schema-dependent rows, and all persisted
states already belong to schema v1. Before rollback, let active runs finish or
Stop them, close all Terminal UI clients, and allow the Runtime to drain. Do
not delete `workspace-tasks.db` or managed Worktrees as a rollback mechanism.

## Security and privacy review

- control and terminal named pipes retain current-user ACLs and exact Runtime,
  session, generation, sequence, and payload validation;
- one expiring input lease prevents two windows from controlling a Task
  terminal concurrently;
- runtime-authorized Surface close is generation-bound and consumed once;
- Direct mode uses canonical path identity and a thread-owned lease for the
  entire run; Worktree operations retain repository locks and exact managed
  path validation;
- `rtask` tokens remain random, method/run scoped, ten-minute maximum, and are
  invalidated at terminal Run outcome;
- raw VT remains bounded memory only; structured provider payloads retain
  existing truncation and redaction limits;
- Delete refuses undelivered work, preserves result branches, and leaves only
  the 30-day replay tombstone.

## Upstream-sync review

M6-M8 keep task state, transport, orchestration, and policy in fork-owned
components. Microsoft hotspots are limited to TerminalPage integration,
ordinary close/split routing guards, TerminalPaneContent close-on-exit
suppression, project manifests, and localized resources. Renderer, ConPTY,
parser, and ordinary user-terminal connection behavior are unchanged, so
future Microsoft rendering and PTY updates remain independently syncable.
