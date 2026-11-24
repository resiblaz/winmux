package com.winmux.mobile.ui.components

import com.winmux.mobile.model.DecisionCard

enum class DecisionRiskLevel { HIGH, MEDIUM, LOW }

data class DecisionRiskAssessment(
    val level: DecisionRiskLevel,
    val label: String,
    val reason: String = "",
)

object DecisionRiskAnalyzer {
    private val highRiskPatterns: List<Pair<Regex, String>> = listOf(
        Regex("""\brm\s+-[a-zA-Z]*r[a-zA-Z]*f|\brm\s+-[a-zA-Z]*f[a-zA-Z]*r""", RegexOption.IGNORE_CASE) to "包含强制递归删除 (rm -rf)",
        Regex("""remove-item\b[^\n]*-recurse[^\n]*-force|remove-item\b[^\n]*-force[^\n]*-recurse""", RegexOption.IGNORE_CASE) to "包含强制递归删除 (Remove-Item)",
        Regex("""\b(del|erase)\s+/[^\n]*[fq]|\b(rd|rmdir)\s+/s""", RegexOption.IGNORE_CASE) to "包含批量强制删除命令",
        Regex("""\bgit\s+push\b[^\n]*(--force|-f\b|--force-with-lease)""", RegexOption.IGNORE_CASE) to "包含强制推送 (git push --force)",
        Regex("""\bgit\s+reset\s+--hard\b""", RegexOption.IGNORE_CASE) to "包含硬重置 (git reset --hard)",
        Regex("""\bgit\s+clean\s+-[a-zA-Z]*f""", RegexOption.IGNORE_CASE) to "包含清理未追踪文件 (git clean -f)",
        Regex("""\b(drop\s+table|drop\s+database|truncate\s+table)\b""", RegexOption.IGNORE_CASE) to "包含破坏性数据库操作",
        Regex("""\b(shutdown|restart-computer|format\s+[a-z]:|diskpart|reg\s+delete|bcdedit)\b""", RegexOption.IGNORE_CASE) to "包含系统级危险指令",
        Regex("""\b(curl|wget|irm)\b[^\n]*\|\s*(sh|bash|pwsh|powershell|iex)\b""", RegexOption.IGNORE_CASE) to "包含远程下载并直接执行脚本",
        Regex("""\bchmod\s+(-R\s+)?777\b""", RegexOption.IGNORE_CASE) to "包含全局 777 权限修改",
    )

    private val readOnlyTools = setOf("read", "glob", "grep", "ls", "list", "websearch", "webfetch", "search", "view")

    private val readOnlyCommandRegex = Regex(
        """^\s*(git\s+(status|diff|log|branch|show|rev-parse)|ls|dir|pwd|cat|head|tail|type|rg|findstr|select-string|get-childitem|get-content|which|where|whoami|echo)\b""",
        RegexOption.IGNORE_CASE,
    )

    fun assess(card: DecisionCard): DecisionRiskAssessment {
        if (card.kind == "question") {
            return DecisionRiskAssessment(DecisionRiskLevel.LOW, "交互确认")
        }
        val combined = buildString {
            append(card.toolName)
            append('\n')
            append(card.toolInput)
            append('\n')
            append(card.summary)
            append('\n')
            append(card.planText)
        }

        for ((regex, reason) in highRiskPatterns) {
            if (regex.containsMatchIn(combined)) {
                return DecisionRiskAssessment(DecisionRiskLevel.HIGH, "高危操作", reason)
            }
        }

        val toolLower = card.toolName.trim().lowercase()
        if (toolLower in readOnlyTools) {
            return DecisionRiskAssessment(DecisionRiskLevel.LOW, "只读操作")
        }
        if (card.toolInput.isNotBlank() && readOnlyCommandRegex.containsMatchIn(card.toolInput) &&
            !card.toolInput.contains(">") && !card.toolInput.contains(";") && !card.toolInput.contains("&&")
        ) {
            return DecisionRiskAssessment(DecisionRiskLevel.LOW, "只读命令")
        }
        if (card.kind == "exitPlan") {
            return DecisionRiskAssessment(DecisionRiskLevel.MEDIUM, "执行计划")
        }
        return DecisionRiskAssessment(DecisionRiskLevel.MEDIUM, "写/执行操作")
    }

    fun formatRelativeTime(epochMs: Long): String {
        if (epochMs <= 0L) return ""
        val diffSec = ((System.currentTimeMillis() - epochMs) / 1000L).coerceAtLeast(0L)
        return when {
            diffSec < 30L -> "刚刚"
            diffSec < 3600L -> "${diffSec / 60} 分钟前"
            diffSec < 86400L -> "${diffSec / 3600} 小时前"
            diffSec < 7 * 86400L -> "${diffSec / 86400} 天前"
            else -> "${diffSec / 86400} 天前"
        }
    }
}
