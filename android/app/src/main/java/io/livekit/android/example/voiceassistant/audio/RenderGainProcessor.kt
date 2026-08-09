package io.livekit.android.example.voiceassistant.audio

import io.livekit.android.audio.AudioProcessorInterface
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Amplifies remote assistant audio before it reaches the speaker. Playback
 * uses the media stream after the voice-call stream swallowed short replies,
 * and MIUI can play media quietly with the screen off; a fixed headroom-safe
 * boost keeps wake/sleep confirmations audible.
 *
 * The render buffer uses the same FloatS16 scale as capture
 * ([-32768, 32767]); we multiply and clamp to the int16 range.
 */
class RenderGainProcessor(
    private val gain: Float = DEFAULT_GAIN,
) : AudioProcessorInterface {

    override fun isEnabled(): Boolean = true

    override fun getName(): String = "RenderGain"

    override fun initializeAudioProcessing(sampleRate: Int, channels: Int) = Unit

    override fun resetAudioProcessing(newRate: Int) = Unit

    override fun processAudio(numBands: Int, numFrames: Int, buffer: ByteBuffer) {
        val bytes = buffer.remaining()
        if (bytes < 4 || numFrames <= 0) return
        val floatCount = bytes / 4
        if (floatCount < numFrames) return

        buffer.order(ByteOrder.LITTLE_ENDIAN)
        val samples = FloatArray(floatCount)
        buffer.asFloatBuffer().get(samples)

        // Diagnostic: report whether the remote audio actually reaches the
        // renderer (helps distinguish "agent never sent audio" from "MIUI
        // muted the track" when replies are silent).
        var peak = 0f
        for (sample in samples) {
            val abs = kotlin.math.abs(sample)
            if (abs > peak) peak = abs
        }
        if (peak > 100f) { // FloatS16 scale; 100 ≈ -50 dBFS
            if (++loudChunks % 100 == 1L) {
                Log.i(TAG, "render audio peak=%.0f chunks=%d".format(peak, loudChunks))
            }
        } else {
            silentChunks++
            if (silentChunks % 500 == 1L) {
                Log.i(TAG, "render silent peak=%.1f silentChunks=%d".format(peak, silentChunks))
            }
        }

        if (gain == 1f) return
        for (i in samples.indices) {
            samples[i] = (samples[i] * gain).coerceIn(-32768f, 32767f)
        }
        val out = buffer.asFloatBuffer()
        out.position(0)
        out.put(samples)
    }

    companion object {
        private const val DEFAULT_GAIN = 1.8f
        private const val TAG = "RenderGain"
    }

    private var loudChunks = 0L
    private var silentChunks = 0L
}
