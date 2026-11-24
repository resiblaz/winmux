package com.winmux.mobile.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
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
import com.winmux.mobile.model.PermissionsInfo
import com.winmux.mobile.ui.components.AgentBadgeIcon
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun TerminalScreen(
    target: ActiveTerminalTarget,
    permissions: PermissionsInfo,
    models: AllModelsConfig,
    onBack: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val scrollState = rememberScrollState()
    var screenText by remember { mutableStateOf("正在读取电脑终端输出...") }
    var errorBanner by remember { mutableStateOf<String?>(null) }
    var inputText by remember { mutableStateOf("") }
    var sending by remember { mutableStateOf(false) }
    var showAgentActions by remember { mutableStateOf(false) }

    suspend fun loadScreen() {
        runCatching {
            WinmuxRepository.apiClient.readTerminalContent(
                config = WinmuxRepository.config.value,
                workspaceId = target.workspaceId,
                surfaceId = target.surfaceId,
                paneId = target.paneId,
                maxLines = 120,
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
                delay(350)
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
        WinmuxRepository.subscribeTerminal(target.workspaceId, target.surfaceId, target.paneId, maxLines = 120)
        onDispose {
            WinmuxRepository.unsubscribeTerminal()
        }
    }

    // Initial HTTP load fallback in case WebSocket is still connecting.
    LaunchedEffect(target) {
        loadScreen()
    }

    LaunchedEffect(screenText) {
        scrollState.animateScrollTo(scrollState.maxValue)
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
                .padding(horizontal = 6.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = onBack) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "返回", tint = WinmuxColors.TextPrimary)
            }
            AgentBadgeIcon(agentKind = target.agentKind, size = 22.dp)
            Spacer(Modifier.width(8.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(target.paneTitle, color = WinmuxColors.TextPrimary, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(target.workspaceTitle, color = WinmuxColors.TextSecondary, fontSize = 11.sp, maxLines = 1)
            }
            IconButton(onClick = { showAgentActions = true }) {
                Icon(Icons.Default.Tune, contentDescription = "切换模型与快捷指令", tint = WinmuxColors.Accent)
            }
            IconButton(onClick = { scope.launch { loadScreen() } }) {
                Icon(Icons.Default.Refresh, contentDescription = "刷新", tint = WinmuxColors.TextSecondary)
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

        // Terminal screen buffer
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .padding(horizontal = 10.dp, vertical = 6.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0xFF0A0C10))
                .padding(10.dp)
                .verticalScroll(scrollState),
        ) {
            SelectionContainer {
                Text(
                    text = screenText,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.5.sp,
                    lineHeight = 16.sp,
                    color = Color(0xFFE5E9F0),
                )
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
            val quickKeys = listOf(
                "y + ⏎" to "y\r",
                "n + ⏎" to "n\r",
                "⏎ Enter" to "\r",
                "Esc" to "\u001B",
                "Ctrl+C" to "\u0003",
                "Tab" to "\t",
                "↑" to "\u001B[A",
                "↓" to "\u001B[B",
            )
            quickKeys.forEach { (label, seq) ->
                Surface(
                    onClick = { sendRaw(seq) },
                    shape = RoundedCornerShape(8.dp),
                    color = WinmuxColors.SurfaceElevated,
                    border = BorderStroke(1.dp, WinmuxColors.Border),
                ) {
                    Text(
                        text = label,
                        color = WinmuxColors.TextPrimary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 12.sp,
                        modifier = Modifier.padding(horizontal = 11.dp, vertical = 7.dp),
                    )
                }
            }
        }

        // Prompt input bar
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(WinmuxColors.Surface)
                .padding(horizontal = 10.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
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
                    delay(450)
                    loadScreen()
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
                Text("当前终端：切换模型 & 快捷指令", fontWeight = FontWeight.Bold, fontSize = 16.sp, color = WinmuxColors.TextPrimary)

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
            }
        }
    }
}
