package com.winmux.mobile.model

import org.json.JSONArray
import org.json.JSONObject

data class BridgeConfig(
    val lanUrl: String = "",
    val remoteUrl: String = "",
    val token: String = "",
    val certFingerprint: String = "",
    val hostname: String = "",
    val deviceName: String = "Android",
) {
    val isPaired: Boolean
        get() = token.isNotBlank() && (lanUrl.isNotBlank() || remoteUrl.isNotBlank())
}

sealed class ConnectionState {
    data object Disconnected : ConnectionState()
    data class Connecting(val target: String) : ConnectionState()
    data class Connected(val endpointLabel: String, val baseUrl: String) : ConnectionState()
    data class Error(val message: String) : ConnectionState()
}

data class PermissionsInfo(
    val sendInput: Boolean = false,
    val readContent: Boolean = false,
)

data class PaneInfo(
    val id: Int,
    val surfaceId: String,
    val workspaceId: String,
    val active: Boolean,
    val connectionClosed: Boolean,
    val hasAttention: Boolean,
    val title: String,
    val readOnly: Boolean,
    val profile: String,
    val workingDirectory: String,
    val hasActiveChild: Boolean,
    val activeProcessName: String,
    val agentKind: String, // "claude" | "codex" | "agy" | "terminal"
    val runtimeState: String = "idle", // "idle" | "working" | "waiting" | "closed"
    val runningSinceMs: Long? = null,
)

data class SystemStatusInfo(
    val hostname: String = "",
    val cpuPercent: Double = 0.0,
    val cpuCores: Int = 0,
    val memoryUsedMb: Int = 0,
    val memoryTotalMb: Int = 0,
    val memoryPercent: Double = 0.0,
    val bridgeUptimeSec: Long = 0L,
    val winmuxConnected: Boolean = false,
    val connectedClients: Int = 0,
    val pendingDecisions: Int = 0,
    val totalWorkspaces: Int = 0,
    val totalPanes: Int = 0,
    val workingPanes: Int = 0,
    val waitingPanes: Int = 0,
    val idlePanes: Int = 0,
)

data class AgentModelConfig(
    val agent: String,
    val currentModel: String = "",
    val reasoningEffort: String = "",
    val availableReasoningEfforts: List<String> = emptyList(),
    val availableModels: List<String> = emptyList(),
)

data class AllModelsConfig(
    val claude: AgentModelConfig = AgentModelConfig("claude"),
    val codex: AgentModelConfig = AgentModelConfig("codex"),
    val agy: AgentModelConfig = AgentModelConfig("agy"),
    val updatedAt: Long = 0L,
)

data class TokenBucket(
    val inputTokens: Long = 0L,
    val outputTokens: Long = 0L,
    val cacheReadTokens: Long = 0L,
    val totalTokens: Long = 0L,
    val turnsOrSessions: Int = 0,
)

data class RateLimitWindow(
    val usedPercent: Double = 0.0,
    val windowMinutes: Int = 0,
    val resetsAt: String = "",
)

data class AgentUsageInfo(
    val agent: String,
    val currentModel: String = "",
    val reasoningEffort: String = "",
    val today: TokenBucket = TokenBucket(),
    val week: TokenBucket = TokenBucket(),
    val latestSessionTokens: Long = 0L,
    val latestSessionModel: String = "",
    val primaryLimit: RateLimitWindow? = null,
    val secondaryLimit: RateLimitWindow? = null,
)

data class AllUsageSummary(
    val claude: AgentUsageInfo = AgentUsageInfo("claude"),
    val codex: AgentUsageInfo = AgentUsageInfo("codex"),
    val agy: AgentUsageInfo = AgentUsageInfo("agy"),
    val updatedAt: Long = 0L,
)

data class WorkspaceInfo(
    val id: String,
    val index: Int,
    val title: String,
    val selected: Boolean,
    val pinned: Boolean,
    val surfaceCount: Int,
    val unreadCount: Int,
    val workingDirectory: String,
    val gitBranch: String,
    val listeningPorts: List<Int>,
    val panes: List<PaneInfo>,
) {
    val hasRunning: Boolean
        get() = listeningPorts.isNotEmpty() || panes.any { it.hasActiveChild }
    val hasAttention: Boolean
        get() = unreadCount > 0 || panes.any { it.hasAttention }
}

data class BridgeSnapshot(
    val connected: Boolean = false,
    val error: String = "",
    val message: String = "",
    val permissions: PermissionsInfo = PermissionsInfo(),
    val workspaces: List<WorkspaceInfo> = emptyList(),
    val systemStatus: SystemStatusInfo = SystemStatusInfo(),
    val updatedAt: Long = 0L,
)

data class DecisionOption(
    val id: String,
    val label: String,
    val style: String, // "primary" | "default" | "danger"
)

data class QuestionOption(
    val label: String,
    val description: String,
)

data class QuestionItem(
    val header: String,
    val prompt: String,
    val multiSelect: Boolean,
    val options: List<QuestionOption>,
)

data class DecisionResolution(
    val decision: String,
    val answers: List<String>,
    val label: String,
    val color: String, // "green" | "red" | "grey"
)

data class DecisionCard(
    val id: String,
    val kind: String, // "permissionRequest" | "exitPlan" | "question" | "launchCheck"
    val status: String, // "pending" | "resolved" | "expired"
    val agent: String,
    val workspaceId: String,
    val surfaceId: String,
    val paneId: Int?,
    val workspaceTitle: String,
    val title: String,
    val summary: String,
    val toolName: String,
    val toolInput: String,
    val planText: String,
    val questions: List<QuestionItem>,
    val options: List<DecisionOption>,
    val resolution: DecisionResolution?,
    val createdAt: Long,
)

data class BridgeNotification(
    val id: String,
    val agent: String,
    val workspaceId: String,
    val surfaceId: String,
    val paneId: Int?,
    val workspaceTitle: String,
    val title: String,
    val body: String,
    val createdAt: Long,
)

data class ActiveTerminalTarget(
    val workspaceId: String,
    val workspaceTitle: String,
    val surfaceId: String,
    val paneId: Int,
    val paneTitle: String,
    val agentKind: String,
)

data class PairedDeviceInfo(
    val id: String,
    val name: String,
    val createdAt: Long,
    val lastSeenAt: Long,
)

object ModelParsers {
    fun parseSnapshot(obj: JSONObject?): BridgeSnapshot {
        if (obj == null) return BridgeSnapshot()
        val permObj = obj.optJSONObject("permissions")
        val permissions = PermissionsInfo(
            sendInput = permObj?.optBoolean("sendInput", false) ?: false,
            readContent = permObj?.optBoolean("readContent", false) ?: false,
        )
        val wsArray = obj.optJSONArray("workspaces") ?: JSONArray()
        val workspaces = buildList {
            for (i in 0 until wsArray.length()) {
                val w = wsArray.optJSONObject(i) ?: continue
                val wsId = w.optString("id")
                val portsArray = w.optJSONArray("listeningPorts") ?: JSONArray()
                val ports = buildList {
                    for (p in 0 until portsArray.length()) {
                        val port = portsArray.optInt(p, 0)
                        if (port > 0) add(port)
                    }
                }
                val panesArray = w.optJSONArray("panes") ?: JSONArray()
                val panes = buildList {
                    for (j in 0 until panesArray.length()) {
                        val p = panesArray.optJSONObject(j) ?: continue
                        add(
                            PaneInfo(
                                id = p.optInt("id", 0),
                                surfaceId = p.optString("surfaceId"),
                                workspaceId = wsId,
                                active = p.optBoolean("active", false),
                                connectionClosed = p.optBoolean("connectionClosed", false),
                                hasAttention = p.optBoolean("hasAttention", false),
                                title = p.optString("title", "Terminal"),
                                readOnly = p.optBoolean("readOnly", false),
                                profile = p.optString("profile"),
                                workingDirectory = p.optString("workingDirectory"),
                                hasActiveChild = p.optBoolean("hasActiveChild", false),
                                activeProcessName = p.optString("activeProcessName"),
                                agentKind = p.optString("agentKind", "terminal"),
                                runtimeState = p.optString("runtimeState", if (p.optBoolean("hasActiveChild", false)) "working" else "idle"),
                                runningSinceMs = if (p.isNull("runningSinceMs")) null else p.optLong("runningSinceMs", 0L).takeIf { it > 0L },
                            )
                        )
                    }
                }
                add(
                    WorkspaceInfo(
                        id = wsId,
                        index = w.optInt("index", i),
                        title = w.optString("title", "Workspace"),
                        selected = w.optBoolean("selected", false),
                        pinned = w.optBoolean("pinned", false),
                        surfaceCount = w.optInt("surfaceCount", panes.size),
                        unreadCount = w.optInt("unreadCount", 0),
                        workingDirectory = w.optString("workingDirectory"),
                        gitBranch = w.optString("gitBranch"),
                        listeningPorts = ports,
                        panes = panes,
                    )
                )
            }
        }
        val sysObj = obj.optJSONObject("systemStatus")
        val sysStatus = if (sysObj != null) {
            SystemStatusInfo(
                hostname = sysObj.optString("hostname"),
                cpuPercent = sysObj.optDouble("cpuPercent", 0.0),
                cpuCores = sysObj.optInt("cpuCores", 0),
                memoryUsedMb = sysObj.optInt("memoryUsedMb", 0),
                memoryTotalMb = sysObj.optInt("memoryTotalMb", 0),
                memoryPercent = sysObj.optDouble("memoryPercent", 0.0),
                bridgeUptimeSec = sysObj.optLong("bridgeUptimeSec", 0L),
                winmuxConnected = sysObj.optBoolean("winmuxConnected", obj.optBoolean("connected", false)),
                connectedClients = sysObj.optInt("connectedClients", 0),
                pendingDecisions = sysObj.optInt("pendingDecisions", 0),
                totalWorkspaces = sysObj.optInt("totalWorkspaces", workspaces.size),
                totalPanes = sysObj.optInt("totalPanes", workspaces.sumOf { it.panes.size }),
                workingPanes = sysObj.optInt("workingPanes", 0),
                waitingPanes = sysObj.optInt("waitingPanes", 0),
                idlePanes = sysObj.optInt("idlePanes", 0),
            )
        } else {
            SystemStatusInfo(winmuxConnected = obj.optBoolean("connected", false))
        }
        return BridgeSnapshot(
            connected = obj.optBoolean("connected", false),
            error = obj.optString("error"),
            message = obj.optString("message"),
            permissions = permissions,
            workspaces = workspaces,
            systemStatus = sysStatus,
            updatedAt = obj.optLong("updatedAt", System.currentTimeMillis()),
        )
    }

    fun parseModelsConfig(obj: JSONObject?): AllModelsConfig {
        if (obj == null) return AllModelsConfig()
        fun parseOne(name: String): AgentModelConfig {
            val o = obj.optJSONObject(name) ?: return AgentModelConfig(name)
            val modelsArr = o.optJSONArray("availableModels") ?: JSONArray()
            val effortsArr = o.optJSONArray("availableReasoningEfforts") ?: JSONArray()
            return AgentModelConfig(
                agent = name,
                currentModel = o.optString("currentModel"),
                reasoningEffort = o.optString("reasoningEffort"),
                availableModels = List(modelsArr.length()) { modelsArr.optString(it) }.filter(String::isNotBlank),
                availableReasoningEfforts = List(effortsArr.length()) { effortsArr.optString(it) }.filter(String::isNotBlank),
            )
        }
        return AllModelsConfig(
            claude = parseOne("claude"),
            codex = parseOne("codex"),
            agy = parseOne("agy"),
            updatedAt = obj.optLong("updatedAt", System.currentTimeMillis()),
        )
    }

    fun parseUsageSummary(obj: JSONObject?): AllUsageSummary {
        if (obj == null) return AllUsageSummary()
        fun parseBucket(b: JSONObject?): TokenBucket {
            if (b == null) return TokenBucket()
            return TokenBucket(
                inputTokens = b.optLong("inputTokens", 0L),
                outputTokens = b.optLong("outputTokens", 0L),
                cacheReadTokens = b.optLong("cacheReadTokens", 0L),
                totalTokens = b.optLong("totalTokens", 0L),
                turnsOrSessions = b.optInt("turns", b.optInt("sessions", 0)),
            )
        }
        fun parseWindow(w: JSONObject?): RateLimitWindow? {
            if (w == null) return null
            return RateLimitWindow(
                usedPercent = w.optDouble("usedPercent", 0.0),
                windowMinutes = w.optInt("windowMinutes", 0),
                resetsAt = if (w.isNull("resetsAt")) "" else w.optString("resetsAt"),
            )
        }
        fun parseOne(name: String): AgentUsageInfo {
            val o = obj.optJSONObject(name) ?: return AgentUsageInfo(name)
            val latest = o.optJSONObject("latestSession")
            val limits = o.optJSONObject("rateLimits")
            return AgentUsageInfo(
                agent = name,
                currentModel = o.optString("currentModel"),
                reasoningEffort = o.optString("reasoningEffort"),
                today = parseBucket(o.optJSONObject("today")),
                week = parseBucket(o.optJSONObject("week")),
                latestSessionTokens = latest?.optLong("totalTokens", 0L) ?: 0L,
                latestSessionModel = latest?.optString("model").orEmpty(),
                primaryLimit = parseWindow(limits?.optJSONObject("primary")),
                secondaryLimit = parseWindow(limits?.optJSONObject("secondary")),
            )
        }
        return AllUsageSummary(
            claude = parseOne("claude"),
            codex = parseOne("codex"),
            agy = parseOne("agy"),
            updatedAt = obj.optLong("updatedAt", System.currentTimeMillis()),
        )
    }

    fun parseDecision(obj: JSONObject): DecisionCard {
        val optionsArray = obj.optJSONArray("options") ?: JSONArray()
        val options = buildList {
            for (i in 0 until optionsArray.length()) {
                val o = optionsArray.optJSONObject(i) ?: continue
                add(
                    DecisionOption(
                        id = o.optString("id"),
                        label = o.optString("label"),
                        style = o.optString("style", "default"),
                    )
                )
            }
        }

        val questionsArray = obj.optJSONArray("questions") ?: JSONArray()
        val questions = buildList {
            for (i in 0 until questionsArray.length()) {
                val q = questionsArray.optJSONObject(i) ?: continue
                val qOpts = q.optJSONArray("options") ?: JSONArray()
                val parsedOpts = buildList {
                    for (j in 0 until qOpts.length()) {
                        val qo = qOpts.optJSONObject(j) ?: continue
                        add(
                            QuestionOption(
                                label = qo.optString("label"),
                                description = qo.optString("description"),
                            )
                        )
                    }
                }
                add(
                    QuestionItem(
                        header = q.optString("header"),
                        prompt = q.optString("prompt"),
                        multiSelect = q.optBoolean("multiSelect", false),
                        options = parsedOpts,
                    )
                )
            }
        }

        val resObj = obj.optJSONObject("resolution")
        val resolution = resObj?.let { r ->
            val ansArr = r.optJSONArray("answers") ?: JSONArray()
            val ansList = buildList {
                for (i in 0 until ansArr.length()) add(ansArr.optString(i))
            }
            DecisionResolution(
                decision = r.optString("decision"),
                answers = ansList,
                label = r.optString("label"),
                color = r.optString("color", "green"),
            )
        }

        val paneId = if (obj.isNull("paneId")) null else obj.optInt("paneId", -1).takeIf { it >= 0 }
        return DecisionCard(
            id = obj.optString("id"),
            kind = obj.optString("kind", "permissionRequest"),
            status = obj.optString("status", "pending"),
            agent = obj.optString("agent", "claude"),
            workspaceId = obj.optString("workspaceId"),
            surfaceId = obj.optString("surfaceId"),
            paneId = paneId,
            workspaceTitle = obj.optString("workspaceTitle"),
            title = obj.optString("title", "待处理决策"),
            summary = obj.optString("summary"),
            toolName = obj.optString("toolName"),
            toolInput = obj.optString("toolInput"),
            planText = obj.optString("planText"),
            questions = questions,
            options = options,
            resolution = resolution,
            createdAt = obj.optLong("createdAt", System.currentTimeMillis()),
        )
    }

    fun parseDecisions(arr: JSONArray?): List<DecisionCard> {
        if (arr == null) return emptyList()
        return buildList {
            for (i in 0 until arr.length()) {
                arr.optJSONObject(i)?.let { add(parseDecision(it)) }
            }
        }
    }

    fun parseNotification(obj: JSONObject): BridgeNotification {
        val paneId = if (obj.isNull("paneId")) null else obj.optInt("paneId", -1).takeIf { it >= 0 }
        return BridgeNotification(
            id = obj.optString("id"),
            agent = obj.optString("agent", "claude"),
            workspaceId = obj.optString("workspaceId"),
            surfaceId = obj.optString("surfaceId"),
            paneId = paneId,
            workspaceTitle = obj.optString("workspaceTitle"),
            title = obj.optString("title", "Agent 更新"),
            body = obj.optString("body"),
            createdAt = obj.optLong("createdAt", System.currentTimeMillis()),
        )
    }

    fun parseNotifications(arr: JSONArray?): List<BridgeNotification> {
        if (arr == null) return emptyList()
        return buildList {
            for (i in 0 until arr.length()) {
                arr.optJSONObject(i)?.let { add(parseNotification(it)) }
            }
        }
    }

    fun parseDevices(arr: JSONArray?): List<PairedDeviceInfo> {
        if (arr == null) return emptyList()
        return buildList {
            for (i in 0 until arr.length()) {
                val d = arr.optJSONObject(i) ?: continue
                add(
                    PairedDeviceInfo(
                        id = d.optString("id"),
                        name = d.optString("name", "Android"),
                        createdAt = d.optLong("createdAt", 0L),
                        lastSeenAt = d.optLong("lastSeenAt", 0L),
                    )
                )
            }
        }
    }
}
