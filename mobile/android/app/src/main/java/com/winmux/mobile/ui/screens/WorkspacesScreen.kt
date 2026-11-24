package com.winmux.mobile.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
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
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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
import com.winmux.mobile.model.ActiveTerminalTarget
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.ConnectionState
import com.winmux.mobile.model.PaneInfo
import com.winmux.mobile.model.WorkspaceInfo
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.theme.WinmuxColors

private enum class WorkspaceFilter { ALL, RUNNING, ATTENTION }

@Composable
fun WorkspacesScreen(
    snapshot: BridgeSnapshot,
    connectionState: ConnectionState,
    onRefresh: () -> Unit,
    onOpenTerminal: (ActiveTerminalTarget) -> Unit,
    onOpenNewTask: () -> Unit,
    onGoToPairing: () -> Unit,
) {
    var filter by remember { mutableStateOf(WorkspaceFilter.ALL) }
    val expandedOverrides = remember { mutableStateMapOf<String, Boolean>() }

    val totalPanes = snapshot.workspaces.sumOf { it.panes.size }
    val runningPanes = snapshot.workspaces.sumOf { ws -> ws.panes.count { it.hasActiveChild } }
    val attentionPanes = snapshot.workspaces.sumOf { ws -> ws.panes.count { it.hasAttention } }

    val filteredWorkspaces = snapshot.workspaces.filter { ws ->
        when (filter) {
            WorkspaceFilter.ALL -> true
            WorkspaceFilter.RUNNING -> ws.hasRunning
            WorkspaceFilter.ATTENTION -> ws.hasAttention
        }
    }

    Scaffold(
        containerColor = WinmuxColors.Bg,
        floatingActionButton = {
            if (snapshot.connected) {
                FloatingActionButton(
                    onClick = onOpenNewTask,
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
                        val isExpanded = expandedOverrides[ws.id] ?: (ws.selected || filter != WorkspaceFilter.ALL)
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
                        )
                    }
                    item { Spacer(Modifier.height(72.dp)) }
                }
            }
        }
    }
}

@Composable
private fun WorkspaceCard(
    workspace: WorkspaceInfo,
    expanded: Boolean,
    onToggleExpand: () -> Unit,
    onSelectPane: (PaneInfo) -> Unit,
) {
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
                    .padding(horizontal = 12.dp, vertical = 10.dp),
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
