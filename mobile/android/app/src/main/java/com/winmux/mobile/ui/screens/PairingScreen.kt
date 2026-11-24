package com.winmux.mobile.ui.screens

import android.os.Build
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.winmux.mobile.data.SettingsRepository
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.BridgeConfig
import com.winmux.mobile.model.ConnectionState
import com.winmux.mobile.model.PairedDeviceInfo
import com.winmux.mobile.ui.components.DecisionRiskAnalyzer
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlinx.coroutines.launch

@Composable
fun PairingScreen(
    config: BridgeConfig,
    connectionState: ConnectionState,
    incomingPairUri: String?,
    onIncomingPairConsumed: () -> Unit,
    onPaired: () -> Unit,
    onUnpair: () -> Unit,
    onReconnect: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    var pairUriInput by remember { mutableStateOf("") }
    var lanUrl by remember(config.lanUrl) { mutableStateOf(config.lanUrl.ifBlank { "https://192.168.3.21:47710" }) }
    var remoteUrl by remember(config.remoteUrl) { mutableStateOf(config.remoteUrl) }
    var certFingerprint by remember(config.certFingerprint) { mutableStateOf(config.certFingerprint) }
    var pairingCode by remember { mutableStateOf("") }
    var deviceName by remember(config.deviceName) { mutableStateOf(config.deviceName.ifBlank { Build.MODEL ?: "Android" }) }
    var busy by remember { mutableStateOf(false) }
    var feedback by remember { mutableStateOf<String?>(null) }
    var pairedDevices by remember { mutableStateOf<List<PairedDeviceInfo>>(emptyList()) }
    var loadingDevices by remember { mutableStateOf(false) }
    var pendingRevokeDevice by remember { mutableStateOf<PairedDeviceInfo?>(null) }

    fun loadDevices() {
        if (!config.isPaired) return
        loadingDevices = true
        scope.launch {
            runCatching { WinmuxRepository.apiClient.fetchDevices(config) }
                .onSuccess { pairedDevices = it }
            loadingDevices = false
        }
    }

    LaunchedEffect(config.isPaired, connectionState) {
        if (config.isPaired && connectionState is ConnectionState.Connected) {
            loadDevices()
        } else if (!config.isPaired) {
            pairedDevices = emptyList()
        }
    }

    LaunchedEffect(incomingPairUri) {
        if (!incomingPairUri.isNullOrBlank()) {
            SettingsRepository.parsePairUri(incomingPairUri)?.let { parsed ->
                if (parsed.lanUrl.isNotBlank()) lanUrl = parsed.lanUrl
                if (parsed.remoteUrl.isNotBlank()) remoteUrl = parsed.remoteUrl
                if (parsed.certFingerprint.isNotBlank()) certFingerprint = parsed.certFingerprint
                if (parsed.code.isNotBlank()) pairingCode = parsed.code
                pairUriInput = incomingPairUri
                feedback = "已解析加密配对链接，点击下方「完成配对」即可连接"
            }
            onIncomingPairConsumed()
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Text(
            text = "电脑端远程连接",
            fontSize = 20.sp,
            fontWeight = FontWeight.Bold,
            color = WinmuxColors.TextPrimary,
        )

        // Status Card
        Card(
            colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
            border = BorderStroke(1.dp, WinmuxColors.Border),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                val (statusTitle, statusColor) = when (connectionState) {
                    is ConnectionState.Connected -> "已连接 (${connectionState.endpointLabel})" to WinmuxColors.Running
                    is ConnectionState.Connecting -> "连接中：${connectionState.target}" to WinmuxColors.Attention
                    is ConnectionState.Error -> "连接中断：${connectionState.message}" to WinmuxColors.Danger
                    ConnectionState.Disconnected -> "未连接" to WinmuxColors.TextSecondary
                }
                Text(statusTitle, color = statusColor, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                if (config.isPaired) {
                    if (config.hostname.isNotBlank()) {
                        Text("主机：${config.hostname}", color = WinmuxColors.TextSecondary, fontSize = 12.sp)
                    }
                    if (config.lanUrl.isNotBlank()) {
                        Text("局域网：${config.lanUrl}", color = WinmuxColors.TextSecondary, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                    }
                    if (config.remoteUrl.isNotBlank()) {
                        Text("远程直连：${config.remoteUrl}", color = WinmuxColors.TextSecondary, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
                    }
                    if (config.certFingerprint.length == 64) {
                        Text(
                            "TLS 指纹已锁定：${config.certFingerprint.take(16)}…",
                            color = WinmuxColors.Running,
                            fontSize = 11.5.sp,
                            fontFamily = FontFamily.Monospace,
                        )
                    }
                    Spacer(Modifier.height(4.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = onReconnect) {
                            Text("立即重连")
                        }
                        OutlinedButton(
                            onClick = onUnpair,
                            colors = ButtonDefaults.outlinedButtonColors(contentColor = WinmuxColors.Danger),
                        ) {
                            Text("解除配对")
                        }
                    }
                }
            }
        }

        // Quick Pair URI Paste
        Card(
            colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
            border = BorderStroke(1.dp, WinmuxColors.Border),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("配对本机 Winmux Bridge", fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary)
                Text(
                    "在电脑运行 node tools/remote-bridge/bin/winmux-bridge.mjs 后，直接粘贴 Pair URI 或填写局域网/Tailscale 地址和 6 位配对码。",
                    color = WinmuxColors.TextSecondary,
                    fontSize = 12.sp,
                )

                OutlinedTextField(
                    value = pairUriInput,
                    onValueChange = { raw ->
                        pairUriInput = raw
                        SettingsRepository.parsePairUri(raw)?.let { parsed ->
                            if (parsed.lanUrl.isNotBlank()) lanUrl = parsed.lanUrl
                            if (parsed.remoteUrl.isNotBlank()) remoteUrl = parsed.remoteUrl
                            if (parsed.certFingerprint.isNotBlank()) certFingerprint = parsed.certFingerprint
                            if (parsed.code.isNotBlank()) pairingCode = parsed.code
                            feedback = "已自动填充地址、TLS 证书指纹与配对码"
                        }
                    },
                    label = { Text("一键粘贴 winmux://pair?...（推荐）") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                OutlinedTextField(
                    value = lanUrl,
                    onValueChange = { lanUrl = it },
                    label = { Text("局域网地址（在家 Wi-Fi 优先）") },
                    placeholder = { Text("https://192.168.3.21:47710") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                OutlinedTextField(
                    value = remoteUrl,
                    onValueChange = { remoteUrl = it },
                    label = { Text("远程地址（出门 5G 自动切换，如公网 IPv6）") },
                    placeholder = { Text("https://[240e:...]:47710") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                OutlinedTextField(
                    value = certFingerprint,
                    onValueChange = { certFingerprint = it.filter { ch -> ch.isDigit() || ch.lowercaseChar() in 'a'..'f' }.take(64) },
                    label = { Text("TLS 证书 SHA-256 指纹（HTTPS 时防中间人）") },
                    placeholder = { Text("粘贴 Pair URI 自动填入 64 位指纹") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                Row(horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
                    OutlinedTextField(
                        value = pairingCode,
                        onValueChange = { pairingCode = it.filter(Char::isDigit).take(6) },
                        label = { Text("6 位配对码") },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedTextField(
                        value = deviceName,
                        onValueChange = { deviceName = it },
                        label = { Text("设备名") },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                }

                if (feedback != null) {
                    Text(feedback!!, color = WinmuxColors.Attention, fontSize = 12.sp)
                }

                Button(
                    enabled = !busy && pairingCode.length == 6 && (lanUrl.isNotBlank() || remoteUrl.isNotBlank()),
                    onClick = {
                        busy = true
                        feedback = null
                        scope.launch {
                            try {
                                val res = WinmuxRepository.apiClient.pair(
                                    lanUrl = lanUrl.trim(),
                                    remoteUrl = remoteUrl.trim(),
                                    code = pairingCode.trim(),
                                    deviceName = deviceName.trim(),
                                    certFingerprint = certFingerprint.trim(),
                                )
                                WinmuxRepository.updateConfig(
                                    BridgeConfig(
                                        lanUrl = res.lanUrl.ifBlank { lanUrl.trim() },
                                        remoteUrl = res.remoteUrl.ifBlank { remoteUrl.trim() },
                                        token = res.token,
                                        certFingerprint = res.certFingerprint.ifBlank { certFingerprint.trim() },
                                        hostname = res.hostname,
                                        deviceName = deviceName.trim(),
                                    )
                                )
                                pairingCode = ""
                                feedback = "配对成功！"
                                onPaired()
                            } catch (e: Exception) {
                                feedback = e.message ?: "配对失败"
                            } finally {
                                busy = false
                            }
                        }
                    },
                    modifier = Modifier.fillMaxWidth().height(46.dp),
                ) {
                    if (busy) {
                        CircularProgressIndicator(strokeWidth = 2.dp, color = Color.White, modifier = Modifier.height(20.dp))
                    } else {
                        Text("完成配对并连接")
                    }
                }
            }
        }

        // Paired Devices Management Card (visible when paired)
        if (config.isPaired) {
            Card(
                colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
                border = BorderStroke(1.dp, WinmuxColors.Border),
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Text(
                            "电脑已授权设备 (${pairedDevices.size})",
                            fontWeight = FontWeight.SemiBold,
                            color = WinmuxColors.TextPrimary,
                            fontSize = 14.sp,
                        )
                        IconButton(enabled = !loadingDevices, onClick = { loadDevices() }) {
                            Icon(Icons.Default.Refresh, contentDescription = "刷新设备列表", tint = WinmuxColors.TextSecondary)
                        }
                    }
                    if (pairedDevices.isEmpty()) {
                        Text(
                            if (loadingDevices) "正在加载已配对设备..." else "连接后可在此查看并吊销旧手机的访问 Token。",
                            color = WinmuxColors.TextSecondary,
                            fontSize = 12.sp,
                        )
                    } else {
                        pairedDevices.forEachIndexed { idx, dev ->
                            if (idx > 0) {
                                HorizontalDivider(color = WinmuxColors.Border, thickness = 0.5.dp)
                            }
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(dev.name, color = WinmuxColors.TextPrimary, fontWeight = FontWeight.Medium, fontSize = 13.sp)
                                    val lastSeen = DecisionRiskAnalyzer.formatRelativeTime(dev.lastSeenAt)
                                    Text(
                                        text = "ID ${dev.id.take(8)} · 最近活跃 ${lastSeen.ifBlank { "未知" }}",
                                        color = WinmuxColors.TextSecondary,
                                        fontSize = 11.sp,
                                        fontFamily = FontFamily.Monospace,
                                    )
                                }
                                IconButton(onClick = { pendingRevokeDevice = dev }) {
                                    Icon(Icons.Default.Delete, contentDescription = "吊销设备", tint = WinmuxColors.Danger)
                                }
                            }
                        }
                    }
                }
            }
        }

        // Remote access guide
        Card(
            colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
            border = BorderStroke(1.dp, WinmuxColors.Border),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("连接与安全说明", fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary, fontSize = 14.sp)
                Text(
                    "1. 在桌面 Winmux 左侧栏顶部点「远程控制」图标即可一键启动内置 Bridge 并复制配对链接。\n" +
                        "2. 全链路默认启用自签名 ECDSA TLS + SHA-256 证书指纹锁定，无需域名也防中间人窃听。\n" +
                        "3. 在家优先走局域网毫秒级直连；出门切 5G 后自动无感切换到电脑公网 IPv6 或 Tailscale 地址。",
                    color = WinmuxColors.TextSecondary,
                    fontSize = 12.sp,
                    lineHeight = 18.sp,
                )
            }
        }
    }

    if (pendingRevokeDevice != null) {
        val targetDev = pendingRevokeDevice!!
        AlertDialog(
            onDismissRequest = { pendingRevokeDevice = null },
            containerColor = WinmuxColors.Surface,
            title = { Text("吊销设备授权？", color = WinmuxColors.TextPrimary) },
            text = {
                Text(
                    "吊销后，设备「${targetDev.name}」的访问 Token 将立即失效，需重新输入 6 位配对码才能连接。",
                    color = WinmuxColors.TextSecondary,
                    fontSize = 13.sp,
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        pendingRevokeDevice = null
                        scope.launch {
                            runCatching { WinmuxRepository.apiClient.revokeDevice(config, targetDev.id) }
                                .onSuccess {
                                    feedback = "已吊销设备「${targetDev.name}」"
                                    loadDevices()
                                }
                                .onFailure { err ->
                                    feedback = err.message ?: "吊销失败"
                                }
                        }
                    },
                ) {
                    Text("确认吊销", color = WinmuxColors.Danger)
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingRevokeDevice = null }) {
                    Text("取消", color = WinmuxColors.TextSecondary)
                }
            },
        )
    }
}
