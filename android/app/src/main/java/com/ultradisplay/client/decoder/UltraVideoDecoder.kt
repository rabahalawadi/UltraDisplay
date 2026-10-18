package com.ultradisplay.client.decoder

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

class UltraVideoDecoder(
    private val surface: Surface,
    private val width: Int,
    private val height: Int,
    private val mimeType: String = "video/avc" // or "video/hevc"
) {
    private var codec: MediaCodec? = null
    private var isRunning = false
    private val bufferInfo = MediaCodec.BufferInfo()

    companion object {
        private const val TAG = "UltraVideoDecoder"
    }

    fun start(): Boolean {
        try {
            codec = MediaCodec.createDecoderByType(mimeType)
            val format = MediaFormat.createVideoFormat(mimeType, width, height)

            // Configure Low Latency Decoder settings (Android 11+)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }

            codec?.configure(format, surface, null, 0)
            codec?.start()
            isRunning = true
            Log.i(TAG, "Hardware decoder started ($mimeType, ${width}x${height})")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start MediaCodec decoder", e)
            return false
        }
    }

    fun decodeNalUnit(nalData: ByteArray, timestampUs: Long, isKeyframe: Boolean) {
        if (!isRunning || codec == null) return

        try {
            val inputIndex = codec!!.dequeueInputBuffer(10000) // 10ms timeout
            if (inputIndex >= 0) {
                val inputBuffer = codec!!.getInputBuffer(inputIndex)
                inputBuffer?.clear()
                inputBuffer?.put(nalData)

                val flags = if (isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                codec!!.queueInputBuffer(inputIndex, 0, nalData.size, timestampUs, flags)
            }

            // Drain decoded frames immediately to surface
            var outputIndex = codec!!.dequeueOutputBuffer(bufferInfo, 0)
            while (outputIndex >= 0) {
                // Render frame to surface immediately with vsync
                codec!!.releaseOutputBuffer(outputIndex, true)
                outputIndex = codec!!.dequeueOutputBuffer(bufferInfo, 0)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error decoding frame", e)
        }
    }

    fun stop() {
        isRunning = false
        try {
            codec?.stop()
            codec?.release()
            codec = null
            Log.i(TAG, "Hardware decoder stopped.")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping decoder", e)
        }
    }
}
