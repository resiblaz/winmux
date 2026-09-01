---
author: RicherLife / Codex collaboration
created on: 2026-09-01
status: M4 closure passed on feature/task-runtime-foundation; not merged
---

# Task-driven Terminal M4 closure report

## Outcome

M4 is implementation-complete. `TaskProvider` is a separate static component;
it does not accumulate Codex parsing, process transport, persistence, or UI in
`TaskRuntimeHost` or `TerminalPage`.

The Runtime owns one lazily started Codex App Server process. Runtime startup
and empty Task queries succeed when Codex is not installed. The process is
started only for provider work, directly from a validated native executable,
with a sanitized environment, restricted inherited handles, hidden window,
and kill-on-Runtime-close Job ownership.

## Provider contract

- `initialize/initialized`, `thread/start`, `thread/resume`, `turn/start`,
  `turn/steer`, `turn/interrupt`, and `thread/unsubscribe` use bounded JSONL
  JSON-RPC correlation.
- The compatibility gate matches Codex App Server v2 and rejects versions
  older than the accepted 0.150 minimum. A checked 0.151 generated schema is
  represented by a deterministic minimal-contract fixture.
- Command, file-change, and permission approvals are correlated as App Server
  requests and answered by request ID. They are not parsed from terminal text.
- Item, message, tool, file, plan, diff, token, lifecycle, error, and unknown
  optional notifications become bounded provider-neutral records. Credential
  fields are redacted before persistence.
- `RICHER_TASK_ENDPOINT`, `RICHER_TASK_RUN_ID`, the short-lived capability, and
  the `rtask` path are injected through per-thread configuration. A Task cannot
  override those authority variables.
- Structured events render to a bounded synthesized VT projection; raw VT is
  not the durable record.

## Shared-provider recovery

`TaskProviderCoordinator` owns only provider session/run registration and
lifecycle policy. Translation, transport, retry classification, persistence,
and Runtime pumping remain separate modules.

- Multiple Tasks share one adapter/process.
- A crash reconciles every active Run as Interrupted/runtime_recovery and
  retains the accepted resume/retry verdict.
- Rejected steering is durably enqueued as a deferred user message.
- A close interrupt acknowledged by the Provider enters Closing. A five-second
  timeout enters ClosePending. Neither path terminates the shared process for
  one Task; projection close happens after the terminal turn event.
- Closed sessions and terminal Run registrations are pruned at bounded
  capacity, so completed Tasks do not create unbounded resident state.

## Durable integration and verification

TaskCore now provides transactional AgentSession recording and structured
Provider event append commands. Runtime maps coordinator callbacks to those
commands, TaskRun state transitions, failure/recovery metadata, and deferred
messages. Every write uses Task version, event sequence, request digest, and a
durable receipt.

Verified targeted suites include:

- `TaskProviderLocalTests` (15/15): method, event, approval, schema, error, crash,
  ClosePending, suspension/resume, retry classification, VT, and scoped-context
  matrix;
- `TaskCoreProviderStoreTests`: session binding/state, event ordering,
  idempotent replay, cross-Task rejection, and transaction rollback;
- `RuntimeTaskProviderSinkTests` and `RuntimeCodexDiscoveryTests`: durable
  mapping, 100-event ordering, deferred steering, native executable safety,
  and no-install startup;
- an explicit installed Codex CLI 0.151.0 initialize/initialized smoke, with no
  thread, turn, model request, login, or network dependency.

All builds use the unified incremental Release graph with `/m:1` and
`CL_MPCount=1`; caches are preserved and no user application is restarted.
