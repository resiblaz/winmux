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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
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
import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.DecisionCard
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.launch

@Composable
fun DecisionsScreen(
    decisions: List<DecisionCard>,
    onResolve: (decisionId: String, choice: String, answers: List<String>) -> Unit,
    onOpenNewTask: () -> Unit,
) {
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
            val pendingCount = decisions.count { it.status == "pending" }
            Text("Agent 决策卡片", fontSize = 20.sp, fontWeight = FontWeight.Bold, color = WinmuxColors.TextPrimary)
            Text(
                if (pendingCount > 0) "有 $pendingCount 项等待你确认（锁屏通知栏也可直接点按钮）" else "当前没有待审批项",
                fontSize = 12.sp,
                color = if (pendingCount > 0) WinmuxColors.Attention else WinmuxColors.TextSecondary,
            )
            Spacer(Modifier.height(10.dp))

            if (decisions.isEmpty()) {
                Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text(
                        "当 Claude / Codex 请求权限、提交计划或提问时，\n交互卡片会实时出现在这里并同步弹窗提醒。",
                        color = WinmuxColors.TextSecondary,
                        fontSize = 13.sp,
                        lineHeight = 20.sp,
                    )
                }
            } else {
                LazyColumn(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    items(decisions, key = { it.id }) { card ->
                        DecisionCardView(card = card, onResolve = onResolve)
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
    onResolve: (decisionId: String, choice: String, answers: List<String>) -> Unit,
) {
    val accentColor = when {
        card.status == "resolved" && card.resolution?.color == "green" -> WinmuxColors.Running
        card.status == "resolved" && card.resolution?.color == "red" -> WinmuxColors.Danger
        card.status != "pending" -> WinmuxColors.TextSecondary
        card.kind == "exitPlan" -> WinmuxColors.Accent
        card.kind == "question" -> WinmuxColors.Agy
        else -> WinmuxColors.Attention
    }

    Card(
        colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
        border = BorderStroke(1.dp, accentColor.copy(alpha = 0.55f)),
        shape = RoundedCornerShape(14.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                AgentBadgeIcon(agentKind = card.agent, isRunning = card.status == "pending", size = 22.dp)
                Column(modifier = Modifier.weight(1f)) {
                    Text(card.title, fontWeight = FontWeight.SemiBold, fontSize = 15.sp, color = WinmuxColors.TextPrimary)
                    if (card.workspaceTitle.isNotBlank()) {
                        Text(card.workspaceTitle, fontSize = 11.sp, color = WinmuxColors.TextSecondary)
                    }
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

            if (card.summary.isNotBlank()) {
                Text(card.summary, fontSize = 13.sp, color = WinmuxColors.TextPrimary, lineHeight = 19.sp)
            }

            val codeBlock = when {
                card.toolInput.isNotBlank() -> card.toolInput
                card.planText.isNotBlank() -> card.planText
                else -> ""
            }
            if (codeBlock.isNotBlank()) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(WinmuxColors.Bg)
                        .padding(10.dp),
                ) {
                    Text(
                        text = codeBlock.take(1500),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.5.sp,
                        color = WinmuxColors.TextPrimary,
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
                                        Button(onClick = { onResolve(card.id, opt.label, listOf(opt.label)) }) {
                                            Text(opt.label)
                                        }
                                    } else {
                                        OutlinedButton(onClick = { onResolve(card.id, opt.label, listOf(opt.label)) }) {
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
                            onResolve(card.id, "answered", answers)
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
                                onClick = { onResolve(card.id, opt.id, emptyList()) },
                                colors = ButtonDefaults.buttonColors(containerColor = WinmuxColors.Accent),
                            ) { Text(opt.label) }
                            "danger" -> OutlinedButton(
                                onClick = { onResolve(card.id, opt.id, emptyList()) },
                                colors = ButtonDefaults.outlinedButtonColors(contentColor = WinmuxColors.Danger),
                            ) { Text(opt.label) }
                            else -> OutlinedButton(onClick = { onResolve(card.id, opt.id, emptyList()) }) {
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
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun NewTaskBottomSheet(
    snapshot: BridgeSnapshot,
    models: AllModelsConfig,
    onDismiss: () -> Unit,
    onLaunched: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val recentDirs = remember(snapshot) {
        snapshot.workspaces.mapNotNull { it.workingDirectory.takeIf(String::isNotBlank) }.distinct()
    }
    var selectedWorkspaceId by remember { mutableStateOf("new") }
    var directory by remember { mutableStateOf(recentDirs.firstOrNull().orEmpty()) }
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
