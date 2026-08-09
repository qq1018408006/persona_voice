package io.livekit.android.example.voiceassistant

import android.content.Context

/**
 * Remembers whether the assistant screen is the intended destination, so a
 * cold start after backgrounding/locking still opens the voice page instead
 * of the ConnectScreen home page.
 */
object AssistantState {
    private const val PREFS_NAME = "assistant_state"
    private const val KEY_ACTIVE = "assistant_active"

    fun isActive(context: Context): Boolean =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getBoolean(KEY_ACTIVE, false)

    fun setActive(context: Context, active: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_ACTIVE, active)
            .apply()
    }
}
