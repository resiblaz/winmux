package com.winmux.mobile.ui.components

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withStyle

object AnsiTerminalParser {
    private val defaultFg = Color(0xFFE5E9F0)
    private val defaultBg = Color(0xFF0A0C10)

    private val standardColors = listOf(
        Color(0xFF3B4252), // 0 Black (lifted slightly for dark bg legibility)
        Color(0xFFEF4444), // 1 Red
        Color(0xFF22C55E), // 2 Green
        Color(0xFFEAB308), // 3 Yellow
        Color(0xFF60A5FA), // 4 Blue
        Color(0xFFC084FC), // 5 Magenta
        Color(0xFF22D3EE), // 6 Cyan
        Color(0xFFE5E9F0), // 7 White
    )

    private val brightColors = listOf(
        Color(0xFF64748B), // 8 Bright Black
        Color(0xFFF87171), // 9 Bright Red
        Color(0xFF4ADE80), // 10 Bright Green
        Color(0xFFFDE047), // 11 Bright Yellow
        Color(0xFF93C5FD), // 12 Bright Blue
        Color(0xFFD8B4FE), // 13 Bright Magenta
        Color(0xFF67E8F9), // 14 Bright Cyan
        Color(0xFFFFFFFF), // 15 Bright White
    )

    private val promptRegex = Regex("""^((?:PS\s+[^>]+>|[A-Za-z]:\\[^>]*>|[^\s]+@[^\s]+:[^$#]*[$#])\s*)(.*)$""")

    fun parse(raw: String): AnnotatedString {
        if (raw.isEmpty()) return AnnotatedString("")
        return if (raw.indexOf('\u001B') >= 0) {
            parseAnsi(raw)
        } else {
            highlightPlainTerminal(raw)
        }
    }

    private fun parseAnsi(raw: String): AnnotatedString = buildAnnotatedString {
        var fg: Color? = null
        var bg: Color? = null
        var bold = false
        var dim = false
        var italic = false
        var underline = false
        var strike = false
        var inverse = false

        fun currentStyle(): SpanStyle {
            val resolvedFg = if (inverse) (bg ?: defaultBg) else (fg ?: defaultFg)
            val resolvedBg = if (inverse) (fg ?: defaultFg) else bg
            val finalFg = if (dim) resolvedFg.copy(alpha = 0.62f) else resolvedFg
            val deco = when {
                underline && strike -> TextDecoration.combine(listOf(TextDecoration.Underline, TextDecoration.LineThrough))
                underline -> TextDecoration.Underline
                strike -> TextDecoration.LineThrough
                else -> TextDecoration.None
            }
            return SpanStyle(
                color = finalFg,
                background = resolvedBg ?: Color.Unspecified,
                fontWeight = if (bold) FontWeight.Bold else FontWeight.Normal,
                fontStyle = if (italic) FontStyle.Italic else FontStyle.Normal,
                textDecoration = deco,
            )
        }

        val chunk = StringBuilder()
        fun flush() {
            if (chunk.isNotEmpty()) {
                withStyle(currentStyle()) { append(chunk.toString()) }
                chunk.setLength(0)
            }
        }

        var i = 0
        val n = raw.length
        while (i < n) {
            val c = raw[i]
            if (c == '\u001B' && i + 1 < n) {
                val next = raw[i + 1]
                when (next) {
                    '[' -> {
                        // CSI sequence: ESC [ params finalByte
                        var j = i + 2
                        while (j < n && raw[j].code !in 0x40..0x7E) {
                            j++
                        }
                        if (j < n) {
                            val finalByte = raw[j]
                            if (finalByte == 'm') {
                                flush()
                                val paramStr = raw.substring(i + 2, j)
                                val codes = if (paramStr.isEmpty()) listOf(0)
                                else paramStr.split(';').map { it.toIntOrNull() ?: 0 }
                                var k = 0
                                while (k < codes.size) {
                                    when (val code = codes[k]) {
                                        0 -> {
                                            fg = null; bg = null; bold = false; dim = false
                                            italic = false; underline = false; strike = false; inverse = false
                                        }
                                        1 -> bold = true
                                        2 -> dim = true
                                        3 -> italic = true
                                        4 -> underline = true
                                        7 -> inverse = true
                                        9 -> strike = true
                                        22 -> { bold = false; dim = false }
                                        23 -> italic = false
                                        24 -> underline = false
                                        27 -> inverse = false
                                        29 -> strike = false
                                        in 30..37 -> fg = standardColors[code - 30]
                                        39 -> fg = null
                                        in 40..47 -> bg = standardColors[code - 40]
                                        49 -> bg = null
                                        in 90..97 -> fg = brightColors[code - 90]
                                        in 100..107 -> bg = brightColors[code - 100]
                                        38, 48 -> {
                                            val isFg = code == 38
                                            if (k + 2 < codes.size && codes[k + 1] == 5) {
                                                val color256 = colorFrom256(codes[k + 2])
                                                if (isFg) fg = color256 else bg = color256
                                                k += 2
                                            } else if (k + 4 < codes.size && codes[k + 1] == 2) {
                                                val rgb = Color(
                                                    red = codes[k + 2].coerceIn(0, 255),
                                                    green = codes[k + 3].coerceIn(0, 255),
                                                    blue = codes[k + 4].coerceIn(0, 255),
                                                )
                                                if (isFg) fg = rgb else bg = rgb
                                                k += 4
                                            }
                                        }
                                    }
                                    k++
                                }
                            }
                            i = j + 1
                            continue
                        }
                    }
                    ']' -> {
                        // OSC sequence: terminated by BEL (\u0007) or ST (ESC \)
                        var j = i + 2
                        while (j < n) {
                            if (raw[j] == '\u0007') { j++; break }
                            if (raw[j] == '\u001B' && j + 1 < n && raw[j + 1] == '\\') { j += 2; break }
                            j++
                        }
                        i = j
                        continue
                    }
                    else -> {
                        i += 2
                        continue
                    }
                }
            }
            if (c != '\r') {
                chunk.append(c)
            }
            i++
        }
        flush()
    }

    private fun colorFrom256(idx: Int): Color {
        val i = idx.coerceIn(0, 255)
        if (i < 8) return standardColors[i]
        if (i < 16) return brightColors[i - 8]
        if (i < 232) {
            val cube = i - 16
            val r = cube / 36
            val g = (cube % 36) / 6
            val b = cube % 6
            val levels = intArrayOf(0, 95, 135, 175, 215, 255)
            return Color(levels[r], levels[g], levels[b])
        }
        val gray = 8 + (i - 232) * 10
        return Color(gray, gray, gray)
    }

    private fun highlightPlainTerminal(raw: String): AnnotatedString = buildAnnotatedString {
        val lines = raw.replace("\r\n", "\n").replace("\r", "\n").split('\n')
        lines.forEachIndexed { idx, line ->
            val trimmed = line.trimStart()
            val promptMatch = promptRegex.matchEntire(line)
            when {
                promptMatch != null -> {
                    withStyle(SpanStyle(color = Color(0xFF60A5FA), fontWeight = FontWeight.SemiBold)) {
                        append(promptMatch.groupValues[1])
                    }
                    withStyle(SpanStyle(color = Color(0xFFF8FAFC))) {
                        append(promptMatch.groupValues[2])
                    }
                }
                trimmed.startsWith("@@") && trimmed.contains("@@") -> {
                    withStyle(SpanStyle(color = Color(0xFF38BDF8))) { append(line) }
                }
                line.startsWith("+") && !line.startsWith("+++") -> {
                    withStyle(SpanStyle(color = Color(0xFF4ADE80), background = Color(0x1A22C55E))) { append(line) }
                }
                line.startsWith("-") && !line.startsWith("---") -> {
                    withStyle(SpanStyle(color = Color(0xFFF87171), background = Color(0x1AEF4444))) { append(line) }
                }
                containsErrorMarker(trimmed) -> {
                    withStyle(SpanStyle(color = Color(0xFFF87171))) { append(line) }
                }
                containsSuccessMarker(trimmed) -> {
                    withStyle(SpanStyle(color = Color(0xFF4ADE80))) { append(line) }
                }
                containsWarningMarker(trimmed) -> {
                    withStyle(SpanStyle(color = Color(0xFFFBBF24))) { append(line) }
                }
                else -> {
                    withStyle(SpanStyle(color = defaultFg)) { append(line) }
                }
            }
            if (idx < lines.lastIndex) append('\n')
        }
    }

    private fun containsErrorMarker(s: String): Boolean {
        val lower = s.lowercase()
        return s.startsWith("✖") ||
            s.startsWith("✗") ||
            lower.startsWith("error:") ||
            lower.startsWith("fatal:") ||
            lower.startsWith("exception:") ||
            lower.contains(" error c") ||
            lower.contains(" failed") ||
            lower.contains("build failed") ||
            lower.contains("[failed]")
    }

    private fun containsSuccessMarker(s: String): Boolean {
        val lower = s.lowercase()
        return s.startsWith("✔") ||
            s.startsWith("✓") ||
            s.startsWith("PASS:") ||
            lower.contains("build successful") ||
            lower.contains("[passed]") ||
            lower.contains("gate passed")
    }

    private fun containsWarningMarker(s: String): Boolean {
        val lower = s.lowercase()
        return s.startsWith("⚠") ||
            lower.startsWith("warning:") ||
            lower.startsWith("warn:") ||
            lower.contains(" warning ")
    }
}
