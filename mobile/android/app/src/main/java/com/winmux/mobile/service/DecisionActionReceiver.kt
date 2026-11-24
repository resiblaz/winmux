package com.winmux.mobile.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.winmux.mobile.data.WinmuxRepository
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class DecisionActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_RESOLVE) return
        val decisionId = intent.getStringExtra(EXTRA_DECISION_ID) ?: return
        val choice = intent.getStringExtra(EXTRA_CHOICE) ?: return

        val pendingResult = goAsync()
        WinmuxRepository.initialize(context)
        CoroutineScope(Dispatchers.IO).launch {
            try {
                WinmuxRepository.resolveDecision(decisionId = decisionId, decision = choice)
            } finally {
                pendingResult.finish()
            }
        }
    }

    companion object {
        const val ACTION_RESOLVE = "com.winmux.mobile.ACTION_RESOLVE_DECISION"
        const val EXTRA_DECISION_ID = "decision_id"
        const val EXTRA_CHOICE = "choice"
    }
}
