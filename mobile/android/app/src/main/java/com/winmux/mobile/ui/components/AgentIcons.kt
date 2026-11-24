package com.winmux.mobile.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.winmux.mobile.ui.theme.WinmuxColors
import kotlin.math.cos
import kotlin.math.sin

@Composable
fun AgentBadgeIcon(
    agentKind: String,
    isRunning: Boolean = false,
    hasAttention: Boolean = false,
    size: Dp = 24.dp,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier.size(size),
        contentAlignment = Alignment.Center,
    ) {
        when (agentKind.lowercase()) {
            "claude" -> ClawdIcon(size = size * 0.85f)
            "codex" -> CodexIcon(size = size * 0.85f)
            "agy", "antigravity" -> AgyIcon(size = size * 0.82f)
            else -> TerminalGlyphIcon(size = size * 0.82f)
        }

        val dotColor = when {
            hasAttention -> WinmuxColors.Attention
            isRunning -> WinmuxColors.Running
            else -> null
        }
        if (dotColor != null) {
            Box(
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .size(7.dp)
                    .background(dotColor, CircleShape),
            )
        }
    }
}

@Composable
private fun ClawdIcon(size: Dp) {
    Canvas(modifier = Modifier.size(width = size, height = size * 0.78f)) {
        val ux = this.size.width / 11f
        val uy = this.size.height / 8f
        val body = Path().apply {
            moveTo(1f * ux, 0f)
            lineTo(10f * ux, 0f)
            lineTo(10f * ux, 2f * uy)
            lineTo(11f * ux, 2f * uy)
            lineTo(11f * ux, 4f * uy)
            lineTo(10f * ux, 4f * uy)
            lineTo(10f * ux, 8f * uy)
            lineTo(9f * ux, 8f * uy)
            lineTo(9f * ux, 6f * uy)
            lineTo(8f * ux, 6f * uy)
            lineTo(8f * ux, 8f * uy)
            lineTo(7f * ux, 8f * uy)
            lineTo(7f * ux, 6f * uy)
            lineTo(4f * ux, 6f * uy)
            lineTo(4f * ux, 8f * uy)
            lineTo(3f * ux, 8f * uy)
            lineTo(3f * ux, 6f * uy)
            lineTo(2f * ux, 6f * uy)
            lineTo(2f * ux, 8f * uy)
            lineTo(1f * ux, 8f * uy)
            lineTo(1f * ux, 4f * uy)
            lineTo(0f, 4f * uy)
            lineTo(0f, 2f * uy)
            lineTo(1f * ux, 2f * uy)
            close()
        }
        drawPath(body, color = WinmuxColors.Claude)
        drawRect(
            color = WinmuxColors.ClaudeEye,
            topLeft = Offset(2.1f * ux, 2.0f * uy),
            size = Size(1.2f * ux, 1.4f * uy),
        )
        drawRect(
            color = WinmuxColors.ClaudeEye,
            topLeft = Offset(7.7f * ux, 2.0f * uy),
            size = Size(1.2f * ux, 1.4f * uy),
        )
    }
}

@Composable
private fun CodexIcon(size: Dp) {
    Canvas(modifier = Modifier.size(size)) {
        val cx = this.size.width / 2f
        val cy = this.size.height / 2f
        val r = this.size.minDimension * 0.25f
        val stroke = Stroke(width = this.size.minDimension * 0.11f)
        for (i in 0 until 6) {
            val angle = Math.toRadians(i * 60.0 - 90.0)
            val px = cx + (r * 0.92f * cos(angle)).toFloat()
            val py = cy + (r * 0.92f * sin(angle)).toFloat()
            drawCircle(
                color = WinmuxColors.Codex,
                radius = r,
                center = Offset(px, py),
                style = stroke,
            )
        }
    }
}

@Composable
private fun AgyIcon(size: Dp) {
    Canvas(modifier = Modifier.size(size)) {
        val w = this.size.width
        val h = this.size.height
        val outer = Path().apply {
            moveTo(w * 0.50f, h * 0.08f)
            lineTo(w * 0.88f, h * 0.85f)
            lineTo(w * 0.70f, h * 0.85f)
            lineTo(w * 0.50f, h * 0.42f)
            lineTo(w * 0.30f, h * 0.85f)
            lineTo(w * 0.12f, h * 0.85f)
            close()
        }
        val inner = Path().apply {
            moveTo(w * 0.50f, h * 0.58f)
            lineTo(w * 0.60f, h * 0.78f)
            lineTo(w * 0.40f, h * 0.78f)
            close()
        }
        drawPath(outer, color = WinmuxColors.Agy)
        drawPath(inner, color = WinmuxColors.Agy)
    }
}

@Composable
private fun TerminalGlyphIcon(size: Dp) {
    Canvas(modifier = Modifier.size(size)) {
        val w = this.size.width
        val h = this.size.height
        val stroke = Stroke(width = w * 0.12f, cap = StrokeCap.Round)
        val chevron = Path().apply {
            moveTo(w * 0.18f, h * 0.28f)
            lineTo(w * 0.44f, h * 0.50f)
            lineTo(w * 0.18f, h * 0.72f)
        }
        drawPath(chevron, color = WinmuxColors.Terminal, style = stroke)
        drawLine(
            color = WinmuxColors.Terminal,
            start = Offset(w * 0.52f, h * 0.72f),
            end = Offset(w * 0.84f, h * 0.72f),
            strokeWidth = w * 0.12f,
            cap = StrokeCap.Round,
        )
    }
}
