package com.winmux.mobile.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

object WinmuxColors {
    val Bg = Color(0xFF0F1117)
    val Surface = Color(0xFF171B24)
    val SurfaceElevated = Color(0xFF202634)
    val Border = Color(0xFF2B3242)
    val TextPrimary = Color(0xFFF3F5F8)
    val TextSecondary = Color(0xFF9BA4B5)
    val Accent = Color(0xFF4F8CFF)
    val Running = Color(0xFF22C55E)
    val Attention = Color(0xFFF59E0B)
    val Danger = Color(0xFFEF4444)
    val Claude = Color(0xFFD97757)
    val ClaudeEye = Color(0xFF141210)
    val Codex = Color(0xFF10A37F)
    val Agy = Color(0xFF7FA3FF)
    val Terminal = Color(0xFF8B95A8)
}

private val WinmuxDarkScheme = darkColorScheme(
    primary = WinmuxColors.Accent,
    onPrimary = Color.White,
    secondary = WinmuxColors.Codex,
    onSecondary = Color.White,
    background = WinmuxColors.Bg,
    onBackground = WinmuxColors.TextPrimary,
    surface = WinmuxColors.Surface,
    onSurface = WinmuxColors.TextPrimary,
    surfaceVariant = WinmuxColors.SurfaceElevated,
    onSurfaceVariant = WinmuxColors.TextSecondary,
    outline = WinmuxColors.Border,
    error = WinmuxColors.Danger,
)

@Composable
fun WinmuxTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = WinmuxDarkScheme,
        content = content,
    )
}
