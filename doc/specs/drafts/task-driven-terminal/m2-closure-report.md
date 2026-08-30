---
author: RicherLife / Codex collaboration
created on: 2026-08-31
status: M2 closure passed on feature/task-runtime-foundation
---

# Task-driven Terminal: M2 Closure Report

M2 is closed on `feature/task-runtime-foundation` through
`d5f983f12 feat: add task runtime idle exit`. This report records the automated
evidence for the Runtime/process/protocol milestone; it does not merge `main`.

## Scope and composition

| M2 boundary | Product composition | Primary evidence |
|---|---|---|
| Control server and subscriptions | Hello-first multi-client server, atomic subscription reservations, bounded queues | `RuntimeControlServerTests`, `RuntimeControlNotificationTests`, `RuntimeControlNotificationConcurrencyTests` |
| Reconnecting client and heartbeat | `TaskRuntimeClient` Reader/Writer/Supervisor with virtual scheduler and reserved heartbeat work | `TaskRuntimeClientReconnectTests`, `TaskRuntimeClientSubscriptionIntegrationTests`, `TaskRuntimeClientHeartbeatIntegrationTests`, `TaskRuntimeClientHeartbeatLifecycleTests` |
| Real per-user host and activation | `TaskRuntime.exe --serve`, stable instance identity, connect-first launcher, launch mutex, parent-Job independence | `RuntimeProductHostProcessTests`, `RuntimeOnDemandActivationTests` |
| Terminal transport and live session | Separate binary pipe, bounded output ring/client queues, one versioned controller lease, injected Backend | `RuntimeTerminalChannelTests`, `RuntimeTerminalSessionLifecycleTests`, `RuntimeTerminalSessionFrameTests`, `RuntimeTerminalInputLeaseTests` |
| Agent-scoped commands | TaskCtl-only product router, Run/Task/method capability, atomic TaskCore mutation/event/receipt, packaged `rtask.exe` | `RuntimeRtaskCommandServiceTests`, `RuntimeProductRouterTests`, `RuntimeRtaskEndToEndTests` |
| Terminal/helper idle lifecycle | Five-minute projection suspend policy and two-minute ProductHost exit monitor backed by one TaskCore activity query | `RuntimeTerminalLifecycleTests`, `RuntimeHelperIdleStateTests`, `TaskCoreRuntimeActivityTests`, `RuntimeProductHostIdleProcessTests` |
| Control/data head-of-line isolation | Independent control and terminal pipes; terminal backpressure cannot stall control Ping | `RuntimeTerminalChannelTests::TerminalBackpressureDoesNotBlockControlPipe` |
| Parent exit and recovery | Helper is outside parent Jobs, survives short-lived parents and every UI disconnect while work is active, reconnects with the same instance | `RuntimeOnDemandActivationTests`, `RuntimeProductHostIdleProcessTests::ActiveRunSurvivesAllUiClientsAndAllowsReconnect` |

The Runtime owns `workspace-tasks.db`, the concrete product router, `rtask`
authorization, and helper idle exit. The terminal lifecycle/session types remain
caller-serialized seams for the later provider/task-Surface dispatcher. Real
Codex App Server execution, Task-owned Surface attachment, and Git execution are
explicitly outside M2 and remain M4/M6 and M3 work respectively.

## Automated gates

The canonical unskipped gate passed from the repository root:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\test-workspace-release.ps1
```

It completed the unified Release/MSIX and native-test graph, 246 durable Hook
assertions, all selected application/settings tests, and the `wt workspace`
CLI smoke including its bounded transient retry.

The complete M2 native DLL totals were also run directly:

| Suite | Passed |
|---|---:|
| `TaskCore.LocalTests.dll` | 61 / 61 |
| `TaskRuntimeProtocol.LocalTests.dll` | 71 / 71 |
| `TaskRuntimeActivation.LocalTests.dll` | 66 / 66 |
| `TaskRuntimeClient.LocalTests.dll` | 132 / 132 |
| `TaskRuntime.LocalTests.dll` | 182 / 182 |
| **Total** | **512 / 512** |

Focused repeated stability evidence:

- M2F2 Agent mutation/`rtask`/router/crypto paths: 100/100 rounds;
- M2G1 terminal lifecycle plus TaskCore terminal policy: 100/100 rounds;
- M2G2 idle policy/TaskCore activity/command parsing: 100/100 rounds;
- M2G2 real idle ProductHost scenarios: 100/100 rounds, 300 helper process lifecycles;
- on-demand short-parent/concurrent launch/crash recovery: 100/100 rounds.

No test used real Codex or network access.

## Release artifacts

MSIX:

```text
src/cascadia/CascadiaPackage/AppPackages/
  CascadiaPackage_0.0.1.0_x64_Test/CascadiaPackage_0.0.1.0_x64.msix
bytes: 8103187
sha256: 3507A60AF47A48E6D1525C1D3D41FD267469682AC6EA94C010429250557C47C3
```

The MSIX contains `TaskRuntime.exe` (590336 bytes) and `rtask.exe` (283648
bytes).

The canonical portable workflow also passed without rebuilding or launching:

```powershell
pwsh -NoLogo -NoProfile -File .\tools\deploy\build-portable-release.ps1 `
    -SkipBuild -NoLaunch
```

```text
dist/WindowsTerminalDev_0.0.1.0_x64_workspace-sidebar.zip
bytes: 11986987
sha256: 41BDE2DA632F0A0FF881C689236DC6E90642C9C067F29C6C37EE519D681E3AA4
```

Its `terminal-0.0.1.0/` layout contains `WindowsTerminal.exe`,
`TaskRuntime.exe`, `rtask.exe`, and the preconfigured
`settings/settings.json`. `dist/` remains generated and ignored.

## Remaining acceptance boundary

The release gate continues to report existing PRI resource warnings and leaves
manual Narrator/DPI acceptance separate. Neither is an M2 Runtime regression.
No merge to `main`, registered-package deployment, or live application launch
was performed by this closure.
