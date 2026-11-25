package com.winmux.mobile.service

import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.Build
import android.os.IBinder
import androidx.core.content.ContextCompat
import com.winmux.mobile.data.BridgeHttpException
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.ConnectionState
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.util.concurrent.atomic.AtomicBoolean

class WinmuxConnectionService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var loopJob: Job? = null
    private var healthCheckJob: Job? = null

    @Volatile
    private var activeWebSocket: WebSocket? = null

    @Volatile
    private var lastNetworkHandle: Long = -1L

    @Volatile
    private var lastTransportTag: String = ""

    private var connectivityCallback: ConnectivityManager.NetworkCallback? = null
    private var screenReceiver: BroadcastReceiver? = null

    override fun onCreate() {
        super.onCreate()
        NotificationHelper.ensureChannels(this)
        WinmuxRepository.initialize(this)
        registerNetworkMonitor()
        registerScreenReceiver()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }

        startForeground(
            NotificationHelper.SERVICE_NOTIFICATION_ID,
            NotificationHelper.buildServiceNotification(this, "正在连接电脑端 Winmux..."),
        )
        when (intent?.action) {
            ACTION_ENSURE_CONNECTED -> ensureConnected()
            ACTION_RECONNECT_NOW -> restartConnectionLoop(initialDelayMs = 0L)
            else -> {
                if (loopJob?.isActive == true && WinmuxRepository.connectionState.value is ConnectionState.Connected) {
                    ensureConnected()
                } else {
                    restartConnectionLoop(initialDelayMs = 0L)
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        unregisterScreenReceiver()
        unregisterNetworkMonitor()
        healthCheckJob?.cancel()
        activeWebSocket?.close(1000, "Service stopped")
        activeWebSocket = null
        WinmuxRepository.setConnectionState(ConnectionState.Disconnected)
        scope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    @Synchronized
    private fun ensureConnected() {
        if (!WinmuxRepository.config.value.isPaired) return
        val ws = activeWebSocket
        val state = WinmuxRepository.connectionState.value
        if (ws == null || state !is ConnectionState.Connected) {
            // Immediately wake out of any backoff sleep or stopped state.
            restartConnectionLoop(initialDelayMs = 0L)
            return
        }

        // Probe existing socket for half-open/zombie TCP after Doze or screen sleep.
        if (healthCheckJob?.isActive == true) return
        val beforePingAt = WinmuxRepository.lastMessageReceivedAt
        val sent = runCatching { ws.send("""{"type":"ping"}""") }.getOrDefault(false)
        if (!sent) {
            restartConnectionLoop(initialDelayMs = 0L)
            return
        }
        healthCheckJob = scope.launch {
            delay(3500L)
            if (activeWebSocket === ws && WinmuxRepository.lastMessageReceivedAt <= beforePingAt) {
                restartConnectionLoop(initialDelayMs = 0L)
            }
        }
    }

    @Synchronized
    private fun restartConnectionLoop(initialDelayMs: Long = 0L) {
        healthCheckJob?.cancel()
        loopJob?.cancel()
        val previousWs = activeWebSocket
        activeWebSocket = null
        WinmuxRepository.attachWebSocketSender(null)
        previousWs?.close(1000, "Reconnect requested")

        loopJob = scope.launch {
            if (initialDelayMs > 0L) {
                delay(initialDelayMs)
            }
            var backoffMs = 1500L
            while (isActive) {
                val cfg = WinmuxRepository.config.value
                if (!cfg.isPaired) {
                    WinmuxRepository.setConnectionState(ConnectionState.Disconnected)
                    updateServiceNotification("等待扫码或输入配对码连接")
                    delay(3000L)
                    continue
                }

                try {
                    WinmuxRepository.setConnectionState(ConnectionState.Connecting("正在探测局域网 / 远程通道..."))
                    updateServiceNotification("正在连接电脑...")
                    val endpoint = WinmuxRepository.apiClient.resolveReachableEndpoint(cfg)
                    backoffMs = 1500L

                    val closedSignal = CompletableDeferred<Unit>()
                    val revokedFlag = AtomicBoolean(false)
                    var openedWs: WebSocket? = null

                    val ws = WinmuxRepository.apiClient.openWebSocket(
                        endpoint = endpoint,
                        token = cfg.token,
                        certFingerprint = cfg.certFingerprint,
                        listener = object : WebSocketListener() {
                            override fun onOpen(webSocket: WebSocket, response: Response) {
                                if (activeWebSocket !== webSocket) {
                                    webSocket.close(1000, "Superseded")
                                    return
                                }
                                WinmuxRepository.attachWebSocketSender { msg -> webSocket.send(msg) }
                                WinmuxRepository.setConnectionState(
                                    ConnectionState.Connected(endpoint.label, endpoint.baseUrl),
                                )
                                updateServiceNotification("已连接 ${cfg.hostname.ifBlank { "电脑" }} (${endpoint.label})")
                            }

                            override fun onMessage(webSocket: WebSocket, text: String) {
                                if (activeWebSocket !== webSocket) return
                                WinmuxRepository.onWebSocketMessage(text)
                            }

                            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                                if (activeWebSocket !== webSocket) return
                                if (code == 4001) revokedFlag.set(true)
                                WinmuxRepository.attachWebSocketSender(null)
                                webSocket.close(1000, null)
                                closedSignal.complete(Unit)
                            }

                            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                                if (activeWebSocket !== webSocket) return
                                if (code == 4001) revokedFlag.set(true)
                                WinmuxRepository.attachWebSocketSender(null)
                                closedSignal.complete(Unit)
                            }

                            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                                if (activeWebSocket !== webSocket) return
                                WinmuxRepository.attachWebSocketSender(null)
                                if (response?.code == 401) {
                                    revokedFlag.set(true)
                                    WinmuxRepository.setConnectionState(
                                        ConnectionState.Error("设备授权已失效或被吊销，请重新配对"),
                                    )
                                } else {
                                    val sec = (backoffMs / 1000L).coerceAtLeast(1L)
                                    WinmuxRepository.setConnectionState(
                                        ConnectionState.Error("${t.message ?: "连接已断开"}（${sec}秒后自动重连）"),
                                    )
                                }
                                closedSignal.complete(Unit)
                            }
                        },
                    )
                    openedWs = ws
                    activeWebSocket = ws

                    try {
                        closedSignal.await()
                    } finally {
                        if (activeWebSocket === openedWs) {
                            activeWebSocket = null
                            WinmuxRepository.attachWebSocketSender(null)
                        }
                    }

                    if (revokedFlag.get()) {
                        WinmuxRepository.setConnectionState(ConnectionState.Error("设备授权已被吊销，请重新配对"))
                        updateServiceNotification("授权已失效，请在 App 中重新配对")
                        break
                    }
                } catch (e: BridgeHttpException) {
                    if (e.statusCode == 401) {
                        WinmuxRepository.setConnectionState(ConnectionState.Error("设备授权已失效，请重新配对"))
                        updateServiceNotification("授权已失效，请在 App 中重新配对")
                        break
                    }
                    val sec = (backoffMs / 1000L).coerceAtLeast(1L)
                    WinmuxRepository.setConnectionState(
                        ConnectionState.Error("${e.message ?: "连接失败"}（${sec}秒后自动重连）"),
                    )
                    updateServiceNotification("自动重连中：${e.message?.take(40) ?: "请检查网络"}")
                } catch (e: Exception) {
                    val sec = (backoffMs / 1000L).coerceAtLeast(1L)
                    WinmuxRepository.setConnectionState(
                        ConnectionState.Error("${e.message ?: "连接失败"}（${sec}秒后自动重连）"),
                    )
                    updateServiceNotification("自动重连中：${e.message?.take(40) ?: "请检查网络"}")
                }

                delay(backoffMs)
                backoffMs = when {
                    backoffMs < 3000L -> 3000L
                    backoffMs < 5000L -> 5000L
                    else -> 8000L
                }
            }
        }
    }

    private fun updateServiceNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        nm.notify(
            NotificationHelper.SERVICE_NOTIFICATION_ID,
            NotificationHelper.buildServiceNotification(this, text),
        )
    }

    private fun registerNetworkMonitor() {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                val handle = network.networkHandle
                val networkChanged = lastNetworkHandle != -1L && lastNetworkHandle != handle
                lastNetworkHandle = handle
                if (WinmuxRepository.config.value.isPaired &&
                    (networkChanged || WinmuxRepository.connectionState.value !is ConnectionState.Connected)
                ) {
                    // Brief settle delay lets Android DNS/routing finish switching between Wi-Fi and 5G.
                    restartConnectionLoop(initialDelayMs = 350L)
                }
            }

            override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) {
                val transport = when {
                    capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
                    capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cell"
                    capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "eth"
                    else -> ""
                }
                if (transport.isBlank()) return
                val changed = lastTransportTag.isNotBlank() && lastTransportTag != transport
                lastTransportTag = transport
                if (changed && WinmuxRepository.config.value.isPaired) {
                    restartConnectionLoop(initialDelayMs = 350L)
                }
            }

            override fun onLost(network: Network) {
                if (network.networkHandle == lastNetworkHandle && WinmuxRepository.config.value.isPaired) {
                    restartConnectionLoop(initialDelayMs = 500L)
                }
            }
        }
        connectivityCallback = cb
        runCatching { cm.registerDefaultNetworkCallback(cb) }
    }

    private fun unregisterNetworkMonitor() {
        val cm = getSystemService(ConnectivityManager::class.java) ?: return
        connectivityCallback?.let { runCatching { cm.unregisterNetworkCallback(it) } }
        connectivityCallback = null
    }

    private fun registerScreenReceiver() {
        val filter = IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_ON)
            addAction(Intent.ACTION_USER_PRESENT)
        }
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (WinmuxRepository.config.value.isPaired) {
                    ensureConnected()
                }
            }
        }
        screenReceiver = receiver
        runCatching {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                registerReceiver(receiver, filter)
            }
        }
    }

    private fun unregisterScreenReceiver() {
        screenReceiver?.let { runCatching { unregisterReceiver(it) } }
        screenReceiver = null
    }

    companion object {
        const val ACTION_START = "com.winmux.mobile.SERVICE_START"
        const val ACTION_ENSURE_CONNECTED = "com.winmux.mobile.SERVICE_ENSURE_CONNECTED"
        const val ACTION_RECONNECT_NOW = "com.winmux.mobile.SERVICE_RECONNECT_NOW"
        const val ACTION_STOP = "com.winmux.mobile.SERVICE_STOP"

        fun start(context: Context) {
            val intent = Intent(context, WinmuxConnectionService::class.java).apply { action = ACTION_START }
            ContextCompat.startForegroundService(context, intent)
        }

        fun ensureConnected(context: Context) {
            val intent = Intent(context, WinmuxConnectionService::class.java).apply { action = ACTION_ENSURE_CONNECTED }
            ContextCompat.startForegroundService(context, intent)
        }

        fun reconnectNow(context: Context) {
            val intent = Intent(context, WinmuxConnectionService::class.java).apply { action = ACTION_RECONNECT_NOW }
            ContextCompat.startForegroundService(context, intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, WinmuxConnectionService::class.java)
            context.stopService(intent)
        }
    }
}
