package com.winmux.mobile.data

import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.AllUsageSummary
import com.winmux.mobile.model.BridgeConfig
import com.winmux.mobile.model.BridgeNotification
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.DecisionCard
import com.winmux.mobile.model.ModelParsers
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.net.URLEncoder
import java.util.concurrent.TimeUnit

data class ResolvedEndpoint(
    val baseUrl: String,
    val label: String,
)

data class PairResponse(
    val token: String,
    val deviceId: String,
    val lanUrl: String,
    val remoteUrl: String,
    val hostname: String,
)

data class SnapshotPayload(
    val snapshot: BridgeSnapshot,
    val decisions: List<DecisionCard>,
    val notifications: List<BridgeNotification>,
)

class WinmuxApiClient {
    private val jsonMediaType = "application/json; charset=utf-8".toMediaType()

    private val probeClient = OkHttpClient.Builder()
        .connectTimeout(2500, TimeUnit.MILLISECONDS)
        .readTimeout(2500, TimeUnit.MILLISECONDS)
        .writeTimeout(2500, TimeUnit.MILLISECONDS)
        .build()

    private val apiClient = OkHttpClient.Builder()
        .connectTimeout(6, TimeUnit.SECONDS)
        .readTimeout(12, TimeUnit.SECONDS)
        .writeTimeout(10, TimeUnit.SECONDS)
        .pingInterval(25, TimeUnit.SECONDS)
        .build()

    @Volatile
    private var activeEndpoint: ResolvedEndpoint? = null

    fun currentEndpoint(): ResolvedEndpoint? = activeEndpoint

    fun candidateEndpoints(config: BridgeConfig): List<ResolvedEndpoint> {
        val list = mutableListOf<ResolvedEndpoint>()
        if (config.lanUrl.isNotBlank()) {
            list.add(ResolvedEndpoint(config.lanUrl.trimEnd('/'), "局域网"))
        }
        if (config.remoteUrl.isNotBlank() && config.remoteUrl.trimEnd('/') != config.lanUrl.trimEnd('/')) {
            list.add(ResolvedEndpoint(config.remoteUrl.trimEnd('/'), "远程穿透"))
        }
        return list
    }

    suspend fun resolveReachableEndpoint(config: BridgeConfig): ResolvedEndpoint = withContext(Dispatchers.IO) {
        val candidates = candidateEndpoints(config)
        if (candidates.isEmpty()) {
            throw IOException("尚未配置电脑端 Bridge 地址")
        }
        var lastError: Exception? = null
        for (candidate in candidates) {
            try {
                val req = Request.Builder().url("${candidate.baseUrl}/api/health").get().build()
                probeClient.newCall(req).execute().use { res ->
                    if (res.isSuccessful) {
                        activeEndpoint = candidate
                        return@withContext candidate
                    }
                }
            } catch (e: Exception) {
                lastError = e
            }
        }
        throw IOException("无法连接电脑端 Winmux Bridge (${lastError?.message ?: "请检查网络或 Tailscale"})")
    }

    suspend fun pair(lanUrl: String, remoteUrl: String, code: String, deviceName: String): PairResponse =
        withContext(Dispatchers.IO) {
            val tempConfig = BridgeConfig(lanUrl = lanUrl, remoteUrl = remoteUrl, deviceName = deviceName)
            val endpoint = resolveReachableEndpoint(tempConfig)
            val bodyJson = JSONObject()
                .put("code", code.trim())
                .put("deviceName", deviceName.ifBlank { "Android" })
            val req = Request.Builder()
                .url("${endpoint.baseUrl}/api/pair")
                .post(bodyJson.toString().toRequestBody(jsonMediaType))
                .build()

            apiClient.newCall(req).execute().use { res ->
                val raw = res.body?.string().orEmpty()
                if (!res.isSuccessful) {
                    val msg = runCatching { JSONObject(raw).optString("message") }.getOrNull()
                    throw IOException(msg?.ifBlank { null } ?: "配对失败 (HTTP ${res.code})")
                }
                val parsed = JSONObject(raw)
                val pairing = parsed.optJSONObject("pairing")
                PairResponse(
                    token = parsed.getString("token"),
                    deviceId = parsed.optString("deviceId"),
                    lanUrl = lanUrl.ifBlank { pairing?.optString("lanUrl").orEmpty() },
                    remoteUrl = remoteUrl.ifBlank { pairing?.optString("remoteUrl").orEmpty() },
                    hostname = pairing?.optString("hostname").orEmpty(),
                )
            }
        }

    suspend fun fetchSnapshot(config: BridgeConfig): SnapshotPayload = withContext(Dispatchers.IO) {
        val raw = executeAuthed(config, "GET", "/api/snapshot")
        val obj = JSONObject(raw)
        SnapshotPayload(
            snapshot = ModelParsers.parseSnapshot(obj.optJSONObject("snapshot")),
            decisions = ModelParsers.parseDecisions(obj.optJSONArray("decisions")),
            notifications = ModelParsers.parseNotifications(obj.optJSONArray("notifications")),
        )
    }

    suspend fun readTerminalContent(
        config: BridgeConfig,
        workspaceId: String,
        surfaceId: String,
        paneId: Int,
        maxLines: Int = 100,
    ): String = withContext(Dispatchers.IO) {
        val qs = "workspaceId=${enc(workspaceId)}&surfaceId=${enc(surfaceId)}&paneId=$paneId&maxLines=$maxLines"
        val raw = executeAuthed(config, "GET", "/api/terminal/content?$qs")
        JSONObject(raw).optString("text", "")
    }

    suspend fun sendTerminalInput(
        config: BridgeConfig,
        workspaceId: String,
        surfaceId: String,
        paneId: Int,
        text: String,
    ) = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("workspaceId", workspaceId)
            .put("surfaceId", surfaceId)
            .put("paneId", paneId)
            .put("text", text)
        executeAuthed(config, "POST", "/api/terminal/input", body)
    }

    suspend fun focusPane(config: BridgeConfig, workspaceId: String, surfaceId: String, paneId: Int) =
        withContext(Dispatchers.IO) {
            val body = JSONObject()
                .put("workspaceId", workspaceId)
                .put("surfaceId", surfaceId)
                .put("paneId", paneId)
            executeAuthed(config, "POST", "/api/terminal/focus", body)
        }

    suspend fun launchTask(
        config: BridgeConfig,
        workspaceId: String,
        directory: String,
        agent: String,
        prompt: String,
        model: String = "",
    ): JSONObject = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("workspaceId", workspaceId)
            .put("directory", directory)
            .put("agent", agent)
            .put("prompt", prompt)
            .put("model", model)
        JSONObject(executeAuthed(config, "POST", "/api/tasks/launch", body))
    }

    suspend fun fetchUsage(config: BridgeConfig): AllUsageSummary = withContext(Dispatchers.IO) {
        val raw = executeAuthed(config, "GET", "/api/usage")
        ModelParsers.parseUsageSummary(JSONObject(raw))
    }

    suspend fun fetchModels(config: BridgeConfig): AllModelsConfig = withContext(Dispatchers.IO) {
        val raw = executeAuthed(config, "GET", "/api/agents/models")
        ModelParsers.parseModelsConfig(JSONObject(raw))
    }

    suspend fun updateDefaultModel(
        config: BridgeConfig,
        agent: String,
        model: String,
        reasoningEffort: String? = null,
    ): AllModelsConfig = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("agent", agent)
            .put("model", model)
        if (!reasoningEffort.isNullOrBlank()) {
            body.put("reasoningEffort", reasoningEffort)
        }
        val raw = executeAuthed(config, "POST", "/api/agents/models", body)
        ModelParsers.parseModelsConfig(JSONObject(raw).optJSONObject("models"))
    }

    suspend fun sendAgentAction(
        config: BridgeConfig,
        workspaceId: String,
        surfaceId: String,
        paneId: Int,
        action: String,
        model: String = "",
    ) = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("workspaceId", workspaceId)
            .put("surfaceId", surfaceId)
            .put("paneId", paneId)
            .put("action", action)
        if (model.isNotBlank()) {
            body.put("model", model)
        }
        executeAuthed(config, "POST", "/api/agent/action", body)
    }

    suspend fun resolveDecision(
        config: BridgeConfig,
        decisionId: String,
        decision: String,
        answers: List<String> = emptyList(),
        sendText: String? = null,
    ): DecisionCard = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("decision", decision)
            .put("answers", JSONArray(answers))
        if (!sendText.isNullOrEmpty()) {
            body.put("sendText", sendText)
        }
        val raw = executeAuthed(config, "POST", "/api/decisions/${enc(decisionId)}/resolve", body)
        ModelParsers.parseDecision(JSONObject(raw).getJSONObject("decision"))
    }

    fun openWebSocket(endpoint: ResolvedEndpoint, token: String, listener: WebSocketListener): WebSocket {
        val wsUrl = endpoint.baseUrl
            .replaceFirst("https://", "wss://")
            .replaceFirst("http://", "ws://") + "/ws?token=${enc(token)}"
        val req = Request.Builder()
            .url(wsUrl)
            .header("Authorization", "Bearer $token")
            .build()
        return apiClient.newWebSocket(req, listener)
    }

    private fun executeAuthed(
        config: BridgeConfig,
        method: String,
        path: String,
        body: JSONObject? = null,
    ): String {
        val ordered = buildList {
            activeEndpoint?.let { add(it) }
            for (c in candidateEndpoints(config)) {
                if (none { it.baseUrl == c.baseUrl }) add(c)
            }
        }
        if (ordered.isEmpty()) throw IOException("未配置电脑端地址")

        var lastException: Exception? = null
        for (endpoint in ordered) {
            try {
                val builder = Request.Builder()
                    .url("${endpoint.baseUrl}$path")
                    .header("Authorization", "Bearer ${config.token}")
                if (method == "POST") {
                    builder.post((body?.toString() ?: "{}").toRequestBody(jsonMediaType))
                } else {
                    builder.get()
                }
                apiClient.newCall(builder.build()).execute().use { res ->
                    val raw = res.body?.string().orEmpty()
                    if (res.isSuccessful) {
                        activeEndpoint = endpoint
                        return raw
                    }
                    val parsedMsg = runCatching { JSONObject(raw).optString("message") }.getOrNull()
                    val err = IOException(parsedMsg?.ifBlank { null } ?: "请求失败 (HTTP ${res.code})")
                    if (res.code in 400..499) {
                        throw err
                    }
                    lastException = err
                }
            } catch (e: IOException) {
                if (e.message?.contains("HTTP 4") == true || e.message?.contains("Enable experimental") == true) {
                    throw e
                }
                lastException = e
            }
        }
        throw lastException ?: IOException("无法连接电脑端 Winmux Bridge")
    }

    private fun enc(value: String): String = URLEncoder.encode(value, "UTF-8")
}
