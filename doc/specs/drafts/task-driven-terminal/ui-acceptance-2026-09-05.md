# Tasks 实机功能测试记录（2026-09-05）

后续：用户已授权修复全部 10 项，[修复与验证结果](fix-verification-2026-09-05.md)另行记录。以下保留修复前的测试事实。

本轮确认 **10 项问题：P1 3 项、P2 5 项、P3 2 项**。已启动实际便携版，通过桌面操作测试 Tasks，并用只读数据库查询、Runtime RPC、真实 Provider 协议探测和调试器补充证据。所有问题均待用户决定是否修复；本轮没有修改产品代码，也没有提交本报告。

P1 表示阻断任务主流程、破坏任务列表可用性或导致整个 GUI 进程崩溃；P2 表示关键操作失败或状态错误；P3 表示交互反馈问题。优先级是本次测试的建议。

| 编号 | 优先级 | 已复现问题 | 用户决定 |
| --- | --- | --- | --- |
| T01 | P1 | 新任务无法通过真实 Codex 0.153.4 启动执行 | 待定 |
| T02 | P1 | 删除排队消息后，任务数据无法编码，页面停留在旧状态或漏任务 | 待定 |
| T03 | P2 | 取消任务关闭了终端，但任务仍是 Todo | 待定 |
| T04 | P2 | 关闭会话后重试，复用 Closed 会话并再次失败 | 待定 |
| T05 | P3 | 从 Tasks 关闭任务终端后，页面跳回普通终端 | 待定 |
| T06 | P3 | 任务终端标签显示可点击的关闭按钮，但点击无反馈 | 待定 |
| T07 | P2 | 打开被另一窗口占用的任务终端，无提示也不激活原窗口 | 待定 |
| T08 | P1 | 重开断连终端时，关闭占位标签触发整个 GUI 进程崩溃 | 待定 |
| T09 | P2 | Worktree 清理失败并留下阻塞后续操作的失败 intent | 待定 |
| T10 | P2 | 删除任务的确认框无法打开，GUI 删除流程不可用 | 待定 |

## 测试基线和隔离环境

- 代码基线：`6bb4ee1d9`，`fix: correct task lifecycle, recovery, and terminal interactions`。
- 使用已有 x64 Release 便携包，不重新构建、不清理 `bin/obj`。
- 独立测试目录：`dist/task-acceptance-20260905-154230/`；应用位于其中的 `app/terminal-0.0.1.0/`，测试 Git 仓库位于 `repo/`。
- 设置及任务数据库都属于该测试副本。没有操作日常使用的 Terminal 窗口，也没有修改主仓库中的产品源文件。
- Windows 内核 build 22621，中文界面；桌面 1920×1080，测试窗口大小包括 1560×900 和 1500×870。
- Provider 使用本机真实 `codex-cli 0.153.4`，未使用模拟 Provider。任务要求仅在测试仓库中创建小型文本文件。
- 实机操作结合 UI Automation 和物理鼠标点击。UIA 偶尔只读到 tooltip 的情况属于测试工具限制，没有计入问题。
- SQLite 全程只读查询，最终快照使用 `mode=ro`、`query_only=ON` 和单个读事务；没有修复或移除异常数据。

[环境与二进制 SHA256](ui-acceptance-2026-09-05/environment.json)、[最终状态快照](ui-acceptance-2026-09-05/final-state.json)、[证据清单与 SHA256](ui-acceptance-2026-09-05/manifest.json)可用于对照复现。

| 测试对象 | Task ID | 用途 |
| --- | --- | --- |
| QA-01 Save only | `c4b19534-baa9-4e65-b5f7-e9f64aca01c8` | 仅保存、Worktree 启动、消息编辑/删除、关闭终端 |
| QA-02 Direct run | `03d4ec42-8f44-4d6f-b1ba-b54a4a2c05b1` | Direct 启动、取消、重试、多窗口和断连恢复 |
| QA-03 Worktree cleanup | `f2f12643-7c2d-4c36-b893-3cf57c77b285` | Worktree 清理和失败后的恢复入口 |
| QA-04 Delete saved task | `0809fab1-6a4d-4632-be00-bb69518d1e61` | 无 Run 的已保存任务删除 |

## T01 — P1：新任务无法通过真实 Codex 启动执行

**复现：** 在测试 Git 仓库中新建任务，描述要求创建一个文本文件。分别使用 Worktree 模式的“仅保存 → 交给 Codex”和 Direct 模式的“保存并运行”。

**实际：** Worktree 准备成功，但 Run 在 Provider 启动阶段失败；Direct 同样失败。数据库记录 `state=6 / Failed`、`reason_code=provider-start-failed`，没有产生要求的文件，也没有进入真实 Agent turn。

**预期：** 新任务创建会话后能够开始第一次执行；启动失败时应能看到具体原因。

**原因与置信度：高。** [CodexAppServerAdapter.cpp](../../../../src/cascadia/TaskProvider/CodexAppServerAdapter.cpp) 的 `StartSession` 调用 `thread/start`，`StartRun` 随即在首次 turn 前调用 `thread/resume`（约 330 行）。对同一真实 app-server 的独立协议探测得到：`initialize` 成功、`thread/start` 成功、`thread/resume` 返回 `-32600: no rollout found for thread id ...`。应用界面只保留通用的 `provider-start-failed`。

**证据：** [任务失败界面](ui-acceptance-2026-09-05/06-task-terminal.png)、[真实协议探测结果](ui-acceptance-2026-09-05/provider-protocol-probe.json)、最终状态快照中的 `task_run`。结论限定于本次实测版本及该调用流程。

## T02 — P1：删除排队消息使任务数据无法编码

**复现：** QA-01 处于暂停调度状态时，排队添加消息 A、B；编辑 A 为 `QA pending message A revised`；移除 B。随后等待页面刷新、尝试重试，再创建 QA-02。

**实际：** 数据库存储了 A 的修订；B 变为 `Removed=3`、revision 2、内容为空。但界面仍显示已移除的 B，任务版本停留在旧值，后续操作无有效反馈。只读 RPC 查询单个任务和全部任务均返回 `internal_error: Task projection could not be encoded`。创建较新的 QA-02 后，查询成功却只返回 QA-02，数据库当时仍有两个任务。

**预期：** Removed 消息能够正常表达，删除后刷新任务列表和详情，不影响其他任务或后续操作。

**原因与置信度：高。** [TaskProjectionValidation.cpp](../../../../src/cascadia/TaskRuntimeProtocol/TaskProjectionValidation.cpp) 约 99–108 行对所有消息要求非空内容，没有允许 Removed 消息的空内容。[RuntimeProductRouter.cpp](../../../../src/cascadia/TaskRuntime/RuntimeProductRouter.cpp) 约 164–170 行在任何编码失败时缩小任务列表，进一步将该问题表现为漏任务。UI 显示旧消息、查询报错和任务缺失作为同一问题记录。

**证据：** [编辑/移除后的界面](ui-acceptance-2026-09-05/10-message-edit-remove.png)、[后续操作仍使用旧页面](ui-acceptance-2026-09-05/11-retry-after-close.png)、[单任务查询错误](ui-acceptance-2026-09-05/removed-message-query-error.json)、[全任务查询错误](ui-acceptance-2026-09-05/removed-message-all-query-error.json)、[创建第二个任务后的不完整结果](ui-acceptance-2026-09-05/query-after-second-task.json)。异常消息保留在测试数据库中。

## T03 — P2：取消任务后仍为 Todo

**复现：** QA-02 首次执行失败后，点击“取消任务”，等待完成并查看详情。

**实际：** 终端和 Provider 会话关闭，`manual_hold=1`；取消 intent 为 `kind=1 / Close`、`stage=8 / Completed`，详情包含 `"cancel":true`。任务状态却仍是 `Todo=1`，仍提供取消入口，没有进入可重新打开的 Cancelled 状态。

**预期：** 取消流程完成后，任务进入 Cancelled，并提供与该状态一致的操作。

**原因与置信度：高。** [RuntimeTaskIntentProcessor.cpp](../../../../src/cascadia/TaskRuntime/RuntimeTaskIntentProcessor.cpp) 约 142–147 行在完成自身 intent 前调用 `ChangeStatus(Cancelled)` 并忽略结果；[SqliteTaskStoreTasks.cpp](../../../../src/cascadia/TaskCore/SqliteTaskStoreTasks.cpp) 约 312–322 行禁止未完成 intent 期间修改 Task 状态。持久化记录与此顺序冲突一致。

**证据：** [取消后的界面](ui-acceptance-2026-09-05/13-cancel-status.png)、最终状态快照中 QA-02 的 Task 及已完成取消 intent。快照采集前进行了 T04 重试，因此最终 `manual_hold` 已重新变为 0。

## T04 — P2：会话关闭后重试仍复用 Closed 会话

**复现：** 完成 QA-02 的取消操作后，点击“重试”。

**实际：** 新建了 retry Run `e7759192-ea10-4ac2-a344-48f8c2b96985`，正确关联原 Run `44f0fdc8-0925-40dc-b16d-6af31f6e7e3b`；终端 generation 增加。但执行再次失败，Provider 会话仍是原来的 `01a070a6-feac-7110-9f0a-601396886131`，状态为 Closed，未创建新会话。

**预期：** 用户明确重试时，应使用可运行的 Provider 会话。

**原因与置信度：高，来自运行记录与代码交叉验证。** [RuntimeTaskRunExecutor.cpp](../../../../src/cascadia/TaskRuntime/RuntimeTaskRunExecutor.cpp) 会排除持久化的 Closed 会话并尝试 `StartSession`；[TaskProviderCoordinator.cpp](../../../../src/cascadia/TaskProvider/TaskProviderCoordinator.cpp) 约 124–129 行只检查任务是否已有映射，直接返回已有 Closed 会话，随后 `StartRun` 约 245–250 行拒绝 Closed 状态。这是 T01 之外的独立阻塞点；本次 GUI 只显示通用失败码，未直接捕获该 Conflict 的明文。

**证据：** [重试界面](ui-acceptance-2026-09-05/14-retry-closed-session.png)、[重试后的 Run/会话/消息状态](ui-acceptance-2026-09-05/state-after-retry.json)。

## T05 — P3：关闭任务终端会离开 Tasks 页面

**复现：** 打开 QA-01 的任务终端，返回 Tasks，在详情里点击“关闭任务终端”。

**实际：** 后端关闭成功、终端状态为 `Closed=7`，标签移除；当前页面同时变成普通 PowerShell 终端，Tasks 详情消失。

**预期：** 从详情发起关闭后继续停留在 Tasks，方便查看状态或安排下一步。

**定位与置信度：现象已实测；导航变化的精确触发点尚未单独调试。** 相关入口是 [TerminalPage.Tasks.cpp](../../../../src/cascadia/TerminalApp/TerminalPage.Tasks.cpp) 的 `_CloseTaskSurface`，其调用通用 `tab.Close()`。需要结合通用标签移除流程检查页面选择状态。

**证据：** [关闭后的普通终端页面](ui-acceptance-2026-09-05/09-task-terminal-closed.png)。

## T06 — P3：任务标签上的关闭按钮点击无反馈

**复现：** 打开 QA-01 任务终端，在工作区标签条点击该任务标签的关闭 X。

**实际：** X 呈现为可用操作，点击后标签仍保留，没有解释为何不能关闭。

**预期：** 标签应隐藏/禁用不支持的关闭入口，或明确引导到任务详情中的关闭操作。

**定位与置信度：现象已实测，关闭保护本身有效。** Task surface 的所有权策略拒绝普通标签关闭；需要核对标签呈现与该策略是否一致。该问题不代表用户能够绕过 Task 的关闭保护。

**证据：** [任务标签关闭入口](ui-acceptance-2026-09-05/07-opened-task-terminal.png)、[点击后标签仍保留](ui-acceptance-2026-09-05/08-tab-close-guard.png)。

## T07 — P2：另一窗口占用任务终端时，打开操作无反馈

**复现：** 窗口 A 已连接 QA-02 任务终端。打开窗口 B，在 Tasks 的“全部工作区”中选择同一任务，点击“打开终端”。

**实际：** B 的页面没有变化，没有提示终端正在 A 中使用，也没有激活 A。关闭 A 后再从 B 打开，能够连接该非运行中的任务终端。

**预期：** 激活已拥有终端的窗口，或清楚说明所有权及可行操作。

**原因与置信度：高。** [TerminalPage.Tasks.cpp](../../../../src/cascadia/TerminalApp/TerminalPage.Tasks.cpp) 约 58–60 行遇到 `descriptor.OwnedElsewhere` 直接返回，没有用户反馈或窗口切换。

**证据：** [第二窗口点击打开后](ui-acceptance-2026-09-05/15-second-window-owned-terminal.png)、[原窗口关闭后可重新连接](ui-acceptance-2026-09-05/17-reattach-after-owner-close.png)。没有据此宣称运行中的任务已通过跨窗口恢复测试。

## T08 — P1：重开断连终端触发整个 GUI 进程崩溃

**复现：**

1. 在窗口 A 的工作区 A 创建并打开 QA-02 任务终端。
2. 打开窗口 B，保留其普通工作区 B；关闭窗口 A。
3. 从 B 的 Tasks 打开 QA-02，使工作区 A 在 B 内出现，任务终端是该工作区唯一标签。
4. 确认没有活动 Run 后，仅终止该隔离副本的 Runtime 进程（当时 PID 28936），模拟 Runtime 故障。
5. Runtime 自动重启为 PID 24820；界面留下断连终端。返回 Tasks 再点“打开终端”，触发替换占位标签。

**实际：** GUI PID 34740 崩溃，同一进程的窗口全部消失。Windows Application Error 1000 记录时间为 `2026-09-05 16:40:13 +08:00`，模块 `ucrtbase.dll`，异常码 `0xc0000409`。

**预期：** 清理断连占位标签并重新连接，保留其他标签和窗口。

**原因与置信度：高，有匹配本地 PDB 的崩溃栈。** 原始 HRESULT 为 `0x8000000B / E_BOUNDS`，调用链为：

```text
_HandleTaskSurface → _CloseTaskSurface → Tab::Close → _RemoveTab
→ WorkspaceCollection::RemoveSurface → 工作区集合变更通知
→ AgentHistoryCoordinator::_UpdateConclusionSidebar
→ _GetFocusedTab → _tabs.GetAt → throw / terminate
```

工作区移除通知期间，焦点标签索引仍指向已经移除的元素，最终以未处理异常终止进程。相关代码位于 [TerminalPage.Tasks.cpp](../../../../src/cascadia/TerminalApp/TerminalPage.Tasks.cpp)、[WorkspaceCollection.cpp](../../../../src/cascadia/TerminalApp/WorkspaceCollection.cpp) 和 [AgentHistoryCoordinator.cpp](../../../../src/cascadia/TerminalApp/AgentHistoryCoordinator.cpp) 的 `_UpdateConclusionSidebar`。

**证据：** [Runtime 断连占位界面](ui-acceptance-2026-09-05/18-runtime-disconnected.png)、[再次打开前的 Tasks 页面](ui-acceptance-2026-09-05/19-before-reconnect-crash.png)、[Windows 崩溃事件](ui-acceptance-2026-09-05/runtime-reconnect-crash-event.json)、[异常与调用栈](ui-acceptance-2026-09-05/runtime-reconnect-crash-stack.txt)。第二张截图是崩溃前的状态，不能作为重连成功证据。

原始 dump 保留于本机 `C:\Users\u1s1\AppData\Local\CrashDumps\WindowsTerminal.exe.34740.dmp`，未复制进文档附件。测试后重新启动了隔离 GUI。

## T09 — P2：Worktree 清理失败后无法恢复生命周期操作

**复现：** 新建 QA-03，使用 Worktree 模式启动。待其因 T01 失败后，点击“交付并清理 Worktree”，等待结束，再尝试后续操作。

**实际：** Worktree 变为 `state=7 / Preserved`，cleanup intent 为 `kind=2 / WorktreeCleanup`、`stage=7 / Failed`，原因 `git-finalize-operation-failed`。Git worktree 注册已移除，目录内容已清空，但目录仍存在。只申请目录 DELETE 访问权限、未执行删除的探测返回 Win32 32（sharing violation）。界面没有足够的失败说明；失败 intent 阻止新的生命周期 intent，无法正常重试清理。

**预期：** 清理成功，或保留可恢复状态并说明失败原因，允许解除占用后重试。

**原因与置信度：失败和阻塞状态已确认；目录占用者未确认。** [RuntimeTaskIntentProcessor.cpp](../../../../src/cascadia/TaskRuntime/RuntimeTaskIntentProcessor.cpp) 的 WorktreeCleanup 分支直接清理，不先关闭 Provider 会话；仍存活的 Provider/MCP 工作目录句柄可能导致占用，但未定位到具体持有进程。该处理器跳过 Failed intent，而 [SqliteTaskStoreIntents.cpp](../../../../src/cascadia/TaskCore/SqliteTaskStoreIntents.cpp) 约 234 行拒绝在非 Completed intent 存在时建立新 intent，构成失败后的恢复障碍。

**证据：** [清理失败后的界面](ui-acceptance-2026-09-05/21-worktree-cleanup-failed.png)、[失败状态及只读访问探测](ui-acceptance-2026-09-05/worktree-cleanup-failure.json)、最终快照中的 `task_intent`、`task_git_worktree` 和 Git worktree 列表。本用例没有成功产出的修改，不能代表已验证成果交付。

## T10 — P2：删除确认框无法打开

**复现：** 新建 QA-04，选择“仅保存”，不启动任何 Run。在详情里点击“删除任务”；分别用 UIA Invoke 和物理鼠标重复操作。

**实际：** 没有出现确认框，也没有删除请求。任务始终存在、version 为 1，没有删除 intent 或 tombstone。调试器捕获 `HRESULT 0x80070057 / E_INVALIDARG`，栈为 `ContentDialog::ShowAsync → TaskPage::_Execute`。

**预期：** 显示确认框，用户确认后执行删除，取消则保留任务。

**定位与置信度：异常位置已实测，具体属性原因仍待修复验证。** [TaskPage.cpp](../../../../src/cascadia/TerminalApp/Tasks/TaskPage.cpp) 约 295–302 行新建 ContentDialog 后直接 `ShowAsync`，没有设置 XamlRoot 或通过现有对话框宿主显示。高度怀疑缺少 XAML Islands 宿主关联；尚未修改属性验证。约 324 行的“关闭活动任务终端”确认框使用相同方式，但受 T01 阻断，未对活动场景实测，未另计问题。

**证据：** [点击删除后没有弹框](ui-acceptance-2026-09-05/25-delete-no-dialog.png)、[HRESULT 和调用栈](ui-acceptance-2026-09-05/delete-dialog-hresult.txt)、最终快照中的 QA-04 和空的 `delete_tombstone`。调试器在取证后已分离。

## 测试覆盖和未完成项

| 功能 | 本轮结果 | 说明 |
| --- | --- | --- |
| 启动应用、进入空 Tasks 页面 | 通过 | 实际便携 GUI |
| 仅保存任务 | 通过 | Todo，未创建 Run；[截图](ui-acceptance-2026-09-05/03-saved-task.png) |
| Worktree 准备及执行前 checkpoint | 通过 | 后续 Provider 启动被 T01 阻断 |
| Direct / Worktree 真实执行 | 失败 | T01；四个 Run 均失败 |
| 打开任务终端 | 通过 | 正常首次打开路径 |
| 普通标签关闭保护 | 通过，交互有问题 | 保护有效，关闭入口问题见 T06 |
| 关闭非活动任务终端 | 后端通过，导航有问题 | T05 |
| 未提交追加文本跨刷新保留 | 通过 | 等待超过 3.5 秒后仍保留；[截图](ui-acceptance-2026-09-05/04-draft-after-refresh.png) |
| 两条 Deferred 消息排队、编辑修订 | 通过 | 移除消息时出现 T02 |
| 移除排队消息 | 失败 | T02 |
| 手动重试新建并关联 Run | 部分通过 | 新 Run 和 retryOf 正确；执行受 T04/T01 阻断 |
| 取消任务 | 失败 | T03 |
| 多窗口打开已有终端 | 部分通过 | 原窗口退出后的非活动连接通过；占用提示见 T07 |
| Runtime 自动重启 | 通过 | 自动拉起新 helper；GUI 恢复路径见 T08 |
| Worktree 清理 | 失败 | T09；未验证成功产出后的交付 |
| GUI 删除已保存任务 | 失败 | T10 |
| 运行中的 Stop / Esc / Steer | 未验证 | 没有成功进入真实 Agent turn |
| 成功结果、Done / Reopen | 未验证 | 被执行启动失败阻断 |
| 活动 Run 跨窗口存活和恢复 | 未验证 | 已通过的重新连接用例仅针对非活动任务 |

此前提交的自动化测试与构建结果不替代本轮 GUI 验收。本轮不能判定 Tasks 整体验收通过。

## 现场保留与附件说明

整理报告时，隔离 GUI PID 17572 和 Runtime PID 24820 仍在运行。测试目录、四个任务、异常 Removed 消息、失败 intent 和原始取证文件均保留，便于后续选择问题并复现。未提交修复或本报告。

附件共 27 个证据文件，另有一份清单。截图和 JSON 按字节复制，清单记录 SHA256。两个调试文本仅裁去模块加载或 NatVis 卸载噪声，保留异常与调用栈；未附带二进制程序、原始数据库或进程 dump。

原始截图 `19-runtime-reconnected.png` 在附件中命名为 `19-before-reconnect-crash.png`，原始 `21-worktree-finalized.png` 命名为 `21-worktree-cleanup-failed.png`，以准确表示截图时的状态；图片内容没有修改。映射保存在 `manifest.json`。
