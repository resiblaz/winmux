package com.winmux.mobile.data

import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.AllUsageSummary
import com.winmux.mobile.model.BridgeConfig
import com.winmux.mobile.model.BridgeNotification
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.DecisionCard
import com.winmux.mobile.model.ModelParsers
import com.winmux.mobile.model.PairedDeviceInfo
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelChildren
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
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
import java.net.URI
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager

class BridgeHttpException(
    val statusCode: Int,
    message: String,
) : IOException(message)

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
    val certFingerprint: String,
)

data class SnapshotPayload(
    val snapshot: BridgeSnapshot,
    val decisions: List<DecisionCard>,
    val notifications: List<BridgeNotification>,
    val lanUrl: String = "",
    val remoteUrl: String = "",
)

class WinmuxApiClient {
    private val jsonMediaType = "application/json; charset=utf-8".toMediaType()

    private val defaultProbeClient = OkHttpClient.Builder()
        .connectTimeout(2500, TimeUnit.MILLISECONDS)
        .readTimeout(2500, TimeUnit.MILLISECONDS)
        .writeTimeout(2500, TimeUnit.MILLISECONDS)
        .build()

    // Public HTTPS tunnels (e.g. Cloudflare Quick Tunnel over 5G) need a wider cold-start TLS window.
    private val defaultRemoteProbeClient = OkHttpClient.Builder()
        .connectTimeout(8000, TimeUnit.MILLISECONDS)
        .readTimeout(8000, TimeUnit.MILLISECONDS)
        .writeTimeout(8000, TimeUnit.MILLISECONDS)
        .build()

    private val defaultApiClient = OkHttpClient.Builder()
        .connectTimeout(6, TimeUnit.SECONDS)
        .readTimeout(12, TimeUnit.SECONDS)
        .writeTimeout(10, TimeUnit.SECONDS)
        .pingInterval(25, TimeUnit.SECONDS)
        .build()

    @Volatile
    private var cachedFingerprint: String = ""

    @Volatile
    private var pinnedProbeClient: OkHttpClient? = null

    @Volatile
    private var pinnedApiClient: OkHttpClient? = null

    @Volatile
    private var activeEndpoint: ResolvedEndpoint? = null

    private fun shouldPinSelfSigned(baseUrl: String): Boolean {
        if (!baseUrl.startsWith("https://", ignoreCase = true)) return false
        val host = runCatching { URI(baseUrl).host }.getOrNull()?.trim('[', ']')?.lowercase() ?: return true
        if (host.isBlank() || host == "localhost" || host.endsWith(".local")) return true
        // IPv6 literal contains ':' ; IPv4 consists solely of digits and dots.
        if (host.contains(':')) return true
        return host.all { it.isDigit() || it == '.' }
    }

    private fun clientForUrl(baseUrl: String, fingerprint: String, probe: Boolean): OkHttpClient {
        if (!shouldPinSelfSigned(baseUrl)) {
            return if (probe) defaultRemoteProbeClient else defaultApiClient
        }
        return clientFor(fingerprint, probe)
    }

    private fun clientFor(fingerprint: String, probe: Boolean): OkHttpClient {
        val clean = fingerprint.trim().lowercase().replace(":", "")
        if (clean.length != 64) {
            return if (probe) defaultProbeClient else defaultApiClient
        }
        if (clean == cachedFingerprint) {
            val existing = if (probe) pinnedProbeClient else pinnedApiClient
            if (existing != null) return existing
        }
        val trustManager = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) = Unit
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                val leaf = chain?.firstOrNull() ?: throw CertificateException("Empty certificate chain")
                val digest = MessageDigest.getInstance("SHA-256").digest(leaf.encoded)
                val actual = digest.joinToString("") { "%02x".format(it) }
                if (!MessageDigest.isEqual(actual.toByteArray(), clean.toByteArray())) {
                    throw CertificateException("TLS fingerprint mismatch: expected $clean, got $actual")
                }
            }
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
        val sslContext = SSLContext.getInstance("TLS").apply {
            init(null, arrayOf<TrustManager>(trustManager), SecureRandom())
        }
        val newProbe = defaultProbeClient.newBuilder()
            .sslSocketFactory(sslContext.socketFactory, trustManager)
            .hostnameVerifier { _, _ -> true }
            .build()
        val newApi = defaultApiClient.newBuilder()
            .sslSocketFactory(sslContext.socketFactory, trustManager)
            .hostnameVerifier { _, _ -> true }
            .build()
        cachedFingerprint = clean
        pinnedProbeClient = newProbe
        pinnedApiClient = newApi
        return if (probe) newProbe else newApi
    }

    fun currentEndpoint(): ResolvedEndpoint? = activeEndpoint

    fun candidateEndpoints(config: BridgeConfig): List<ResolvedEndpoint> {
        val list = mutableListOf<ResolvedEndpoint>()
        if (config.lanUrl.isNotBlank()) {
            list.add(ResolvedEndpoint(config.lanUrl.trimEnd('/'), "局域网"))
        }
        if (config.remoteUrl.isNotBlank() && config.remoteUrl.trimEnd('/') != config.lanUrl.trimEnd('/')) {
            list.add(ResolvedEndpoint(config.remoteUrl.trimEnd('/'), "远程直连"))
        }
        return list
    }

    suspend fun resolveReachableEndpoint(config: BridgeConfig): ResolvedEndpoint = withContext(Dispatchers.IO) {
        val candidates = candidateEndpoints(config)
        if (candidates.isEmpty()) {
            throw IOException("尚未配置电脑端 Bridge 地址")
        }
        if (candidates.size == 1) {
            val only = candidates.first()
            try {
                probeEndpoint(only, config.certFingerprint)
                activeEndpoint = only
                return@withContext only
            } catch (e: Exception) {
                throw IOException("无法连接电脑端 Winmux Bridge (${e.message ?: "请检查网络或证书指纹"})")
            }
        }

        // Happy Eyeballs parallel racing: probe LAN immediately and Remote after a 150ms head-start.
        // On home Wi-Fi, LAN wins in <50ms; on 5G/cellular, Remote gets an 8s TLS budget without blocking Wi-Fi.
        val winner = CompletableDeferred<ResolvedEndpoint>()
        val errors = java.util.Collections.synchronizedList(mutableListOf<String>())
        coroutineScope {
            val jobs = candidates.mapIndexed { index, candidate ->
                launch {
                    if (index > 0) {
                        delay(150L)
                    }
                    if (winner.isCompleted) return@launch
                    try {
                        probeEndpoint(candidate, config.certFingerprint)
                        if (winner.complete(candidate)) {
                            activeEndpoint = candidate
                        }
                    } catch (e: Exception) {
                        errors.add("${candidate.label}: ${e.message ?: "连接失败"}")
                    }
                }
            }
            launch {
                jobs.forEach { it.join() }
                if (!winner.isCompleted) {
                    val detail = synchronized(errors) { errors.joinToString(" | ") }.ifBlank { "请检查网络或证书指纹" }
                    winner.completeExceptionally(
                        IOException("无法连接电脑端 Winmux Bridge ($detail)")
                    )
                }
            }
            try {
                val resolved = winner.await()
                coroutineContext.cancelChildren()
                resolved
            } finally {
                coroutineContext.cancelChildren()
            }
        }
    }

    private fun probeEndpoint(candidate: ResolvedEndpoint, certFingerprint: String) {
        val probe = clientForUrl(candidate.baseUrl, certFingerprint, probe = true)
        val req = Request.Builder().url("${candidate.baseUrl}/api/health").get().build()
        probe.newCall(req).execute().use { res ->
            if (!res.isSuccessful) {
                throw IOException("HTTP ${res.code}")
            }
        }
    }

    suspend fun pair(
        lanUrl: String,
        remoteUrl: String,
        code: String,
        deviceName: String,
        certFingerprint: String = "",
    ): PairResponse =
        withContext(Dispatchers.IO) {
            val tempConfig = BridgeConfig(
                lanUrl = lanUrl,
                remoteUrl = remoteUrl,
                certFingerprint = certFingerprint,
                deviceName = deviceName,
            )
            val endpoint = resolveReachableEndpoint(tempConfig)
            val bodyJson = JSONObject()
                .put("code", code.trim())
                .put("deviceName", deviceName.ifBlank { "Android" })
            val req = Request.Builder()
                .url("${endpoint.baseUrl}/api/pair")
                .post(bodyJson.toString().toRequestBody(jsonMediaType))
                .build()

            clientForUrl(endpoint.baseUrl, certFingerprint, probe = false).newCall(req).execute().use { res ->
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
                    certFingerprint = certFingerprint.ifBlank { pairing?.optString("certFingerprint").orEmpty() },
                )
            }
        }

    suspend fun fetchSnapshot(config: BridgeConfig): SnapshotPayload = withContext(Dispatchers.IO) {
        val raw = executeAuthed(config, "GET", "/api/snapshot")
        val obj = JSONObject(raw)
        val endpoints = obj.optJSONObject("endpoints")
        SnapshotPayload(
            snapshot = ModelParsers.parseSnapshot(obj.optJSONObject("snapshot")),
            decisions = ModelParsers.parseDecisions(obj.optJSONArray("decisions")),
            notifications = ModelParsers.parseNotifications(obj.optJSONArray("notifications")),
            lanUrl = endpoints?.optString("lanUrl").orEmpty(),
            remoteUrl = endpoints?.optString("remoteUrl").orEmpty(),
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

    suspend fun splitPane(
        config: BridgeConfig,
        workspaceId: String,
        surfaceId: String,
        paneId: Int,
        direction: String = "right",
        size: Double = 0.5,
    ): JSONObject = withContext(Dispatchers.IO) {
        val body = JSONObject()
            .put("workspaceId", workspaceId)
            .put("surfaceId", surfaceId)
            .put("paneId", paneId)
            .put("direction", direction)
            .put("size", size)
        JSONObject(executeAuthed(config, "POST", "/api/terminal/split", body))
    }

    suspend fun selectWorkspace(config: BridgeConfig, workspaceId: String) = withContext(Dispatchers.IO) {
        val body = JSONObject().put("workspaceId", workspaceId)
        executeAuthed(config, "POST", "/api/workspaces/select", body)
    }

    suspend fun closeWorkspace(config: BridgeConfig, workspaceId: String) = withContext(Dispatchers.IO) {
        val body = JSONObject().put("workspaceId", workspaceId)
        executeAuthed(config, "POST", "/api/workspaces/close", body)
    }

    suspend fun fetchDevices(config: BridgeConfig): List<PairedDeviceInfo> = withContext(Dispatchers.IO) {
        val raw = executeAuthed(config, "GET", "/api/devices")
        ModelParsers.parseDevices(JSONObject(raw).optJSONArray("devices"))
    }

    suspend fun revokeDevice(config: BridgeConfig, deviceId: String): Boolean = withContext(Dispatchers.IO) {
        val raw = executeAuthed(config, "DELETE", "/api/devices/${enc(deviceId)}")
        JSONObject(raw).optBoolean("ok", false)
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

    fun openWebSocket(
        endpoint: ResolvedEndpoint,
        token: String,
        certFingerprint: String = "",
        listener: WebSocketListener,
    ): WebSocket {
        val wsUrl = endpoint.baseUrl
            .replaceFirst("https://", "wss://")
            .replaceFirst("http://", "ws://") + "/ws?token=${enc(token)}"
        val req = Request.Builder()
            .url(wsUrl)
            .header("Authorization", "Bearer $token")
            .build()
        return clientForUrl(endpoint.baseUrl, certFingerprint, probe = false).newWebSocket(req, listener)
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
                val client = clientForUrl(endpoint.baseUrl, config.certFingerprint, probe = false)
                val builder = Request.Builder()
                    .url("${endpoint.baseUrl}$path")
                    .header("Authorization", "Bearer ${config.token}")
                when (method) {
                    "POST" -> builder.post((body?.toString() ?: "{}").toRequestBody(jsonMediaType))
                    "DELETE" -> builder.delete()
                    else -> builder.get()
                }
                client.newCall(builder.build()).execute().use { res ->
                    val raw = res.body?.string().orEmpty()
                    if (res.isSuccessful) {
                        activeEndpoint = endpoint
                        return raw
                    }
                    val parsedMsg = runCatching { JSONObject(raw).optString("message") }.getOrNull()
                    val msg = parsedMsg?.ifBlank { null } ?: "请求失败 (HTTP ${res.code})"
                    if (res.code in 400..499) {
                        throw BridgeHttpException(res.code, msg)
                    }
                    lastException = BridgeHttpException(res.code, msg)
                }
            } catch (e: BridgeHttpException) {
                if (e.statusCode in 400..499) {
                    throw e
                }
                lastException = e
            } catch (e: IOException) {
                lastException = e
            }
        }
        throw lastException ?: IOException("无法连接电脑端 Winmux Bridge")
    }

    private fun enc(value: String): String = URLEncoder.encode(value, "UTF-8")
}
