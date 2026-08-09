package io.livekit.android.example.voiceassistant.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.SavedStateHandle
import androidx.navigation.toRoute
import io.livekit.android.LiveKit
import io.livekit.android.example.voiceassistant.createLocalTokenSource
import io.livekit.android.example.voiceassistant.screen.VoiceAssistantRoute
import io.livekit.android.token.TokenSource
import io.livekit.android.token.cached

/**
 * This ViewModel handles holding onto the Room object, so that it is
 * maintained across configuration changes, such as rotation.
 */
class VoiceAssistantViewModel(application: Application, savedStateHandle: SavedStateHandle) : AndroidViewModel(application) {

    // Keep the default voice-communication audio type: MIUI suppresses media
    // playback from a background/locked app for tens of seconds, while the
    // call route keeps playing. The render gain processor handles loudness.
    val room = LiveKit.create(application)

    val tokenSource: TokenSource

    init {
        val sandboxId = savedStateHandle.toRoute<VoiceAssistantRoute>().sandboxId

        tokenSource = if (sandboxId.isNotEmpty()) {
            TokenSource.fromSandboxTokenServer(sandboxId = sandboxId).cached()
        } else {
            createLocalTokenSource()
        }
    }

    override fun onCleared() {
        super.onCleared()
        room.disconnect()
        room.release()
    }
}
