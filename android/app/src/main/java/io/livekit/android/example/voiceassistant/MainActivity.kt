package io.livekit.android.example.voiceassistant

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.ui.Modifier
import androidx.compose.runtime.LaunchedEffect
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.NavDestination.Companion.hasRoute
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import io.livekit.android.LiveKit
import io.livekit.android.example.voiceassistant.screen.ConnectRoute
import io.livekit.android.example.voiceassistant.screen.ConnectScreen
import io.livekit.android.example.voiceassistant.screen.VoiceAssistantRoute
import io.livekit.android.example.voiceassistant.screen.VoiceAssistantScreen
import io.livekit.android.example.voiceassistant.ui.theme.LiveKitVoiceAssistantExampleTheme
import io.livekit.android.example.voiceassistant.viewmodel.VoiceAssistantViewModel
import io.livekit.android.example.voiceassistant.wake.WakeWordService
import io.livekit.android.util.LoggingLevel

class MainActivity : ComponentActivity() {

    private var navController: NavHostController? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        LiveKit.loggingLevel = LoggingLevel.DEBUG

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(
                WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
                    WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON,
            )
        }

        setContent {
            val navController = rememberNavController()
            this.navController = navController
            LiveKitVoiceAssistantExampleTheme(dynamicColor = false) {
                Scaffold { innerPadding ->
                    Box(modifier = Modifier.padding(innerPadding)) {

                        // Default entry is the home page (future settings
                        // entry point). Only a cold start triggered by the
                        // wake intent goes straight to the voice page.
                        val startDestination =
                            if (intent.getBooleanExtra(WakeWordService.EXTRA_AUTO_WAKE, false)) {
                                VoiceAssistantRoute(sandboxId = "")
                            } else {
                                ConnectRoute
                            }
                        NavHost(navController, startDestination = startDestination) {
                            composable<ConnectRoute> {
                                ConnectScreen(navigateToVoiceAssistant = { voiceAssistantRoute ->
                                    runOnUiThread {
                                        navController.navigate(voiceAssistantRoute)
                                    }
                                })
                            }

                            composable<VoiceAssistantRoute> {
                                val viewModel = viewModel<VoiceAssistantViewModel>()
                                VoiceAssistantScreen(
                                    viewModel = viewModel,
                                    onEndCall = {
                                        runOnUiThread { navController.navigateUp() }
                                    }
                                )
                            }
                        }
                    }
                }
            }

        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.getBooleanExtra(WakeWordService.EXTRA_AUTO_WAKE, false)) {
            navigateToVoiceAssistant()
        }
    }

    override fun onResume() {
        super.onResume()
        WakeWordService.setAppForeground(true)
    }

    override fun onPause() {
        super.onPause()
        WakeWordService.setAppForeground(false)
    }

    private fun navigateToVoiceAssistant() {
        val nav = navController ?: return
        // Already on the voice page: do nothing, otherwise the wake intent
        // causes a visible re-navigation / reload flash.
        if (nav.currentDestination?.hasRoute<VoiceAssistantRoute>() == true) return
        nav.navigate(VoiceAssistantRoute(sandboxId = "")) {
            launchSingleTop = true
        }
    }
}
