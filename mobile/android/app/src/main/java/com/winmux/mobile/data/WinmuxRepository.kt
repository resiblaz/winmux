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
        settingsRepo.clearPairing()
        _config.value = settingsRepo.loadConfig()
        _connectionState.value = ConnectionState.Disconnected
        _snapshot.value = BridgeSnapshot()
        _decisions.value = emptyList()
    }

    fun setConnectionState(state: ConnectionState) {
        _connectionState.value = state
    }

    suspend fun refreshSnapshot(): Result<Unit> = runCatching {
        val cfg = _config.value
        if (!cfg.isPaired) return@runCatching
        val payload = apiClient.fetchSnapshot(cfg)
        _snapshot.value = payload.snapshot
        _decisions.value = payload.decisions
        _notifications.value = payload.notifications
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
                _snapshot.value = ModelParsers.parseSnapshot(obj.optJSONObject("snapshot"))
                _decisions.value = ModelParsers.parseDecisions(obj.optJSONArray("decisions"))
                _notifications.value = ModelParsers.parseNotifications(obj.optJSONArray("notifications"))
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
