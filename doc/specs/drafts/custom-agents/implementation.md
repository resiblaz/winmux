# 自定义 Agent 首版实现记录

范围：设计中的 A–C，Codex 后端、名称/简介/长期指令/模型覆盖、Workspace 默认选择、
Task 执行快照。Skills、MCP、凭据、自定义 CLI 后端、模型目录与独立并发额度属于 D 项。

## 原生入口

- Tasks 顶部和新建 Task 表单均有“管理 Agent”入口。
- 管理页支持新建、编辑、另存为新 Agent、归档/恢复、指令预览、设置/清除 Workspace
  默认值。简介只展示；模型留空时继承执行环境默认模型。
- 选择已有 Agent 时，新建 Task 固定选中的配置版本。Task 的“Agent 配置”入口显式
  更新后续执行配置；活动/排队 Run 和未完成生命周期操作由数据库事务拒绝。
- 管理页是独立 UserControl，按需创建，持有自己的编辑状态。草稿不会被列表刷新
  覆盖；未确认的写操作保留 command ID，成功创建后的身份在后续详情加载之前就确定。
- Task 保存等待实际命令结果，失败保留表单；已保存但启动未确认时保留已创建的 Task。
- Task 属性和 Run 历史展示各自保存的 Agent 名称、版本及请求模型。

## 数据与兼容

`workspace-tasks.db` 升级到 schema 5。升级前创建
`workspace-tasks.db.pre-agent-profiles-<timestamp>.db` 备份，复制失败时不迁移原数据库。
已有 Task 的 Agent 来源为空，执行配置与历史数据保持原值。回退旧 Runtime 使用升级前
备份；旧二进制仍拒绝较新 schema。

新增 `agent_profile`、`agent_profile_revision`、`workspace_agent_default`、`agent_event`
和 `agent_command_receipt`。Agent 管理版本用于 CAS，配置修订不可变，事件游标独立；
这些命令不递增 Task 版本。归档清除默认关联并递增各默认项的版本，保留历史执行。

协议 minor 3 增加可选 `agent_profiles` 能力和 `query_agents` / `agent_command` 方法。
Get 由 query 的 `agentId` 表达；列表按 ID 游标分页，每页最多 100 项，列表不读取长期
指令和完整默认关联。详情最多支持 4096 个默认 Workspace 关联，写入端同步限制容量。
TaskCtl 无法使用 Agent 管理方法。未协商该能力的客户端继续接收原来的 Task JSON 结构。

## 执行约束

Task 创建与 Agent 修订检查、配置快照插入在同一事务中完成。每个 Run 按其
`ExecutionContextId` 读取配置；历史重试不取 Task 当前最高修订。切换 Agent 身份、
模型或长期指令时，下次执行卸载旧的空闲会话并创建新会话；仅名称变化可沿用会话。
同一 Agent 的不同 Task 不共享 provider thread、终端控制权或 Run capability。

Run 进入 Running 时，将真实 AgentSession 与状态、事件、receipt 一起提交。会话必须属于
该 Task，已绑定的 Run 不允许改绑。重复回调不会重置启动时间；无法确定来源的旧 Run
保持原值，不推断补写历史关联。

Worktree 的所有权仍绑定创建时的执行位置。Agent 更新产生新 ExecutionContext 后，
checkpoint 校验同一 Task 的冻结目录、仓库、分支和相对工作目录，而不要求配置快照 ID
相同。仅 Agent 配置改变可复用原 Worktree；位置变化仍被拒绝。

## 验证

验证日期：2026-09-06。实现分支：`feature/custom-agents`。
全过程保留 bin/obj 和已有构建缓存，通过统一解决方案图增量构建，并使用 `-SkipBuild`
更新隔离便携版。验证记录根目录为 `dist/custom-agents-20260906-002228/`，不纳入版本控制。

- 完整 Release gate：850 个不同原生测试、246 个 Hook 断言、MSIX 构建与 Workspace CLI
  smoke 通过。后续新增回归后，累计通过 856 个不同原生测试，去重统计见
  `verification-summary.json`。
- 会话绑定修复后：TaskCore 96 项，ProviderSink/Orchestrator/Projection/真实 Host 34 项
  全部通过；日志为 `session-binding-core-tests.log`、`session-binding-runtime-tests.log`。
- Worktree 配置更新修复后：TaskCore 97 项、真实 Git checkpoint 4 项、Orchestrator 13 项
  全部通过；日志为 `worktree-agent-tests.log`。`worktree-agent-build.log` 记录最终统一
  Release 构建和 CLI 验证通过。
- UI 已通过 11 项 TaskPageViewModel 和 3 项 AgentEditorViewModel 回归，并完成下表实机操作。

实机使用独立的 Git 仓库、数据库、Workspace 和便携版目录 `acceptance/`，通过原生界面
操作，SQLite 仅用于只读核验。实际执行使用已安装的 Codex 后端。

| 场景 | 结果与证据（相对 acceptance/） |
| --- | --- |
| schema 4 升级至 5 | 7 个旧 Task、12 个 Run 的版本和执行上下文在迁移时保持，备份可读取；`migration-proof.json` |
| 创建、复制、编辑、指令预览、默认选择 | 通过原生管理页完成；`02-agent-a-saved.png`、`03-instruction-preview.png`、`04-two-agents.png` |
| 仅保存与版本固定 | A v1 Task 保存时零 Run；Agent 改为 v2 后，原 Task 仍执行 v1，写出 `role-a-v1`；`first-runs-proof.json` |
| 双 Agent 并行 | 峰值两个 Running Task，各自 Worktree 和 provider thread，不同指令分别写入 A/B 标记；`parallel-runs.json`、`first-runs-proof.json` |
| Task 显式更新 Agent | 固定 v2 后保留 v1 历史；新会话执行并写出 `role-a-v2`，checkpoint 完成；`v2-session-binding.json`、`v2-completed-proof.json` |
| 归档时仍有活动 Run | 默认值被清除且版本递增，新建选择中移除 A，已启动 Run 正常完成；`archive-proof.json`、`16-archived-agent-excluded.png` |
| 归档后的重试 | 重试完成，使用固定 v2，复用同一 AgentSession/provider thread；`archived-retry-session-proof.json`、`archived-retry-completed.json` |
| 恢复 | Agent 恢复可选，Workspace 默认值继续为空；`restore-proof.json`、`18-agent-restored.png` |
| 双窗口编辑冲突 | 旧版本保存被拒绝，草稿保留；显式重新加载得到另一窗口的保存值；`conflict-proof.json`、`13-second-window-conflict.png` |
| 收尾 | 两个测试 Task 终端关闭，活动 Run 为零，数据库完整性和外键检查通过，最终二进制 SHA256 已记录；`final-proof.json` |

实机验收发现并修复了真实 Host 漏声明 Agent 能力、Run 未保存 AgentSession 关联、
以及更新 Agent 后 Worktree checkpoint 仍要求原配置 ID 的问题。复现失败的 Run 和旧的
空会话关联保留在测试数据库中；后续成功 Run 记录修复结果，不修改历史掩盖失败。

验证边界：实机 A/B 均使用运行环境默认模型；模型覆盖透传和固定快照由原生测试覆盖，
不声称已验证每种模型的可用性。已检查 1024 像素窗口下的管理页及滚动编辑区；完整
Narrator/多 DPI 矩阵未执行。D 项扩展不属于本轮交付范围。
