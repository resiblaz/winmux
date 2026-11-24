package com.winmux.mobile.service

import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import android.os.IBinder
import androidx.core.content.ContextCompat
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.ConnectionState
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
    private var activeWebSocket: WebSocket? = null
    private val wsClosedSignal = AtomicBoolean(false)
    private var connectivityCallback: ConnectivityManager.NetworkCallback? = null

    override fun onCreate() {
        super.onCreate()
        NotificationHelper.ensureChannels(this)
        WinmuxRepository.initialize(this)
        registerNetworkMonitor()
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
        restartConnectionLoop()
        return START_STICKY
    }

    override fun onDestroy() {
        unregisterNetworkMonitor()
        activeWebSocket?.close(1000, "Service stopped")
        activeWebSocket = null
        WinmuxRepository.setConnectionState(ConnectionState.Disconnected)
        scope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun restartConnectionLoop() {
        loopJob?.cancel()
        activeWebSocket?.close(1000, "Reconnect requested")
        activeWebSocket = null

        loopJob = scope.launch {
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

                    wsClosedSignal.set(false)
                    val ws = WinmuxRepository.apiClient.openWebSocket(
                        endpoint = endpoint,
                        token = cfg.token,
                        certFingerprint = cfg.certFingerprint,
                        listener = object : WebSocketListener() {
                            override fun onOpen(webSocket: WebSocket, response: Response) {
                                WinmuxRepository.attachWebSocketSender { msg -> webSocket.send(msg) }
                                WinmuxRepository.setConnectionState(
                                    ConnectionState.Connected(endpoint.label, endpoint.baseUrl),
                                )
                                updateServiceNotification("已连接 ${cfg.hostname.ifBlank { "电脑" }} (${endpoint.label})")
                            }

                            override fun onMessage(webSocket: WebSocket, text: String) {
                                WinmuxRepository.onWebSocketMessage(text)
                            }

                            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                                WinmuxRepository.attachWebSocketSender(null)
                                webSocket.close(1000, null)
                                wsClosedSignal.set(true)
                            }

                            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                                WinmuxRepository.attachWebSocketSender(null)
                                wsClosedSignal.set(true)
                            }

                            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                                WinmuxRepository.attachWebSocketSender(null)
                                WinmuxRepository.setConnectionState(
                                    ConnectionState.Error(t.message ?: "连接已断开，准备重连"),
                                )
                                wsClosedSignal.set(true)
                            }
                        },
                    )
                    activeWebSocket = ws

                    while (isActive && !wsClosedSignal.get()) {
                        delay(1000L)
                    }
                    activeWebSocket = null
                } catch (e: Exception) {
                    WinmuxRepository.setConnectionState(
                        ConnectionState.Error(e.message ?: "连接失败，重试中"),
                    )
                    updateServiceNotification("重连中：${e.message?.take(40) ?: "请检查网络"}")
                }

                delay(backoffMs)
                backoffMs = (backoffMs * 2).coerceAtMost(15_000L)
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
                if (WinmuxRepository.config.value.isPaired && WinmuxRepository.connectionState.value !is ConnectionState.Connected) {
                    restartConnectionLoop()
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

    companion object {
        const val ACTION_START = "com.winmux.mobile.SERVICE_START"
        const val ACTION_STOP = "com.winmux.mobile.SERVICE_STOP"

        fun start(context: Context) {
            val intent = Intent(context, WinmuxConnectionService::class.java).apply { action = ACTION_START }
            ContextCompat.startForegroundService(context, intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, WinmuxConnectionService::class.java).apply { action = ACTION_STOP }
            context.startService(intent)
        }
    }
}
