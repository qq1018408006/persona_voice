package io.livekit.android.example.voiceassistant.session

import android.annotation.SuppressLint
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.State
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import com.vdurmont.semver4j.Semver
import io.livekit.android.compose.flow.DataSendOptions
import io.livekit.android.compose.flow.DataTopic
import io.livekit.android.compose.flow.LegacyDataTopic
import io.livekit.android.compose.flow.rememberDataMessageHandler
import io.livekit.android.compose.types.ReceivedAgentTranscriptionMessage
import io.livekit.android.compose.types.ReceivedChatMessage
import io.livekit.android.compose.types.ReceivedMessage
import io.livekit.android.compose.types.ReceivedUserTranscriptionMessage
import io.livekit.android.room.Room
import io.livekit.android.room.ServerInfo
import io.livekit.android.room.datastream.StreamInfo
import io.livekit.android.room.datastream.StreamTextOptions
import io.livekit.android.room.datastream.incoming.TextStreamReceiver
import io.livekit.android.room.participant.Participant
import io.livekit.android.room.track.DataPublishReliability
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.scan
import kotlinx.coroutines.launch
import kotlinx.serialization.Serializable
import kotlinx.serialization.Transient
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.util.Collections
import java.util.Date
import java.util.UUID

private const val TAG = "SafeSessionMessages"

/**
 * Drop-in replacement for the compose components' [io.livekit.android.compose.state.rememberSessionMessages].
 *
 * The upstream implementation does not catch the StreamException.TerminatedException that the SDK
 * delivers to open text-stream collectors when a room is disconnected. Entering standby with an
 * in-flight agent transcription therefore crashed the app. This version catches those exceptions
 * so disconnect is always clean.
 */
class SafeSessionMessages internal constructor(
    private val messagesState: State<List<ReceivedMessage>>,
    private val isSendingState: State<Boolean>,
    private val sendImpl: suspend (String) -> Result<ReceivedChatMessage>,
) {
    val messages: List<ReceivedMessage>
        get() = messagesState.value

    val isSending: Boolean
        get() = isSendingState.value

    suspend fun send(message: String): Result<ReceivedChatMessage> = sendImpl(message)
}

@Composable
fun rememberSafeSessionMessages(room: Room): SafeSessionMessages {
    val coroutineScope = rememberCoroutineScope()

    val transcriptionMessages = remember { mutableStateOf(listOf<ReceivedMessage>()) }
    val chatMessages = remember { mutableStateOf(listOf<ReceivedChatMessage>()) }
    val receivedTimeMap = remember { mutableStateMapOf<String, Long>() }

    val messages = remember {
        derivedStateOf {
            (transcriptionMessages.value + chatMessages.value)
                .sortedBy { receivedTimeMap[it.id] ?: Long.MAX_VALUE }
        }
    }

    // Transcriptions (user + agent). Collecting the SDK stream must happen on IO and must
    // tolerate the TerminatedException the SDK uses to close streams on disconnect.
    DisposableEffect(room, coroutineScope) {
        val textStreams = Collections.synchronizedList(mutableListOf<SafeTextStreamData>())

        room.registerTextStreamHandler(DataTopic.TRANSCRIPTION.value) { reader, fromIdentity ->
            coroutineScope.launch(Dispatchers.IO) {
                try {
                    reader.flow
                        .scan("") { accumulator, value -> accumulator + value }
                        .drop(1)
                        .collect { nextText ->
                            synchronized(textStreams) {
                                val index = textStreams.indexOfFirst {
                                    it.streamInfo.id == reader.info.id
                                }
                                if (index == -1) {
                                    textStreams.add(
                                        SafeTextStreamData(
                                            text = nextText,
                                            participantIdentity = fromIdentity,
                                            streamInfo = reader.info,
                                        ),
                                    )
                                } else {
                                    textStreams[index] = textStreams[index].copy(text = nextText)
                                }

                                val updated = textStreams.map { stream ->
                                    if (stream.participantIdentity == room.localParticipant.identity) {
                                        ReceivedUserTranscriptionMessage(
                                            id = stream.streamInfo.id,
                                            message = stream.text,
                                            timestamp = stream.streamInfo.timestampMs,
                                            fromParticipant = room.localParticipant,
                                        )
                                    } else {
                                        ReceivedAgentTranscriptionMessage(
                                            id = stream.streamInfo.id,
                                            message = stream.text,
                                            timestamp = stream.streamInfo.timestampMs,
                                            fromParticipant =
                                                room.getParticipantByIdentity(stream.participantIdentity),
                                        )
                                    }
                                }
                                transcriptionMessages.value = updated
                                for (message in updated) {
                                    if (receivedTimeMap[message.id] == null) {
                                        receivedTimeMap[message.id] = SystemClock.elapsedRealtime()
                                    }
                                }
                            }
                        }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    Log.w(TAG, "transcription stream closed by disconnect", e)
                }
            }
        }

        onDispose {
            room.unregisterTextStreamHandler(DataTopic.TRANSCRIPTION.value)
        }
    }

    val dataHandler = rememberDataMessageHandler(room, LegacyDataTopic.CHAT)
    val isSending = remember { mutableStateOf(false) }

    // Chat streams, same disconnect tolerance as transcriptions.
    DisposableEffect(room, coroutineScope) {
        room.registerTextStreamHandler(DataTopic.CHAT.value) { reader, fromIdentity ->
            coroutineScope.launch(Dispatchers.IO) {
                try {
                    reader.flow
                        .scan("") { accumulator, value -> accumulator + value }
                        .drop(1)
                        .collect { text ->
                            val message = ReceivedChatMessage(
                                id = reader.info.id,
                                message = text,
                                timestamp = reader.info.timestampMs,
                                fromParticipant = room.getParticipantByIdentity(fromIdentity),
                            )
                            chatMessages.value =
                                chatMessages.value.filterNot { it.id == message.id } + message
                            if (receivedTimeMap[message.id] == null) {
                                receivedTimeMap[message.id] = SystemClock.elapsedRealtime()
                            }
                        }
                } catch (e: CancellationException) {
                    throw e
                } catch (e: Exception) {
                    Log.w(TAG, "chat stream closed by disconnect", e)
                }
            }
        }

        onDispose {
            room.unregisterTextStreamHandler(DataTopic.CHAT.value)
        }
    }

    // Legacy chat (pre-data-stream protocol) receiving.
    LaunchedEffect(dataHandler) {
        dataHandler.messageFlow.collect { dataMessage ->
            val payloadString = dataMessage.payload.decodeToString()
            try {
                val legacyChatMessage = json.decodeFromString<LegacyChatMessage>(payloadString)
                if (legacyChatMessage.ignoreLegacy == false) {
                    val message = ReceivedChatMessage(
                        id = legacyChatMessage.id ?: UUID.randomUUID().toString(),
                        message = legacyChatMessage.message,
                        timestamp = legacyChatMessage.timestamp,
                        fromParticipant = legacyChatMessage.participant,
                    )
                    chatMessages.value =
                        chatMessages.value.filterNot { it.id == message.id } + message
                    if (receivedTimeMap[message.id] == null) {
                        receivedTimeMap[message.id] = SystemClock.elapsedRealtime()
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "malformed legacy chat message: $payloadString", e)
            }
        }
    }

    val serverSupportsDataStreams = remember(room) {
        {
            val serverInfo = room.serverInfo
            val serverVersion = serverInfo?.version
            serverInfo?.edition == ServerInfo.Edition.CLOUD ||
                (serverVersion != null && serverVersion > Semver("1.8.2"))
        }
    }

    val sendChat: suspend (String) -> Result<ReceivedChatMessage> = { message ->
        isSending.value = true
        try {
            val streamInfo = room.localParticipant.sendText(
                message,
                StreamTextOptions(topic = DataTopic.CHAT.value),
            ).getOrThrow()
            val timestamp = Date().time
            val sentMessage = ReceivedChatMessage(
                id = streamInfo.id,
                message = message,
                timestamp = timestamp,
                fromParticipant = room.localParticipant,
            )
            chatMessages.value =
                chatMessages.value.filterNot { it.id == sentMessage.id } + sentMessage
            if (receivedTimeMap[sentMessage.id] == null) {
                receivedTimeMap[sentMessage.id] = SystemClock.elapsedRealtime()
            }

            // Legacy chat sending; the agent side ignores it when data streams are supported.
            val chatMessage = LegacyChatMessage(
                id = UUID.randomUUID().toString(),
                timestamp = timestamp,
                message = message,
                ignoreLegacy = serverSupportsDataStreams(),
            )
            val encodedMessage = json.encodeToString(chatMessage).toByteArray(Charsets.UTF_8)
            dataHandler.sendMessage(
                payload = encodedMessage,
                options = DataSendOptions(reliability = DataPublishReliability.RELIABLE),
            )

            Result.success(sentMessage)
        } catch (e: Exception) {
            Result.failure(e)
        } finally {
            isSending.value = false
        }
    }

    return remember(room, messages) {
        SafeSessionMessages(
            messagesState = messages,
            isSendingState = isSending,
            sendImpl = sendChat,
        )
    }
}

private data class SafeTextStreamData(
    val text: String,
    val participantIdentity: Participant.Identity,
    val streamInfo: StreamInfo,
)

@SuppressLint("UnsafeOptInUsageError")
@Serializable
private data class LegacyChatMessage(
    val id: String? = null,
    /** Millis since UNIX epoch */
    val timestamp: Long,
    /** The message */
    val message: String,
    @Transient
    val participant: Participant? = null,
    internal val ignoreLegacy: Boolean? = false,
)

private val json = Json {
    ignoreUnknownKeys = true
}
