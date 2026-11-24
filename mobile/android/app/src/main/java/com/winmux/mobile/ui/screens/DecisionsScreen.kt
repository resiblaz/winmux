package com.winmux.mobile.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.ActiveTerminalTarget
import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.DecisionCard
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.components.AnsiTerminalParser
import com.winmux.mobile.ui.components.DecisionRiskAnalyzer
import com.winmux.mobile.ui.components.DecisionRiskLevel
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.launch

private enum class DecisionFilter { ALL, PENDING, HISTORY }

@Composable
fun DecisionsScreen(
    decisions: List<DecisionCard>,
    onResolve: (decisionId: String, choice: String, answers: List<String>, sendText: String?) -> Unit,
    onOpenTerminal: (ActiveTerminalTarget) -> Unit,
    onOpenNewTask: () -> Unit,
) {
    var filter by remember { mutableStateOf(DecisionFilter.ALL) }
    val pendingCount = decisions.count { it.status == "pending" }
    val historyCount = decisions.size - pendingCount
    val filtered = remember(decisions, filter) {
        when (filter) {
            DecisionFilter.ALL -> decisions
            DecisionFilter.PENDING -> decisions.filter { it.status == "pending" }
            DecisionFilter.HISTORY -> decisions.filter { it.status != "pending" }
        }
    }

    Scaffold(
        containerColor = WinmuxColors.Bg,
        floatingActionButton = {
            FloatingActionButton(
                onClick = onOpenNewTask,
                containerColor = WinmuxColors.Accent,
                contentColor = Color.White,
            ) {
                Icon(Icons.Default.Add, contentDescription = "新建任务")
            }
        },
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 14.dp, vertical = 10.dp),
        ) {
            Text("Agent 决策卡片", fontSize = 20.sp, fontWeight = FontWeight.Bold, color = WinmuxColors.TextPrimary)
            Text(
                if (pendingCount > 0) "有 $pendingCount 项等待你确认（锁屏通知栏也可直接点按钮）" else "当前没有待审批项",
                fontSize = 12.sp,
                color = if (pendingCount > 0) WinmuxColors.Attention else WinmuxColors.TextSecondary,
            )

            Row(
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                FilterChip(
                    selected = filter == DecisionFilter.ALL,
                    onClick = { filter = DecisionFilter.ALL },
                    label = { Text("全部 ${decisions.size}", fontSize = 12.sp) },
                )
                FilterChip(
                    selected = filter == DecisionFilter.PENDING,
                    onClick = { filter = DecisionFilter.PENDING },
                    label = { Text("待审批 $pendingCount", fontSize = 12.sp) },
                )
                FilterChip(
                    selected = filter == DecisionFilter.HISTORY,
                    onClick = { filter = DecisionFilter.HISTORY },
                    label = { Text("已处理 $historyCount", fontSize = 12.sp) },
                )
            }

            if (filtered.isEmpty()) {
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        if (decisions.isEmpty()) {
                            "当 Claude / Codex 请求权限、提交计划或提问时，\n交互卡片会实时出现在这里并同步弹窗提醒。"
                        } else {
                            "当前筛选下暂无审批记录"
                        },
                        color = WinmuxColors.TextSecondary,
                        fontSize = 13.sp,
                        lineHeight = 20.sp,
                    )
                }
            } else {
                LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    items(filtered, key = { it.id }) { card ->
                        DecisionCardView(
                            card = card,
                            onResolve = onResolve,
                            onOpenTerminal = onOpenTerminal,
                        )
                    }
                    item { Spacer(Modifier.height(72.dp)) }
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun DecisionCardView(
    card: DecisionCard,
    onResolve: (decisionId: String, choice: String, answers: List<String>, sendText: String?) -> Unit,
    onOpenTerminal: (ActiveTerminalTarget) -> Unit,
) {
    val risk = remember(card.id, card.toolName, card.toolInput, card.summary) {
        DecisionRiskAnalyzer.assess(card)
    }
    val riskColor = when (risk.level) {
        DecisionRiskLevel.HIGH -> WinmuxColors.Danger
        DecisionRiskLevel.MEDIUM -> WinmuxColors.Attention
        DecisionRiskLevel.LOW -> WinmuxColors.Running
    }
    val accentColor = when {
        card.status == "resolved" && card.resolution?.color == "green" -> WinmuxColors.Running
        card.status == "resolved" && card.resolution?.color == "red" -> WinmuxColors.Danger
        card.status != "pending" -> WinmuxColors.TextSecondary
        risk.level == DecisionRiskLevel.HIGH -> WinmuxColors.Danger
        card.kind == "exitPlan" -> WinmuxColors.Accent
        card.kind == "question" -> WinmuxColors.Agy
        else -> WinmuxColors.Attention
    }
    var showCustomReply by remember(card.id) { mutableStateOf(false) }
    var customReply by remember(card.id) { mutableStateOf("") }

    Card(
        colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
        border = BorderStroke(if (card.status == "pending" && risk.level == DecisionRiskLevel.HIGH) 1.5.dp else 1.dp, accentColor.copy(alpha = 0.65f)),
        shape = RoundedCornerShape(14.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AgentBadgeIcon(agentKind = card.agent, isRunning = card.status == "pending", size = 22.dp)
                Column(modifier = Modifier.weight(1f)) {
                    Text(card.title, fontWeight = FontWeight.SemiBold, fontSize = 15.sp, color = WinmuxColors.TextPrimary)
                    val metaLine = buildList {
                        if (card.workspaceTitle.isNotBlank()) add(card.workspaceTitle)
                        val rel = DecisionRiskAnalyzer.formatRelativeTime(card.createdAt)
                        if (rel.isNotBlank()) add(rel)
                    }.joinToString(" · ")
                    if (metaLine.isNotBlank()) {
                        Text(metaLine, fontSize = 11.sp, color = WinmuxColors.TextSecondary)
                    }
                }
                // Risk pill
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(riskColor.copy(alpha = 0.16f))
                        .padding(horizontal = 7.dp, vertical = 3.dp),
                ) {
                    Text(risk.label, fontSize = 10.5.sp, color = riskColor, fontWeight = FontWeight.SemiBold)
                }
                val badgeText = when (card.status) {
                    "pending" -> "待处理"
                    "resolved" -> card.resolution?.label?.take(8) ?: "已处理"
                    else -> "已过期"
                }
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(accentColor.copy(alpha = 0.18f))
                        .padding(horizontal = 8.dp, vertical = 3.dp),
                ) {
                    Text(badgeText, fontSize = 11.sp, color = accentColor, fontWeight = FontWeight.SemiBold)
                }
            }

            if (risk.level == DecisionRiskLevel.HIGH && risk.reason.isNotBlank()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(WinmuxColors.Danger.copy(alpha = 0.15f))
                        .padding(horizontal = 10.dp, vertical = 7.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    Icon(Icons.Default.Warning, contentDescription = null, tint = WinmuxColors.Danger, modifier = Modifier.size(16.dp))
                    Text(
                        text = "高危预警：${risk.reason}，请仔细确认后再放行。",
                        color = WinmuxColors.Danger,
                        fontSize = 11.5.sp,
                        fontWeight = FontWeight.Medium,
                    )
                }
            }

            if (card.summary.isNotBlank()) {
                Text(card.summary, fontSize = 13.sp, color = WinmuxColors.TextPrimary, lineHeight = 19.sp)
            }

            val codeBlock = when {
                card.toolInput.isNotBlank() -> card.toolInput
                card.planText.isNotBlank() -> card.planText
                else -> ""
            }
            if (codeBlock.isNotBlank()) {
                val styledCode = remember(codeBlock) { AnsiTerminalParser.parse(codeBlock.take(1500)) }
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(WinmuxColors.Bg)
                        .padding(10.dp),
                ) {
                    Text(
                        text = styledCode,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.5.sp,
                        lineHeight = 16.sp,
                    )
                }
            }

            // Question UI
            if (card.kind == "question" && card.questions.isNotEmpty() && card.status == "pending") {
                val selections = remember(card.id) { mutableStateMapOf<Int, MutableSet<String>>() }
                card.questions.forEachIndexed { qi, q ->
                    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(
                            text = if (q.header.isNotBlank()) "${q.header} · ${q.prompt}" else q.prompt,
                            fontWeight = FontWeight.Medium,
                            fontSize = 13.sp,
                            color = WinmuxColors.TextPrimary,
                        )
                        if (card.questions.size == 1 && !q.multiSelect) {
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                q.options.forEachIndexed { idx, opt ->
                                    if (idx == 0) {
                                        Button(onClick = { onResolve(card.id, opt.label, listOf(opt.label), null) }) {
                                            Text(opt.label)
                                        }
                                    } else {
                                        OutlinedButton(onClick = { onResolve(card.id, opt.label, listOf(opt.label), null) }) {
                                            Text(opt.label)
                                        }
                                    }
                                }
                            }
                        } else {
                            val chosen = selections.getOrPut(qi) { mutableSetOf() }
                            q.options.forEach { opt ->
                                val checked = chosen.contains(opt.label)
                                Row(
                                    verticalAlignment = Alignment.CenterVertically,
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .clickable {
                                            if (q.multiSelect) {
                                                if (checked) chosen.remove(opt.label) else chosen.add(opt.label)
                                            } else {
                                                chosen.clear()
                                                chosen.add(opt.label)
                                            }
                                            selections[qi] = chosen.toMutableSet()
                                        },
                                ) {
                                    Checkbox(
                                        checked = checked,
                                        onCheckedChange = { isChecked ->
                                            if (q.multiSelect) {
                                                if (isChecked) chosen.add(opt.label) else chosen.remove(opt.label)
                                            } else {
                                                chosen.clear()
                                                if (isChecked) chosen.add(opt.label)
                                            }
                                            selections[qi] = chosen.toMutableSet()
                                        },
                                    )
                                    Column {
                                        Text(opt.label, color = WinmuxColors.TextPrimary, fontSize = 13.sp)
                                        if (opt.description.isNotBlank()) {
                                            Text(opt.description, color = WinmuxColors.TextSecondary, fontSize = 11.sp)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (card.questions.size > 1 || card.questions.any { it.multiSelect }) {
                    Button(
                        onClick = {
                            val answers = card.questions.indices.map { qi ->
                                selections[qi]?.joinToString(", ").orEmpty()
                            }
                            onResolve(card.id, "answered", answers, null)
                        },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text("提交答案")
                    }
                }
            } else if (card.status == "pending" && card.options.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    card.options.forEach { opt ->
                        when (opt.style) {
                            "primary" -> Button(
                                onClick = { onResolve(card.id, opt.id, emptyList(), null) },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = if (risk.level == DecisionRiskLevel.HIGH) WinmuxColors.Attention else WinmuxColors.Accent,
                                ),
                            ) { Text(opt.label) }
                            "danger" -> Button(
                                onClick = { onResolve(card.id, opt.id, emptyList(), null) },
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = if (risk.level == DecisionRiskLevel.HIGH) WinmuxColors.Danger else WinmuxColors.Danger.copy(alpha = 0.2f),
                                    contentColor = if (risk.level == DecisionRiskLevel.HIGH) Color.White else WinmuxColors.Danger,
                                ),
                            ) { Text(opt.label) }
                            else -> OutlinedButton(onClick = { onResolve(card.id, opt.id, emptyList(), null) }) {
                                Text(opt.label)
                            }
                        }
                    }
                }
            } else if (card.resolution != null) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(accentColor.copy(alpha = 0.14f))
                        .padding(horizontal = 10.dp, vertical = 8.dp),
                ) {
                    Text(card.resolution.label, color = accentColor, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                }
            }

            // Footer actions: jump to source terminal + optional custom reply
            val canJumpTerminal = card.workspaceId.isNotBlank() && card.surfaceId.isNotBlank() && card.paneId != null
            if (canJumpTerminal || card.status == "pending") {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    if (canJumpTerminal) {
                        TextButton(
                            onClick = {
                                onOpenTerminal(
                                    ActiveTerminalTarget(
                                        workspaceId = card.workspaceId,
                                        workspaceTitle = card.workspaceTitle.ifBlank { "工作区" },
                                        surfaceId = card.surfaceId,
                                        paneId = card.paneId!!,
                                        paneTitle = card.agent.uppercase(),
                                        agentKind = card.agent,
                                    )
                                )
                            },
                        ) {
                            Icon(Icons.Default.Terminal, contentDescription = null, modifier = Modifier.size(15.dp))
                            Spacer(Modifier.width(4.dp))
                            Text("查看终端现场", fontSize = 12.sp)
                        }
                    } else {
                        Spacer(Modifier.width(1.dp))
                    }
                    if (card.status == "pending") {
                        TextButton(onClick = { showCustomReply = !showCustomReply }) {
                            Text(if (showCustomReply) "收起附言" else "拒绝并附说明", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                        }
                    }
                }
            }

            if (showCustomReply && card.status == "pending") {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    OutlinedTextField(
                        value = customReply,
                        onValueChange = { customReply = it },
                        placeholder = { Text("告诉 Agent 原因或替代方案...", fontSize = 12.sp) },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    Button(
                        enabled = customReply.isNotBlank(),
                        onClick = {
                            val reply = customReply.trim()
                            onResolve(card.id, "deny", listOf(reply), "$reply\r")
                            customReply = ""
                            showCustomReply = false
                        },
                        colors = ButtonDefaults.buttonColors(containerColor = WinmuxColors.Danger),
                    ) {
                        Text("发送", fontSize = 12.sp)
                    }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun NewTaskBottomSheet(
    snapshot: BridgeSnapshot,
    models: AllModelsConfig,
    initialWorkspaceId: String = "new",
    onDismiss: () -> Unit,
    onLaunched: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val recentDirs = remember(snapshot) {
        snapshot.workspaces.mapNotNull { it.workingDirectory.takeIf(String::isNotBlank) }.distinct()
    }
    val preselectedWs = remember(snapshot, initialWorkspaceId) {
        snapshot.workspaces.firstOrNull { it.id == initialWorkspaceId }
    }
    var selectedWorkspaceId by remember(initialWorkspaceId) { mutableStateOf(preselectedWs?.id ?: "new") }
    var directory by remember(initialWorkspaceId) {
        mutableStateOf(preselectedWs?.workingDirectory?.ifBlank { null } ?: recentDirs.firstOrNull().orEmpty())
    }
    var selectedAgent by remember { mutableStateOf("claude") }
    var selectedModel by remember { mutableStateOf("") }
    var prompt by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    val agentModels = when (selectedAgent) {
        "claude" -> models.claude.availableModels
        "codex" -> models.codex.availableModels
        "agy" -> models.agy.availableModels
        else -> emptyList()
    }

    ModalBottomSheet(
        onDismissRequest = onDismiss,
        containerColor = WinmuxColors.Surface,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
                .padding(bottom = 28.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("新建 Agent 任务", fontSize = 18.sp, fontWeight = FontWeight.Bold, color = WinmuxColors.TextPrimary)

            Text("选择 Agent", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf("claude" to "Claude", "codex" to "Codex", "agy" to "AGY", "pwsh" to "PowerShell").forEach { (key, label) ->
                    FilterChip(
                        selected = selectedAgent == key,
                        onClick = {
                            selectedAgent = key
                            selectedModel = ""
                        },
                        label = { Text(label) },
                    )
                }
            }

            if (agentModels.isNotEmpty()) {
                Text("指定模型（留空则使用默认）", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    FilterChip(
                        selected = selectedModel.isEmpty(),
                        onClick = { selectedModel = "" },
                        label = { Text("默认", fontSize = 11.5.sp) },
                    )
                    agentModels.take(5).forEach { m ->
                        FilterChip(
                            selected = selectedModel == m,
                            onClick = { selectedModel = m },
                            label = { Text(m, fontSize = 11.5.sp, fontFamily = FontFamily.Monospace) },
                        )
                    }
                }
            }

            Text("目标工作区", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                FilterChip(
                    selected = selectedWorkspaceId == "new",
                    onClick = { selectedWorkspaceId = "new" },
                    label = { Text("新建工作区") },
                )
                snapshot.workspaces.take(5).forEach { ws ->
                    FilterChip(
                        selected = selectedWorkspaceId == ws.id,
                        onClick = {
                            selectedWorkspaceId = ws.id
                            if (ws.workingDirectory.isNotBlank()) directory = ws.workingDirectory
                        },
                        label = { Text(ws.title.take(14)) },
                    )
                }
            }

            OutlinedTextField(
                value = directory,
                onValueChange = { directory = it },
                label = { Text("工作目录（可选）") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            OutlinedTextField(
                value = prompt,
                onValueChange = { prompt = it },
                label = { Text("要 Agent 执行的任务 Prompt") },
                minLines = 3,
                maxLines = 6,
                modifier = Modifier.fillMaxWidth(),
            )

            if (error != null) {
                Text(error!!, color = WinmuxColors.Danger, fontSize = 12.sp)
            }

            Button(
                enabled = !busy,
                onClick = {
                    busy = true
                    error = null
                    scope.launch {
                        runCatching {
                            WinmuxRepository.apiClient.launchTask(
                                config = WinmuxRepository.config.value,
                                workspaceId = selectedWorkspaceId,
                                directory = directory.trim(),
                                agent = selectedAgent,
                                prompt = prompt.trim(),
                                model = selectedModel,
                            )
                        }.onSuccess {
                            WinmuxRepository.refreshSnapshot()
                            onLaunched()
                        }.onFailure {
                            error = it.message ?: "启动失败"
                        }
                        busy = false
                    }
                },
                modifier = Modifier.fillMaxWidth().height(46.dp),
            ) {
                Text(if (busy) "正在电脑端启动..." else "在 Winmux 中创建并启动")
            }
        }
    }
}
