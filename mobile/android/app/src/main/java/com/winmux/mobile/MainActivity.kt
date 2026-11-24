package com.winmux.mobile

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Analytics
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.model.ActiveTerminalTarget
import com.winmux.mobile.model.ConnectionState
import com.winmux.mobile.service.NotificationHelper
import com.winmux.mobile.service.WinmuxConnectionService
import com.winmux.mobile.ui.screens.DecisionsScreen
import com.winmux.mobile.ui.screens.NewTaskBottomSheet
import com.winmux.mobile.ui.screens.PairingScreen
import com.winmux.mobile.ui.screens.StatusScreen
import com.winmux.mobile.ui.screens.TerminalScreen
import com.winmux.mobile.ui.screens.WorkspacesScreen
import com.winmux.mobile.ui.theme.WinmuxColors
import com.winmux.mobile.ui.theme.WinmuxTheme
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    private val incomingPairUriState = mutableStateOf<String?>(null)
    private val requestedTabState = mutableStateOf<String?>(null)

    private val notificationPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WinmuxRepository.initialize(this)
        requestNotificationPermissionIfNeeded()
        handleIntent(intent)

        if (WinmuxRepository.config.value.isPaired) {
            WinmuxConnectionService.start(this)
        }

        setContent {
            WinmuxTheme {
                val scope = rememberCoroutineScope()
                val config by WinmuxRepository.config.collectAsState()
                val connectionState by WinmuxRepository.connectionState.collectAsState()
                val snapshot by WinmuxRepository.snapshot.collectAsState()
                val decisions by WinmuxRepository.decisions.collectAsState()
                val usage by WinmuxRepository.usage.collectAsState()
                val models by WinmuxRepository.models.collectAsState()

                var selectedTab by remember { mutableStateOf(if (config.isPaired) "workspaces" else "pairing") }
                var activeTerminal by remember { mutableStateOf<ActiveTerminalTarget?>(null) }
                var showNewTaskSheet by remember { mutableStateOf(false) }

                LaunchedEffect(connectionState) {
                    if (connectionState is ConnectionState.Connected) {
                        WinmuxRepository.refreshUsageAndModels()
                    }
                }

                val incomingPair by incomingPairUriState
                val requestedTab by requestedTabState

                LaunchedEffect(incomingPair) {
                    if (!incomingPair.isNullOrBlank()) {
                        selectedTab = "pairing"
                        activeTerminal = null
                    }
                }
                LaunchedEffect(requestedTab) {
                    if (!requestedTab.isNullOrBlank()) {
                        selectedTab = requestedTab!!
                        activeTerminal = null
                        requestedTabState.value = null
                    }
                }

                val pendingDecisionCount = decisions.count { it.status == "pending" }

                if (activeTerminal != null) {
                    TerminalScreen(
                        target = activeTerminal!!,
                        permissions = snapshot.permissions,
                        models = models,
                        onBack = { activeTerminal = null },
                    )
                } else {
                    Scaffold(
                        containerColor = WinmuxColors.Bg,
                        bottomBar = {
                            NavigationBar(containerColor = WinmuxColors.Surface) {
                                NavigationBarItem(
                                    selected = selectedTab == "workspaces",
                                    onClick = { selectedTab = "workspaces" },
                                    icon = { Icon(Icons.Default.Terminal, contentDescription = "工作区") },
                                    label = { Text("工作区") },
                                )
                                NavigationBarItem(
                                    selected = selectedTab == "decisions",
                                    onClick = { selectedTab = "decisions" },
                                    icon = {
                                        BadgedBox(
                                            badge = {
                                                if (pendingDecisionCount > 0) {
                                                    Badge { Text("$pendingDecisionCount") }
                                                }
                                            },
                                        ) {
                                            Icon(Icons.Default.CheckCircle, contentDescription = "审批")
                                        }
                                    },
                                    label = { Text("审批") },
                                )
                                NavigationBarItem(
                                    selected = selectedTab == "status",
                                    onClick = { selectedTab = "status" },
                                    icon = { Icon(Icons.Default.Analytics, contentDescription = "状态") },
                                    label = { Text("状态") },
                                )
                                NavigationBarItem(
                                    selected = selectedTab == "pairing",
                                    onClick = { selectedTab = "pairing" },
                                    icon = { Icon(Icons.Default.Link, contentDescription = "连接") },
                                    label = { Text("连接") },
                                )
                            }
                        },
                    ) { innerPadding ->
                        Box(modifier = Modifier.padding(innerPadding)) {
                            when (selectedTab) {
                                "workspaces" -> WorkspacesScreen(
                                    snapshot = snapshot,
                                    connectionState = connectionState,
                                    onRefresh = { scope.launch { WinmuxRepository.refreshSnapshot() } },
                                    onOpenTerminal = { target -> activeTerminal = target },
                                    onOpenNewTask = { showNewTaskSheet = true },
                                    onGoToPairing = { selectedTab = "pairing" },
                                )
                                "decisions" -> DecisionsScreen(
                                    decisions = decisions,
                                    onResolve = { id, choice, answers ->
                                        scope.launch { WinmuxRepository.resolveDecision(id, choice, answers) }
                                    },
                                    onOpenNewTask = { showNewTaskSheet = true },
                                )
                                "status" -> StatusScreen(
                                    systemStatus = snapshot.systemStatus,
                                    usage = usage,
                                    models = models,
                                    connectionState = connectionState,
                                    onRefresh = {
                                        scope.launch {
                                            WinmuxRepository.refreshSnapshot()
                                            WinmuxRepository.refreshUsageAndModels()
                                        }
                                    },
                                )
                                else -> PairingScreen(
                                    config = config,
                                    connectionState = connectionState,
                                    incomingPairUri = incomingPair,
                                    onIncomingPairConsumed = { incomingPairUriState.value = null },
                                    onPaired = {
                                        WinmuxConnectionService.start(this@MainActivity)
                                        selectedTab = "workspaces"
                                    },
                                    onUnpair = {
                                        WinmuxConnectionService.stop(this@MainActivity)
                                        WinmuxRepository.unpair()
                                    },
                                    onReconnect = {
                                        WinmuxConnectionService.start(this@MainActivity)
                                        scope.launch {
                                            WinmuxRepository.refreshSnapshot()
                                            WinmuxRepository.refreshUsageAndModels()
                                        }
                                    },
                                )
                            }
                        }
                    }
                }

                if (showNewTaskSheet) {
                    NewTaskBottomSheet(
                        snapshot = snapshot,
                        models = models,
                        onDismiss = { showNewTaskSheet = false },
                        onLaunched = {
                            showNewTaskSheet = false
                            selectedTab = "workspaces"
                        },
                    )
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        if (intent == null) return
        val dataUri = intent.dataString
        if (!dataUri.isNullOrBlank() && dataUri.startsWith("winmux://pair", ignoreCase = true)) {
            incomingPairUriState.value = dataUri
        }
        intent.getStringExtra(NotificationHelper.EXTRA_OPEN_TAB)?.let { tab ->
            requestedTabState.value = tab
        }
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) ==
                PackageManager.PERMISSION_GRANTED
            if (!granted) {
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
    }
}
