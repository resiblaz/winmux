# Tasks 10 项修复与验证（2026-09-05）

[原测试清单](ui-acceptance-2026-09-05.md)中的 T01–T10 已完成修复及对应验证。验证基线为 `6bb4ee1d9` 加当时的工作区改动。后续并发和事件版本调整见[架构收尾记录](architecture-hardening-2026-09-05.md)。

先集中修改，再运行统一 Release 检查；验证阶段暴露的 SDK 兼容细节、清理落库竞争和测试夹具问题，使用增量构建与定向补测处理。保留了构建缓存，已通过的无关测试组没有反复执行。

## 验证结果

- **822 项原生测试通过**，包含本轮新增的 9 项回归；各用例最后一次结果及原始日志位置见[测试结果](fix-verification-2026-09-05/validation-results.json)。
- **246 项 Hook 断言通过**；Release/MSIX 构建、Runtime/rtask 与 workspace CLI 检查通过。
- 使用真实 **codex-cli 0.153.4** 完成 Direct、Worktree 和追加一轮执行；目标文件内容正确。
- 实机复测使用隔离便携副本和测试 Git 仓库。保留原数据库备份，通过正常界面操作验证旧数据恢复，没有直接修复数据库记录。
- 最终便携包和运行中的 TaskRuntime 与构建输出的 SHA256 一致；见[环境与产物指纹](fix-verification-2026-09-05/environment.json)。

| 问题 | 修复内容 | 对应验证 |
| --- | --- | --- |
| T01 新任务无法启动 | 首次会话创建时注入运行配置，首次 turn 不恢复尚未产生的 rollout；显式传递仓库既有的 `--yolo` 权限策略；后续 Run 在重新加载会话后更新环境 | Direct、Worktree 均达到 Run Completed，生成文件内容正确；追加 Run 写出的环境标识与当前 Run ID 一致 |
| T02 删除消息破坏任务数据 | Removed 消息允许空内容；查询只有遇到传输大小限制才缩减结果，其他校验错误不再被当成容量问题 | 原本隐藏的任务重新出现；通过界面移除剩余消息后继续刷新和操作成功；[截图](fix-verification-2026-09-05/03-remove-message-fixed.png) |
| T03 取消后仍是 Todo | 允许取消 intent 在关闭阶段提交 Cancelled，并在提交成功后完成 intent | 两个实际执行中的任务均取消为 `status=6`，intent 完成；[状态记录](fix-verification-2026-09-05/15-cancel-active-runs.json) |
| T04 关闭后重试复用 Closed 会话 | 丢弃 Closed 会话的内存映射，重新创建可运行的会话 | 取消后重新打开、重试，产生新会话和新 Run，最终完成；[完成记录](fix-verification-2026-09-05/17-completed-runs.json) |
| T05 关闭终端离开 Tasks | 关闭任务 surface 后恢复原 Tasks 页面 | 关闭成功，终端显示“已关闭”，Tasks 页面仍可操作；[截图](fix-verification-2026-09-05/11-closed-terminal-stays-in-tasks.png) |
| T06 关闭 X 点击无反馈 | 工作区标签条绑定真实可关闭状态；设置刷新继续保留任务标签的关闭保护 | 任务标签上的 X 消失；普通标签关闭入口保留；[截图](fix-verification-2026-09-05/10-task-tab-close-hidden.png) |
| T07 另一窗口占用时无反馈 | 显示窗口占用说明及重新连接方式 | 第二窗口出现明确提示，原窗口关闭后可连接；[截图](fix-verification-2026-09-05/14-owned-elsewhere-dialog.png) |
| T08 断连重连使 GUI 崩溃 | 焦点标签按存活对象匹配，避免标签与视图集合变更期间使用旧索引；空集合禁止选中标签 | 复现原多工作区、单任务标签场景，终止隔离 Runtime 后重新打开终端；GUI PID 2556 保持存活，Runtime 从 31160 重启为 17700；[记录](fix-verification-2026-09-05/runtime-reconnect-fixed.json)、[重新连接截图](fix-verification-2026-09-05/17-runtime-reconnected-fixed.png) |
| T09 Worktree 清理失败且不能重试 | 释放任务会话进程；清理前检查删除共享权限；允许失败清理重试；恢复有所有权证据的遗留空目录；提交结果时重试任务版本竞争，保留 Worktree 修订号校验 | 旧失败任务恢复完成；新任务执行后首次清理成功，目录移除、成果分支保留、Worktree 和 intent 均为完成状态；[完整证据](fix-verification-2026-09-05/final-worktree-cleanup-proof.json)、[截图](fix-verification-2026-09-05/20-final-cleanup-success.png) |
| T10 删除确认框打不开 | 使用现有窗口对话框宿主，等待后按 Task ID 校验目标 | 新建 QA-06，仅保存；取消删除后任务仍为 version 1，确认删除后任务消失并留下 tombstone；[对话框](fix-verification-2026-09-05/09-delete-dialog-fixed.png)、[取消后](fix-verification-2026-09-05/13-delete-cancel.json)、[确认后](fix-verification-2026-09-05/14-delete-confirmed.json) |

## 清理与会话兼容处理

实机定位到 `node_repl.exe` 的工作目录仍指向任务 Worktree，见[目录取证](fix-verification-2026-09-05/node-worktree-directory.txt)。Codex 的取消订阅会保留会话一段时间，并不立即卸载；其公开说明给出的空闲保留时间为 30 分钟。[App Server 文档](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md)

当前适配器通过单会话的归档、恢复接口释放进程并保留历史，随后按需要重新加载。真实协议探测确认恢复历史后会话仍处于未加载状态，见[卸载探测](fix-verification-2026-09-05/provider-unload-probe.json)。对于尚无 rollout 的新会话，先通过 section 操作持久化，再进行卸载，见[新会话探测](fix-verification-2026-09-05/provider-unload-materialized-probe.json)。恢复失败会返回错误并保留重试状态；恢复路径也处理进程中断后遗留的归档状态。

文件清理成功后，后台 Provider 通知可能继续推进 Task 版本。最终提交现在只在 Worktree 修订号、状态和执行上下文未变化时更新任务版本并重试 SQLite 提交，不重做 Git 操作。对应回归用例用旧快照和一次中间状态更新确定性地覆盖该竞争。

权限参数的真实探测还确认：仅向 App Server 进程传入 `--yolo` 时，线程仍可能采用只读默认值；显式线程参数才会落实本仓库原有策略。见[权限探测](fix-verification-2026-09-05/provider-permissions-probe.json)。

## 现场与范围

最终[数据库只读快照](fix-verification-2026-09-05/final-state.json)中没有活动 Run。QA-03、QA-05、QA-07、QA-08 的清理均已完成；旧失败 Run 作为历史保留。截图、状态和哈希见[证据清单](fix-verification-2026-09-05/manifest.json)。

本轮覆盖上述 10 条复现路径及相关回归，没有进行长时压力测试或多子代理组合测试。

额外观察：一次 Direct 执行中，Agent 直接调用 `rtask --help` 得到命令不存在，随后仍完成了目标文件创建。便携版已注入 `RICHER_TASK_EXECUTABLE`，但 Agent 没有使用这个显式路径；该现象保留在任务终端截图中，作为后续提示与结构化状态上报的改进记录，未计入本次 10 项修复。
