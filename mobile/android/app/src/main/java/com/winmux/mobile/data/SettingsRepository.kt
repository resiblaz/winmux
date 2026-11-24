package com.winmux.mobile.data

import android.content.Context
import android.net.Uri
import android.os.Build
import com.winmux.mobile.model.BridgeConfig

data class ParsedPairUri(
    val lanUrl: String,
    val remoteUrl: String,
    val code: String,
    val hostname: String,
)

class SettingsRepository(context: Context) {
    private val prefs = context.getSharedPreferences("winmux_bridge_prefs", Context.MODE_PRIVATE)

    fun loadConfig(): BridgeConfig {
        return BridgeConfig(
            lanUrl = prefs.getString("lan_url", "") ?: "",
            remoteUrl = prefs.getString("remote_url", "") ?: "",
            token = prefs.getString("token", "") ?: "",
            hostname = prefs.getString("hostname", "") ?: "",
            deviceName = prefs.getString("device_name", Build.MODEL ?: "Android") ?: "Android",
        )
    }

    fun saveConfig(config: BridgeConfig) {
        prefs.edit()
            .putString("lan_url", config.lanUrl.trim().trimEnd('/'))
            .putString("remote_url", config.remoteUrl.trim().trimEnd('/'))
            .putString("token", config.token.trim())
            .putString("hostname", config.hostname.trim())
            .putString("device_name", config.deviceName.trim())
            .apply()
    }

    fun clearPairing() {
        prefs.edit().remove("token").apply()
    }

    companion object {
        fun parsePairUri(raw: String): ParsedPairUri? {
            val trimmed = raw.trim()
            if (!trimmed.startsWith("winmux://pair", ignoreCase = true)) return null
            val uri = runCatching { Uri.parse(trimmed) }.getOrNull() ?: return null
            return ParsedPairUri(
                lanUrl = uri.getQueryParameter("lan")?.trim().orEmpty(),
                remoteUrl = uri.getQueryParameter("remote")?.trim().orEmpty(),
                code = uri.getQueryParameter("code")?.trim().orEmpty(),
                hostname = uri.getQueryParameter("host")?.trim().orEmpty(),
            )
        }
    }
}
