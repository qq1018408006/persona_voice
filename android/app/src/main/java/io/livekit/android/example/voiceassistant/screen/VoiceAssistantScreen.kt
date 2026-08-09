package io.livekit.android.example.voiceassistant.screen

import android.app.Activity
import android.content.Context
import android.content.Context.MEDIA_PROJECTION_SERVICE
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.projection.MediaProjectionManager
import android.media.ToneGenerator
import android.net.Uri
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Cameraswitch
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.constraintlayout.compose.ChainStyle
import androidx.constraintlayout.compose.ConstrainScope
import androidx.constraintlayout.compose.ConstraintLayout
import androidx.constraintlayout.compose.ConstraintSet
import androidx.constraintlayout.compose.Dimension
import androidx.constraintlayout.compose.Visibility
import androidx.constraintlayout.compose.layoutId
import com.google.accompanist.permissions.ExperimentalPermissionsApi
import io.livekit.android.annotations.Beta
import io.livekit.android.events.RoomEvent
import io.livekit.android.compose.local.SessionScope
import io.livekit.android.compose.local.requireRoom
import io.livekit.android.compose.state.SessionConnectOptions
import io.livekit.android.compose.state.SessionConnectTrackOptions
import io.livekit.android.compose.state.SessionOptions
import io.livekit.android.compose.state.rememberAgent
import io.livekit.android.compose.state.rememberLocalMedia
import io.livekit.android.compose.state.rememberSession
import io.livekit.android.compose.types.ReceivedUserTranscriptionMessage
import io.livekit.android.compose.ui.VideoTrackView
import io.livekit.android.example.voiceassistant.rememberCanEnableCamera
import io.livekit.android.example.voiceassistant.rememberCanEnableMic
import io.livekit.android.example.voiceassistant.requirePermissions
import io.livekit.android.example.voiceassistant.AssistantState
import io.livekit.android.example.voiceassistant.audio.RenderGainProcessor
import io.livekit.android.example.voiceassistant.session.rememberSafeSessionMessages
import io.livekit.android.example.voiceassistant.ui.AgentVisualization
import io.livekit.android.example.voiceassistant.ui.ChatBar
import io.livekit.android.example.voiceassistant.ui.ChatLog
import io.livekit.android.example.voiceassistant.ui.ControlBar
import io.livekit.android.example.voiceassistant.viewmodel.VoiceAssistantViewModel
import io.livekit.android.example.voiceassistant.wake.WakePhraseMatcher
import io.livekit.android.example.voiceassistant.wake.WakeRecognizerStatus
import io.livekit.android.example.voiceassistant.wake.WakeMode
import io.livekit.android.example.voiceassistant.wake.WakeWordService
import io.livekit.android.room.track.DataPublishReliability
import io.livekit.android.room.track.screencapture.ScreenCaptureParams
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable

@Serializable
data class VoiceAssistantRoute(
    val sandboxId: String,
)

@Composable
fun VoiceAssistantScreen(
    viewModel: VoiceAssistantViewModel,
    onEndCall: () -> Unit,
) {
    VoiceAssistant(
        viewModel = viewModel,
        modifier = Modifier.fillMaxSize(),
        onEndCall = onEndCall
    )
}

@OptIn(Beta::class, ExperimentalPermissionsApi::class)
@Composable
fun VoiceAssistant(
    viewModel: VoiceAssistantViewModel,
    modifier: Modifier = Modifier,
    onEndCall: () -> Unit
) {
    val context = LocalContext.current

    // If the foreground service detected the wake phrase in the background it
    // launches this screen with EXTRA_AUTO_WAKE so we skip standby.
    val autoWake = remember {
        (context as? Activity)?.intent
            ?.getBooleanExtra(WakeWordService.EXTRA_AUTO_WAKE, false) == true
    }

    // Start in standby. Android listens locally for "贾维斯" while the
    // LiveKit microphone track is disabled, so idle room audio is not sent to Gemini.
    var isStandby by rememberSaveable { mutableStateOf(!autoWake) }
    var requestedAudio by rememberSaveable { mutableStateOf(autoWake) }
    var requestedVideo by remember { mutableStateOf(false) }
    var wakeConnectionPending by rememberSaveable { mutableStateOf(autoWake) }

    // Wake-word recognition also needs microphone permission while LiveKit is muted.
    requirePermissions(microphone = true, camera = requestedVideo)

    val canEnableMic by rememberCanEnableMic()
    val canEnableVideo by rememberCanEnableCamera()

    val wakeStatus by WakeWordService.status.collectAsState()
    val appForeground by WakeWordService.appForeground.collectAsState()

    LaunchedEffect(Unit) {
        AssistantState.setActive(context, true)
    }

    val session = rememberSession(
        tokenSource = viewModel.tokenSource,
        options = SessionOptions(
            room = viewModel.room
        )
    )

    val feedbackTone = remember {
        // STREAM_NOTIFICATION is commonly muted by MIUI/DND. The assistant's
        // normal audio uses the media stream, so feedback follows that volume.
        ToneGenerator(AudioManager.STREAM_MUSIC, FEEDBACK_TONE_VOLUME)
    }

    DisposableEffect(feedbackTone) {
        onDispose {
            AssistantState.setActive(context, false)
            // Deliberately do NOT stop the wake service here: MIUI can destroy
            // the activity while the screen is off, which would otherwise kill
            // lock-screen wake recognition. The notification's "停止监听"
            // action remains the explicit way to stop it.
            feedbackTone.release()
        }
    }

    SessionScope(session = session) { session ->

        var standbyPending by remember { mutableStateOf(false) }

        // A muted LiveKit room still keeps Android in communication audio mode on
        // some MIUI devices, which prevents SpeechRecognizer from opening the mic.
        // Standby therefore disconnects the realtime session completely. Waking
        // reconnects it with microphone publishing disabled until Android's local
        // recognizer has released AudioRecord.
        LaunchedEffect(canEnableMic, isStandby) {
            if (!canEnableMic) {
                WakeWordService.stop(context)
                return@LaunchedEffect
            }

            if (isStandby) {
                // MIUI kills a render AudioTrack that stays open during
                // standby, so the next wake's audio would be silently dropped
                // (the "second round hears first round's tail" symptom).
                // Disconnect fully in standby and let wake reconnect fresh.
                if (session.isConnected || session.isReconnecting) {
                    session.end()
                    session.waitUntilDisconnected()
                }
                // MIUI releases MODE_IN_COMMUNICATION asynchronously; wait
                // until it is back to MODE_NORMAL before the wake recognizer
                // tries to open the microphone.
                val audioReady = waitForAudioModeNormal(context)
                Log.d(
                    "WakeWord",
                    "standby enabled; audioModeNormal=$audioReady; " +
                        "LiveKit session disconnected",
                )
                WakeWordService.setMode(context, WakeMode.STANDBY)
                return@LaunchedEffect
            }

            val result = session.start(
                SessionConnectOptions(
                    tracks = SessionConnectTrackOptions(
                        microphoneEnabled = false,
                        usePreconnectBuffer = false,
                    ),
                ),
            )
            if (result.isFailure) {
                Toast.makeText(
                    context,
                    "Error connecting to the session.",
                    Toast.LENGTH_SHORT,
                ).show()
                wakeConnectionPending = false
                requestedAudio = false
                isStandby = true
            }
        }

        // A wake detected by the foreground service while this screen was in
        // the background reaches us through this flow.
        LaunchedEffect(Unit) {
            WakeWordService.wakePending.collect { pending ->
                if (pending && isStandby) {
                    Log.d("WakeWord", "wake request from background service")
                    // Warm the voice-call audio route immediately so the
                    // assistant's first reply is not swallowed by MIUI's
                    // ~1s route-switch mute on the call stream.
                    runCatching {
                        val audioManager =
                            context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
                        audioManager.isSpeakerphoneOn = true
                    }
                    // MIUI mutes the first ~1s of a freshly switched voice-call
                    // route, which would cut the start of the reply. Play a
                    // short silent voice-communication track now so that mute
                    // happens before Gemini's audio arrives.
                    Thread {
                        runCatching { warmVoiceRoute(context) }
                    }.start()
                    playFeedbackTone(
                        feedbackTone,
                        ToneGenerator.TONE_PROP_ACK,
                        WAKE_DETECTED_TONE_DURATION_MILLIS,
                    )
                    vibrateFeedback(context, WAKE_DETECTED_VIBRATION)
                    wakeConnectionPending = true
                    isStandby = false
                    requestedAudio = true
                    WakeWordService.consumeWake()
                    Toast.makeText(
                        context,
                        "已识别唤醒词，正在连接…",
                        Toast.LENGTH_SHORT,
                    ).show()
                }
            }
        }

        // Keep the persistent notification alive during the conversation.
        LaunchedEffect(appForeground, isStandby, session.connectionState) {
            if (!isStandby && session.isConnected) {
                WakeWordService.setMode(context, WakeMode.ACTIVE)
            }
        }

        // End the session when leaving the screen.
        DisposableEffect(Unit) {
            onDispose {
                session.end()
            }
        }

        val room = requireRoom()

        // Boost remote assistant audio so short wake/sleep confirmations stay
        // audible, especially with the screen off.
        val renderGainProcessor = remember { RenderGainProcessor() }
        LaunchedEffect(room) {
            room.audioProcessingController.renderPreProcessor = renderGainProcessor
            room.audioProcessorIsEnabled = true
        }

        var chatVisible by remember { mutableStateOf(false) }
        // LocalMedia provides state information about the user's local devices
        val localMedia = rememberLocalMedia()
        val isMicEnabled by localMedia::isMicrophoneEnabled
        val isCameraEnabled by localMedia::isCameraEnabled
        val isScreenShareEnabled by localMedia::isScreenShareEnabled

        LaunchedEffect(
            session.connectionState,
            room,
            canEnableMic,
            requestedAudio,
            isStandby,
        ) {
            if (!canEnableMic || !session.isConnected) return@LaunchedEffect

            if (isStandby) {
                room.localParticipant.setMicrophoneEnabled(false)
                return@LaunchedEffect
            }

            if (!requestedAudio) {
                room.localParticipant.setMicrophoneEnabled(false)
                WakeWordService.setMode(
                    context,
                    if (standbyPending) WakeMode.SLEEP_ACK else WakeMode.ACTIVE,
                )
                Log.d("MicDebug", "microphone disabled")
                return@LaunchedEffect
            }

            // The current device can initially publish a silent capture track.
            // Restarting it once is equivalent to toggling the mic twice manually.
            val wakePending = wakeConnectionPending
            wakeConnectionPending = false

            room.localParticipant.setMicrophoneEnabled(false)
            delay(100)

            val success = room.localParticipant.setMicrophoneEnabled(true)
            Log.d("MicDebug", "microphone restarted after wake, success=$success")

            if (success) {
                // Keep the foreground service/notification alive so it is
                // obvious the assistant is still running during the call.
                WakeWordService.setMode(context, WakeMode.ACTIVE)
            }

            if (wakePending) {
                if (success) {
                    // Let MIUI finish switching the voice-call route before
                    // Gemini's reply starts; otherwise the first ~1s of the
                    // reply is silently dropped by the route-switch mute.
                    delay(WAKE_REPLY_ROUTE_SETTLE_MILLIS)
                    runCatching {
                        room.localParticipant.publishData(
                            data = """{"event":"local_wake","phrase":"贾维斯"}"""
                                .toByteArray(Charsets.UTF_8),
                            reliability = DataPublishReliability.RELIABLE,
                            topic = "sparkbot.wake",
                        )
                    }.onFailure { error ->
                        Log.w("WakeWord", "failed to publish wake event", error)
                    }
                    playFeedbackTone(
                        feedbackTone,
                        ToneGenerator.TONE_CDMA_CONFIRM,
                        WAKE_READY_TONE_DURATION_MILLIS,
                    )
                    vibrateFeedback(context, WAKE_READY_VIBRATION)
                    Toast.makeText(
                        context,
                        "已唤醒，我在，请说",
                        Toast.LENGTH_SHORT,
                    ).show()
                } else {
                    Toast.makeText(
                        context,
                        "麦克风启动失败，请重试",
                        Toast.LENGTH_SHORT,
                    ).show()
                    requestedAudio = false
                    isStandby = true
                }
            }
        }

        LaunchedEffect(canEnableVideo, requestedVideo) {
            session.waitUntilConnected()
            localMedia.setCameraEnabled(canEnableVideo && requestedVideo)
        }

        // SessionMessages handles all transcriptions and chat messages
        val sessionMessages = rememberSafeSessionMessages(room)
        var handledSleepMessageId by rememberSaveable { mutableStateOf<String?>(null) }

        // User speech transcriptions are already delivered by LiveKit. Detecting the
        // "休息吧" here avoids opening a second Android microphone capture while
        // Gemini is actively listening.
        // Publish the sleep request synchronously on first match: the previous
        // 2.5 s grace delay waited for the agent's request_standby tool, which no
        // longer exists in the no-voiceprint baseline. That delay lived inside a
        // LaunchedEffect keyed on the message list, so any new transcription
        // (partial update, final result, agent reply) cancelled the effect and
        // the request was never sent for that utterance.
        LaunchedEffect(sessionMessages.messages) {
            if (isStandby || standbyPending) return@LaunchedEffect

            val latestUserTranscript = sessionMessages.messages
                .lastOrNull { it is ReceivedUserTranscriptionMessage }
                as? ReceivedUserTranscriptionMessage

            if (
                latestUserTranscript != null &&
                latestUserTranscript.id != handledSleepMessageId &&
                WakePhraseMatcher.containsSleepPhrase(latestUserTranscript.message)
            ) {
                handledSleepMessageId = latestUserTranscript.id
                // Ask the agent for the orderly sleep acknowledgement; keep the
                // session connected until Gemini has spoken it (sleep_ready).
                // Deliberately no sparkbot.stop here: resetting the realtime
                // transport cuts off the confirmation audio.
                runCatching {
                    room.localParticipant.publishData(
                        data = """{"event":"local_sleep_request","phrase":"休息吧"}"""
                            .toByteArray(Charsets.UTF_8),
                        reliability = DataPublishReliability.RELIABLE,
                        topic = "sparkbot.sleep_request",
                    )
                }.onFailure { error ->
                    Log.w("WakeWord", "failed to publish sleep request", error)
                }
                requestedAudio = false
                standbyPending = true
                WakeWordService.setMode(context, WakeMode.SLEEP_ACK)
                playFeedbackTone(
                    feedbackTone,
                    ToneGenerator.TONE_PROP_NACK,
                    STANDBY_TONE_DURATION_MILLIS,
                )
                vibrateFeedback(context, STANDBY_VIBRATION)
                Log.d("WakeWord", "sleep phrase detected; waiting for agent ack")
                Toast.makeText(context, "正在播报待机确认…", Toast.LENGTH_SHORT).show()
            }
        }

        // Gemini may understand the user's intent to rest even when the raw
        // transcription is in another language/script (e.g. "휴식 봐" for
        // "休息吧"). The agent sends this packet when it decides to enter
        // standby, so the app doesn't have to rely on Chinese text matching.
        LaunchedEffect(room) {
            room.events.events
                .filterIsInstance<RoomEvent.DataReceived>()
                .filter { it.topic == "sparkbot.android_standby" }
                .collect {
                    if (isStandby || standbyPending) return@collect
                    // Gemini already spoke its confirmation via the
                    // request_standby tool. Enter standby directly instead of
                    // asking the agent for a second acknowledgement.
                    Log.d("WakeWord", "agent requested standby; entering standby")
                    requestedAudio = false
                    standbyPending = false
                    isStandby = true
                    playFeedbackTone(
                        feedbackTone,
                        ToneGenerator.TONE_PROP_NACK,
                        STANDBY_TONE_DURATION_MILLIS,
                    )
                    vibrateFeedback(context, STANDBY_VIBRATION)
                    Toast.makeText(context, "正在进入待机…", Toast.LENGTH_SHORT).show()
                }
        }

        // The agent sends this after Gemini has finished speaking the standby
        // acknowledgement; only then disconnect and re-arm local wake.
        LaunchedEffect(room) {
            room.events.events
                .filterIsInstance<RoomEvent.DataReceived>()
                .filter { it.topic == "sparkbot.sleep_ready" }
                .collect {
                    if (standbyPending) {
                        Log.d("WakeWord", "agent sleep ready; entering standby")
                        standbyPending = false
                        isStandby = true
                    }
                }
        }

        // Safety net: if the ack never arrives (network/session issue), still
        // enter standby so the wake service comes back.
        LaunchedEffect(standbyPending) {
            if (!standbyPending) return@LaunchedEffect
            delay(STANDBY_ACK_TIMEOUT_MILLIS)
            if (standbyPending) {
                Log.w("WakeWord", "standby ack timeout; entering standby anyway")
                standbyPending = false
                isStandby = true
            }
        }

        // Agent provides state information about the agent participant.
        val agent = rememberAgent()

        val constraints = getConstraints(chatVisible, isCameraEnabled, isScreenShareEnabled)
        Box(modifier = modifier) {
            ConstraintLayout(
                constraintSet = constraints,
                modifier = Modifier.fillMaxSize(),
                animateChangesSpec = spring(),
            ) {
            val coroutineScope = rememberCoroutineScope { Dispatchers.IO }

            ChatLog(
                room = room,
                messages = sessionMessages.messages,
                modifier = Modifier.layoutId(LAYOUT_ID_CHAT_LOG)
            )

            var message by rememberSaveable {
                mutableStateOf("")
            }
            ChatBar(
                value = message,
                onValueChange = { message = it },
                onChatSend = { msg ->
                    coroutineScope.launch {
                        sessionMessages.send(msg)
                    }
                    message = ""
                },
                modifier = Modifier.layoutId(LAYOUT_ID_CHAT_BAR)
            )

            // Amplitude visualization of the Assistant's voice track.
            val agentBorderAlpha by animateFloatAsState(if (chatVisible) 1f else 0f, label = "agentBorderAlpha")
            AgentVisualization(
                agent = agent,
                modifier = Modifier
                    .layoutId(LAYOUT_ID_AGENT)
                    .clip(RoundedCornerShape(8.dp))
                    .border(1.dp, MaterialTheme.colorScheme.outline.copy(alpha = agentBorderAlpha), RoundedCornerShape(8.dp))
            )

            val context = LocalContext.current
            val screenSharePermissionLauncher =
                rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
                    val resultCode = result.resultCode
                    val data = result.data
                    if (resultCode != Activity.RESULT_OK || data == null) {
                        return@rememberLauncherForActivityResult
                    }
                    coroutineScope.launch {
                        // Agents only support one video stream at a time.
                        requestedVideo = false
                        localMedia.setScreenShareEnabled(true, ScreenCaptureParams(data))
                    }
                }

            ControlBar(
                isMicEnabled = isMicEnabled,
                onMicClick = {
                    if (isStandby) {
                        isStandby = false
                        requestedAudio = true
                    } else {
                        requestedAudio = !requestedAudio
                    }
                },
                localAudioTrack = localMedia.microphoneTrack,
                isCameraEnabled = isCameraEnabled,
                onCameraClick = {
                    requestedVideo = !requestedVideo
                    if (requestedVideo) {
                        // Agents only support one video stream at a time.
                        coroutineScope.launch { localMedia.setScreenShareEnabled(false) }
                    }
                },
                isScreenShareEnabled = isScreenShareEnabled,
                onScreenShareClick = {
                    if (!isScreenShareEnabled) {
                        // Screenshare permission needs to be requested each time.
                        val mediaProjectionManager = context.getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                        screenSharePermissionLauncher.launch(mediaProjectionManager.createScreenCaptureIntent())
                    } else {
                        coroutineScope.launch { localMedia.setScreenShareEnabled(false) }
                    }
                },
                isChatEnabled = chatVisible,
                onChatClick = { chatVisible = !chatVisible },
                onExitClick = onEndCall,
                modifier = Modifier
                    .layoutId(LAYOUT_ID_CONTROL_BAR)
            )

            val cameraAlpha by animateFloatAsState(targetValue = if (isCameraEnabled) 1f else 0f, label = "Camera Alpha")
            Box(
                modifier = Modifier
                    .layoutId(LAYOUT_ID_CAMERA)
                    .clickable { localMedia.switchCamera() }
                    .clip(RoundedCornerShape(8.dp))
                    .alpha(cameraAlpha)
            ) {
                VideoTrackView(
                    trackReference = localMedia.cameraTrack,
                    modifier = Modifier.fillMaxSize()
                )

                Box(
                    contentAlignment = Alignment.Center,
                    modifier = Modifier
                        .align(Alignment.BottomEnd)
                        .padding(end = 8.dp, bottom = 8.dp)
                        .background(Color.Black.copy(alpha = 0.5f), RoundedCornerShape(50))
                        .fillMaxWidth(.35f)
                        .aspectRatio(1f)
                ) {
                    Icon(
                        Icons.Default.Cameraswitch,
                        tint = Color.White.copy(alpha = 0.7f),
                        contentDescription = "Flip Camera",
                        modifier = Modifier.fillMaxSize(0.6f)
                    )
                }
            }

            val screenShareAlpha by animateFloatAsState(targetValue = if (isScreenShareEnabled) 1f else 0f, label = "Screen Share Alpha")
            VideoTrackView(
                trackReference = localMedia.screenShareTrack,
                modifier = Modifier
                    .layoutId(LAYOUT_ID_SCREENSHARE)
                    .clip(RoundedCornerShape(8.dp))
                    .alpha(screenShareAlpha)
            )
            }

            if (isStandby) {
                Column(
                    modifier = Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = 24.dp)
                        .clip(RoundedCornerShape(24.dp))
                        .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.9f))
                        .padding(horizontal = 20.dp, vertical = 10.dp),
                ) {
                    Text(
                        text = "待命中 · 说“贾维斯”唤醒",
                        color = MaterialTheme.colorScheme.onPrimary,
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        text = when (wakeStatus) {
                            WakeRecognizerStatus.CLOUD -> "云端唤醒监听中"
                            WakeRecognizerStatus.ON_DEVICE -> "本地唤醒（云端不可用）"
                            WakeRecognizerStatus.STUCK_RESTARTING -> "识别器卡住，正在重启…"
                        },
                        color = MaterialTheme.colorScheme.onPrimary.copy(alpha = 0.8f),
                        style = MaterialTheme.typography.bodySmall,
                    )
                    if (!Settings.canDrawOverlays(context)) {
                        Text(
                            text = "未开启“后台弹出”，唤醒不会自动弹窗 · 点此开启",
                            color = MaterialTheme.colorScheme.onPrimary.copy(alpha = 0.9f),
                            style = MaterialTheme.typography.bodySmall,
                            modifier = Modifier.clickable {
                                runCatching {
                                    context.startActivity(
                                        Intent(
                                            Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                            Uri.parse("package:${context.packageName}"),
                                        ),
                                    )
                                }
                            },
                        )
                    }
                }
            } else if (wakeConnectionPending) {
                Text(
                    text = "已识别唤醒词 · 正在连接…",
                    color = MaterialTheme.colorScheme.onPrimary,
                    style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = 24.dp)
                        .clip(RoundedCornerShape(24.dp))
                        .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.9f))
                        .padding(horizontal = 20.dp, vertical = 10.dp),
                )
            } else if (requestedAudio && session.isConnected) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = 24.dp)
                        .clip(RoundedCornerShape(24.dp))
                        .background(MaterialTheme.colorScheme.primary.copy(alpha = 0.9f))
                        .padding(horizontal = 20.dp, vertical = 10.dp),
                ) {
                    Text(
                        text = "已唤醒 · 正在聆听",
                        color = MaterialTheme.colorScheme.onPrimary,
                        style = MaterialTheme.typography.titleMedium,
                    )
                }
            }

        }
    }
}


private const val LAYOUT_ID_AGENT = "agentVisualizer"
private const val LAYOUT_ID_CHAT_LOG = "chatLog"
private const val LAYOUT_ID_CONTROL_BAR = "controlBar"
private const val LAYOUT_ID_CHAT_BAR = "chatBar"
private const val LAYOUT_ID_CAMERA = "camera"
private const val LAYOUT_ID_SCREENSHARE = "screenshare"
private const val FEEDBACK_TONE_VOLUME = 100
private const val WAKE_DETECTED_TONE_DURATION_MILLIS = 260
private const val WAKE_READY_TONE_DURATION_MILLIS = 320
private const val STANDBY_TONE_DURATION_MILLIS = 420
private const val STANDBY_ACK_TIMEOUT_MILLIS = 15_000L
private const val WAKE_REPLY_ROUTE_SETTLE_MILLIS = 900L

private val WAKE_DETECTED_VIBRATION = longArrayOf(0, 90)
private val WAKE_READY_VIBRATION = longArrayOf(0, 70, 70, 110)
private val STANDBY_VIBRATION = longArrayOf(0, 220)

private fun playFeedbackTone(
    toneGenerator: ToneGenerator,
    tone: Int,
    durationMillis: Int,
) {
    toneGenerator.stopTone()
    toneGenerator.startTone(tone, durationMillis)
}

private fun vibrateFeedback(context: Context, pattern: LongArray) {
    val vibrator = context.getSystemService(Vibrator::class.java) ?: return
    if (!vibrator.hasVibrator()) return

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
        vibrator.vibrate(VibrationEffect.createWaveform(pattern, -1))
    } else {
        @Suppress("DEPRECATION")
        vibrator.vibrate(pattern, -1)
    }
}

private suspend fun waitForAudioModeNormal(
    context: Context,
    timeoutMillis: Long = 3_000L,
): Boolean {
    val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    val deadline = System.currentTimeMillis() + timeoutMillis
    while (System.currentTimeMillis() < deadline) {
        if (audioManager.mode == AudioManager.MODE_NORMAL) return true
        delay(100)
    }
    return audioManager.mode == AudioManager.MODE_NORMAL
}

/** Plays ~1s of silence on the voice-communication route to settle MIUI's
 *  route-switch mute before the assistant's reply starts. */
private fun warmVoiceRoute(context: Context) {
    val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
        .build()
    val format = AudioFormat.Builder()
        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
        .setSampleRate(48_000)
        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
        .build()
    val track = AudioTrack.Builder()
        .setAudioAttributes(attributes)
        .setAudioFormat(format)
        .setBufferSizeInBytes(48_000 * 2)
        .setTransferMode(AudioTrack.MODE_STREAM)
        .build()
    track.play()
    val silence = ByteArray(4_800 * 2) // 100 ms
    repeat(10) { track.write(silence, 0, silence.size) }
    runCatching { track.stop() }
    track.release()
}

private fun getConstraints(chatVisible: Boolean, cameraVisible: Boolean, screenShareVisible: Boolean) = ConstraintSet {
    val (agentVisualizer, chatLog, controlBar, chatBar, camera, screenShare) = createRefsFor(
        LAYOUT_ID_AGENT,
        LAYOUT_ID_CHAT_LOG,
        LAYOUT_ID_CONTROL_BAR,
        LAYOUT_ID_CHAT_BAR,
        LAYOUT_ID_CAMERA,
        LAYOUT_ID_SCREENSHARE,
    )
    val chatTopGuideline = createGuidelineFromTop(0.2f)

    constrain(chatLog) {
        top.linkTo(chatTopGuideline)
        bottom.linkTo(chatBar.top)
        start.linkTo(parent.start)
        end.linkTo(parent.end)
        width = Dimension.fillToConstraints
        height = Dimension.fillToConstraints
    }

    constrain(chatBar) {
        bottom.linkTo(controlBar.top, 16.dp)
        start.linkTo(parent.start, 16.dp)
        end.linkTo(parent.end, 16.dp)
        width = Dimension.fillToConstraints
        height = Dimension.wrapContent
    }

    constrain(controlBar) {
        bottom.linkTo(parent.bottom, 10.dp)
        start.linkTo(parent.start, 16.dp)
        end.linkTo(parent.end, 16.dp)

        width = Dimension.fillToConstraints
        height = Dimension.value(60.dp)
    }

    if (chatVisible) {
        val chain = createHorizontalChain(agentVisualizer, screenShare, camera, chainStyle = ChainStyle.Spread)

        constrain(chain) {
            start.linkTo(parent.start)
            end.linkTo(parent.end)
        }

        fun ConstrainScope.itemConstraints(visible: Boolean = true) {
            top.linkTo(parent.top)
            bottom.linkTo(chatTopGuideline)
            width = Dimension.percent(0.3f)
            height = Dimension.fillToConstraints
            visibility = if (visible) Visibility.Visible else Visibility.Gone
        }
        constrain(agentVisualizer) {
            itemConstraints()
        }
        constrain(camera) {
            itemConstraints(cameraVisible)
        }
        constrain(screenShare) {
            itemConstraints(screenShareVisible)
        }
    } else {
        constrain(agentVisualizer) {
            top.linkTo(parent.top)
            bottom.linkTo(parent.bottom)
            start.linkTo(parent.start)
            end.linkTo(parent.end)
            height = Dimension.fillToConstraints
            width = Dimension.fillToConstraints
        }
        constrain(camera) {
            end.linkTo(parent.end, 16.dp)
            bottom.linkTo(controlBar.top, 16.dp)
            width = Dimension.percent(0.25f)
            height = Dimension.percent(0.2f)
            visibility = if (cameraVisible) Visibility.Visible else Visibility.Gone
        }
        constrain(screenShare) {
            if (cameraVisible) {
                end.linkTo(camera.start, 16.dp)
            } else {
                end.linkTo(parent.end, 16.dp)
            }
            bottom.linkTo(controlBar.top, 16.dp)
            width = Dimension.percent(0.25f)
            height = Dimension.percent(0.2f)
            visibility = if (screenShareVisible) Visibility.Visible else Visibility.Gone
        }
    }
}
