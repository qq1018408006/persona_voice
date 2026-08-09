package io.livekit.android.example.voiceassistant

import io.livekit.android.token.TokenSource
import io.livekit.android.token.TokenSourceResponse
import io.livekit.android.token.cached
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject

// TODO: Add your Sandbox ID here
const val sandboxID = ""

// Local LAN token service shared with the SparkBot firmware. It issues
// short-lived JWTs (10 minutes), so the phone must fetch a fresh token on
// every connection; a hardcoded token silently expires and the session then
// fails with "Error connecting to the session."
const val localTokenServerUrl = "http://<你的电脑局域网IP>:8082/livekit/token"
const val localDeviceKey = "test"
const val localIdentity = "sparkbot-android-002"
const val localRoomName = "sparkbot-android-room"

private val localTokenHttpClient = OkHttpClient()

fun createLocalTokenSource(): TokenSource =
    TokenSource.fromCustom { _ ->
        runCatching {
            withContext(Dispatchers.IO) {
                val request = Request.Builder()
                    .url(localTokenServerUrl)
                    .header("X-Device-Key", localDeviceKey)
                    .post(
                        """{"identity":"$localIdentity","room":"$localRoomName"}"""
                            .toRequestBody("application/json".toMediaType()),
                    )
                    .build()
                localTokenHttpClient.newCall(request).execute().use { response ->
                    if (!response.isSuccessful) {
                        error("token server HTTP ${response.code}")
                    }
                    val json = JSONObject(response.body?.string().orEmpty())
                    TokenSourceResponse(
                        serverUrl = json.getString("server_url"),
                        participantToken = json.getString("token"),
                        roomName = localRoomName,
                        participantName = localIdentity,
                    )
                }
            }
        }
    }.cached()
