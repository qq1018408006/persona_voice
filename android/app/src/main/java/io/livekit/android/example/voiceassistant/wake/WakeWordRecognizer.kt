package io.livekit.android.example.voiceassistant.wake

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.util.Log
import java.util.Locale

/** Exposed to the UI so the user can see which recognizer is active and if it is stuck. */
enum class WakeRecognizerStatus {
    /** System/cloud recognizer (XiaoAI / vendor service). */
    CLOUD,

    /** Android on-device recognizer fallback. */
    ON_DEVICE,

    /** No callback arrived inside the watchdog window; the service is being restarted. */
    STUCK_RESTARTING,
}

/**
 * Listens for the wake phrase while the LiveKit microphone track is disabled.
 *
 * The system recognition service is preferred because vendor/cloud engines are
 * noticeably more accurate than the Android on-device recognizer for short
 * phrases like "贾维斯". The on-device recognizer is only a temporary fallback:
 * after a cooldown we switch back to the system service.
 */
class WakeWordRecognizer(
    context: Context,
    private val onWakeWord: () -> Unit,
) : RecognitionListener {

    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())
    private var usingSystemRecognizer = SpeechRecognizer.isRecognitionAvailable(appContext)
    private var recognizer: SpeechRecognizer = createRecognizer(usingSystemRecognizer)
    private var consecutiveNetworkErrors = 0
    private var consecutiveHardErrors = 0

    /** UI/notification listener for the current recognizer state. */
    var onStatusChanged: ((WakeRecognizerStatus) -> Unit)? = null

    val isUsingSystemRecognizer: Boolean
        get() = usingSystemRecognizer

    private val recognitionIntent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
        putExtra(
            RecognizerIntent.EXTRA_LANGUAGE_MODEL,
            RecognizerIntent.LANGUAGE_MODEL_FREE_FORM,
        )
        putExtra(RecognizerIntent.EXTRA_LANGUAGE, Locale.SIMPLIFIED_CHINESE.toLanguageTag())
        putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
        putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 10)
        // Recognizers that implement contextual biasing give these short,
        // easily-confused names more probability. Older/vendor recognizers
        // safely ignore unknown extras.
        putStringArrayListExtra(
            RecognizerIntent.EXTRA_BIASING_STRINGS,
            ArrayList(WakePhraseMatcher.recognitionBiases),
        )
        putExtra(RecognizerIntent.EXTRA_ENABLE_BIASING_DEVICE_CONTEXT, true)
    }

    private var shouldListen = false
    private var listening = false
    private var destroyed = false
    private var preferOnDevice = false

    init {
        recognizer.setRecognitionListener(this)
    }

    private fun createRecognizer(useSystem: Boolean): SpeechRecognizer {
        if (useSystem) return SpeechRecognizer.createSpeechRecognizer(appContext)
        return if (
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            SpeechRecognizer.isOnDeviceRecognitionAvailable(appContext)
        ) {
            SpeechRecognizer.createOnDeviceSpeechRecognizer(appContext)
        } else {
            SpeechRecognizer.createSpeechRecognizer(appContext)
        }
    }

    private fun setStatus(status: WakeRecognizerStatus) {
        onStatusChanged?.invoke(status)
    }

    /**
     * Destroys the old recognizer binding and creates a fresh one. MIUI's
     * recognition service keeps VAD/noise-adaptive state per client, so a
     * fresh instance clears any long-lived "insensitive" state.
     */
    private fun recreateRecognizer(useSystem: Boolean) {
        if (destroyed) return
        mainHandler.removeCallbacksAndMessages(null)
        listening = false
        val old = recognizer
        old.setRecognitionListener(NO_OP_LISTENER)
        runCatching { old.cancel() }
        runCatching { old.destroy() }
        usingSystemRecognizer = useSystem
        recognizer = createRecognizer(useSystem)
        recognizer.setRecognitionListener(this)
        consecutiveHardErrors = 0
        setStatus(
            if (useSystem) WakeRecognizerStatus.CLOUD
            else WakeRecognizerStatus.ON_DEVICE
        )
        Log.i(
            TAG,
            if (useSystem) "Using system/cloud recognizer"
            else "Using on-device recognizer",
        )
    }

    /**
     * Temporary fallback: after repeated network/server failures, try the
     * on-device recognizer, then switch back to the cloud service after a
     * cooldown instead of degrading for the whole standby period.
     */
    private fun switchToOnDeviceRecognizer() {
        if (!usingSystemRecognizer) return
        if (
            Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            !SpeechRecognizer.isOnDeviceRecognitionAvailable(appContext)
        ) return

        recreateRecognizer(useSystem = false)
        Log.w(
            TAG,
            "System recognizer failed repeatedly; temporary on-device fallback " +
                "(${ON_DEVICE_COOLDOWN_MILLIS / 1000}s cooldown)",
        )
        scheduleSwitchBackToSystem()
    }

    /**
     * Used while the screen is off: MIUI's cloud speech service is frequently
     * unavailable in Doze (RECOGNIZER_BUSY), so prefer the local recognizer
     * for the whole lock-screen period.
     */
    fun useOnDevice() {
        preferOnDevice = true
        mainHandler.removeCallbacks(SWITCH_BACK_RUNNABLE)
        if (usingSystemRecognizer && shouldListen) {
            if (
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                SpeechRecognizer.isOnDeviceRecognitionAvailable(appContext)
            ) {
                recreateRecognizer(useSystem = false)
                scheduleListening(0)
            }
        }
    }

    /** Called when the screen turns back on: restore the cloud recognizer. */
    fun useSystem() {
        preferOnDevice = false
        mainHandler.removeCallbacks(SWITCH_BACK_RUNNABLE)
        if (!usingSystemRecognizer && shouldListen) {
            recreateRecognizer(useSystem = true)
            scheduleListening(0)
        }
    }

    private fun scheduleSwitchBackToSystem() {
        if (preferOnDevice) return
        mainHandler.removeCallbacks(SWITCH_BACK_RUNNABLE)
        mainHandler.postDelayed(
            SWITCH_BACK_RUNNABLE,
            ON_DEVICE_COOLDOWN_MILLIS,
        )
    }

    private val SWITCH_BACK_RUNNABLE = Runnable {
        if (shouldListen && !destroyed && !usingSystemRecognizer) {
            recreateRecognizer(useSystem = true)
            Log.i(TAG, "Switched back to system/cloud recognizer")
        }
    }

    fun start() {
        if (destroyed || shouldListen) return
        shouldListen = true
        // Recreate on every standby entry so MIUI service-side state is reset.
        recreateRecognizer(!preferOnDevice && usingSystemRecognizer)
        if (!usingSystemRecognizer) {
            scheduleSwitchBackToSystem()
        }
        scheduleListening(0)
    }

    fun stop() {
        shouldListen = false
        mainHandler.removeCallbacksAndMessages(null)
        if (listening) {
            listening = false
            runCatching { recognizer.cancel() }
        }
    }

    fun destroy() {
        if (destroyed) return
        stop()
        destroyed = true
        mainHandler.removeCallbacksAndMessages(null)
        runCatching { recognizer.destroy() }
    }

    private fun scheduleListening(delayMillis: Long) {
        mainHandler.postDelayed(
            {
                if (!shouldListen || destroyed || listening) return@postDelayed
                try {
                    listening = true
                    recognizer.startListening(recognitionIntent)
                    armWatchdog()
                } catch (error: Exception) {
                    listening = false
                    Log.w(TAG, "Unable to start wake-word recognition", error)
                    if (shouldListen) scheduleListening(RETRY_DELAY_MILLIS)
                }
            },
            delayMillis,
        )
    }

    private fun armWatchdog() {
        mainHandler.removeCallbacks(WATCHDOG_RUNNABLE)
        mainHandler.postDelayed(WATCHDOG_RUNNABLE, WATCHDOG_TIMEOUT_MILLIS)
    }

    private val WATCHDOG_RUNNABLE = Runnable {
        if (!shouldListen || destroyed || !listening) return@Runnable
        Log.w(
            TAG,
            "Recognizer stuck (no callback for ${WATCHDOG_TIMEOUT_MILLIS}ms); restarting",
        )
        setStatus(WakeRecognizerStatus.STUCK_RESTARTING)
        listening = false
        runCatching { recognizer.cancel() }
        scheduleListening(0)
    }

    private fun handleResults(results: Bundle?) {
        // MIUI's speech service can deliver buffered results after stop()/cancel();
        // never act on them once listening has been disabled.
        if (!shouldListen || destroyed) return
        val candidates = results
            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
            .orEmpty()

        if (candidates.isNotEmpty()) {
            Log.d(TAG, "Recognition candidates: ${candidates.joinToString()}")
        }

        if (candidates.any(WakePhraseMatcher::containsWakePhrase)) {
            shouldListen = false
            listening = false
            mainHandler.removeCallbacksAndMessages(null)
            recognizer.cancel()
            onWakeWord()
        }
    }

    private fun restart(delayMillis: Long = RETRY_DELAY_MILLIS) {
        listening = false
        mainHandler.removeCallbacks(WATCHDOG_RUNNABLE)
        if (shouldListen) scheduleListening(delayMillis)
    }

    override fun onReadyForSpeech(params: Bundle?) = Unit
    override fun onBeginningOfSpeech() = Unit
    override fun onRmsChanged(rmsdB: Float) = Unit
    override fun onBufferReceived(buffer: ByteArray?) = Unit
    override fun onEndOfSpeech() = Unit

    override fun onError(error: Int) {
        Log.d(TAG, "onError code=$error")
        mainHandler.removeCallbacks(WATCHDOG_RUNNABLE)
        val delay = when (error) {
            SpeechRecognizer.ERROR_NO_MATCH,
            SpeechRecognizer.ERROR_SPEECH_TIMEOUT,
            -> {
                consecutiveHardErrors = 0
                SHORT_RETRY_DELAY_MILLIS
            }
            SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> {
                consecutiveHardErrors++
                if (consecutiveHardErrors >= MAX_HARD_ERRORS) {
                    if (usingSystemRecognizer) {
                        switchToOnDeviceRecognizer()
                    } else {
                        recreateRecognizer(usingSystemRecognizer)
                    }
                }
                BUSY_RETRY_DELAY_MILLIS
            }
            SpeechRecognizer.ERROR_NETWORK,
            SpeechRecognizer.ERROR_SERVER,
            -> {
                consecutiveNetworkErrors++
                if (
                    consecutiveNetworkErrors >= MAX_NETWORK_ERRORS &&
                    usingSystemRecognizer
                ) {
                    switchToOnDeviceRecognizer()
                }
                RETRY_DELAY_MILLIS
            }
            SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> {
                // MIUI also reports this transiently while LiveKit is releasing
                // communication audio mode, even when RECORD_AUDIO is granted.
                Log.w(TAG, "Recognizer could not acquire the microphone; retrying")
                PERMISSION_RETRY_DELAY_MILLIS
            }
            SpeechRecognizer.ERROR_CLIENT -> {
                consecutiveHardErrors++
                if (consecutiveHardErrors >= MAX_HARD_ERRORS) {
                    recreateRecognizer(usingSystemRecognizer)
                }
                RETRY_DELAY_MILLIS
            }
            else -> RETRY_DELAY_MILLIS
        }
        restart(delay)
    }

    override fun onResults(results: Bundle?) {
        consecutiveNetworkErrors = 0
        consecutiveHardErrors = 0
        mainHandler.removeCallbacks(WATCHDOG_RUNNABLE)
        handleResults(results)
        if (shouldListen) restart(SHORT_RETRY_DELAY_MILLIS)
    }

    override fun onPartialResults(partialResults: Bundle?) {
        // A session that keeps emitting partials but never ends is still
        // healthy; reset the watchdog instead of letting it kill the session.
        armWatchdog()
        handleResults(partialResults)
    }

    override fun onEvent(eventType: Int, params: Bundle?) = Unit

    companion object {
        private const val TAG = "WakeWordRecognizer"
        private const val SHORT_RETRY_DELAY_MILLIS = 80L
        private const val RETRY_DELAY_MILLIS = 300L
        private const val BUSY_RETRY_DELAY_MILLIS = 750L
        private const val PERMISSION_RETRY_DELAY_MILLIS = 1_000L
        private const val MAX_NETWORK_ERRORS = 3
        private const val MAX_HARD_ERRORS = 5
        private const val WATCHDOG_TIMEOUT_MILLIS = 15_000L
        private const val ON_DEVICE_COOLDOWN_MILLIS = 60_000L
        private val NO_OP_LISTENER = object : RecognitionListener {
            override fun onReadyForSpeech(params: Bundle?) = Unit
            override fun onBeginningOfSpeech() = Unit
            override fun onRmsChanged(rmsdB: Float) = Unit
            override fun onBufferReceived(buffer: ByteArray?) = Unit
            override fun onEndOfSpeech() = Unit
            override fun onError(error: Int) = Unit
            override fun onResults(results: Bundle?) = Unit
            override fun onPartialResults(partialResults: Bundle?) = Unit
            override fun onEvent(eventType: Int, params: Bundle?) = Unit
        }
    }
}
