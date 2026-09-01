"use strict";

const workspaceNames = {
    win_terminal: "win_terminal",
    richerlife_api: "richerlife-api",
    website: "website",
};

const statusMeta = {
    backlog: { label: "待规划", symbol: "·" },
    todo: { label: "待处理", symbol: "·" },
    in_progress: { label: "进行中", symbol: "↻" },
    in_review: { label: "待验收", symbol: "◇" },
    done: { label: "已完成", symbol: "✓" },
    blocked: { label: "受阻", symbol: "!" },
    cancelled: { label: "已取消", symbol: "×" },
};

const runStatusMeta = {
    queued: "排队中",
    dispatched: "正在启动",
    waiting_local_directory: "等待目录",
    running: "运行中",
    completed: "已完成",
    failed: "失败",
    cancelled: "已停止",
};

const terminalStatusMeta = {
    not_created: "尚未创建",
    starting: "正在创建",
    ready: "已就绪",
    busy: "执行中",
    exited: "已退出",
    suspended: "已挂起",
    error: "错误",
    closed: "已关闭",
};

const now = Date.now();

const tasks = [
    {
        id: "RWT-42",
        workspace: "win_terminal",
        title: "实现 Tasks 固定页骨架与任务流转",
        description: "在 Workspace Surface 标签之外增加不可关闭的 Tasks 固定工具页。任务目标、验收条件、运行过程和最终结果应聚合在同一个详情页中，同时保持终端 Surface 独立。",
        acceptance: [
            { text: "Tasks 与总结是平级固定页，不计入终端 Surface", done: true },
            { text: "任务状态和单次运行状态分别展示", done: true },
            { text: "活动运行可停止，失败运行可重试且不覆盖历史", done: false },
            { text: "任务完成后先进入待验收，由用户确认 Done", done: false },
        ],
        status: "in_progress",
        priority: "high",
        createdAt: now - 1000 * 60 * 92,
        updatedAt: now - 1000 * 22,
        cwd: "F:\\RicherLife\\codes\\win_terminal",
        branch: "feature/task-page-ui-prototype",
        executionMode: "当前目录 · 串行",
        assignee: "Codex",
        result: "",
        pastExpanded: false,
        runs: [
            {
                id: "RUN-104",
                attempt: 2,
                status: "running",
                trigger: "根据确认后的交互方案继续实现",
                createdAt: now - 1000 * 60 * 7,
                startedAt: now - 1000 * 60 * 6 - 1000 * 18,
                finishedAt: null,
                model: "Codex · 默认模型",
                sessionId: "session-7f3a",
                result: "",
                error: "",
                events: [
                    { at: now - 1000 * 60 * 7, text: "TaskRun 已创建并进入队列" },
                    { at: now - 1000 * 60 * 6 - 1000 * 50, text: "本地 Codex Runtime 已接收任务" },
                    { at: now - 1000 * 60 * 6 - 1000 * 18, text: "正在检查 TerminalPage 固定工具页结构" },
                    { at: now - 1000 * 60 * 3, text: "正在生成 Tasks 页面交互原型" },
                ],
            },
            {
                id: "RUN-103",
                attempt: 1,
                status: "failed",
                trigger: "首次尝试生成页面骨架",
                createdAt: now - 1000 * 60 * 47,
                startedAt: now - 1000 * 60 * 46,
                finishedAt: now - 1000 * 60 * 41,
                model: "Codex · 默认模型",
                sessionId: "session-64bb",
                result: "",
                error: "原型服务启动端口被占用。",
                events: [
                    { at: now - 1000 * 60 * 47, text: "TaskRun 已创建" },
                    { at: now - 1000 * 60 * 46, text: "开始生成原型" },
                    { at: now - 1000 * 60 * 41, text: "运行失败：预览端口被占用" },
                ],
            },
        ],
    },
    {
        id: "RWT-41",
        workspace: "win_terminal",
        title: "修复 Ctrl+N 新建 Workspace 的快捷键行为",
        description: "确保 Ctrl+N 创建新的 Workspace，而不是在当前 Workspace 内创建终端 Surface，并保留用户现有快捷键配置。",
        acceptance: [
            { text: "Ctrl+N 创建新的 Workspace", done: true },
            { text: "新建终端仍可通过独立命令执行", done: true },
            { text: "快捷键自动化测试通过", done: true },
        ],
        status: "in_review",
        priority: "medium",
        createdAt: now - 1000 * 60 * 60 * 25,
        updatedAt: now - 1000 * 60 * 34,
        cwd: "F:\\RicherLife\\codes\\win_terminal",
        branch: "feature/workspace-terminal-alt-shortcuts",
        executionMode: "当前目录 · 串行",
        assignee: "Codex",
        result: "快捷键路由已修复，相关自动化测试通过。请确认实际键盘操作是否符合预期。",
        pastExpanded: false,
        runs: [
            {
                id: "RUN-101",
                attempt: 1,
                status: "completed",
                trigger: "修复并验证 Ctrl+N 行为",
                createdAt: now - 1000 * 60 * 94,
                startedAt: now - 1000 * 60 * 92,
                finishedAt: now - 1000 * 60 * 34,
                model: "Codex · 默认模型",
                sessionId: "session-c0de",
                result: "修改完成，测试通过。",
                error: "",
                events: [
                    { at: now - 1000 * 60 * 94, text: "开始分析快捷键路由" },
                    { at: now - 1000 * 60 * 72, text: "修复 Workspace 新建动作" },
                    { at: now - 1000 * 60 * 34, text: "完整验证通过，等待用户验收" },
                ],
            },
        ],
    },
    {
        id: "RWT-39",
        workspace: "win_terminal",
        title: "同步 Microsoft Terminal 上游更新",
        description: "通过统一同步脚本检查 Microsoft Terminal 新版本，将渲染和稳定性修复安全合并到 fork。",
        acceptance: [
            { text: "生成上游漂移报告", done: true },
            { text: "冲突按 Workspace/Surface 合同逐项解决", done: false },
            { text: "Release/MSIX 完整门禁通过", done: false },
        ],
        status: "todo",
        priority: "medium",
        createdAt: now - 1000 * 60 * 60 * 31,
        updatedAt: now - 1000 * 60 * 60 * 4,
        cwd: "F:\\RicherLife\\codes\\win_terminal",
        branch: "sync/microsoft-terminal-preview",
        executionMode: "当前目录 · 串行",
        assignee: "Codex",
        result: "",
        pastExpanded: true,
        runs: [
            {
                id: "RUN-98",
                attempt: 1,
                status: "failed",
                trigger: "准备上游同步分支",
                createdAt: now - 1000 * 60 * 60 * 5,
                startedAt: now - 1000 * 60 * 60 * 5 + 1000 * 38,
                finishedAt: now - 1000 * 60 * 60 * 4,
                model: "Codex · 默认模型",
                sessionId: "session-upstream",
                result: "",
                error: "检测到同步热点，需要先确认合并策略。",
                events: [
                    { at: now - 1000 * 60 * 60 * 5, text: "获取 Microsoft Terminal upstream" },
                    { at: now - 1000 * 60 * 60 * 4, text: "检测到 TerminalPage 合并热点，暂停等待确认" },
                ],
            },
        ],
    },
    {
        id: "RWT-37",
        workspace: "richerlife_api",
        title: "等待测试环境凭据后执行发布验证",
        description: "测试环境当前无法访问。保留任务上下文，待凭据恢复后继续执行验证。",
        acceptance: [
            { text: "测试环境恢复访问", done: false },
            { text: "部署冒烟测试通过", done: false },
        ],
        status: "blocked",
        priority: "high",
        createdAt: now - 1000 * 60 * 60 * 48,
        updatedAt: now - 1000 * 60 * 60 * 8,
        cwd: "F:\\RicherLife\\codes\\richerlife-api",
        branch: "main",
        executionMode: "当前目录 · 串行",
        assignee: "Codex",
        blockedReason: "缺少测试环境凭据。",
        result: "",
        pastExpanded: false,
        runs: [],
    },
    {
        id: "RWT-35",
        workspace: "win_terminal",
        title: "Workspace Sidebar 不显示终端数量",
        description: "左侧只表达 Workspace 层级，不用数量强调其下打开了多少终端 Surface。",
        acceptance: [
            { text: "Sidebar 保持 Workspace-only", done: true },
            { text: "Surface 数量不出现在 Workspace 行", done: true },
        ],
        status: "done",
        priority: "low",
        createdAt: now - 1000 * 60 * 60 * 72,
        updatedAt: now - 1000 * 60 * 60 * 28,
        cwd: "F:\\RicherLife\\codes\\win_terminal",
        branch: "feature/workspace-terminal-alt-shortcuts",
        executionMode: "当前目录 · 串行",
        assignee: "Codex",
        result: "Sidebar 已保持 Workspace-only。",
        pastExpanded: false,
        runs: [
            {
                id: "RUN-90",
                attempt: 1,
                status: "completed",
                trigger: "移除终端数量信息",
                createdAt: now - 1000 * 60 * 60 * 30,
                startedAt: now - 1000 * 60 * 60 * 30 + 1000 * 20,
                finishedAt: now - 1000 * 60 * 60 * 28,
                model: "Codex · 默认模型",
                sessionId: "session-sidebar",
                result: "界面调整完成。",
                error: "",
                events: [
                    { at: now - 1000 * 60 * 60 * 30, text: "检查 Workspace Sidebar 信息层级" },
                    { at: now - 1000 * 60 * 60 * 28, text: "修改完成并验证" },
                ],
            },
        ],
    },
    {
        id: "WEB-12",
        workspace: "website",
        title: "优化产品页的移动端首屏",
        description: "重新组织移动端首屏内容层级，提升主要行动按钮的可见性。",
        acceptance: [
            { text: "375px 宽度不产生横向滚动", done: false },
            { text: "主要行动按钮位于首屏", done: false },
        ],
        status: "backlog",
        priority: "low",
        createdAt: now - 1000 * 60 * 60 * 9,
        updatedAt: now - 1000 * 60 * 60 * 9,
        cwd: "F:\\RicherLife\\codes\\website",
        branch: "feature/landing",
        executionMode: "当前目录 · 串行",
        assignee: "Codex",
        result: "",
        pastExpanded: false,
        runs: [],
    },
];

tasks.forEach((task) => {
    task.followups ??= [];
    task.manualHold ??= false;
    const hasActiveRun = task.runs.some((run) => ["queued", "dispatched", "waiting_local_directory", "running"].includes(run.status));
    task.terminalState ??= task.status === "done" ? "closed" : hasActiveRun ? "busy" : "ready";
    if (task.cwd.includes("win_terminal")) {
        task.executionMode = "Worktree · Task 专属";
        task.worktree = `F:\\RicherLife\\task-worktrees\\${task.id.toLowerCase()}`;
        task.branch = task.status === "done" ? task.branch : `agent/codex/${task.id.toLowerCase()}`;
    } else {
        task.worktree = "—";
    }
});
tasks.find((task) => task.id === "RWT-39").terminalState = "suspended";

const state = {
    currentWorkspace: "win_terminal",
    scope: "current",
    statusFilter: "open",
    search: "",
    selectedTaskId: "RWT-42",
    sortNewestFirst: true,
    page: "tasks",
    draftMode: false,
    terminalTaskId: "RWT-42",
    idSequence: 43,
    runSequence: 105,
};

const taskList = document.querySelector("#task-list");
const taskListEmpty = document.querySelector("#task-list-empty");
const taskCount = document.querySelector("#task-count");
const taskDetail = document.querySelector("#task-detail");
const taskSearch = document.querySelector("#task-search");
const statusFilter = document.querySelector("#status-filter");
const runDetailDialog = document.querySelector("#run-detail-dialog");
const toastRegion = document.querySelector("#toast-region");

function escapeHtml(value) {
    return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

function formatRelative(timestamp) {
    if (!timestamp) return "—";
    const seconds = Math.max(0, Math.round((Date.now() - timestamp) / 1000));
    if (seconds < 45) return "刚刚";
    const minutes = Math.round(seconds / 60);
    if (minutes < 60) return `${minutes} 分钟前`;
    const hours = Math.round(minutes / 60);
    if (hours < 24) return `${hours} 小时前`;
    const days = Math.round(hours / 24);
    return `${days} 天前`;
}

function formatClock(timestamp) {
    if (!timestamp) return "—";
    return new Intl.DateTimeFormat("zh-CN", {
        month: "2-digit",
        day: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
        hour12: false,
    }).format(new Date(timestamp));
}

function formatDuration(startedAt, finishedAt = Date.now()) {
    if (!startedAt) return "—";
    const total = Math.max(0, Math.floor((finishedAt - startedAt) / 1000));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const seconds = total % 60;
    if (hours > 0) return `${hours}h ${String(minutes).padStart(2, "0")}m`;
    if (minutes > 0) return `${minutes}m ${String(seconds).padStart(2, "0")}s`;
    return `${seconds}s`;
}

function isActiveRun(run) {
    return ["queued", "dispatched", "waiting_local_directory", "running"].includes(run.status);
}

function isPendingRun(run) {
    return ["queued", "dispatched", "waiting_local_directory"].includes(run.status);
}

function getActiveRun(task) {
    return task.runs.find(isActiveRun) ?? null;
}

function priorityLabel(priority) {
    return { high: "高优先级", medium: "中优先级", low: "低优先级" }[priority] ?? "无优先级";
}

function statusPill(status) {
    const meta = statusMeta[status] ?? statusMeta.todo;
    return `<span class="status-pill ${escapeHtml(status)}"><span aria-hidden="true">${meta.symbol}</span>${meta.label}</span>`;
}

function runStatusPill(status) {
    return `<span class="run-status-pill ${escapeHtml(status)}">${escapeHtml(runStatusMeta[status] ?? status)}</span>`;
}

function statusIcon(status) {
    const meta = statusMeta[status] ?? statusMeta.todo;
    return `<span class="task-status-icon ${escapeHtml(status)}" aria-label="${meta.label}">${meta.symbol}</span>`;
}

function filteredTasks() {
    const normalizedSearch = state.search.trim().toLocaleLowerCase("zh-CN");
    return tasks
        .filter((task) => state.scope === "all" || task.workspace === state.currentWorkspace)
        .filter((task) => {
            if (state.statusFilter === "all") return true;
            if (state.statusFilter === "open") return !["done", "cancelled"].includes(task.status);
            return task.status === state.statusFilter;
        })
        .filter((task) => {
            if (!normalizedSearch) return true;
            const haystack = `${task.id} ${task.title} ${workspaceNames[task.workspace] ?? task.workspace}`.toLocaleLowerCase("zh-CN");
            return haystack.includes(normalizedSearch);
        })
        .sort((a, b) => state.sortNewestFirst ? b.updatedAt - a.updatedAt : a.updatedAt - b.updatedAt);
}

function renderTaskList() {
    const visible = filteredTasks();
    taskCount.textContent = `${visible.length} 项`;
    taskListEmpty.hidden = visible.length > 0;

    if (visible.length > 0 && !visible.some((task) => task.id === state.selectedTaskId)) {
        state.selectedTaskId = visible[0].id;
    }

    taskList.innerHTML = visible.map((task) => {
        const activeRun = getActiveRun(task);
        const selected = task.id === state.selectedTaskId;
        return `
            <button class="task-list-item${selected ? " selected" : ""}"
                    type="button"
                    role="option"
                    aria-selected="${selected}"
                    data-task-id="${escapeHtml(task.id)}">
                ${statusIcon(task.status)}
                <span class="task-item-copy">
                    <span class="task-item-topline">
                        <span class="task-id">${escapeHtml(task.id)}</span>
                        ${statusPill(task.status)}
                    </span>
                    <strong class="task-item-title">${escapeHtml(task.title)}</strong>
                    <span class="task-item-meta">
                        <span class="workspace-name">${escapeHtml(workspaceNames[task.workspace] ?? task.workspace)}</span>
                        <span class="meta-separator">·</span>
                        <span>${escapeHtml(formatRelative(task.updatedAt))}</span>
                    </span>
                    ${activeRun ? `
                        <span class="task-item-run">
                            <span class="run-pulse" aria-hidden="true"></span>
                            <span>${escapeHtml(runStatusMeta[activeRun.status])}${activeRun.status === "running" ? ` · <span data-run-elapsed="${escapeHtml(activeRun.id)}">${formatDuration(activeRun.startedAt)}</span>` : ""}</span>
                        </span>` : ""}
                </span>
            </button>`;
    }).join("");
}

function renderDetailActions(task, activeRun) {
    if (activeRun) {
        return `
            <button class="button secondary" type="button" data-action="open-terminal">打开终端</button>
            <button class="button danger" type="button" data-action="stop-run" data-run-id="${escapeHtml(activeRun.id)}">停止（Esc）</button>
            <button class="button ghost" type="button" data-action="close-task">关闭任务终端</button>
            <button class="button danger" type="button" data-action="cancel-task">取消任务</button>`;
    }

    const maintenanceActions = `
        ${task.executionMode.startsWith("Worktree") ? `<button class="button ghost" type="button" data-action="finalize-worktree">交付并清理 Worktree</button>` : ""}
        <button class="button danger" type="button" data-action="delete-task">删除任务</button>`;

    switch (task.status) {
        case "backlog":
        case "todo":
            if (task.terminalState === "not_created") {
                return `<button class="button primary" type="button" data-action="start-run">打开并运行</button>`;
            }
            return `
                <button class="button primary" type="button" data-action="start-run">交给 Codex</button>
                <button class="button secondary" type="button" data-action="open-terminal">${task.terminalState === "closed" || task.terminalState === "suspended" ? "打开任务终端" : "打开终端"}</button>
                ${task.terminalState === "closed" ? "" : `<button class="button ghost" type="button" data-action="close-task">关闭任务终端</button>`}
                <button class="button ghost" type="button" data-action="cancel-task">取消任务</button>${maintenanceActions}`;
        case "in_review":
            return `
                <button class="button secondary" type="button" data-action="focus-followup">继续处理</button>
                <button class="button success" type="button" data-action="mark-done">确认完成</button>
                <button class="button secondary" type="button" data-action="open-terminal">${task.terminalState === "closed" || task.terminalState === "suspended" ? "打开任务终端" : "打开终端"}</button>
                ${task.terminalState === "closed" ? "" : `<button class="button ghost" type="button" data-action="close-task">关闭任务终端</button>`}
                <button class="button ghost" type="button" data-action="cancel-task">取消任务</button>${maintenanceActions}`;
        case "blocked":
            return `
                <button class="button secondary" type="button" data-action="unblock">解除阻塞</button>
                <button class="button secondary" type="button" data-action="open-terminal">${task.terminalState === "closed" || task.terminalState === "suspended" ? "打开任务终端" : "打开终端"}</button>
                ${task.terminalState === "closed" ? "" : `<button class="button ghost" type="button" data-action="close-task">关闭任务终端</button>`}
                <button class="button ghost" type="button" data-action="cancel-task">取消任务</button>${maintenanceActions}`;
        case "done":
            return `<button class="button secondary" type="button" data-action="reopen">重新打开</button>${maintenanceActions}`;
        case "cancelled":
            return `<button class="button secondary" type="button" data-action="reopen">重新打开</button>${maintenanceActions}`;
        default:
            return "";
    }
}

function activeRunMarkup(run) {
    return `
        <section class="active-run-card" aria-label="活动运行">
            <div class="run-card-topline">
                <div class="run-card-title">
                    <span class="run-pulse" aria-hidden="true"></span>
                    <strong>${escapeHtml(runStatusMeta[run.status])}</strong>
                    ${runStatusPill(run.status)}
                </div>
                ${run.status === "running" ? `<span class="execution-summary" data-run-elapsed="${escapeHtml(run.id)}">${formatDuration(run.startedAt)}</span>` : ""}
            </div>
            <p class="run-card-meta">${escapeHtml(run.events.at(-1)?.text ?? run.trigger)}${run.simulated ? " · 原型将在数秒后模拟完成" : ""}</p>
            <div class="run-progress-track" aria-label="运行进行中"><div class="run-progress-bar"></div></div>
        </section>`;
}

function reviewMarkup(task) {
    if (task.status !== "in_review") return "";
    return `
        <section class="review-card" aria-label="等待验收">
            <div class="run-card-title">
                <span class="task-status-icon in_review" aria-hidden="true">◇</span>
                <strong>本次运行已结束，等待你的验收</strong>
            </div>
            <p>${escapeHtml(task.result || "请检查变更和测试结果；运行完成不等于任务目标已经满足。")}</p>
        </section>`;
}

function blockedMarkup(task) {
    if (task.status !== "blocked") return "";
    return `
        <section class="blocked-card" aria-label="任务受阻">
            <div class="run-card-title">
                <span class="task-status-icon blocked" aria-hidden="true">!</span>
                <strong>任务当前无法继续</strong>
            </div>
            <p>${escapeHtml(task.blockedReason || "等待外部条件变化。")}</p>
        </section>`;
}

function renderRunRow(run, active) {
    const canRetry = ["failed", "cancelled"].includes(run.status);
    const detail = active
        ? `${run.trigger} · ${run.status === "running" ? formatDuration(run.startedAt) : runStatusMeta[run.status]}`
        : `${run.trigger} · ${formatRelative(run.finishedAt ?? run.createdAt)}`;
    return `
        <div class="execution-row">
            <span class="run-avatar" aria-hidden="true">C</span>
            <span class="execution-copy">
                <strong>运行 #${run.attempt} · ${escapeHtml(run.trigger)}</strong>
                <small>${escapeHtml(detail)}</small>
            </span>
            ${runStatusPill(run.status)}
            <span class="run-actions">
                <button class="run-action" type="button" data-action="view-run" data-run-id="${escapeHtml(run.id)}" aria-label="查看运行详情" title="查看运行详情">↗</button>
                ${canRetry ? `<button class="run-action retry" type="button" data-action="retry-run" data-run-id="${escapeHtml(run.id)}" aria-label="重试这次运行" title="重试这次运行">↻</button>` : ""}
            </span>
        </div>`;
}

function executionLogMarkup(task) {
    const activeRuns = task.runs.filter(isActiveRun);
    const pastRuns = task.runs.filter((run) => !isActiveRun(run));
    if (task.runs.length === 0) {
        return `
            <section class="execution-log">
                <div class="execution-header">
                    <h3>执行记录</h3>
                    <span class="execution-summary">尚未运行</span>
                </div>
                <div class="execution-list"><div class="execution-row"><span class="execution-copy"><strong>创建一次运行后，这里会保留完整历史</strong><small>重试不会覆盖以前的记录</small></span></div></div>
            </section>`;
    }

    return `
        <section class="execution-log" aria-label="执行记录">
            <div class="execution-header">
                <h3>执行记录</h3>
                <span class="execution-summary">${activeRuns.length ? `${activeRuns.length} 个活动运行` : "无活动运行"}</span>
            </div>
            ${activeRuns.length ? `<div class="execution-list">${activeRuns.map((run) => renderRunRow(run, true)).join("")}</div>` : ""}
            ${pastRuns.length ? `
                <button class="past-runs-toggle" type="button" data-action="toggle-past" aria-expanded="${task.pastExpanded}">
                    <span class="chevron${task.pastExpanded ? " open" : ""}" aria-hidden="true">›</span>
                    ${task.pastExpanded ? "隐藏" : "显示"}历史运行（${pastRuns.length}）
                </button>
                <div class="past-runs"${task.pastExpanded ? "" : " hidden"}>
                    <div class="execution-list">${pastRuns.map((run) => renderRunRow(run, false)).join("")}</div>
                </div>` : ""}
        </section>`;
}

function flowMapMarkup(task) {
    const steps = [
        ["todo", "待处理：目标已整理"],
        ["in_progress", "进行中：TaskRun 正在执行"],
        ["in_review", "待验收：运行结束，人工检查"],
        ["done", "已完成：用户确认目标达成"],
    ];
    return steps.map(([status, label]) => `<div class="flow-step${task.status === status ? " current" : ""}">${escapeHtml(label)}</div>`).join("");
}

function followupQueueMarkup(task) {
    if (!task.followups.length) return "";
    const activeRun = getActiveRun(task);
    const queueHint = task.manualHold
        ? "手动暂停，等待显式运行或恢复"
        : isPendingRun(activeRun ?? {})
            ? "Agent 启动前会合并到当前 TaskRun"
            : activeRun?.status === "running"
                ? "当前运行结束后合并为一次 TaskRun"
                : "等待下一次 TaskRun 领取";
    return `
        <section class="followup-queue" aria-label="已排队补充要求">
            <div class="followup-queue__header">
                <span>已排队 ${task.followups.length} 条补充要求</span>
                <span>${queueHint}</span>
            </div>
            ${task.followups.map((message, index) => `
                <div class="followup-queue__item">
                    <span>${escapeHtml(message.text)}</span>
                    <span class="followup-queue__actions">
                        <button class="run-action" type="button" data-action="edit-followup" data-followup-index="${index}" aria-label="编辑补充要求" title="编辑补充要求">✎</button>
                        <button class="run-action" type="button" data-action="remove-followup" data-followup-index="${index}" aria-label="移除补充要求" title="移除补充要求">×</button>
                    </span>
                </div>`).join("")}
        </section>`;
}

function renderTaskDraft() {
    taskDetail.innerHTML = `
        <div class="draft-shell">
            <header class="draft-heading">
                <span class="eyebrow">NEW TASK</span>
                <h2>新建任务</h2>
                <p>保存只创建 Todo 记录；保存并运行会在持久化后准备 Task 专属 Worktree 和 Agent Session。</p>
            </header>
            <form id="inline-task-form" class="draft-form">
                <label class="form-field">
                    <span>标题 <b aria-hidden="true">*</b></span>
                    <input name="title" required maxlength="120" placeholder="要完成什么？" autocomplete="off">
                </label>
                <label class="form-field">
                    <span>目标与背景</span>
                    <textarea name="description" rows="5" placeholder="描述目标、上下文和边界……"></textarea>
                </label>
                <label class="form-field">
                    <span>验收条件</span>
                    <textarea name="acceptance" rows="4" placeholder="每行一项验收条件"></textarea>
                </label>
                <div class="form-grid">
                    <label class="form-field">
                        <span>Workspace</span>
                        <select name="workspace">
                            <option value="win_terminal" ${state.currentWorkspace === "win_terminal" ? "selected" : ""}>win_terminal</option>
                            <option value="richerlife_api" ${state.currentWorkspace === "richerlife_api" ? "selected" : ""}>richerlife-api</option>
                            <option value="website" ${state.currentWorkspace === "website" ? "selected" : ""}>website</option>
                        </select>
                    </label>
                    <label class="form-field">
                        <span>执行方式</span>
                        <select name="mode">
                            <option value="worktree">Task 专属 Worktree（Git 默认）</option>
                            <option value="direct">Direct（托管运行串行）</option>
                        </select>
                    </label>
                </div>
                <div class="consent-note">
                    <span aria-hidden="true">◆</span>
                    <span>Agent 将读取并修改所选目录。Worktree 会完整快照当前 tracked 与受限 untracked 状态，无法忠实恢复时拒绝启动。</span>
                </div>
                <div class="draft-actions">
                    <button class="button ghost" type="button" data-action="cancel-draft">取消</button>
                    <button class="button secondary" type="submit" value="save">仅保存</button>
                    <button class="button primary" type="submit" value="run">保存并运行</button>
                </div>
            </form>
        </div>`;
    taskDetail.querySelector("input[name='title']")?.focus();
}

function renderTaskDetail() {
    if (state.draftMode) {
        renderTaskDraft();
        return;
    }
    const task = tasks.find((candidate) => candidate.id === state.selectedTaskId);
    if (!task) {
        taskDetail.innerHTML = `<div class="detail-empty"><div><strong>请选择一个任务</strong><p>任务详情会显示在这里。</p></div></div>`;
        return;
    }

    const activeRun = getActiveRun(task);
    const completedAcceptance = task.acceptance.filter((item) => item.done).length;
    const composerDisabled = task.manualHold || ["done", "cancelled", "blocked"].includes(task.status);
    const composerHint = activeRun
        ? isPendingRun(activeRun)
            ? "发送后会在 Agent 启动前并入当前 TaskRun。"
            : "发送后会持久化排队，当前运行保持不变。"
        : "发送后会持久化消息并创建新的 TaskRun。";
    const composerActionLabel = activeRun
        ? isPendingRun(activeRun) ? "并入当前运行" : "排队补充要求"
        : "发送给 Codex";

    taskDetail.innerHTML = `
        <div class="detail-shell">
            <header class="detail-header">
                <div class="detail-heading">
                    <div class="detail-identifier">
                        <span>${escapeHtml(task.id)}</span>
                        ${statusPill(task.status)}
                        <span class="priority-pill ${escapeHtml(task.priority)}">${escapeHtml(priorityLabel(task.priority))}</span>
                    </div>
                    <h2>${escapeHtml(task.title)}</h2>
                    <div class="detail-subtitle">
                        <span>${escapeHtml(workspaceNames[task.workspace])}</span>
                        <span class="meta-separator">·</span>
                        <span>更新于 ${escapeHtml(formatRelative(task.updatedAt))}</span>
                        <span class="meta-separator">·</span>
                        <span>${completedAcceptance}/${task.acceptance.length} 项验收条件</span>
                    </div>
                </div>
                <div class="detail-actions">${renderDetailActions(task, activeRun)}</div>
            </header>

            <div class="detail-grid">
                <div class="detail-main">
                    ${activeRun ? activeRunMarkup(activeRun) : ""}
                    ${followupQueueMarkup(task)}
                    ${reviewMarkup(task)}
                    ${blockedMarkup(task)}

                    <section class="content-section">
                        <h3>目标与背景</h3>
                        <p>${escapeHtml(task.description)}</p>
                    </section>

                    <section class="content-section">
                        <h3>验收条件</h3>
                        <div class="acceptance-list">
                            ${task.acceptance.length ? task.acceptance.map((item, index) => `
                                <label class="acceptance-item">
                                    <input type="checkbox" data-acceptance-index="${index}" ${item.done ? "checked" : ""}>
                                    <span>${escapeHtml(item.text)}</span>
                                </label>`).join("") : `<p>尚未填写验收条件。</p>`}
                        </div>
                    </section>

                    ${executionLogMarkup(task)}

                    <section class="composer-card" aria-label="继续处理">
                        <textarea id="followup-input" placeholder="补充要求、指出问题，或告诉 Codex 下一步……" ${composerDisabled ? "disabled" : ""}></textarea>
                        <div class="composer-actions">
                            <span class="composer-hint">${composerHint}</span>
                            <button class="button primary small" type="button" data-action="send-followup" ${composerDisabled ? "disabled" : ""}>${composerActionLabel}</button>
                        </div>
                    </section>
                </div>

                <aside class="detail-aside" aria-label="任务属性">
                    <section class="aside-section">
                        <h3>属性</h3>
                        <div class="property-list">
                            <div class="property-row"><span>状态</span>${statusPill(task.status)}</div>
                            <div class="property-row"><span>负责人</span><span class="property-chip">✦ ${escapeHtml(task.assignee)}</span></div>
                            <div class="property-row"><span>Workspace</span><span>${escapeHtml(workspaceNames[task.workspace])}</span></div>
                            <div class="property-row"><span>执行方式</span><span>${escapeHtml(task.executionMode)}</span></div>
                            <div class="property-row"><span>终端</span><span class="terminal-state">◆ ${escapeHtml(terminalStatusMeta[task.terminalState] ?? task.terminalState)}</span></div>
                            <div class="property-row"><span>Worktree</span><span title="${escapeHtml(task.worktree)}">${escapeHtml(task.worktree)}</span></div>
                            <div class="property-row"><span>分支</span><span title="${escapeHtml(task.branch)}">${escapeHtml(task.branch)}</span></div>
                            <div class="property-row"><span>工作目录</span><span title="${escapeHtml(task.cwd)}">${escapeHtml(task.cwd)}</span></div>
                        </div>
                    </section>
                    <section class="aside-section">
                        <h3>任务流转</h3>
                        <div class="flow-map">${flowMapMarkup(task)}</div>
                    </section>
                    <section class="aside-section">
                        <h3>设计约束</h3>
                        <div class="flow-map">
                            <div class="flow-step">Task 完成不自动等于目标完成</div>
                            <div class="flow-step">重试始终创建新运行记录</div>
                            <div class="flow-step">关闭 Surface 不删除任务</div>
                        </div>
                    </section>
                </aside>
            </div>
        </div>`;
}

function renderAll() {
    renderTaskList();
    renderTaskDetail();
    updateLiveIndicator();
    renderAgentTerminal();
}

function updateLiveIndicator() {
    const hasActive = tasks.some((task) => task.runs.some(isActiveRun));
    document.querySelectorAll(".live-indicator").forEach((indicator) => {
        indicator.hidden = !hasActive;
    });
}

function renderAgentTerminal() {
    const task = tasks.find((candidate) => candidate.id === state.terminalTaskId) ?? selectedTask();
    if (!task) return;
    const activeRun = getActiveRun(task);
    const title = `${task.id} · Codex Agent`;
    const titleElement = document.querySelector("#agent-terminal-title");
    const tabTitle = document.querySelector("#task-surface-title");
    const taskSurfaceTab = document.querySelector("#task-surface-tab");
    if (titleElement) titleElement.textContent = title;
    if (tabTitle) tabTitle.textContent = `${task.id} · Agent`;
    if (taskSurfaceTab) taskSurfaceTab.hidden = ["closed", "not_created", "suspended"].includes(task.terminalState);

    const lines = [
        `[runtime] App Server thread attached · ${task.executionMode}`,
        `[context] ${task.worktree !== "—" ? task.worktree : task.cwd}`,
        `[state] terminal=${task.terminalState} task=${statusMeta[task.status]?.label ?? task.status}`,
        "",
    ];
    const runs = task.runs.slice().reverse();
    runs.forEach((run) => {
        lines.push(`── TaskRun #${run.attempt} · ${runStatusMeta[run.status] ?? run.status} ──`);
        run.events.forEach((event) => lines.push(`[event] ${event.text}`));
        if (run.error) lines.push(`[error] ${run.error}`);
        if (run.result) lines.push(`[result] ${run.result}`);
        lines.push("");
    });
    if (task.followups.length) {
        lines.push(`[queue] ${task.followups.length} 条持久化补充要求等待后续 TaskRun`);
    }
    if (activeRun) {
        lines.push("[input] 当前 turn 可 steer；按 Esc 与 Tasks 页停止行为一致。");
    } else if (task.terminalState === "closed") {
        lines.push("[closed] Agent 终端已关闭。请从 Tasks 页重新打开。");
    } else {
        lines.push("[ready] 输入消息将创建新的 TaskRun。");
    }
    const output = document.querySelector("#agent-terminal-output");
    if (output) {
        output.textContent = lines.join("\n");
        output.scrollTop = output.scrollHeight;
    }
    const input = document.querySelector("#agent-terminal-message");
    const submit = document.querySelector("#agent-terminal-form button[type='submit']");
    const inputDisabled = ["closed", "suspended", "not_created"].includes(task.terminalState);
    if (input) input.disabled = inputDisabled;
    if (submit) submit.disabled = inputDisabled;
}

function showToast(message, tone = "info") {
    const toast = document.createElement("div");
    toast.className = `toast ${tone}`;
    toast.textContent = message;
    toastRegion.append(toast);
    window.setTimeout(() => toast.remove(), 4200);
}

function showPage(page, surface = null) {
    state.page = page;
    document.querySelector("#tasks-page").hidden = page !== "tasks";
    document.querySelector("#summary-page").hidden = page !== "summary";
    document.querySelector("#terminal-page").hidden = page !== "terminal";

    document.querySelectorAll(".tool-tab").forEach((button) => {
        const selected = button.dataset.toolPage === page;
        button.classList.toggle("selected", selected);
        button.setAttribute("aria-selected", String(selected));
    });
    document.querySelectorAll(".surface-tab").forEach((button) => {
        const selected = page === "terminal" && button.dataset.surface === surface;
        button.classList.toggle("selected", selected);
        button.setAttribute("aria-selected", String(selected));
    });
    if (page === "terminal") renderAgentTerminal();
}

function transitionRun(task, run, status, message) {
    if (!isActiveRun(run) && !["completed", "failed", "cancelled"].includes(status)) return false;
    run.status = status;
    const eventTime = Date.now();
    run.events.push({ at: eventTime, text: message });
    task.updatedAt = eventTime;
    if (status === "running" && !run.startedAt) run.startedAt = eventTime;
    if (["completed", "failed", "cancelled"].includes(status)) run.finishedAt = eventTime;
    renderAll();
    return true;
}

function claimFollowups(task, run) {
    if (!task.followups.length) return 0;
    const claimedMessages = task.followups.splice(0);
    run.messages ??= [];
    run.messages.push(...claimedMessages.map((message) => ({ ...message, claimedAt: Date.now() })));
    run.trigger = `${run.trigger} · 合并 ${claimedMessages.length} 条补充要求`;
    claimedMessages.forEach((message) => {
        run.events.push({ at: Date.now(), text: `已领取补充要求：${message.text}` });
    });
    task.updatedAt = Date.now();
    return claimedMessages.length;
}

function startRun(task, trigger = "手动启动任务") {
    if (getActiveRun(task)) {
        showToast("该任务已有活动运行。", "warning");
        return;
    }

    const run = {
        id: `RUN-${state.runSequence++}`,
        attempt: task.runs.length + 1,
        status: "queued",
        trigger,
        createdAt: Date.now(),
        startedAt: null,
        finishedAt: null,
        model: "Codex · 默认模型",
        sessionId: `prototype-${Math.random().toString(16).slice(2, 8)}`,
        result: "",
        error: "",
        messages: [],
        simulated: true,
        events: [
            { at: Date.now(), text: "TaskRun 已创建并进入队列" },
            { at: Date.now(), text: task.executionMode.startsWith("Worktree") ? "复用 Task 专属 Worktree 并记录 start commit" : "等待 Direct 目录租约" },
        ],
    };

    task.manualHold = false;
    task.terminalState = "busy";
    task.result = "";
    task.runs.unshift(run);
    task.updatedAt = Date.now();
    state.selectedTaskId = task.id;
    showPage("tasks");
    renderAll();
    showToast("已创建新的 TaskRun；原型正在模拟 Codex 执行。", "success");

    window.setTimeout(() => {
        if (run.status !== "queued") return;
        transitionRun(task, run, "dispatched", "本地 Codex Runtime 已接收任务");
    }, 650);

    window.setTimeout(() => {
        if (run.status !== "dispatched") return;
        claimFollowups(task, run);
        task.status = "in_progress";
        run.events.push({ at: Date.now(), text: "Agent 通过 rtask 将 Task 设置为 InProgress" });
        transitionRun(task, run, "running", "Codex 正在读取目标与 Workspace 上下文");
    }, 1450);

    window.setTimeout(() => {
        if (run.status !== "running") return;
        transitionRun(task, run, "running", "正在修改文件并执行相关验证");
    }, 3900);

    window.setTimeout(() => {
        if (run.status !== "running") return;
        run.result = "原型模拟：本次运行已正常结束，变更和验证结果等待人工检查。";
        task.result = run.result;
        task.status = "in_review";
        task.terminalState = "ready";
        run.events.push({ at: Date.now(), text: "Runtime 创建 TaskRun checkpoint commit" });
        run.events.push({ at: Date.now(), text: "Agent 通过 rtask 将 Task 设置为 InReview" });
        transitionRun(task, run, "completed", "运行正常结束，任务进入待验收");
        if (task.followups.length && !task.manualHold) {
            window.setTimeout(() => {
                if (task.followups.length && !task.manualHold && !getActiveRun(task)) {
                    startRun(task, "运行结束后的跟进批次");
                }
            }, 900);
        }
        showToast(`${task.id} 已进入待验收；运行完成不自动标记 Done。`, "success");
    }, 7600);
}

function stopRun(task, runId) {
    const run = task.runs.find((candidate) => candidate.id === runId);
    if (!run || !isActiveRun(run)) return;
    run.error = "用户主动停止本次运行。";
    task.manualHold = true;
    task.terminalState = "ready";
    if (task.status === "in_progress") task.status = "todo";
    const wasPending = isPendingRun(run);
    transitionRun(task, run, "cancelled", wasPending
        ? "尚未启动的 TaskRun 已取消；Task 进入手动暂停"
        : "收到 turn/interrupt（Esc）；Task 进入手动暂停");
    showToast(wasPending
        ? "已取消待启动运行；Agent 终端和未领取消息均保留。"
        : "已发送 Esc；Agent 终端保留，跟进消息不会自动重启。", "warning");
}

function closeTask(task) {
    const activeRun = getActiveRun(task);
    if (activeRun) {
        const confirmed = window.confirm("关闭任务终端？\n\n当前 turn 将被中断。Task、Worktree、检查点与历史记录会保留。");
        if (!confirmed) return;
        stopRun(task, activeRun.id);
    }
    task.manualHold = true;
    task.terminalState = "closed";
    task.updatedAt = Date.now();
    showPage("tasks");
    renderAll();
    showToast(`${task.id} 的 Agent 终端已关闭；Task 可随时重新打开。`, "info");
}

function markDone(task) {
    if (getActiveRun(task) || task.followups.length) {
        showToast("请先结束活动运行并处理已排队的补充要求。", "warning");
        return;
    }
    task.status = "done";
    task.terminalState = "closed";
    task.manualHold = true;
    task.updatedAt = Date.now();
    renderAll();
    showPage("tasks");
    showToast(`${task.id} 已确认完成，Agent 终端已关闭；Worktree 与历史保留。`, "success");
}

function cancelTask(task) {
    const activeRun = getActiveRun(task);
    if (activeRun) stopRun(task, activeRun.id);
    task.status = "cancelled";
    task.terminalState = "closed";
    task.manualHold = true;
    task.updatedAt = Date.now();
    renderAll();
    showToast(`${task.id} 已取消；Task、结果分支、Worktree 与历史均保留。`, "warning");
}

function finalizeWorktree(task, confirm = true) {
    if (getActiveRun(task)) {
        showToast("活动运行结束前不能清理 Worktree。", "warning");
        return false;
    }
    if (confirm && !window.confirm("交付并清理 Task Worktree？\n\n所有剩余改动会先创建 checkpoint；有产出的结果分支会保留。")) return false;
    task.worktree = "已清理；结果分支已保留";
    task.updatedAt = Date.now();
    renderAll();
    showToast("Worktree 已安全交付并清理。", "success");
    return true;
}

function deleteTask(task) {
    if (!window.confirm("永久删除这个 Task？\n\nRuntime 会先停止任务并安全交付 Worktree。任务、运行、消息和结构化历史随后会删除。")) return;
    if (task.executionMode.startsWith("Worktree") && !task.worktree.startsWith("已清理") && !finalizeWorktree(task, false)) return;
    const index = tasks.indexOf(task);
    if (index >= 0) tasks.splice(index, 1);
    state.selectedTaskId = tasks[0]?.id ?? null;
    renderAll();
    showToast("Task 已删除；仅保留 30 天防重放墓碑。", "info");
}

function openRunDialog(task, run) {
    document.querySelector("#run-dialog-title").textContent = `${task.id} · 运行 #${run.attempt}`;
    const transcript = run.events.map((event) => `[${formatClock(event.at)}] ${event.text}`).join("\n");
    document.querySelector("#run-dialog-content").innerHTML = `
        <div class="run-dialog-meta">
            <div><small>状态</small><strong>${escapeHtml(runStatusMeta[run.status])}</strong></div>
            <div><small>运行时长</small><strong>${escapeHtml(formatDuration(run.startedAt, run.finishedAt ?? Date.now()))}</strong></div>
            <div><small>Agent</small><strong>${escapeHtml(run.model)}</strong></div>
        </div>
        ${run.error ? `<div class="consent-note"><span aria-hidden="true">!</span><span>${escapeHtml(run.error)}</span></div>` : ""}
        ${run.result ? `<section class="content-section"><h3>结果摘要</h3><p>${escapeHtml(run.result)}</p></section>` : ""}
        <section class="content-section"><h3>事件与输出</h3><pre class="transcript">${escapeHtml(transcript || "尚无输出。")}</pre></section>`;
    runDetailDialog.showModal();
}

function selectedTask() {
    return tasks.find((task) => task.id === state.selectedTaskId) ?? null;
}

taskList.addEventListener("click", (event) => {
    const item = event.target.closest("[data-task-id]");
    if (!item) return;
    state.draftMode = false;
    state.selectedTaskId = item.dataset.taskId;
    renderAll();
});

taskDetail.addEventListener("click", (event) => {
    const actionTarget = event.target.closest("[data-action]");
    if (!actionTarget) return;
    const task = selectedTask();
    if (!task) return;

    const action = actionTarget.dataset.action;
    if (action === "start-run") {
        startRun(task);
    } else if (action === "stop-run") {
        stopRun(task, actionTarget.dataset.runId);
    } else if (action === "open-terminal") {
        state.terminalTaskId = task.id;
        task.manualHold = false;
        if (["closed", "suspended", "not_created"].includes(task.terminalState)) {
            task.terminalState = "ready";
            showToast("正在恢复 Codex thread 并创建新的 Surface generation。", "info");
        }
        showPage("terminal", "task-agent");
        renderAgentTerminal();
    } else if (action === "mark-done") {
        markDone(task);
    } else if (action === "close-task") {
        closeTask(task);
    } else if (action === "cancel-task") {
        cancelTask(task);
    } else if (action === "finalize-worktree") {
        finalizeWorktree(task);
    } else if (action === "delete-task") {
        deleteTask(task);
    } else if (action === "reopen") {
        task.status = "todo";
        task.updatedAt = Date.now();
        renderAll();
        showToast("任务已重新打开并回到待处理。", "info");
    } else if (action === "unblock") {
        task.status = "todo";
        task.blockedReason = "";
        task.updatedAt = Date.now();
        renderAll();
        showToast("阻塞已解除，任务回到待处理。", "success");
    } else if (action === "focus-followup") {
        if (task.manualHold) {
            task.manualHold = false;
            if (["closed", "suspended", "not_created"].includes(task.terminalState)) {
                task.terminalState = "ready";
            }
            renderAll();
            showToast("已恢复任务终端，可补充下一次运行要求。", "info");
        }
        document.querySelector("#followup-input")?.focus();
    } else if (action === "send-followup") {
        const input = document.querySelector("#followup-input");
        const message = input?.value.trim();
        if (!message) {
            input?.focus();
            showToast("请先补充要交给 Codex 的要求。", "warning");
            return;
        }
        const activeRun = getActiveRun(task);
        task.followups.push({ text: message, createdAt: Date.now() });
        input.value = "";
        task.updatedAt = Date.now();
        if (activeRun) {
            renderAll();
            showToast(isPendingRun(activeRun)
                ? "补充要求已持久化；Agent 启动前会并入当前 TaskRun。"
                : "补充要求已持久化；当前运行结束后会合并成一次跟进 TaskRun。", "success");
        } else {
            startRun(task, "Tasks 页补充要求");
        }
    } else if (action === "edit-followup") {
        const index = Number(actionTarget.dataset.followupIndex);
        const followup = task.followups[index];
        if (Number.isInteger(index) && followup) {
            const revised = window.prompt("编辑尚未被 TaskRun 领取的补充要求：", followup.text)?.trim();
            if (revised && revised !== followup.text) {
                followup.text = revised;
                followup.updatedAt = Date.now();
                task.updatedAt = Date.now();
                renderAll();
                showToast("补充要求已更新。", "success");
            }
        }
    } else if (action === "remove-followup") {
        const index = Number(actionTarget.dataset.followupIndex);
        if (Number.isInteger(index) && task.followups[index]) {
            task.followups.splice(index, 1);
            renderAll();
            showToast("已移除未领取的补充要求。", "info");
        }
    } else if (action === "cancel-draft") {
        state.draftMode = false;
        renderTaskDetail();
    } else if (action === "toggle-past") {
        task.pastExpanded = !task.pastExpanded;
        renderTaskDetail();
    } else if (action === "view-run") {
        const run = task.runs.find((candidate) => candidate.id === actionTarget.dataset.runId);
        if (run) openRunDialog(task, run);
    } else if (action === "retry-run") {
        const run = task.runs.find((candidate) => candidate.id === actionTarget.dataset.runId);
        if (run) startRun(task, `重试运行 #${run.attempt}：${run.trigger}`);
    }
});

taskDetail.addEventListener("submit", (event) => {
    if (event.target.id !== "inline-task-form") return;
    event.preventDefault();
    const form = event.target;
    if (!form.reportValidity()) return;
    const data = new FormData(form);
    const submitMode = event.submitter?.value ?? "save";
    const workspace = String(data.get("workspace"));
    const mode = String(data.get("mode"));
    const prefix = workspace === "win_terminal" ? "RWT" : workspace === "website" ? "WEB" : "API";
    const id = `${prefix}-${state.idSequence++}`;
    const cwd = workspace === "win_terminal"
        ? "F:\\RicherLife\\codes\\win_terminal"
        : `F:\\RicherLife\\codes\\${workspaceNames[workspace]}`;
    const acceptance = String(data.get("acceptance") ?? "")
        .split("\n")
        .map((line) => line.trim())
        .filter(Boolean)
        .map((text) => ({ text, done: false }));
    const task = {
        id,
        workspace,
        title: String(data.get("title")).trim(),
        description: String(data.get("description") ?? "").trim() || "尚未补充目标与背景。",
        acceptance,
        status: "todo",
        priority: "medium",
        createdAt: Date.now(),
        updatedAt: Date.now(),
        cwd,
        branch: mode === "worktree" ? `agent/codex/${id.toLowerCase()}` : "当前分支",
        executionMode: mode === "worktree" ? "Worktree · Task 专属" : "Direct · 托管串行",
        worktree: mode === "worktree" ? `F:\\RicherLife\\task-worktrees\\${id.toLowerCase()}` : "—",
        terminalState: "not_created",
        manualHold: false,
        followups: [],
        assignee: "Codex",
        result: "",
        pastExpanded: false,
        runs: [],
    };
    tasks.unshift(task);
    state.selectedTaskId = task.id;
    state.currentWorkspace = workspace;
    state.scope = "current";
    state.statusFilter = "open";
    state.draftMode = false;
    statusFilter.value = "open";
    updateScopeControls();
    renderAll();
    if (submitMode === "run") {
        task.terminalState = "starting";
        showToast("Task 已保存，正在快照当前状态并准备 Worktree。", "info");
        window.setTimeout(() => startRun(task, "创建任务后立即运行"), 700);
    } else {
        showToast(`${task.id} 已保存为 Todo；尚未创建 Worktree、TaskRun 或终端。`, "success");
    }
});

taskDetail.addEventListener("change", (event) => {
    const checkbox = event.target.closest("[data-acceptance-index]");
    if (!checkbox) return;
    const task = selectedTask();
    if (!task) return;
    const index = Number(checkbox.dataset.acceptanceIndex);
    if (!Number.isInteger(index) || !task.acceptance[index]) return;
    task.acceptance[index].done = checkbox.checked;
    task.updatedAt = Date.now();
    renderTaskList();
    const subtitle = taskDetail.querySelector(".detail-subtitle");
    if (subtitle) {
        const complete = task.acceptance.filter((item) => item.done).length;
        subtitle.lastElementChild.textContent = `${complete}/${task.acceptance.length} 项验收条件`;
    }
});

document.querySelector("#new-task-button").addEventListener("click", () => {
    state.draftMode = true;
    showPage("tasks");
    renderTaskDetail();
});

document.querySelectorAll("[data-close-run-dialog]").forEach((button) => {
    button.addEventListener("click", () => runDetailDialog.close());
});

function updateScopeControls() {
    document.querySelectorAll("[data-scope]").forEach((button) => {
        const selected = button.dataset.scope === state.scope;
        button.classList.toggle("selected", selected);
        button.setAttribute("aria-pressed", String(selected));
    });
}

document.querySelectorAll("[data-scope]").forEach((button) => {
    button.addEventListener("click", () => {
        state.scope = button.dataset.scope;
        updateScopeControls();
        renderAll();
    });
});

taskSearch.addEventListener("input", () => {
    state.search = taskSearch.value;
    renderAll();
});

statusFilter.addEventListener("change", () => {
    state.statusFilter = statusFilter.value;
    renderAll();
});

document.querySelector("#sort-button").addEventListener("click", () => {
    state.sortNewestFirst = !state.sortNewestFirst;
    document.querySelector("#sort-button").title = state.sortNewestFirst ? "按最近更新排序" : "按最早更新排序";
    renderTaskList();
});

document.querySelectorAll("[data-tool-page]").forEach((button) => {
    button.addEventListener("click", () => showPage(button.dataset.toolPage));
});

document.querySelectorAll("[data-return-tasks]").forEach((button) => {
    button.addEventListener("click", () => showPage("tasks"));
});

document.querySelector("[data-open-terminal-task]").addEventListener("click", () => {
    state.draftMode = false;
    state.selectedTaskId = state.terminalTaskId;
    showPage("tasks");
    renderAll();
});

document.querySelectorAll("[data-surface]").forEach((button) => {
    button.addEventListener("click", () => {
        if (button.dataset.surface === "task-agent") {
            const task = tasks.find((candidate) => candidate.id === state.terminalTaskId);
            if (task?.terminalState === "suspended") {
                task.terminalState = "ready";
            }
            if (task) task.manualHold = false;
        }
        showPage("terminal", button.dataset.surface);
    });
});

document.querySelector("#agent-terminal-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const input = document.querySelector("#agent-terminal-message");
    const message = input.value.trim();
    if (!message) return;
    const task = tasks.find((candidate) => candidate.id === state.terminalTaskId);
    if (!task || task.terminalState === "closed") {
        showToast("请先从 Tasks 页面打开 Agent 终端。", "warning");
        return;
    }
    const activeRun = getActiveRun(task);
    if (activeRun?.status === "running") {
        activeRun.events.push({ at: Date.now(), text: `turn/steer：${message}` });
        task.updatedAt = Date.now();
        showToast("实时输入已通过 turn/steer 发送。", "success");
        renderAgentTerminal();
    } else if (activeRun) {
        task.followups.push({ text: message, createdAt: Date.now() });
        task.updatedAt = Date.now();
        showToast("Agent 尚未启动；消息会在启动前并入当前 TaskRun。", "success");
        renderAll();
    } else {
        task.followups.push({ text: message, createdAt: Date.now() });
        startRun(task, "Agent 终端输入");
        showPage("terminal", "task-agent");
    }
    input.value = "";
});

document.querySelector("#agent-terminal-message").addEventListener("keydown", (event) => {
    if (event.key !== "Escape") return;
    const task = tasks.find((candidate) => candidate.id === state.terminalTaskId);
    const activeRun = task && getActiveRun(task);
    if (task && activeRun) {
        event.preventDefault();
        stopRun(task, activeRun.id);
        renderAgentTerminal();
    }
});

document.querySelector(".surface-new").addEventListener("click", () => {
    showToast("这里创建的是终端 Surface，不是 Workspace 或 Task。", "info");
});

document.querySelectorAll("[data-workspace]").forEach((button) => {
    button.addEventListener("click", () => {
        state.currentWorkspace = button.dataset.workspace;
        state.scope = "current";
        document.querySelectorAll("[data-workspace]").forEach((candidate) => candidate.classList.toggle("selected", candidate === button));
        document.querySelectorAll("[data-workspace] .workspace-state").forEach((indicator) => {
            indicator.hidden = indicator.closest("[data-workspace]") !== button;
        });
        updateScopeControls();
        showPage("tasks");
        renderAll();
        showToast(`Tasks 已切换到 ${workspaceNames[state.currentWorkspace]} 范围。`, "info");
    });
});

window.setInterval(() => {
    document.querySelectorAll("[data-run-elapsed]").forEach((element) => {
        const runId = element.dataset.runElapsed;
        const run = tasks.flatMap((task) => task.runs).find((candidate) => candidate.id === runId);
        if (run?.startedAt && run.status === "running") {
            element.textContent = formatDuration(run.startedAt);
        }
    });
}, 1000);

renderAll();
showPage("tasks");
