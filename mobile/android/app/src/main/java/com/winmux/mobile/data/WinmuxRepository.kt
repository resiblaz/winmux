package com.winmux.mobile.data

import android.content.Context
import com.winmux.mobile.model.AllModelsConfig
import com.winmux.mobile.model.AllUsageSummary
import com.winmux.mobile.model.BridgeConfig
import com.winmux.mobile.model.BridgeNotification
import com.winmux.mobile.model.BridgeSnapshot
import com.winmux.mobile.model.ConnectionState
import com.winmux.mobile.model.DecisionCard
import com.winmux.mobile.model.ModelParsers
import com.winmux.mobile.service.NotificationHelper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.json.JSONObject

object WinmuxRepository {
    private lateinit var appContext: Context
    private lateinit var settingsRepo: SettingsRepository
    val apiClient = WinmuxApiClient()

    private val _config = MutableStateFlow(BridgeConfig())
    val config: StateFlow<BridgeConfig> = _config.asStateFlow()

    private val _connectionState = MutableStateFlow<ConnectionState>(ConnectionState.Disconnected)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _snapshot = MutableStateFlow(BridgeSnapshot())
    val snapshot: StateFlow<BridgeSnapshot> = _snapshot.asStateFlow()

    private val _decisions = MutableStateFlow<List<DecisionCard>>(emptyList())
    val decisions: StateFlow<List<DecisionCard>> = _decisions.asStateFlow()

    private val _notifications = MutableStateFlow<List<BridgeNotification>>(emptyList())
    val notifications: StateFlow<List<BridgeNotification>> = _notifications.asStateFlow()

    private val _usage = MutableStateFlow(AllUsageSummary())
    val usage: StateFlow<AllUsageSummary> = _usage.asStateFlow()

    private val _models = MutableStateFlow(AllModelsConfig())
    val models: StateFlow<AllModelsConfig> = _models.asStateFlow()

    private val _terminalOutputs = MutableStateFlow<Map<String, String>>(emptyMap())
    val terminalOutputs: StateFlow<Map<String, String>> = _terminalOutputs.asStateFlow()

    @Volatile
    private var wsSender: ((String) -> Boolean)? = null

    @Volatile
    private var activeTerminalSubscription: String? = null

    val isWebSocketConnected: Boolean
        get() = wsSender != null

    fun attachWebSocketSender(sender: ((String) -> Boolean)?) {
        wsSender = sender
        // Re-subscribe active terminal stream automatically after WebSocket reconnect.
        activeTerminalSubscription?.let { payload ->
            sender?.invoke(payload)
        }
    }

    fun subscribeTerminal(workspaceId: String, surfaceId: String, paneId: Int, maxLines: Int = 120) {
        val payload = JSONObject()
            .put("type", "terminal.subscribe")
            .put("workspaceId", workspaceId)
            .put("surfaceId", surfaceId)
            .put("paneId", paneId)
            .put("maxLines", maxLines)
            .put("supportsDelta", true)
            .toString()
        activeTerminalSubscription = payload
        wsSender?.invoke(payload)
    }

    fun unsubscribeTerminal() {
        activeTerminalSubscription = null
        val payload = JSONObject().put("type", "terminal.unsubscribe").toString()
        wsSender?.invoke(payload)
    }

    fun terminalKey(workspaceId: String, surfaceId: String, paneId: Int): String =
        "$workspaceId:$surfaceId:$paneId"

    fun initialize(context: Context) {
        if (::appContext.isInitialized) return
        appContext = context.applicationContext
        settingsRepo = SettingsRepository(appContext)
        _config.value = settingsRepo.loadConfig()
    }

    fun updateConfig(newConfig: BridgeConfig) {
        settingsRepo.saveConfig(newConfig)
        _config.value = settingsRepo.loadConfig()
    }

    fun unpair() {
        unsubscribeTerminal()
        settingsRepo.clearPairing()
        _config.value = settingsRepo.loadConfig()
        _connectionState.value = ConnectionState.Disconnected
        _snapshot.value = BridgeSnapshot()
        _decisions.value = emptyList()
        _terminalOutputs.value = emptyMap()
    }

    fun setConnectionState(state: ConnectionState) {
        _connectionState.value = state
    }

    suspend fun refreshSnapshot(): Result<Unit> = runCatching {
        val cfg = _config.value
        if (!cfg.isPaired) return@runCatching
        val payload = apiClient.fetchSnapshot(cfg)
        syncEndpointsFromServer(payload.lanUrl, payload.remoteUrl)
        _snapshot.value = payload.snapshot
        _decisions.value = payload.decisions
        _notifications.value = payload.notifications
    }

    private fun syncEndpointsFromJson(endpointsObj: JSONObject?) {
        if (endpointsObj == null) return
        syncEndpointsFromServer(
            lanUrl = endpointsObj.optString("lanUrl"),
            remoteUrl = endpointsObj.optString("remoteUrl"),
        )
    }

    private fun syncEndpointsFromServer(lanUrl: String, remoteUrl: String) {
        val current = _config.value
        if (!current.isPaired) return
        val nextLan = lanUrl.trim().trimEnd('/').ifBlank { current.lanUrl }
        val cleanRemote = remoteUrl.trim().trimEnd('/')
        // Always adopt a live public tunnel URL; otherwise keep any non-blank remote URL reported by Bridge.
        val nextRemote = if (cleanRemote.isNotBlank()) cleanRemote else current.remoteUrl
        if (nextLan != current.lanUrl || nextRemote != current.remoteUrl) {
            updateConfig(current.copy(lanUrl = nextLan, remoteUrl = nextRemote))
        }
    }

    suspend fun refreshUsageAndModels(): Result<Unit> = runCatching {
        val cfg = _config.value
        if (!cfg.isPaired) return@runCatching
        _usage.value = apiClient.fetchUsage(cfg)
        _models.value = apiClient.fetchModels(cfg)
    }

    suspend fun updateDefaultModel(agent: String, model: String, reasoningEffort: String? = null): Result<AllModelsConfig> =
        runCatching {
            val updated = apiClient.updateDefaultModel(_config.value, agent, model, reasoningEffort)
            _models.value = updated
            updated
        }

    suspend fun resolveDecision(
        decisionId: String,
        decision: String,
        answers: List<String> = emptyList(),
        sendText: String? = null,
    ): Result<DecisionCard> = runCatching {
        val updated = apiClient.resolveDecision(_config.value, decisionId, decision, answers, sendText)
        upsertDecision(updated)
        NotificationHelper.cancelDecisionNotification(appContext, decisionId)
        updated
    }

    fun onWebSocketMessage(raw: String) {
        val obj = runCatching { JSONObject(raw) }.getOrNull() ?: return
        when (obj.optString("type")) {
            "hello" -> {
                syncEndpointsFromJson(obj.optJSONObject("endpoints"))
                _snapshot.value = ModelParsers.parseSnapshot(obj.optJSONObject("snapshot"))
                _decisions.value = ModelParsers.parseDecisions(obj.optJSONArray("decisions"))
                _notifications.value = ModelParsers.parseNotifications(obj.optJSONArray("notifications"))
            }
            "endpoints.updated" -> {
                syncEndpointsFromJson(obj.optJSONObject("endpoints"))
            }
            "snapshot" -> {
                _snapshot.value = ModelParsers.parseSnapshot(obj.optJSONObject("snapshot"))
            }
            "decision.created" -> {
                val cardObj = obj.optJSONObject("decision") ?: return
                val card = ModelParsers.parseDecision(cardObj)
                upsertDecision(card)
                if (card.status == "pending") {
                    NotificationHelper.showDecisionNotification(appContext, card)
                }
            }
            "decision.resolved" -> {
                val cardObj = obj.optJSONObject("decision") ?: return
                val card = ModelParsers.parseDecision(cardObj)
                upsertDecision(card)
                NotificationHelper.cancelDecisionNotification(appContext, card.id)
            }
            "notification" -> {
                val noteObj = obj.optJSONObject("notification") ?: return
                val note = ModelParsers.parseNotification(noteObj)
                _notifications.value = (listOf(note) + _notifications.value).take(50)
                NotificationHelper.showEventNotification(appContext, note)
            }
            "terminal.output" -> {
                val wsId = obj.optString("workspaceId")
                val sfId = obj.optString("surfaceId")
                val paneId = obj.optInt("paneId", -1)
                if (wsId.isNotBlank() && sfId.isNotBlank() && paneId >= 0) {
                    val key = terminalKey(wsId, sfId, paneId)
                    val updated = LinkedHashMap(_terminalOutputs.value)
                    val prev = updated.remove(key).orEmpty()
                    val nextText = if (obj.optString("mode") == "append") {
                        val combined = prev + obj.optString("delta", "")
                        if (combined.length > 16384) combined.takeLast(16384) else combined
                    } else {
                        obj.optString("text", "")
                    }
                    updated[key] = nextText
                    while (updated.size > 12) {
                        val oldest = updated.keys.firstOrNull() ?: break
                        updated.remove(oldest)
                    }
                    _terminalOutputs.value = updated
                }
            }
        }
    }

    private fun upsertDecision(card: DecisionCard) {
        val current = _decisions.value.toMutableList()
        val idx = current.indexOfFirst { it.id == card.id }
        if (idx >= 0) {
            current[idx] = card
        } else {
            current.add(0, card)
        }
        _decisions.value = current.take(100)
    }
}
