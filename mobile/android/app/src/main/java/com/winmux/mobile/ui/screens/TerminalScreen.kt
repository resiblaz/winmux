package com.winmux.mobile.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SmallFloatingActionButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
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
import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.PaneInfo
import com.winmux.mobile.model.PermissionsInfo
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.components.AnsiTerminalParser
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

private data class QuickKeySpec(
    val label: String,
    val sequence: String,
    val danger: Boolean = false,
)

private val QUICK_KEYS = listOf(
    QuickKeySpec("Ctrl+C", "\u0003", danger = true),
    QuickKeySpec("Esc", "\u001B"),
    QuickKeySpec("Shift+Tab", "\u001B[Z"),
    QuickKeySpec("Tab", "\t"),
    QuickKeySpec("⏎", "\r"),
    QuickKeySpec("y⏎", "y\r"),
    QuickKeySpec("n⏎", "n\r"),
    QuickKeySpec("↑", "\u001B[A"),
    QuickKeySpec("↓", "\u001B[B"),
    QuickKeySpec("←", "\u001B[D"),
    QuickKeySpec("→", "\u001B[C"),
    QuickKeySpec("Ctrl+D", "\u0004"),
    QuickKeySpec("Ctrl+L", "\u000C"),
)

private val PROMPT_SNIPPETS = listOf(
    "继续" to "继续",
    "先分析不改代码" to "先分析原因，不要直接修改代码",
    "跑测试并修复" to "运行测试并修复所有失败项",
    "总结当前改动" to "用中文简明总结当前改动和测试结果",
    "git status" to "git status --short",
    "git diff --stat" to "git diff --stat",
)

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun TerminalScreen(
    target: ActiveTerminalTarget,
    siblingPanes: List<PaneInfo>,
    permissions: PermissionsInfo,
    models: AllModelsConfig,
    onSwitchTarget: (ActiveTerminalTarget) -> Unit,
    onBack: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val scrollState = rememberScrollState()
    val horizontalScrollState = rememberScrollState()
    var screenText by remember(target) { mutableStateOf("正在读取电脑终端输出...") }
    var errorBanner by remember(target) { mutableStateOf<String?>(null) }
    var statusToast by remember { mutableStateOf<String?>(null) }
    var inputText by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    var showAgentActions by remember { mutableStateOf(false) }
    var showSnippets by remember { mutableStateOf(false) }
    var fontSizeSp by remember { mutableFloatStateOf(11.5f) }
    var softWrap by remember { mutableStateOf(true) }
    var autoFollow by remember(target) { mutableStateOf(true) }

    val isNearBottom by remember {
        derivedStateOf { scrollState.maxValue - scrollState.value <= 80 }
    }

    // Pause auto-scroll when the user manually scrolls up to read history; resume when back near bottom.
    LaunchedEffect(scrollState.isScrollInProgress, isNearBottom) {
        if (scrollState.isScrollInProgress) {
            autoFollow = isNearBottom
        } else if (isNearBottom) {
            autoFollow = true
        }
    }

    val renderedText = remember(screenText) { AnsiTerminalParser.parse(screenText) }

    suspend fun loadScreen() {
        runCatching {
            WinmuxRepository.apiClient.readTerminalContent(
                config = WinmuxRepository.config.value,
                workspaceId = target.workspaceId,
                surfaceId = target.surfaceId,
                paneId = target.paneId,
                maxLines = 150,
            )
        }.onSuccess { text ->
            screenText = text.ifBlank { "（当前终端暂无输出）" }
            errorBanner = null
        }.onFailure { err ->
            errorBanner = err.message
        }
    }

    fun sendRaw(raw: String) {
        if (sending || raw.isEmpty()) return
        sending = true
        autoFollow = true
        scope.launch {
            runCatching {
                WinmuxRepository.apiClient.sendTerminalInput(
                    config = WinmuxRepository.config.value,
                    workspaceId = target.workspaceId,
                    surfaceId = target.surfaceId,
                    paneId = target.paneId,
                    text = raw,
                )
            }.onSuccess {
                errorBanner = null
                delay(320)
                loadScreen()
            }.onFailure { err ->
                errorBanner = err.message
            }
            sending = false
        }
    }

    val terminalOutputs by WinmuxRepository.terminalOutputs.collectAsState()
    val streamKey = WinmuxRepository.terminalKey(target.workspaceId, target.surfaceId, target.paneId)
    val pushedText = terminalOutputs[streamKey]

    LaunchedEffect(pushedText) {
        if (pushedText != null) {
            screenText = pushedText.ifBlank { "（当前终端暂无输出）" }
            errorBanner = null
        }
    }

    // Subscribe to real-time WebSocket diff pushes instead of HTTP polling.
    DisposableEffect(target) {
        WinmuxRepository.subscribeTerminal(target.workspaceId, target.surfaceId, target.paneId, maxLines = 150)
        onDispose {
            WinmuxRepository.unsubscribeTerminal()
        }
    }

    // Initial HTTP load fallback in case WebSocket is still connecting.
    LaunchedEffect(target) {
        loadScreen()
    }

    LaunchedEffect(screenText, autoFollow) {
        if (autoFollow) {
            scrollState.animateScrollTo(scrollState.maxValue)
        }
    }

    LaunchedEffect(statusToast) {
        if (statusToast != null) {
            delay(2200)
            statusToast = null
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(WinmuxColors.Bg)
            .imePadding(),
    ) {
        // Top bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(WinmuxColors.Surface)
                .padding(horizontal = 4.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回", tint = WinmuxColors.TextPrimary)
            }
            AgentBadgeIcon(agentKind = target.agentKind, size = 22.dp)
            Spacer(Modifier.width(8.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    target.paneTitle,
                    color = WinmuxColors.TextPrimary,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 14.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    target.workspaceTitle,
                    color = WinmuxColors.TextSecondary,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Surface(
                onClick = { fontSizeSp = if (fontSizeSp <= 10.5f) 11.5f else if (fontSizeSp < 13.5f) 13.5f else 10f },
                shape = RoundedCornerShape(6.dp),
                color = WinmuxColors.SurfaceElevated,
                border = BorderStroke(1.dp, WinmuxColors.Border),
            ) {
                Text(
                    text = "${fontSizeSp.toInt()}sp",
                    color = WinmuxColors.TextSecondary,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 10.5.sp,
                    modifier = Modifier.padding(horizontal = 7.dp, vertical = 4.dp),
                )
            }
            Spacer(Modifier.width(4.dp))
            Surface(
                onClick = { softWrap = !softWrap },
                shape = RoundedCornerShape(6.dp),
                color = if (softWrap) WinmuxColors.SurfaceElevated else WinmuxColors.Accent.copy(alpha = 0.2f),
                border = BorderStroke(1.dp, if (softWrap) WinmuxColors.Border else WinmuxColors.Accent),
            ) {
                Text(
                    text = if (softWrap) "换行" else "横滑",
                    color = if (softWrap) WinmuxColors.TextSecondary else WinmuxColors.Accent,
                    fontSize = 10.5.sp,
                    modifier = Modifier.padding(horizontal = 7.dp, vertical = 4.dp),
                )
            }
            IconButton(onClick = { showAgentActions = true }) {
                Icon(Icons.Default.Tune, contentDescription = "模型、指令与分屏", tint = WinmuxColors.Accent)
            }
            IconButton(onClick = { scope.launch { loadScreen() } }) {
                Icon(Icons.Default.Refresh, contentDescription = "刷新", tint = WinmuxColors.TextSecondary)
            }
        }

        // Sibling pane switcher strip when workspace has multiple panes/tabs
        if (siblingPanes.size > 1) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(WinmuxColors.Surface.copy(alpha = 0.72f))
                    .horizontalScroll(rememberScrollState())
                    .padding(horizontal = 10.dp, vertical = 5.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                siblingPanes.forEach { pane ->
                    val isCurrent = pane.surfaceId == target.surfaceId && pane.id == target.paneId
                    Surface(
                        onClick = {
                            if (!isCurrent) {
                                onSwitchTarget(
                                    ActiveTerminalTarget(
                                        workspaceId = target.workspaceId,
                                        workspaceTitle = target.workspaceTitle,
                                        surfaceId = pane.surfaceId,
                                        paneId = pane.id,
                                        paneTitle = pane.title,
                                        agentKind = pane.agentKind,
                                    )
                                )
                            }
                        },
                        shape = RoundedCornerShape(8.dp),
                        color = if (isCurrent) WinmuxColors.Accent.copy(alpha = 0.22f) else WinmuxColors.SurfaceElevated,
                        border = BorderStroke(1.dp, if (isCurrent) WinmuxColors.Accent else WinmuxColors.Border),
                    ) {
                        Row(
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(5.dp),
                        ) {
                            AgentBadgeIcon(
                                agentKind = pane.agentKind,
                                isRunning = pane.hasActiveChild,
                                hasAttention = pane.hasAttention,
                                size = 15.dp,
                            )
                            Text(
                                text = pane.title.take(18),
                                fontSize = 11.5.sp,
                                fontWeight = if (isCurrent) FontWeight.SemiBold else FontWeight.Normal,
                                color = if (isCurrent) WinmuxColors.TextPrimary else WinmuxColors.TextSecondary,
                                maxLines = 1,
                            )
                        }
                    }
                }
            }
        }

        if (!permissions.readContent || !permissions.sendInput || errorBanner != null) {
            Card(
                colors = CardDefaults.cardColors(containerColor = WinmuxColors.Attention.copy(alpha = 0.14f)),
                border = BorderStroke(1.dp, WinmuxColors.Attention.copy(alpha = 0.45f)),
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
            ) {
                Text(
                    text = errorBanner ?: "提示：请在桌面 Winmux 设置中开启 experimental.workspaceAutomation.allowInput 和 allowContentRead 以读写终端。",
                    color = WinmuxColors.Attention,
                    fontSize = 12.sp,
                    modifier = Modifier.padding(10.dp),
                )
            }
        }

        if (statusToast != null) {
            Text(
                text = statusToast!!,
                color = WinmuxColors.Running,
                fontSize = 11.5.sp,
                modifier = Modifier.padding(horizontal = 14.dp, vertical = 3.dp),
            )
        }

        // Terminal screen buffer with ANSI colors + smart scroll-lock FAB
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .padding(horizontal = 10.dp, vertical = 6.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0xFF0A0C10)),
        ) {
            val contentModifier = Modifier
                .fillMaxSize()
                .padding(10.dp)
                .verticalScroll(scrollState)
                .let { mod -> if (softWrap) mod else mod.horizontalScroll(horizontalScrollState) }

            Box(modifier = contentModifier) {
                SelectionContainer {
                    Text(
                        text = renderedText,
                        fontFamily = FontFamily.Monospace,
                        fontSize = fontSizeSp.sp,
                        lineHeight = (fontSizeSp * 1.42f).sp,
                        softWrap = softWrap,
                    )
                }
            }

            if (!autoFollow && !isNearBottom) {
                SmallFloatingActionButton(
                    onClick = {
                        autoFollow = true
                        scope.launch { scrollState.animateScrollTo(scrollState.maxValue) }
                    },
                    containerColor = WinmuxColors.Accent,
                    contentColor = Color.White,
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .padding(10.dp),
                ) {
                    Icon(Icons.Default.KeyboardArrowDown, contentDescription = "回到底部")
                }
            }
        }

        // Thumb-friendly quick key bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 10.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            QUICK_KEYS.forEach { key ->
                Surface(
                    onClick = { sendRaw(key.sequence) },
                    shape = RoundedCornerShape(8.dp),
                    color = if (key.danger) WinmuxColors.Danger.copy(alpha = 0.16f) else WinmuxColors.SurfaceElevated,
                    border = BorderStroke(1.dp, if (key.danger) WinmuxColors.Danger.copy(alpha = 0.55f) else WinmuxColors.Border),
                ) {
                    Text(
                        text = key.label,
                        color = if (key.danger) WinmuxColors.Danger else WinmuxColors.TextPrimary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.5.sp,
                        fontWeight = if (key.danger) FontWeight.SemiBold else FontWeight.Normal,
                        modifier = Modifier.padding(horizontal = 10.dp, vertical = 7.dp),
                    )
                }
            }
        }

        // Prompt input bar with snippet trigger
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(WinmuxColors.Surface)
                .padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(
                onClick = { showSnippets = true },
                modifier = Modifier.size(38.dp),
            ) {
                Icon(Icons.Default.Bolt, contentDescription = "常用指令片段", tint = WinmuxColors.Attention)
            }
            Spacer(Modifier.width(4.dp))
            OutlinedTextField(
                value = inputText,
                onValueChange = { inputText = it },
                placeholder = { Text("发指令或 Prompt 给终端...", fontSize = 13.sp) },
                maxLines = 4,
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(6.dp))
            IconButton(
                enabled = !sending && inputText.isNotBlank(),
                onClick = {
                    val toSend = inputText
                    inputText = ""
                    sendRaw("$toSend\r")
                },
            ) {
                Icon(Icons.AutoMirrored.Filled.Send, contentDescription = "发送", tint = WinmuxColors.Accent)
            }
        }
        Spacer(Modifier.height(4.dp))
    }

    // Quick prompt/command snippets sheet
    if (showSnippets) {
        ModalBottomSheet(
            onDismissRequest = { showSnippets = false },
            containerColor = WinmuxColors.Surface,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp)
                    .padding(bottom = 28.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text("常用 Prompt 与指令片段", fontWeight = FontWeight.Bold, fontSize = 16.sp, color = WinmuxColors.TextPrimary)
                Text("点按直接发送，长按或点「填入」可先编辑再发", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                PROMPT_SNIPPETS.forEach { (label, text) ->
                    Card(
                        colors = CardDefaults.cardColors(containerColor = WinmuxColors.SurfaceElevated),
                        border = BorderStroke(1.dp, WinmuxColors.Border),
                        shape = RoundedCornerShape(10.dp),
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(label, color = WinmuxColors.TextPrimary, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                                Text(text, color = WinmuxColors.TextSecondary, fontFamily = FontFamily.Monospace, fontSize = 11.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            }
                            OutlinedButton(
                                onClick = {
                                    inputText = text
                                    showSnippets = false
                                },
                            ) { Text("填入", fontSize = 12.sp) }
                            Button(
                                onClick = {
                                    showSnippets = false
                                    sendRaw("$text\r")
                                },
                            ) { Text("发送", fontSize = 12.sp) }
                        }
                    }
                }
            }
        }
    }

    // Model switch, slash commands & pane layout sheet
    if (showAgentActions) {
        val modelList = when (target.agentKind.lowercase()) {
            "codex" -> models.codex.availableModels
            "agy", "antigravity" -> models.agy.availableModels
            else -> models.claude.availableModels
        }
        var customModel by remember { mutableStateOf("") }

        fun runAction(action: String, model: String = "") {
            showAgentActions = false
            scope.launch {
                runCatching {
                    WinmuxRepository.apiClient.sendAgentAction(
                        config = WinmuxRepository.config.value,
                        workspaceId = target.workspaceId,
                        surfaceId = target.surfaceId,
                        paneId = target.paneId,
                        action = action,
                        model = model,
                    )
                }.onSuccess {
                    statusToast = if (action == "switch_model") "已发送 /model $model" else "已发送 /$action"
                    delay(450)
                    loadScreen()
                }.onFailure { err ->
                    errorBanner = err.message
                }
            }
        }

        fun splitCurrentPane(direction: String) {
            showAgentActions = false
            scope.launch {
                runCatching {
                    WinmuxRepository.apiClient.splitPane(
                        config = WinmuxRepository.config.value,
                        workspaceId = target.workspaceId,
                        surfaceId = target.surfaceId,
                        paneId = target.paneId,
                        direction = direction,
                    )
                }.onSuccess {
                    WinmuxRepository.refreshSnapshot()
                    statusToast = if (direction == "right") "已在右侧拆分新窗格" else "已在下方拆分新窗格"
                }.onFailure { err ->
                    errorBanner = err.message
                }
            }
        }

        ModalBottomSheet(
            onDismissRequest = { showAgentActions = false },
            containerColor = WinmuxColors.Surface,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp)
                    .padding(bottom = 28.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text("当前终端：模型、指令与分屏", fontWeight = FontWeight.Bold, fontSize = 16.sp, color = WinmuxColors.TextPrimary)

                Text("实时切换模型（发送 /model）", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    modelList.forEach { m ->
                        FilterChip(
                            selected = false,
                            onClick = { runAction("switch_model", m) },
                            label = { Text(m, fontSize = 11.5.sp, fontFamily = FontFamily.Monospace) },
                        )
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = customModel,
                        onValueChange = { customModel = it },
                        placeholder = { Text("输入其他模型名", fontSize = 12.sp) },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    Button(enabled = customModel.isNotBlank(), onClick = { runAction("switch_model", customModel.trim()) }) {
                        Text("切换")
                    }
                }

                Text("快捷斜杠指令", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(
                        "usage" to "/usage 查看额度",
                        "cost" to "/cost 查看费用",
                        "compact" to "/compact 压缩上下文",
                        "status" to "/status 查看状态",
                        "clear" to "/clear 清空对话",
                    ).forEach { (actionKey, label) ->
                        OutlinedButton(onClick = { runAction(actionKey) }) {
                            Text(label, fontSize = 12.sp)
                        }
                    }
                }

                HorizontalDivider(color = WinmuxColors.Border, thickness = 0.5.dp)

                Text("窗格与桌面联动", fontSize = 12.sp, color = WinmuxColors.TextSecondary)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    OutlinedButton(onClick = { splitCurrentPane("right") }) {
                        Text("向右分屏", fontSize = 12.sp)
                    }
                    OutlinedButton(onClick = { splitCurrentPane("down") }) {
                        Text("向下分屏", fontSize = 12.sp)
                    }
                    OutlinedButton(
                        onClick = {
                            showAgentActions = false
                            scope.launch {
                                runCatching {
                                    WinmuxRepository.apiClient.focusPane(
                                        WinmuxRepository.config.value,
                                        target.workspaceId,
                                        target.surfaceId,
                                        target.paneId,
                                    )
                                }.onSuccess {
                                    statusToast = "已在桌面 Winmux 聚焦此窗格"
                                }.onFailure { err ->
                                    errorBanner = err.message
                                }
                            }
                        },
                    ) {
                        Text("桌面端聚焦", fontSize = 12.sp)
                    }
                }
            }
        }
    }
}
