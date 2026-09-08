# 原生实现记录

用户于 2026-09-07 授权开始执行详细方案。实现分支为 `feature/workspace-projects`，
从包含自定义 Agent 首版的 `fd0f33840` 基线建立，沿用当前增量构建目录。

## 四列卡片页面还原（2026-09-08）

按已确认的网页原型完善 Task 主区域：紧凑卡片、短 ID/优先级/固定 Agent、执行提示、
有状态标记和计数的列头、空列引导、按需分页按钮、跨列高亮、插入线及操作处理中标记。
“全部项目”只允许跨列状态操作；排序回到当前项目，并使用可见卡片锚点。
原 WorkspaceSidebar 与窗口标题栏代码保持原样。验收副本曾使用
`showTabsInTitlebar=false`，已移除该测试覆盖值以对齐正式版默认设置。
新增 `TaskBoardViewTests`，完整门禁通过 898 项原生测试及 hook/CLI 检查；最后的状态文字、
列对齐和列头落点修正通过统一增量构建及 26 项相关界面复验。已完成独立项目实机验证，
并更新、启动正式 portable 部署。证据目录为 `dist/kanban-layout-20260907-213926/`。

## Task 面板导航修正

用户追加要求：左侧 Workspace 侧边栏保持原样，项目管理只在 Task 面板内。
已将项目选择及管理入口移入 TaskPanelHost，恢复外层 Workspace、新标签、Ctrl+N 和
Summary 的原有导航。关闭 Task 内项目视图不再关闭外层终端。
`dist/project-task-panel-20260907-191554/full-gate-2.log` 通过 895 项原生测试及 hook/CLI 检查。
下拉框实际布局后的刷新、项目管理/Agent 往返、Ctrl+N、双 Workspace 关闭和原任务列表均
完成实机验证；已更新并启动 `dist/terminal-0.0.1.0/WindowsTerminal.exe`。
`release-manifest.json` 记录新包、实际运行路径及原数据/settings 一致性校验。

## 阶段状态

| 阶段 | 状态 | 验证 |
| --- | --- | --- |
| P1 共用任务界面与导航 | 已实现 | 列表、看板、详情共用状态；草稿与 Agent 管理往返通过 |
| P2 持久项目与迁移 | 已实现 | schema 6、协议 1.4、目录去重与固定上下文测试通过 |
| P3 摘要分页与看板 | 已实现并验证 | 原生主区域四列、紧凑卡片、短编号/优先级、状态标记、空列引导与可视拖拽落点已验证 |
| P4 拖拽命令与恢复 | 已实现 | 真实拖拽执行、验收、停止后移动与计划撤销通过 |
| P5 项目视图与终端接入 | 已实现 | 空项目、独立项目导航、视图关闭与后台执行通过 |
| P6 集成与发布 | 已发布并启动 | 898 项完整门禁、最后 26 项界面复验与实机拖拽检查通过；正式数据和 settings 校验一致 |

## 实现落点

- 保留工作区已有的关闭回归测试、发布门禁增补和设计文件。
- `TaskCore/ProjectCatalog`、`SqliteTaskStoreProjects` 与 `TaskStoreProjectSchema`：
  项目复用 WorkspaceId；schema 5 升级前生成 SQLite 备份，旧身份与固定配置不变。
- `TaskCore/SqliteTaskStoreBoard*`、`TaskStoreBoardUndo`：原子启动、持久停止后移动、
  逐项验收和带 Task/列版本校验的计划撤销；执行及删除不支持撤销。
- `TaskRuntime/RuntimeWorkspaceRouter`、`RuntimeProjectCatalogService`、
  `RuntimeTaskBoardService`：桌面端能力协商、查询和幂等命令。
- `TerminalApp/Projects`、`TerminalPage.Projects`：持久项目库与 ActiveProjectId，
  项目工具栏与管理页面仅位于 TaskPanelHost，原 WorkspaceSidebar 继续管理外层终端；
  每窗口 `ProjectViewsJson` 独立于实时终端，快照中的旧 WorkspaceId 异步登记。
- `TerminalApp/Tasks/TaskPageState`、`TaskActionController`、`TaskBoardView`：
  列表/看板/详情共享状态，按项目保留草稿，完整详情按需读取。
- `SqliteTaskStoreSessionIndex` 与 `RuntimeTaskSessionIndex`：追加式 Task 会话归属
  索引；删除 Task 后也不会将其会话混入普通 Summary/History。
- 新项目上下文的跟进执行在创建 Run 的事务内同步为 InProgress；旧版上下文保持
  原语义。Task 面板打开时可保留无终端项目，显式关闭空窗口正常退出；外层 Workspace
  沿用原关闭规则，Task 内关闭项目视图不关闭任何终端。
- 项目和看板能力默认启用；`RICHER_WORKSPACE_PROJECTS=0` 可用于兼容性排查。
  全程沿用统一增量构建，保留 bin、obj、生成输出和依赖缓存。

## 首版验证与迁移产物

日志目录：`dist/project-implementation-20260907-130648/`。
`full-gate-15.log` 通过 893 项原生测试及 hook/CLI 检查，包含最后的零终端
Automatic 关闭修正。最终包空主页和空项目窗口正常退出，项目看板可在重启后恢复。
真实 Codex、鼠标拖拽、验收、停止/撤销和 Agent 固定版本的证据保存在 `qa/`。
具体覆盖与尚未执行的实机检查见[验证记录](validation.md)。

2026-09-07 使用 `build-portable-release.ps1 -SkipBuild` 更新并启动
`dist/terminal-0.0.1.0/WindowsTerminal.exe`，窗口停留在“全部项目”的看板。
现有数据库升级为 schema 6，升级前备份仍为 schema 5；3 个 Task、2 个 Run、
3 份 ExecutionContext 的身份/配置摘要完全一致，settings.json 的 SHA256 未变。
`release-manifest.json` 记录包哈希、实际进程路径、测试结果和迁移校验。
