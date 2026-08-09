package io.livekit.android.example.voiceassistant.wake

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.AudioManager
import android.media.AudioRecordingConfiguration
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import io.livekit.android.example.voiceassistant.AssistantState
import io.livekit.android.example.voiceassistant.MainActivity
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** What the assistant is currently doing; shown in the persistent notification. */
enum class WakeMode {
    STANDBY,
    ACTIVE,
    SLEEP_ACK,
}

/**
 * Foreground service that owns the wake-word recognizer while the app is in
 * standby. Android 14+ requires a microphone-type foreground service for
 * background mic access, and MIUI aggressively kills plain background apps,
 * so the assistant keeps a persistent notification while listening.
 */
class WakeWordService : Service() {

    private var recognizer: WakeWordRecognizer? = null
    private var externalRecordingActive = false
    private var manuallyPaused = false
    private val allowedRecordingUids = mutableSetOf<Int>()

    private val screenStateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                Intent.ACTION_SCREEN_OFF -> {
                    // MIUI's cloud speech service is usually unavailable in
                    // Doze; use the on-device recognizer while locked.
                    recognizer?.useOnDevice()
                    Log.i(TAG, "Screen off; wake recognizer prefers on-device")
                }
                Intent.ACTION_SCREEN_ON -> {
                    recognizer?.useSystem()
                    Log.i(TAG, "Screen on; wake recognizer restored to system/cloud")
                }
            }
        }
    }

    private val recordingCallback = object : AudioManager.AudioRecordingCallback() {
        override fun onRecordingConfigChanged(configs: MutableList<AudioRecordingConfiguration>) {
            val externalActive = configs.any { config ->
                val uid = clientUidOf(config)
                uid != null && uid !in allowedRecordingUids
            }
            if (externalActive && !externalRecordingActive) {
                externalRecordingActive = true
                stopWakeListening()
                updateNotification(
                    status = _status.value,
                    title = "贾维斯待命中",
                    text = "其他应用占用麦克风，唤醒已暂停",
                )
                Log.w(TAG, "External recording started; paused wake listening")
            } else if (!externalActive && externalRecordingActive) {
                externalRecordingActive = false
                if (_mode.value == WakeMode.STANDBY) {
                    startWakeListening()
                }
                updateNotification(_status.value)
                Log.i(TAG, "External recording stopped; resumed wake listening")
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        allowedRecordingUids.add(Process.myUid())
        resolveUid("com.xiaomi.mibrain.speech")?.let(allowedRecordingUids::add)
        resolveUid("com.miui.voiceassist")?.let(allowedRecordingUids::add)
        resolveUid("com.miui.voicetrigger")?.let(allowedRecordingUids::add)
        getSystemService(AudioManager::class.java)
            ?.registerAudioRecordingCallback(recordingCallback, null)
        registerReceiver(
            screenStateReceiver,
            IntentFilter().apply {
                addAction(Intent.ACTION_SCREEN_OFF)
                addAction(Intent.ACTION_SCREEN_ON)
            },
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopWakeListening()
                AssistantState.setActive(this, false)
                ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_PAUSE -> {
                manuallyPaused = !manuallyPaused
                if (manuallyPaused) {
                    stopWakeListening()
                } else if (_mode.value == WakeMode.STANDBY && !externalRecordingActive) {
                    startWakeListening()
                }
                startForegroundCompat()
                return START_STICKY
            }
        }

        startForegroundCompat()
        if (
            _mode.value == WakeMode.STANDBY &&
            !externalRecordingActive &&
            !manuallyPaused
        ) {
            startWakeListening()
        } else {
            stopWakeListening()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        getSystemService(AudioManager::class.java)
            ?.unregisterAudioRecordingCallback(recordingCallback)
        runCatching { unregisterReceiver(screenStateReceiver) }
        stopWakeListening(destroy = true)
        super.onDestroy()
    }

    private fun handleWake() {
        stopWakeListening()
        completeWake()
    }

    private fun completeWake() {
        _wakePending.value = true
        updateNotification(
            status = WakeRecognizerStatus.CLOUD,
            title = "已识别“贾维斯”",
            text = "点击打开对话",
        )

        val activityIntent = Intent(this, MainActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            putExtra(EXTRA_AUTO_WAKE, true)
        }
        runCatching { startActivity(activityIntent) }
            .onFailure { error ->
                Log.w(
                    TAG,
                    "Wake detected but could not bring the app to the foreground; " +
                        "tap the notification to open it",
                    error,
                )
            }
    }

    private fun startWakeListening() {
        if (externalRecordingActive || manuallyPaused) return
        ensureRecognizer()
        recognizer?.start()
    }

    private fun stopWakeListening(destroy: Boolean = false) {
        recognizer?.stop()
        if (destroy) destroyRecognizer()
    }

    private fun ensureRecognizer() {
        if (recognizer != null) return
        recognizer = WakeWordRecognizer(applicationContext) {
            handleWake()
        }.also { wakeRecognizer ->
            wakeRecognizer.onStatusChanged = { status ->
                _status.value = status
                updateNotification(status)
            }
        }
    }

    private fun destroyRecognizer() {
        recognizer?.destroy()
        recognizer = null
    }

    private fun startForegroundCompat() {
        val mode = _mode.value
        val notification = buildNotification(
            status = _status.value,
            title = notificationTitle(mode),
            text = currentNotificationText(mode),
        )
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun updateNotification(
        status: WakeRecognizerStatus,
        title: String = notificationTitle(_mode.value),
        text: String = currentNotificationText(_mode.value),
    ) {
        val notificationManager = getSystemService(NotificationManager::class.java)
        notificationManager?.notify(
            NOTIFICATION_ID,
            buildNotification(status, title, text),
        )
    }

    private fun buildNotification(
        status: WakeRecognizerStatus,
        title: String,
        text: String,
    ): android.app.Notification {
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).putExtra(EXTRA_AUTO_WAKE, true),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val stopIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, WakeWordService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val pauseIntent = PendingIntent.getService(
            this,
            2,
            Intent(this, WakeWordService::class.java).setAction(ACTION_PAUSE),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setVisibility(android.app.Notification.VISIBILITY_PUBLIC)
            .addAction(
                0,
                if (manuallyPaused) "恢复唤醒" else "暂停唤醒",
                pauseIntent,
            )
            .addAction(0, "停止监听", stopIntent)
            .build()
    }

    private fun currentNotificationText(mode: WakeMode): String = when {
        manuallyPaused -> "唤醒已暂停（手动）"
        externalRecordingActive -> "其他应用占用麦克风，唤醒已暂停"
        else -> notificationText(mode, _status.value)
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val channel = NotificationChannel(
            CHANNEL_ID,
            "贾维斯唤醒监听",
            NotificationManager.IMPORTANCE_LOW,
        )
        channel.description = "待机时监听“贾维斯”唤醒词"
        channel.lockscreenVisibility = android.app.Notification.VISIBILITY_PUBLIC
        getSystemService(NotificationManager::class.java)
            ?.createNotificationChannel(channel)
    }

    private fun resolveUid(packageName: String): Int? =
        try {
            packageManager.getApplicationInfo(packageName, 0).uid
        } catch (error: PackageManager.NameNotFoundException) {
            null
        }

    private fun clientUidOf(config: AudioRecordingConfiguration): Int? =
        try {
            config.javaClass.getMethod("getClientUid").invoke(config) as? Int
        } catch (_: Throwable) {
            null
        }

    companion object {
        private const val TAG = "WakeWordService"
        private const val CHANNEL_ID = "wake_word"
        private const val NOTIFICATION_ID = 1001

        const val ACTION_START =
            "io.livekit.android.example.voiceassistant.wake.START"
        const val ACTION_STOP =
            "io.livekit.android.example.voiceassistant.wake.STOP"
        const val ACTION_PAUSE =
            "io.livekit.android.example.voiceassistant.wake.PAUSE"
        const val EXTRA_AUTO_WAKE = "auto_wake"

        private val _status = MutableStateFlow(WakeRecognizerStatus.CLOUD)
        val status: StateFlow<WakeRecognizerStatus> = _status

        private val _wakePending = MutableStateFlow(false)
        val wakePending: StateFlow<Boolean> = _wakePending

        private val _appForeground = MutableStateFlow(false)
        val appForeground: StateFlow<Boolean> = _appForeground

        private val _mode = MutableStateFlow(WakeMode.STANDBY)
        val mode: StateFlow<WakeMode> = _mode

        fun consumeWake() {
            _wakePending.value = false
        }

        fun setAppForeground(foreground: Boolean) {
            _appForeground.value = foreground
        }

        fun setMode(context: Context, mode: WakeMode) {
            _mode.value = mode
            start(context)
        }

        fun start(context: Context) {
            val intent = Intent(context, WakeWordService::class.java)
                .setAction(ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, WakeWordService::class.java))
        }

        private fun defaultStatusText(status: WakeRecognizerStatus): String =
            when (status) {
                WakeRecognizerStatus.CLOUD -> "云端唤醒监听中"
                WakeRecognizerStatus.ON_DEVICE -> "本地唤醒（云端不可用，稍后自动恢复）"
                WakeRecognizerStatus.STUCK_RESTARTING -> "识别器卡住，正在重启…"
            }

        private fun notificationTitle(mode: WakeMode): String =
            when (mode) {
                WakeMode.STANDBY -> "贾维斯待命中"
                WakeMode.ACTIVE -> "对话中"
                WakeMode.SLEEP_ACK -> "正在播报待机确认…"
            }

        private fun notificationText(mode: WakeMode, status: WakeRecognizerStatus): String =
            when (mode) {
                WakeMode.STANDBY -> defaultStatusText(status)
                WakeMode.ACTIVE -> "点击回到对话"
                WakeMode.SLEEP_ACK -> "等待语音确认"
            }
    }
}
