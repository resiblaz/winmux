package com.winmux.mobile

import android.app.Application
import com.winmux.mobile.data.WinmuxRepository
import com.winmux.mobile.service.NotificationHelper

class WinmuxApp : Application() {
    override fun onCreate() {
        super.onCreate()
        NotificationHelper.ensureChannels(this)
        WinmuxRepository.initialize(this)
    }
}
