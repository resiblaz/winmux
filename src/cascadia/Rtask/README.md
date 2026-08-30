# rtask Agent-scoped CLI

`rtask.exe` is the restricted Agent-to-Task Runtime command client. It ships
beside `TaskRuntime.exe` and supports only:

```text
rtask status in-progress
rtask status in-review
rtask status blocked --reason-stdin
rtask result --stdin
```

The Runtime injects `RICHER_TASK_ENDPOINT`, `RICHER_TASK_RUN_ID`, and
`RICHER_TASK_CAPABILITY`. The CLI accepts no Task ID or capability argument,
derives the expected Runtime identity from an exact local pipe endpoint, uses a
TaskCtl Hello requiring `task_commands`, and sends one correlated
`SubmitCommand` request. It never opens `workspace-tasks.db`.

Blocked reasons and results are read as exact binary stdin bytes and then
validated as strict bounded UTF-8 by the shared protocol DTO. Capability and
text values are never printed by local validation failures. Exit codes are:

| Code | Meaning |
|---:|---|
| 0 | success/help |
| 2 | command-line usage error |
| 3 | required injected context missing |
| 4 | invalid context or stdin/DTO |
| 5 | Runtime connection/send/receive failure |
| 6 | Runtime rejected the command |
| 7 | local protocol/resource failure |
