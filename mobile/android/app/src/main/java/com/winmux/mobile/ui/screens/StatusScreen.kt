package com.winmux.mobile.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.AgentModelConfig
import com.winmux.mobile.model.AgentUsageInfo
import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.AllUsageSummary
import com.winmux.mobile.model.ConnectionState
import com.winmux.mobile.model.SystemStatusInfo
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.launch

@Composable
fun StatusScreen(
    systemStatus: SystemStatusInfo,
    usage: AllUsageSummary,
    models: AllModelsConfig,
    connectionState: ConnectionState,
    onRefresh: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var feedback by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(connectionState) {
        if (connectionState is ConnectionState.Connected) {
            onRefresh()
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text("运行状态 · Usage · 模型", fontSize = 19.sp, fontWeight = FontWeight.Bold, color = WinmuxColors.TextPrimary)
                Text(
                    if (systemStatus.hostname.isNotBlank()) "本机 ${systemStatus.hostname} · Bridge 在线 ${formatDuration(systemStatus.bridgeUptimeSec)}"
                    else "电脑资源、Token 用量与默认模型配置",
                    fontSize = 12.sp,
                    color = WinmuxColors.TextSecondary,
                )
            }
            IconButton(onClick = onRefresh) {
                Icon(Icons.Default.Refresh, contentDescription = "刷新", tint = WinmuxColors.TextSecondary)
            }
        }

        if (feedback != null) {
            Card(
                colors = CardDefaults.cardColors(containerColor = WinmuxColors.Running.copy(alpha = 0.14f)),
                border = BorderStroke(1.dp, WinmuxColors.Running.copy(alpha = 0.45f)),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(feedback!!, color = WinmuxColors.Running, fontSize = 12.sp, modifier = Modifier.padding(10.dp))
            }
        }

        // 1. Host Runtime Status Card
        HostRuntimeCard(systemStatus = systemStatus)

        // 2. Agent Usage & Rate Limits Card
        Text("Agent Usage 统计", fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary)
        UsageAgentCard(title = "Claude Code", usage = usage.claude)
        UsageAgentCard(title = "Codex CLI", usage = usage.codex)
        UsageAgentCard(title = "Antigravity (AGY)", usage = usage.agy)

        // 3. Default Model Switchers
        Text("默认模型切换（全局配置）", fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary)
        ModelSwitcherCard(
            title = "Claude Code",
            config = models.claude,
            onApply = { newModel, effort ->
                scope.launch {
                    WinmuxRepository.updateDefaultModel("claude", newModel, effort)
                        .onSuccess { feedback = "已切换 Claude 默认模型为：$newModel" }
                        .onFailure { feedback = "切换失败：${it.message}" }
                }
            },
        )
        ModelSwitcherCard(
            title = "Codex CLI",
            config = models.codex,
            onApply = { newModel, effort ->
                scope.launch {
                    WinmuxRepository.updateDefaultModel("codex", newModel, effort)
                        .onSuccess {
                            val effortText = effort?.takeIf(String::isNotBlank)?.let { " (推理:$it)" }.orEmpty()
                            feedback = "已切换 Codex 默认模型为：$newModel$effortText"
                        }
                        .onFailure { feedback = "切换失败：${it.message}" }
                }
            },
        )
        ModelSwitcherCard(
            title = "Antigravity (AGY)",
            config = models.agy,
            onApply = { newModel, effort ->
                scope.launch {
                    WinmuxRepository.updateDefaultModel("agy", newModel, effort)
                        .onSuccess { feedback = "已切换 AGY 默认模型为：$newModel" }
                        .onFailure { feedback = "切换失败：${it.message}" }
                }
            },
        )

        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun HostRuntimeCard(systemStatus: SystemStatusInfo) {
    Card(
        colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
        border = BorderStroke(1.dp, WinmuxColors.Border),
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("电脑运行状态", fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary, fontSize = 14.sp)
                Text(
                    if (systemStatus.winmuxConnected) "Winmux 在线" else "Winmux 未连接",
                    color = if (systemStatus.winmuxConnected) WinmuxColors.Running else WinmuxColors.Danger,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold,
                )
            }

            // CPU Bar
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("CPU (${systemStatus.cpuCores} 线程)", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                    Text("${"%.1f".format(systemStatus.cpuPercent)}%", fontSize = 12.sp, fontFamily = FontFamily.Monospace, color = WinmuxColors.TextPrimary)
                }
                LinearProgressIndicator(
                    progress = { (systemStatus.cpuPercent / 100.0).toFloat().coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().height(6.dp),
                    color = if (systemStatus.cpuPercent > 80) WinmuxColors.Danger else WinmuxColors.Accent,
                    trackColor = WinmuxColors.Bg,
                )
            }

            // RAM Bar
            val usedGb = systemStatus.memoryUsedMb / 1024.0
            val totalGb = systemStatus.memoryTotalMb / 1024.0
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("内存", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                    Text(
                        "${"%.1f".format(usedGb)} / ${"%.1f".format(totalGb)} GB (${"%.0f".format(systemStatus.memoryPercent)}%)",
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace,
                        color = WinmuxColors.TextPrimary,
                    )
                }
                LinearProgressIndicator(
                    progress = { (systemStatus.memoryPercent / 100.0).toFloat().coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth().height(6.dp),
                    color = if (systemStatus.memoryPercent > 85) WinmuxColors.Attention else WinmuxColors.Running,
                    trackColor = WinmuxColors.Bg,
                )
            }

            HorizontalDivider(color = WinmuxColors.Border, thickness = 0.5.dp)

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                StatPill("运行中", "${systemStatus.workingPanes}", WinmuxColors.Running)
                StatPill("待处理", "${systemStatus.waitingPanes}", WinmuxColors.Attention)
                StatPill("空闲", "${systemStatus.idlePanes}", WinmuxColors.TextSecondary)
                StatPill("总终端", "${systemStatus.totalPanes}", WinmuxColors.TextPrimary)
            }
        }
    }
}

@Composable
private fun StatPill(label: String, value: String, color: androidx.compose.ui.graphics.Color) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(value, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold, fontSize = 16.sp, color = color)
        Text(label, fontSize = 11.sp, color = WinmuxColors.TextSecondary)
    }
}

@Composable
private fun UsageAgentCard(title: String, usage: AgentUsageInfo) {
    Card(
        colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
        border = BorderStroke(1.dp, WinmuxColors.Border),
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AgentBadgeIcon(agentKind = usage.agent, size = 22.dp)
                Column(modifier = Modifier.weight(1f)) {
                    Text(title, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, color = WinmuxColors.TextPrimary)
                    if (usage.currentModel.isNotBlank()) {
                        val effort = usage.reasoningEffort.takeIf(String::isNotBlank)?.let { " · $it" }.orEmpty()
                        Text("当前模型：${usage.currentModel}$effort", fontSize = 11.sp, color = WinmuxColors.TextSecondary, fontFamily = FontFamily.Monospace)
                    }
                }
            }

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                if (usage.agent == "agy") {
                    StatPill("今日交互", "${usage.today.turnsOrSessions}", WinmuxColors.Agy)
                    StatPill("近期记录", "${usage.week.turnsOrSessions}", WinmuxColors.TextPrimary)
                } else {
                    StatPill("今日 Tokens", formatTokens(usage.today.totalTokens), WinmuxColors.Accent)
                    StatPill("7天 Tokens", formatTokens(usage.week.totalTokens), WinmuxColors.TextPrimary)
                    StatPill("最近会话", formatTokens(usage.latestSessionTokens), WinmuxColors.Running)
                }
            }

            usage.primaryLimit?.let { limit ->
                Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("短周期配额 (${limit.windowMinutes / 60}h)", fontSize = 11.sp, color = WinmuxColors.TextSecondary)
                        Text("${"%.0f".format(limit.usedPercent)}%", fontSize = 11.sp, fontFamily = FontFamily.Monospace, color = WinmuxColors.TextPrimary)
                    }
                    LinearProgressIndicator(
                        progress = { (limit.usedPercent / 100.0).toFloat().coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth().height(5.dp),
                        color = if (limit.usedPercent > 80) WinmuxColors.Attention else WinmuxColors.Codex,
                        trackColor = WinmuxColors.Bg,
                    )
                }
            }
            usage.secondaryLimit?.let { limit ->
                Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text("周配额", fontSize = 11.sp, color = WinmuxColors.TextSecondary)
                        Text("${"%.0f".format(limit.usedPercent)}%", fontSize = 11.sp, fontFamily = FontFamily.Monospace, color = WinmuxColors.TextPrimary)
                    }
                    LinearProgressIndicator(
                        progress = { (limit.usedPercent / 100.0).toFloat().coerceIn(0f, 1f) },
                        modifier = Modifier.fillMaxWidth().height(5.dp),
                        color = if (limit.usedPercent > 80) WinmuxColors.Danger else WinmuxColors.Accent,
                        trackColor = WinmuxColors.Bg,
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ModelSwitcherCard(
    title: String,
    config: AgentModelConfig,
    onApply: (model: String, reasoningEffort: String?) -> Unit,
) {
    var selectedModel by remember(config.currentModel) { mutableStateOf(config.currentModel) }
    var customModel by remember(config.currentModel) { mutableStateOf(config.currentModel) }
    var selectedEffort by remember(config.reasoningEffort) { mutableStateOf(config.reasoningEffort) }

    Card(
        colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
        border = BorderStroke(1.dp, WinmuxColors.Border),
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AgentBadgeIcon(agentKind = config.agent, size = 20.dp)
                Text(title, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, color = WinmuxColors.TextPrimary)
            }

            if (config.availableModels.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    config.availableModels.forEach { m ->
                        FilterChip(
                            selected = selectedModel == m,
                            onClick = {
                                selectedModel = m
                                customModel = m
                            },
                            label = { Text(m, fontSize = 11.5.sp, fontFamily = FontFamily.Monospace) },
                        )
                    }
                }
            }

            if (config.availableReasoningEfforts.isNotEmpty()) {
                Text("推理强度", fontSize = 11.sp, color = WinmuxColors.TextSecondary)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    config.availableReasoningEfforts.forEach { effort ->
                        FilterChip(
                            selected = selectedEffort == effort,
                            onClick = { selectedEffort = effort },
                            label = { Text(effort, fontSize = 11.sp) },
                        )
                    }
                }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                OutlinedTextField(
                    value = customModel,
                    onValueChange = {
                        customModel = it
                        selectedModel = it
                    },
                    label = { Text("自定义模型名", fontSize = 11.sp) },
                    singleLine = true,
                    modifier = Modifier.weight(1f),
                )
                Button(
                    enabled = customModel.isNotBlank(),
                    onClick = { onApply(customModel.trim(), selectedEffort.takeIf(String::isNotBlank)) },
                ) {
                    Text("应用")
                }
            }
        }
    }
}

private fun formatTokens(count: Long): String = when {
    count >= 1_000_000L -> "${"%.2f".format(count / 1_000_000.0)}M"
    count >= 1_000L -> "${"%.1f".format(count / 1_000.0)}K"
    else -> "$count"
}

private fun formatDuration(totalSec: Long): String {
    if (totalSec <= 0L) return "0s"
    val h = totalSec / 3600
    val m = (totalSec % 3600) / 60
    val s = totalSec % 60
    return when {
        h > 0 -> "${h}h ${m}m"
        m > 0 -> "${m}m ${s}s"
        else -> "${s}s"
    }
}
