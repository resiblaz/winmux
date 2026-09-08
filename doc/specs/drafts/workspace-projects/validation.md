# 原生验证记录

## 四列卡片页面（2026-09-08，已发布）

本次目录：`dist/kanban-layout-20260907-213926/`。新增原生测试覆盖可见锚点、跨项目
范围约束、完整计数/空列/分页，以及控件加载后的短 ID、固定 Agent 和处理中反馈。
前几次构建修正了 C++/WinRT 字符串、生成控件访问器和测试工程 Shapes 头文件引用，
以及不能通过公开投影访问命名子控件的绑定写法。持续使用增量流程，没有清理缓存。
独立项目通过 Runtime 创建了 4 个验收任务，其中两项已经真实执行并提交待验收，
一项已通过完成命令进入 Done。没有修改正式任务来填充演示列。

验收窗口标题栏已移除 `showTabsInTitlebar=false` 覆盖值，恢复正式版默认的一体化布局；
正式 settings 未修改，标题栏与 WorkspaceSidebar 源文件无本次改动。

- `full-gate-4.log`：898 项原生测试以及 hook/CLI 检查通过。
- `final-ui-build-3.log`：最后的运行状态简化、列对齐和整列命中区域修正通过统一增量
  构建及 CLI 检查；未重复未变动的 Core/Runtime 测试。随后 `final-TaskBoardViewTests.log`、
  `final-TaskPageViewModelTests.log`、`final-ProjectNavigation.log`、`final-ProjectClose.log`
  共 26 项相关界面测试通过。
- 实际鼠标把待办卡片拖入进行中后创建一次 Run，Runtime 为 Running；随后真实提交
  InReview。`drag-start-preview.png` 记录目标列高亮和插入线，`four-columns-active.png`
  记录独立项目四列均有卡片的执行状态。
- 活动 Run 拖回待办时取消确认：同一 Run 继续、hold=false；再次确认后同一 Run
  Interrupted，卡片变为 Todo、hold=true，无待处理 intent。见 `stop-cancelled.json`、
  `stop-confirmed.json`。
- 待验收卡片拖入已完成，确认后 Done/hold=true，执行序号不增加；见 `done-after-drag.json`。
- 最终包可把卡片拖到列头空隙并排到首位；撤销恢复原相对顺序且不创建 Run。
  `final-sort-preview.png`、`final-sort-after.json`、`final-qa-state.json` 保存证据。
- 最终包点击卡片可打开完整详情、固定消息区及返回看板；900×760 窗口显示横向浏览
  提示。见 `final-details.json`、`final-narrow.png`。最后 QA 正常退出，退出码 0、无崩溃事件。
- 使用 `build-portable-release.ps1 -SkipBuild` 更新正式部署，实际启动为
  `dist/terminal-0.0.1.0/WindowsTerminal.exe`（验证时 PID 4960）。原 3 个任务、2 个 Run、
  3 份执行配置及 Agent/default 关系摘要一致，settings.json 哈希不变，数据库完整性正常。
  四个关键二进制与最终 QA 包哈希一致，详见 `release-manifest.json`。
- 正式窗口已打开 Task → 全部项目 → 看板，原 3 个任务仍为待办；其他空列显示引导。
  最终图像为 `production-board.png`。
- 发布包 SHA256：`DCFC3103CF5E6293FB820C3426A9B585BCCBB52F493B97A7F7E38FC0774E0B24`。

## Task 面板内项目管理：追加修正

用户要求保留原 Workspace 侧边栏，仅在 Task 面板内管理项目。
本次证据目录为 `dist/project-task-panel-20260907-191554/`。

- `project-navigation-tests.log`：5 项原生专项测试通过，覆盖原侧栏可见、项目工具栏
  仅属于 TaskPanelHost、切换项目不改外层选择/Surface、普通新标签与 Ctrl+N 路由、
  Task 内关闭项目视图保留终端、外层关闭选择剩余 Workspace，以及端口信息投影。
- `full-gate-1.log`：894 项原生测试及 hook/CLI 门禁通过，但隔离实机发现项目下拉框
  首次布局后刷新空引用，因此这份候选包没有更新正式部署。
- 已将 ComboBoxItem 的显示内容改为文本数据，避免依赖 WinUI 临时移动的选中项控件；
  新增加载、展开/收起、重命名及管理页往返的原生回归。`full-gate-2.log` 最终通过 895 项
  原生测试和 hook/CLI 门禁，无失败项。
- `project-navigation-tests-2.log`：修正后的 6 项原生专项全部通过，包含实际布局后的
  下拉框刷新回归。
- 最终隔离包实机通过：空 Task 项目恢复；项目管理/下拉选择/Agent 管理往返；从项目
  名称输入框按 Ctrl+N 新建普通 Workspace；两 Workspace 关闭一个后保留另一个；
  关闭 Task 内项目视图保留外层终端；总结页无项目控件；原任务列表/详情仍显示固定 v1 配置。
  图像为 `qa-terminal-sidebar.png`、`qa-task-sidebar.png`、`qa-project-manager.png`。
- `qa-clean-exit.json`：最后隔离进程退出码 0，运行期间无应用崩溃事件，随后正常 drain
  其 Runtime。第一份候选包的异常记录保留在 `qa-crash-stack.log`，不计为最终包结果。
- `production-before.json`：现有 schema 6，3 个 Task、2 个 Run、3 份执行配置，
  无活动 Run；记录身份/配置与 settings.json 哈希用于发布后核对。
- 已通过 `build-portable-release.ps1 -SkipBuild` 更新并启动正式部署；验证时进程为
  PID 19652，运行路径为 `dist/terminal-0.0.1.0/WindowsTerminal.exe`。窗口停在 Task
  面板，左侧为原 WorkspaceSidebar，顶部为 Task 内项目工具栏；截图为 `production-task-panel.png`。
- `production-after.json`：原 Task/Run/执行配置及 Agent/default 关系摘要、settings.json
  哈希均一致，数据库完整性正常；四个关键二进制与最终 QA 包哈希一致。
- 新发布包 SHA256：`D6188939A7792AE882040D0256CABD5C08E9BC3F5B8BFF037C9DBED029639691`。
  完整产物和路径见本次目录的 `release-manifest.json`。

以下为修正前首版的历史验证，关闭/导航交互以最新修正规范为准。

实现与验证使用隔离目录 `dist/project-implementation-20260907-130648/`。
本文件记录已观察到的结果；未完成项不视为通过。

## 自动化

- `task-gate-2.log`：749 项 Task 相关测试通过。
- `core-tests-11.log`：12 项项目/看板 Core 测试通过，包括 schema 5 迁移、物理目录去重、
  固定配置、5000 项分页、复合启动事务、停止后移动恢复、验收补丁、撤销与版本冲突、
  Task 会话归属分页及删除后的隔离。
- `protocol-tests-11.log`：6 项新增协议测试通过，包括严格字段和大小边界、1.4 能力协商、
  读写重连策略及会话索引游标。
- `ui-tests-11.log`：17 项任务页测试通过，覆盖旧 Runtime 查询路径、新摘要分页、
  已确认创建不被迟到回复覆盖、稳定列表项、消息草稿与光标、项目元数据草稿及布局。
- `project-close-tests-11.log`：2 项项目关闭测试通过：最后一个终端关闭后保留项目/窗口，
  当前项目关闭后保留其他项目，最后项目移出后保留主页；空项目布局可序列化。
- `session-registry-tests-11.log`：2 项会话隔离测试通过；`agent-tests-9.log`：3 项 Agent 编辑回归通过。
- `full-gate-15.log`：完整增量门禁通过 893 项原生测试，以及 hook/CLI 检查。
  新增覆盖项目跟进执行在 Run 事务内同步状态及回滚、另一个项目仍有终端时关闭
  当前项目的最后终端，以及零终端 Automatic 显式窗口关闭。28 组原生测试无失败、
  阻塞、未运行或跳过项；汇总见 `gate-results.json`。
- 5000 个 Task 的前 50 项摘要查询：40 次热查询的 p95 为 23.3231 ms，最大值 23.805 ms。
  此数字为当前参考机器的 Core 查询测量，不包含渲染时间。

## 真实应用

隔离 portable 应用从 `qa/package/terminal-0.0.1.0/WindowsTerminal.exe` 启动。
UI 使用中文，窗口为 1560 × 950；生产部署和任务数据库未参与测试。

已验证：

1. 没有 Task 时从独立入口打开 Agent 管理，并保存 `QA 固定配置`。
2. 通过原生文件夹选择器创建 `project-a`，项目没有终端标签仍可进入任务页。
3. 填写任务标题、描述和验收条件，进入 Agent 管理再返回，草稿保留。
4. 选择固定 Agent 并仅保存任务，卡片进入待办。
5. 鼠标把卡片拖入进行中，真实 Runtime 创建一次执行，Codex 在隔离目录生成
   `result.txt`，内容为 `native-project-board-ok`。
6. Agent 显式提交后 Task 为 InReview，Run 为 Completed，执行序号为 1。
7. 关闭项目视图后应用继续运行，项目和上述 Task/Run 仍可查询。
8. 使用原生验收清单保存验收项，再确认完成：Task 为 Done，manual_hold=true，终端关闭后无待处理 intent。
9. Done 卡片重新打开后仍使用原 Task；跟进与返工增加 Run 记录。
10. 活动 Run 启动后关闭带 Task Surface 的项目视图：同一 Run 仍处于活动状态，窗口继续响应。
11. 活动卡片拖回待办：取消确认后仍为 InProgress、hold=false、同一 Run；确认后 Run 被中断，
    待停止 intent 完成才变为 Todo，hold=true，没有额外 Run。
12. 待办移到待规划再撤销：状态、顺序和暂停保持正确，执行序号不增加。
13. 公共 Agent 更新到 revision 2 后，已有 Task 及执行仍保留 revision 1 的固定配置。
14. 同目录再次创建返回已有 Project ID；项目库可重新打开关闭的项目。
15. 900 × 760 窄窗口仍可使用筛选、视图切换和任务操作，看板采用横向滚动。
16. 关闭所有项目视图及终端后，全局 Agent 管理仍可增改；没有目标项目时默认设置禁用。
17. 焦点在项目元数据编辑区时，Ctrl+N 仍打开原生项目目录选择器。

界面检查发现并已修正：动态项目容器在加载前访问空父级、项目库点击对象与容器的区别、
保存草稿后看板筛选、列宽受长执行文字影响、分页计数及无下一页按钮状态；
这些修正已在隔离包内检查。

## 最终包复验与部署

1. `ConfirmOnClose=automatic`，没有项目视图或终端时通过系统窗口关闭入口退出，
   进程退出码为 0；重启恢复项目库（`qa/final-empty-close.json`）。
2. project-b 保留普通 PowerShell 终端，关闭 project-a 最后一个终端后仍选中
   project-a；重新切到 project-b，原终端仍在。
3. 看板、原任务列表/完整详情、Agent 管理及返回正常；Task 使用 v1 固定配置，
   公共 Agent 列表显示 v2。最终图像为 `qa/final-task-board.png`、
   `qa/final-task-list.png`、`qa/final-agents.png`。
4. 确认关闭 project-b 后，project-a 的空项目看板保留。显式关闭窗口再重启，
   project-a 和待办卡片恢复，没有生成占位终端（`qa/final-empty-project-restart.json`）。
5. QA 窗口正常关闭后使用 drain 协议退出隔离 Runtime。正式部署使用
   `build-portable-release.ps1 -SkipBuild`，没有再次编译。
6. 实际启动路径为 `dist/terminal-0.0.1.0/WindowsTerminal.exe`（验证时 PID 13148），
   Runtime 为同目录的 `TaskRuntime.exe`；界面停在全部项目看板，显示原有 3 张待办卡片。
   启动后的应用崩溃事件检查为 0；截图为 `production-board.png`。
7. `production-before-final.json` 与 `production-after.json` 核对通过：schema 5 → 6，
   3 个 Task、2 个 Run、3 份 ExecutionContext 的身份/配置摘要及 Agent/默认关系一致；
   settings.json 哈希一致，数据库完整性为 ok，无外键错误、无活动 Run。
8. 自动备份 `settings/workspace-tasks.db.pre-project-board-134332514708091030.db`
   验证为 schema 5，原记录摘要一致（`production-backup-verified.json`）。没有用旧二进制
   覆盖正式部署或对用户数据库执行实际回退。
9. 正式部署的 WindowsTerminal.exe、TerminalApp.dll、TaskRuntime.exe、rtask.exe
   与最终隔离验收包逐一核对 SHA256 相同；发布明细见 `release-manifest.json`。

发布包：`dist/WindowsTerminalDev_0.0.1.0_x64_workspace-sidebar.zip`。

SHA256：`26991ADCA2759B727A53CE39ABC1DEA621EC7EFFDE2C7FAD22122E46CDC38DE1`。

## 覆盖边界

尚未进行完整 Narrator、高对比、大文本与系统 DPI 组合检查，也没有执行两个真实
窗口对同一任务终端争用输入租约的实机检查。相关自动化通过不等于这些实机项目
已经逐项验收。性能数字只覆盖 Core 摘要查询，未测量看板帧时间或输入延迟。
