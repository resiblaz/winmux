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
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedButton
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.winmux.mobile.data.SettingsRepository
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.BridgeConfig
import com.winmux.mobile.model.ConnectionState
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
    var lanUrl by remember(config.lanUrl) { mutableStateOf(config.lanUrl.ifBlank { "http://192.168.1.100:47710" }) }
    var remoteUrl by remember(config.remoteUrl) { mutableStateOf(config.remoteUrl) }
    var pairingCode by remember { mutableStateOf("") }
    var deviceName by remember(config.deviceName) { mutableStateOf(config.deviceName.ifBlank { Build.MODEL ?: "Android" }) }
    var busy by remember { mutableStateOf(false) }
    var feedback by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(incomingPairUri) {
        if (!incomingPairUri.isNullOrBlank()) {
            SettingsRepository.parsePairUri(incomingPairUri)?.let { parsed ->
                if (parsed.lanUrl.isNotBlank()) lanUrl = parsed.lanUrl
                if (parsed.remoteUrl.isNotBlank()) remoteUrl = parsed.remoteUrl
                if (parsed.code.isNotBlank()) pairingCode = parsed.code
                pairUriInput = incomingPairUri
                feedback = "已解析配对链接，点击下方「完成配对」即可连接"
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
                        Text("远程穿透：${config.remoteUrl}", color = WinmuxColors.TextSecondary, fontSize = 12.sp, fontFamily = FontFamily.Monospace)
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
                            if (parsed.code.isNotBlank()) pairingCode = parsed.code
                            feedback = "已自动填充地址与配对码"
                        }
                    },
                    label = { Text("一键粘贴 winmux://pair?...（可选）") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                OutlinedTextField(
                    value = lanUrl,
                    onValueChange = { lanUrl = it },
                    label = { Text("局域网地址（在家 Wi-Fi 优先）") },
                    placeholder = { Text("http://192.168.1.100:47710") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )

                OutlinedTextField(
                    value = remoteUrl,
                    onValueChange = { remoteUrl = it },
                    label = { Text("远程地址（出门 5G 自动切换，如 Tailscale IP）") },
                    placeholder = { Text("http://100.x.y.z:47710") },
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
                                )
                                WinmuxRepository.updateConfig(
                                    BridgeConfig(
                                        lanUrl = res.lanUrl.ifBlank { lanUrl.trim() },
                                        remoteUrl = res.remoteUrl.ifBlank { remoteUrl.trim() },
                                        token = res.token,
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

        // Remote access guide
        Card(
            colors = CardDefaults.cardColors(containerColor = WinmuxColors.Surface),
            border = BorderStroke(1.dp, WinmuxColors.Border),
            shape = RoundedCornerShape(12.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Column(modifier = Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("出门 5G 远程连回本机说明", fontWeight = FontWeight.SemiBold, color = WinmuxColors.TextPrimary, fontSize = 14.sp)
                Text(
                    "1. 电脑与手机分别安装 Tailscale（个人免费，零公网暴露）。\n" +
                        "2. 电脑端 Bridge 会自动识别 Tailscale 100.x.y.z 地址并写入配对链接。\n" +
                        "3. 在家优先走局域网毫秒级直连；出门切 5G 后 App 后台服务自动无感切换到 Tailscale。",
                    color = WinmuxColors.TextSecondary,
                    fontSize = 12.sp,
                    lineHeight = 18.sp,
                )
            }
        }
    }
}
