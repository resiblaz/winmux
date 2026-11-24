package com.winmux.mobile.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.ActiveTerminalTarget
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.ConnectionState
import com.winmux.mobile.model.PaneInfo
import com.winmux.mobile.model.WorkspaceInfo
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.launch

private enum class WorkspaceFilter { ALL, RUNNING, ATTENTION }

@Composable
fun WorkspacesScreen(
    snapshot: BridgeSnapshot,
    connectionState: ConnectionState,
    onRefresh: () -> Unit,
    onOpenTerminal: (ActiveTerminalTarget) -> Unit,
    onOpenNewTask: (workspaceId: String) -> Unit,
    onGoToPairing: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var filter by remember { mutableStateOf(WorkspaceFilter.ALL) }
    var searchQuery by remember { mutableStateOf("") }
    var showSearch by remember { mutableStateOf(false) }
    var pendingCloseWorkspace by remember { mutableStateOf<WorkspaceInfo?>(null) }
    var actionFeedback by remember { mutableStateOf<String?>(null) }
    val expandedOverrides = remember { mutableStateMapOf<String, Boolean>() }

    val (totalPanes, runningPanes, attentionPanes) = remember(snapshot.workspaces) {
        Triple(
            snapshot.workspaces.sumOf { it.panes.size },
            snapshot.workspaces.sumOf { ws -> ws.panes.count { it.hasActiveChild } },
            snapshot.workspaces.sumOf { ws -> ws.panes.count { it.hasAttention } },
        )
    }

    val queryLower = remember(searchQuery) { searchQuery.trim().lowercase() }
    val filteredWorkspaces = remember(snapshot.workspaces, filter, queryLower) {
        snapshot.workspaces.filter { ws ->
            val matchesFilter = when (filter) {
                WorkspaceFilter.ALL -> true
                WorkspaceFilter.RUNNING -> ws.hasRunning
                WorkspaceFilter.ATTENTION -> ws.hasAttention
            }
            val matchesQuery = queryLower.isEmpty() ||
                ws.title.lowercase().contains(queryLower) ||
                ws.workingDirectory.lowercase().contains(queryLower) ||
                ws.gitBranch.lowercase().contains(queryLower) ||
                ws.panes.any {
                    it.title.lowercase().contains(queryLower) ||
                        it.activeProcessName.lowercase().contains(queryLower) ||
                        it.agentKind.lowercase().contains(queryLower)
                }
            matchesFilter && matchesQuery
        }
    }

    Scaffold(
        containerColor = WinmuxColors.Bg,
        contentWindowInsets = WindowInsets(0, 0, 0, 0),
        floatingActionButton = {
            if (snapshot.connected) {
                FloatingActionButton(
                    onClick = { onOpenNewTask("new") },
                    containerColor = WinmuxColors.Accent,
                    contentColor = Color.White,
                ) {
                    Icon(Icons.Default.Add, contentDescription = "新建 Agent 任务")
                }
            }
        },
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 14.dp, vertical = 10.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("工作区", fontSize = 20.sp, fontWeight = FontWeight.Bold, color = WinmuxColors.TextPrimary)
                    val subtitle = when (connectionState) {
                        is ConnectionState.Connected ->
                            if (snapshot.connected) "已连接 (${connectionState.endpointLabel}) · $totalPanes 个终端"
                            else "Bridge 已连，等待桌面 Winmux 启动"
                        is ConnectionState.Connecting -> connectionState.target
                        is ConnectionState.Error -> connectionState.message
                        ConnectionState.Disconnected -> "未配对电脑"
                    }
                    Text(subtitle, fontSize = 12.sp, color = WinmuxColors.TextSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                if (snapshot.connected) {
                    IconButton(
                        onClick = {
                            showSearch = !showSearch
                            if (!showSearch) searchQuery = ""
                        },
                    ) {
                        Icon(
                            if (showSearch) Icons.Default.Close else Icons.Default.Search,
                            contentDescription = "搜索",
                            tint = if (showSearch) WinmuxColors.Accent else WinmuxColors.TextSecondary,
                        )
                    }
                }
                IconButton(onClick = onRefresh) {
                    Icon(Icons.Default.Refresh, contentDescription = "刷新", tint = WinmuxColors.TextSecondary)
                }
            }

            if (connectionState is ConnectionState.Disconnected) {
                Spacer(Modifier.height(24.dp))
                Card(
                    colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
                    border = BorderStroke(1.dp, WinmuxColors.Border),
                    modifier = Modifier.fillMaxWidth().clickable(onClick = onGoToPairing),
                ) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text("尚未连接电脑", fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary)
                        Text("点击前往「连接」页输入配对码，即可远程查看与控制 Winmux 终端。", color = WinmuxColors.TextSecondary, fontSize = 13.sp)
                    }
                }
                return@Column
            }

            if (showSearch) {
                OutlinedTextField(
                    value = searchQuery,
                    onValueChange = { searchQuery = it },
                    placeholder = { Text("搜索工作区、分支、目录或 Agent...", fontSize = 12.5.sp) },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
                )
            }

            if (actionFeedback != null) {
                Text(
                    text = actionFeedback!!,
                    color = WinmuxColors.Accent,
                    fontSize = 11.5.sp,
                    modifier = Modifier.padding(top = 4.dp),
                )
            }

            // Compact host status strip
            val sys = snapshot.systemStatus
            if (sys.memoryTotalMb > 0) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 6.dp)
                        .clip(RoundedCornerShape(8.dp))
                        .background(WinmuxColors.Surface)
                        .padding(horizontal = 10.dp, vertical = 6.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = "CPU ${"%.0f".format(sys.cpuPercent)}%  ·  RAM ${"%.1f".format(sys.memoryUsedMb / 1024.0)}/${"%.0f".format(sys.memoryTotalMb / 1024.0)}G",
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp,
                        color = WinmuxColors.TextSecondary,
                    )
                    Text(
                        text = "运行 ${sys.workingPanes} · 待审 ${sys.pendingDecisions}",
                        fontSize = 11.sp,
                        color = if (sys.pendingDecisions > 0) WinmuxColors.Attention else WinmuxColors.Running,
                        fontWeight = FontWeight.Medium,
                    )
                }
            }

            // Filter Pills
            Row(
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                FilterChip(
                    selected = filter == WorkspaceFilter.ALL,
                    onClick = { filter = WorkspaceFilter.ALL },
                    label = { Text("全部 $totalPanes", fontSize = 12.sp) },
                )
                FilterChip(
                    selected = filter == WorkspaceFilter.RUNNING,
                    onClick = { filter = WorkspaceFilter.RUNNING },
                    label = { Text("运行中 $runningPanes", fontSize = 12.sp) },
                )
                FilterChip(
                    selected = filter == WorkspaceFilter.ATTENTION,
                    onClick = { filter = WorkspaceFilter.ATTENTION },
                    label = { Text("待处理 $attentionPanes", fontSize = 12.sp) },
                )
            }

            if (filteredWorkspaces.isEmpty()) {
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        if (!snapshot.connected) "电脑端 Winmux 未启动或未开启侧边栏" else "当前筛选下暂无工作区",
                        color = WinmuxColors.TextSecondary,
                        fontSize = 13.sp,
                    )
                }
            } else {
                LazyColumn(
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                    modifier = Modifier.fillMaxSize(),
                ) {
                    items(filteredWorkspaces, key = { it.id }) { ws ->
                        val isExpanded = expandedOverrides[ws.id] ?: (ws.selected || filter != WorkspaceFilter.ALL || queryLower.isNotEmpty())
                        WorkspaceCard(
                            workspace = ws,
                            expanded = isExpanded,
                            onToggleExpand = { expandedOverrides[ws.id] = !isExpanded },
                            onSelectPane = { pane ->
                                onOpenTerminal(
                                    ActiveTerminalTarget(
                                        workspaceId = ws.id,
                                        workspaceTitle = ws.title,
                                        surfaceId = pane.surfaceId,
                                        paneId = pane.id,
                                        paneTitle = pane.title,
                                        agentKind = pane.agentKind,
                                    )
                                )
                            },
                            onFocusWorkspaceOnPc = {
                                scope.launch {
                                    runCatching {
                                        WinmuxRepository.apiClient.selectWorkspace(WinmuxRepository.config.value, ws.id)
                                    }.onSuccess {
                                        WinmuxRepository.refreshSnapshot()
                                        actionFeedback = "已在电脑端切换到「${ws.title}」"
                                    }.onFailure { err ->
                                        actionFeedback = err.message ?: "切换失败"
                                    }
                                }
                            },
                            onNewTaskHere = { onOpenNewTask(ws.id) },
                            onSplitActivePane = { direction ->
                                val targetPane = ws.panes.firstOrNull { it.active } ?: ws.panes.firstOrNull()
                                if (targetPane != null) {
                                    scope.launch {
                                        runCatching {
                                            WinmuxRepository.apiClient.splitPane(
                                                WinmuxRepository.config.value,
                                                ws.id,
                                                targetPane.surfaceId,
                                                targetPane.id,
                                                direction,
                                            )
                                        }.onSuccess {
                                            WinmuxRepository.refreshSnapshot()
                                            actionFeedback = "已在「${ws.title}」中拆分新窗格"
                                        }.onFailure { err ->
                                            actionFeedback = err.message ?: "分屏失败"
                                        }
                                    }
                                }
                            },
                            onRequestClose = { pendingCloseWorkspace = ws },
                        )
                    }
                    item { Spacer(Modifier.height(72.dp)) }
                }
            }
        }
    }

    if (pendingCloseWorkspace != null) {
        val targetWs = pendingCloseWorkspace!!
        AlertDialog(
            onDismissRequest = { pendingCloseWorkspace = null },
            containerColor = WinmuxColors.Surface,
            title = { Text("关闭工作区？", color = WinmuxColors.TextPrimary) },
            text = {
                Text(
                    "确定要关闭电脑端工作区「${targetWs.title}」（包含 ${targetWs.panes.size} 个终端）吗？未保存的进程将会终止。",
                    color = WinmuxColors.TextSecondary,
                    fontSize = 13.sp,
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        pendingCloseWorkspace = null
                        scope.launch {
                            runCatching {
                                WinmuxRepository.apiClient.closeWorkspace(WinmuxRepository.config.value, targetWs.id)
                            }.onSuccess {
                                WinmuxRepository.refreshSnapshot()
                                actionFeedback = "已关闭工作区「${targetWs.title}」"
                            }.onFailure { err ->
                                actionFeedback = err.message ?: "关闭失败"
                            }
                        }
                    },
                ) {
                    Text("确认关闭", color = WinmuxColors.Danger)
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingCloseWorkspace = null }) {
                    Text("取消", color = WinmuxColors.TextSecondary)
                }
            },
        )
    }
}

@Composable
private fun WorkspaceCard(
    workspace: WorkspaceInfo,
    expanded: Boolean,
    onToggleExpand: () -> Unit,
    onSelectPane: (PaneInfo) -> Unit,
    onFocusWorkspaceOnPc: () -> Unit,
    onNewTaskHere: () -> Unit,
    onSplitActivePane: (direction: String) -> Unit,
    onRequestClose: () -> Unit,
) {
    var menuExpanded by remember { mutableStateOf(false) }

    Card(
        colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
        border = BorderStroke(
            width = if (workspace.selected) 1.2.dp else 1.dp,
            color = if (workspace.selected) WinmuxColors.Accent.copy(alpha = 0.65f) else WinmuxColors.Border,
        ),
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onToggleExpand)
                    .padding(start = 12.dp, end = 4.dp, top = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    imageVector = if (expanded) Icons.Default.KeyboardArrowDown else Icons.AutoMirrored.Filled.KeyboardArrowRight,
                    contentDescription = null,
                    tint = WinmuxColors.TextSecondary,
                    modifier = Modifier.size(18.dp),
                )
                Spacer(Modifier.width(6.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(
                            text = workspace.title,
                            fontWeight = FontWeight.SemiBold,
                            fontSize = 14.sp,
                            color = WinmuxColors.TextPrimary,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                        if (workspace.unreadCount > 0) {
                            Box(
                                modifier = Modifier
                                    .clip(CircleShape)
                                    .background(WinmuxColors.Attention)
                                    .padding(horizontal = 6.dp, vertical = 1.dp),
                            ) {
                                Text("${workspace.unreadCount}", fontSize = 10.sp, fontWeight = FontWeight.Bold, color = Color.Black)
                            }
                        }
                    }
                    val folder = workspace.workingDirectory.substringAfterLast('\\').substringAfterLast('/')
                    val meta = buildList {
                        if (folder.isNotBlank()) add(folder)
                        if (workspace.gitBranch.isNotBlank()) add("⎇ ${workspace.gitBranch}")
                        if (workspace.listeningPorts.isNotEmpty()) add(workspace.listeningPorts.joinToString(" ") { ":$it" })
                    }.joinToString("  ")
                    if (meta.isNotBlank()) {
                        Text(
                            text = meta,
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace,
                            color = if (workspace.listeningPorts.isNotEmpty()) WinmuxColors.Running else WinmuxColors.TextSecondary,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Text(
                    text = "${workspace.panes.size}",
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    color = WinmuxColors.TextSecondary,
                )
                Box {
                    IconButton(onClick = { menuExpanded = true }, modifier = Modifier.size(32.dp)) {
                        Icon(Icons.Default.MoreVert, contentDescription = "工作区操作", tint = WinmuxColors.TextSecondary, modifier = Modifier.size(18.dp))
                    }
                    DropdownMenu(
                        expanded = menuExpanded,
                        onDismissRequest = { menuExpanded = false },
                        containerColor = WinmuxColors.SurfaceElevated,
                    ) {
                        DropdownMenuItem(
                            text = { Text("在电脑端切换至此工作区", color = WinmuxColors.TextPrimary, fontSize = 13.sp) },
                            onClick = { menuExpanded = false; onFocusWorkspaceOnPc() },
                        )
                        DropdownMenuItem(
                            text = { Text("在此工作区启动 Agent 任务", color = WinmuxColors.TextPrimary, fontSize = 13.sp) },
                            onClick = { menuExpanded = false; onNewTaskHere() },
                        )
                        if (workspace.panes.isNotEmpty()) {
                            DropdownMenuItem(
                                text = { Text("向右拆分窗格", color = WinmuxColors.TextPrimary, fontSize = 13.sp) },
                                onClick = { menuExpanded = false; onSplitActivePane("right") },
                            )
                            DropdownMenuItem(
                                text = { Text("向下拆分窗格", color = WinmuxColors.TextPrimary, fontSize = 13.sp) },
                                onClick = { menuExpanded = false; onSplitActivePane("down") },
                            )
                        }
                        HorizontalDivider(color = WinmuxColors.Border, thickness = 0.5.dp)
                        DropdownMenuItem(
                            text = { Text("关闭工作区", color = WinmuxColors.Danger, fontSize = 13.sp) },
                            onClick = { menuExpanded = false; onRequestClose() },
                        )
                    }
                }
            }

            if (expanded && workspace.panes.isNotEmpty()) {
                HorizontalDivider(color = WinmuxColors.Border, thickness = 0.5.dp)
                workspace.panes.forEach { pane ->
                    PaneRow(pane = pane, onClick = { onSelectPane(pane) })
                }
            }
        }
    }
}

@Composable
private fun PaneRow(
    pane: PaneInfo,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        AgentBadgeIcon(
            agentKind = pane.agentKind,
            isRunning = pane.hasActiveChild,
            hasAttention = pane.hasAttention,
            size = 24.dp,
        )
        Spacer(Modifier.width(10.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = pane.title,
                fontSize = 13.sp,
                fontWeight = if (pane.active) FontWeight.SemiBold else FontWeight.Normal,
                color = WinmuxColors.TextPrimary,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            when {
                pane.runtimeState == "waiting" -> {
                    Text(
                        text = "等待处理${pane.activeProcessName.takeIf(String::isNotBlank)?.let { " · $it" }.orEmpty()}",
                        fontSize = 11.sp,
                        color = WinmuxColors.Attention,
                        maxLines = 1,
                    )
                }
                pane.hasActiveChild && pane.activeProcessName.isNotBlank() -> {
                    val elapsedSec = pane.runningSinceMs?.let { ((System.currentTimeMillis() - it) / 1000L).coerceAtLeast(0L) }
                    val durationSuffix = elapsedSec?.takeIf { it >= 5L }?.let { sec ->
                        if (sec >= 60L) " · ${sec / 60}m ${sec % 60}s" else " · ${sec}s"
                    }.orEmpty()
                    Text(
                        text = "运行中 · ${pane.activeProcessName}$durationSuffix",
                        fontSize = 11.sp,
                        color = WinmuxColors.Running,
                        maxLines = 1,
                    )
                }
                pane.connectionClosed -> {
                    Text("连接已关闭", fontSize = 11.sp, color = WinmuxColors.Danger)
                }
            }
        }
        if (pane.active) {
            Box(
                modifier = Modifier
                    .clip(RoundedCornerShape(4.dp))
                    .background(WinmuxColors.Accent.copy(alpha = 0.2f))
                    .padding(horizontal = 6.dp, vertical = 2.dp),
            ) {
                Text("当前", fontSize = 10.sp, color = WinmuxColors.Accent)
            }
        }
    }
}
