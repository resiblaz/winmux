package com.winmux.mobile.service

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.winmux.mobile.MainActivity
import com.winmux.mobile.R
import com.winmux.mobile.model.BridgeNotification
import com.winmux.mobile.model.DecisionCard

object NotificationHelper {
    const val CHANNEL_SERVICE = "winmux_service"
    const val CHANNEL_DECISIONS = "winmux_decisions"
    const val CHANNEL_EVENTS = "winmux_events"
    const val SERVICE_NOTIFICATION_ID = 1001

    const val EXTRA_OPEN_TAB = "open_tab"
    const val EXTRA_DECISION_ID = "decision_id"
    const val EXTRA_WORKSPACE_ID = "workspace_id"
    const val EXTRA_SURFACE_ID = "surface_id"
    const val EXTRA_PANE_ID = "pane_id"

    fun ensureChannels(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java) ?: return

        val serviceChannel = NotificationChannel(
            CHANNEL_SERVICE,
            "Winmux 后台连接",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "保持与电脑 Winmux Bridge 的加密实时连接"
            setShowBadge(false)
        }

        val decisionChannel = NotificationChannel(
            CHANNEL_DECISIONS,
            "Agent 决策审批",
            NotificationManager.IMPORTANCE_HIGH,
        ).apply {
            description = "Claude / Codex 请求权限、计划确认或提问时弹出快捷审批"
            enableVibration(true)
        }

        val eventChannel = NotificationChannel(
            CHANNEL_EVENTS,
            "Agent 任务通知",
            NotificationManager.IMPORTANCE_DEFAULT,
        ).apply {
            description = "Agent 完成一轮任务或终端提醒"
        }

        manager.createNotificationChannel(serviceChannel)
        manager.createNotificationChannel(decisionChannel)
        manager.createNotificationChannel(eventChannel)
    }

    fun buildServiceNotification(context: Context, statusText: String): Notification {
        val openIntent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val pi = PendingIntent.getActivity(
            context,
            0,
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

        return NotificationCompat.Builder(context, CHANNEL_SERVICE)
            .setSmallIcon(R.drawable.ic_launcher_winmux)
            .setContentTitle("Winmux 远程控制")
            .setContentText(statusText)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setContentIntent(pi)
            .build()
    }

    fun showDecisionNotification(context: Context, card: DecisionCard) {
        if (!canPostNotifications(context)) return

        val openIntent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra(EXTRA_OPEN_TAB, "decisions")
            putExtra(EXTRA_DECISION_ID, card.id)
        }
        val openPi = PendingIntent.getActivity(
            context,
            card.id.hashCode(),
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

        val bodyText = when {
            card.summary.isNotBlank() -> card.summary
            card.toolInput.isNotBlank() -> "${card.toolName}: ${card.toolInput.take(180)}"
            card.planText.isNotBlank() -> card.planText.take(180)
            card.questions.isNotEmpty() -> card.questions.first().prompt
            else -> "点击进入 Winmux 处理"
        }

        val builder = NotificationCompat.Builder(context, CHANNEL_DECISIONS)
            .setSmallIcon(R.drawable.ic_launcher_winmux)
            .setContentTitle("[${card.agent.uppercase()}] ${card.title}")
            .setContentText(bodyText)
            .setStyle(NotificationCompat.BigTextStyle().bigText(bodyText))
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_MESSAGE)
            .setAutoCancel(true)
            .setContentIntent(openPi)

        // Add direct lock-screen action buttons for quick approval without opening the app!
        card.options.take(3).forEachIndexed { idx, opt ->
            val actionIntent = Intent(context, DecisionActionReceiver::class.java).apply {
                action = DecisionActionReceiver.ACTION_RESOLVE
                putExtra(DecisionActionReceiver.EXTRA_DECISION_ID, card.id)
                putExtra(DecisionActionReceiver.EXTRA_CHOICE, opt.id)
            }
            val actionPi = PendingIntent.getBroadcast(
                context,
                card.id.hashCode() + idx + 1,
                actionIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
            builder.addAction(0, opt.label, actionPi)
        }

        NotificationManagerCompat.from(context).notify(notificationIdForDecision(card.id), builder.build())
    }

    fun cancelDecisionNotification(context: Context, decisionId: String) {
        NotificationManagerCompat.from(context).cancel(notificationIdForDecision(decisionId))
    }

    fun showEventNotification(context: Context, note: BridgeNotification) {
        if (!canPostNotifications(context)) return

        val openIntent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra(EXTRA_OPEN_TAB, "workspaces")
            putExtra(EXTRA_WORKSPACE_ID, note.workspaceId)
            putExtra(EXTRA_SURFACE_ID, note.surfaceId)
            note.paneId?.let { putExtra(EXTRA_PANE_ID, it) }
        }
        val pi = PendingIntent.getActivity(
            context,
            note.id.hashCode(),
            openIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )

        val notification = NotificationCompat.Builder(context, CHANNEL_EVENTS)
            .setSmallIcon(R.drawable.ic_launcher_winmux)
            .setContentTitle("[${note.agent.uppercase()}] ${note.title}")
            .setContentText(note.body.ifBlank { "点击查看终端" })
            .setStyle(NotificationCompat.BigTextStyle().bigText(note.body.ifBlank { "点击查看终端" }))
            .setAutoCancel(true)
            .setContentIntent(pi)
            .build()

        NotificationManagerCompat.from(context).notify(note.id.hashCode(), notification)
    }

    private fun notificationIdForDecision(id: String): Int = 20000 + (id.hashCode() and 0x7FFF)

    private fun canPostNotifications(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return true
        return ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED
    }
}
