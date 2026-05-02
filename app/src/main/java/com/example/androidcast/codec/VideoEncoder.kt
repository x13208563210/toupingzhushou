package com.example.androidcast.codec

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.Surface
import com.example.androidcast.diagnostics.SenderDiagnostics
import com.example.androidcast.model.StreamProfile
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.abs

class VideoEncoder(
    private val profile: StreamProfile,
    private val listener: Listener,
) {
    companion object {
        private const val DEFAULT_I_FRAME_INTERVAL_SECONDS = 1
        private const val LOW_LATENCY_I_FRAME_INTERVAL_SECONDS = 0.25f
        private const val MAX_STREAM_FPS = 120
        private const val OUTPUT_DEQUEUE_TIMEOUT_US = 1_000L
        private const val TAG = "VideoEncoder"
        private const val STATS_LOG_INTERVAL_FRAMES = 30L
    }

    interface Listener {
        fun onEncodedAccessUnit(
            data: ByteArray,
            ptsUs: Long,
            isKeyFrame: Boolean,
            isCodecConfig: Boolean,
        )
    }

    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var drainThread: Thread? = null
    private val running = AtomicBoolean(false)
    private var latestCodecConfig: ByteArray? = null
    private var encodedFrameCount = 0L
    private var encodedBytesInWindow = 0L
    private var statsWindowStartNs = 0L
    private var ptsEpochOffsetUs: Long? = null
    private var lastCodecPtsUs = Long.MIN_VALUE
    private var lastCodecOutputPtsUs = Long.MIN_VALUE
    private var lastSenderOutputPtsUs = Long.MIN_VALUE
    private var codecPtsDeltaSumUs = 0L
    private var codecPtsDeltaCount = 0L
    private var codecPtsDeltaMinUs = Long.MAX_VALUE
    private var codecPtsDeltaMaxUs = 0L
    private var senderPtsDeltaSumUs = 0L
    private var senderPtsDeltaCount = 0L
    private var senderPtsDeltaMinUs = Long.MAX_VALUE
    private var senderPtsDeltaMaxUs = 0L
    private var repeatedCodecPtsCount = 0L
    private var repeatedSenderPtsCount = 0L
    private var firstFrameLogged = false
    private val targetFps = profile.fps.coerceIn(1, MAX_STREAM_FPS)

    fun start() {
        val mediaCodec = MediaCodec.createEncoderByType(profile.codec.mimeType)
        var appliedIFrameIntervalSeconds = DEFAULT_I_FRAME_INTERVAL_SECONDS.toDouble()
        val format = MediaFormat.createVideoFormat(profile.codec.mimeType, profile.width, profile.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, profile.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, targetFps)
            runCatching { setFloat(MediaFormat.KEY_CAPTURE_RATE, targetFps.toFloat()) }
            runCatching { setFloat(MediaFormat.KEY_MAX_FPS_TO_ENCODER, targetFps.toFloat()) }
            val preciseIFrameApplied =
                runCatching {
                    setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, LOW_LATENCY_I_FRAME_INTERVAL_SECONDS)
                }.isSuccess
            if (preciseIFrameApplied) {
                appliedIFrameIntervalSeconds = LOW_LATENCY_I_FRAME_INTERVAL_SECONDS.toDouble()
            } else {
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, DEFAULT_I_FRAME_INTERVAL_SECONDS)
            }
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            applyLowLatencyHints(this)
        }

        mediaCodec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val surface = mediaCodec.createInputSurface()
        mediaCodec.start()

        codec = mediaCodec
        inputSurface = surface
        running.set(true)
        firstFrameLogged = false
        encodedFrameCount = 0L
        encodedBytesInWindow = 0L
        statsWindowStartNs = System.nanoTime()
        ptsEpochOffsetUs = null
        lastCodecPtsUs = Long.MIN_VALUE
        resetTimingWindow()
        SenderDiagnostics.i(
            TAG,
            "编码器启动: mime=${profile.codec.mimeType}, profile=${profile.width}x${profile.height}@${profile.fps}, bitrate=${profile.bitrate}, adaptive=${profile.adaptiveFps}",
        )
        SenderDiagnostics.d(
            TAG,
            "低延迟关键帧间隔=${appliedIFrameIntervalSeconds}s",
        )
        startDrainLoop(mediaCodec)
    }

    fun getInputSurface(): Surface =
        inputSurface ?: error("Encoder surface requested before start().")

    fun requestKeyFrame() {
        val params = Bundle().apply {
            putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
        }
        runCatching { codec?.setParameters(params) }
        SenderDiagnostics.d(TAG, "已请求关键帧")
    }

    fun stop() {
        running.set(false)
        runCatching { drainThread?.join(500) }
        runCatching { inputSurface?.release() }
        inputSurface = null
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        codec = null
        latestCodecConfig = null
        ptsEpochOffsetUs = null
        lastCodecPtsUs = Long.MIN_VALUE
        resetTimingWindow()
        firstFrameLogged = false
        SenderDiagnostics.i(TAG, "编码器已停止")
    }

    private fun startDrainLoop(mediaCodec: MediaCodec) {
        drainThread = Thread {
            runCatching {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY)
            }
            val bufferInfo = MediaCodec.BufferInfo()
            while (running.get()) {
                when (val index = mediaCodec.dequeueOutputBuffer(bufferInfo, OUTPUT_DEQUEUE_TIMEOUT_US)) {
                    MediaCodec.INFO_TRY_AGAIN_LATER -> Unit
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        SenderDiagnostics.i(TAG, "编码输出格式变化: ${mediaCodec.outputFormat}")
                        emitCodecConfig(mediaCodec.outputFormat)
                    }
                    else -> if (index >= 0) {
                        mediaCodec.getOutputBuffer(index)?.let { buffer ->
                            emitOutputBuffer(buffer, bufferInfo)
                        }
                        mediaCodec.releaseOutputBuffer(index, false)
                    }
                }
            }
        }.apply {
            name = "encoder-drain"
            start()
        }
    }

    private fun emitCodecConfig(format: MediaFormat) {
        val payloads = listOfNotNull(
            format.getByteBuffer("csd-0")?.let(::toAnnexB),
            format.getByteBuffer("csd-1")?.let(::toAnnexB),
        )
        if (payloads.isEmpty()) {
            return
        }
        val totalSize = payloads.sumOf { it.size }
        val merged = ByteArray(totalSize)
        var offset = 0
        for (payload in payloads) {
            System.arraycopy(payload, 0, merged, offset, payload.size)
            offset += payload.size
        }
        latestCodecConfig = merged
        SenderDiagnostics.i(TAG, "编码器参数集已输出: bytes=$totalSize")
        listener.onEncodedAccessUnit(
            data = merged,
            ptsUs = 0L,
            isKeyFrame = true,
            isCodecConfig = true,
        )
    }

    private fun emitOutputBuffer(buffer: ByteBuffer, info: MediaCodec.BufferInfo) {
        if (info.size <= 0) {
            return
        }
        val bytes = ByteArray(info.size)
        val duplicate = buffer.duplicate()
        duplicate.position(info.offset)
        duplicate.limit(info.offset + info.size)
        duplicate.get(bytes)
        val payload =
            if (profile.codec == com.example.androidcast.model.StreamCodec.AVC) {
                avccToAnnexBInPlace(bytes)
            } else {
                bytes
            }
        val isKeyFrame = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
        val isCodecConfig = isCodecConfigFrame(info)
        val senderPtsUs = if (isCodecConfig) 0L else normalizeSenderPtsUs(info.presentationTimeUs)
        if (!isCodecConfig) {
            if (!firstFrameLogged) {
                firstFrameLogged = true
                SenderDiagnostics.i(
                    TAG,
                    "首帧输出: size=${payload.size}, keyFrame=$isKeyFrame, codecPtsUs=${info.presentationTimeUs}, senderPtsUs=$senderPtsUs",
                )
            }
            recordOutputTiming(info.presentationTimeUs, senderPtsUs)
            recordEncodedFrame(payload.size, isKeyFrame)
        }
        listener.onEncodedAccessUnit(
            data = payload,
            ptsUs = senderPtsUs,
            isKeyFrame = isKeyFrame,
            isCodecConfig = isCodecConfig,
        )
    }

    private fun normalizeSenderPtsUs(codecPtsUs: Long): Long {
        val nowUs = SystemClock.elapsedRealtimeNanos() / 1_000L
        if (codecPtsUs <= 0L) {
            lastCodecPtsUs = codecPtsUs
            return nowUs
        }

        var offsetUs = ptsEpochOffsetUs
        val driftUs =
            if (offsetUs == null) {
                Long.MAX_VALUE
            } else {
                abs(codecPtsUs + offsetUs - nowUs)
            }
        if (offsetUs == null || codecPtsUs < lastCodecPtsUs || driftUs > 5_000_000L) {
            offsetUs = nowUs - codecPtsUs
            ptsEpochOffsetUs = offsetUs
        }

        lastCodecPtsUs = codecPtsUs
        val finalOffsetUs = requireNotNull(offsetUs)
        val normalizedPtsUs = codecPtsUs + finalOffsetUs
        return if (normalizedPtsUs > 0L) normalizedPtsUs else nowUs
    }

    private fun resetTimingWindow() {
        lastCodecOutputPtsUs = Long.MIN_VALUE
        lastSenderOutputPtsUs = Long.MIN_VALUE
        codecPtsDeltaSumUs = 0L
        codecPtsDeltaCount = 0L
        codecPtsDeltaMinUs = Long.MAX_VALUE
        codecPtsDeltaMaxUs = 0L
        senderPtsDeltaSumUs = 0L
        senderPtsDeltaCount = 0L
        senderPtsDeltaMinUs = Long.MAX_VALUE
        senderPtsDeltaMaxUs = 0L
        repeatedCodecPtsCount = 0L
        repeatedSenderPtsCount = 0L
    }

    private fun recordOutputTiming(codecPtsUs: Long, senderPtsUs: Long) {
        if (lastCodecOutputPtsUs != Long.MIN_VALUE) {
            val codecDeltaUs = codecPtsUs - lastCodecOutputPtsUs
            if (codecDeltaUs > 0L) {
                codecPtsDeltaSumUs += codecDeltaUs
                codecPtsDeltaCount += 1
                codecPtsDeltaMinUs = minOf(codecPtsDeltaMinUs, codecDeltaUs)
                codecPtsDeltaMaxUs = maxOf(codecPtsDeltaMaxUs, codecDeltaUs)
            } else {
                repeatedCodecPtsCount += 1
            }
        }

        if (lastSenderOutputPtsUs != Long.MIN_VALUE) {
            val senderDeltaUs = senderPtsUs - lastSenderOutputPtsUs
            if (senderDeltaUs > 0L) {
                senderPtsDeltaSumUs += senderDeltaUs
                senderPtsDeltaCount += 1
                senderPtsDeltaMinUs = minOf(senderPtsDeltaMinUs, senderDeltaUs)
                senderPtsDeltaMaxUs = maxOf(senderPtsDeltaMaxUs, senderDeltaUs)
            } else {
                repeatedSenderPtsCount += 1
            }
        }

        lastCodecOutputPtsUs = codecPtsUs
        lastSenderOutputPtsUs = senderPtsUs
    }

    private fun formatDeltaStats(
        sampleCount: Long,
        minUs: Long,
        maxUs: Long,
        sumUs: Long,
        repeatedCount: Long,
    ): String {
        if (sampleCount <= 0L) {
            return "无有效样本，重复/回退=$repeatedCount"
        }

        val avgUs = sumUs.toDouble() / sampleCount.toDouble()
        return "avg=${"%.2f".format(avgUs / 1000.0)}ms, min=${"%.2f".format(minUs / 1000.0)}ms, max=${"%.2f".format(maxUs / 1000.0)}ms, 重复/回退=$repeatedCount"
    }

    private fun toAnnexB(buffer: ByteBuffer): ByteArray {
        val duplicate = buffer.duplicate()
        val bytes = ByteArray(duplicate.remaining())
        duplicate.get(bytes)
        val hasStartCode =
            bytes.size >= 4 &&
                bytes[0] == 0.toByte() &&
                bytes[1] == 0.toByte() &&
                bytes[2] == 0.toByte() &&
                bytes[3] == 1.toByte()
        return if (hasStartCode) bytes else byteArrayOf(0, 0, 0, 1) + bytes
    }

    private fun avccToAnnexBInPlace(data: ByteArray): ByteArray {
        if (data.size < 4) {
            return data
        }
        var offset = 0
        while (offset + 4 <= data.size) {
            val nalSize =
                ((data[offset].toInt() and 0xFF) shl 24) or
                    ((data[offset + 1].toInt() and 0xFF) shl 16) or
                    ((data[offset + 2].toInt() and 0xFF) shl 8) or
                    (data[offset + 3].toInt() and 0xFF)
            offset += 4
            if (nalSize <= 0 || offset + nalSize > data.size) {
                return data
            }
            offset += nalSize
        }
        if (offset != data.size) {
            return data
        }

        offset = 0
        while (offset + 4 <= data.size) {
            val nalSize =
                ((data[offset].toInt() and 0xFF) shl 24) or
                    ((data[offset + 1].toInt() and 0xFF) shl 16) or
                    ((data[offset + 2].toInt() and 0xFF) shl 8) or
                    (data[offset + 3].toInt() and 0xFF)
            data[offset] = 0
            data[offset + 1] = 0
            data[offset + 2] = 0
            data[offset + 3] = 1
            offset += 4 + nalSize
        }
        return data
    }

    private fun isCodecConfigFrame(info: MediaCodec.BufferInfo): Boolean =
        (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0

    private fun recordEncodedFrame(payloadSize: Int, isKeyFrame: Boolean) {
        encodedFrameCount += 1
        encodedBytesInWindow += payloadSize.toLong()
        if (encodedFrameCount % STATS_LOG_INTERVAL_FRAMES != 0L) {
            return
        }

        val nowNs = System.nanoTime()
        val elapsedNs = nowNs - statsWindowStartNs
        if (elapsedNs <= 0L) {
            return
        }

        val fps = STATS_LOG_INTERVAL_FRAMES * 1_000_000_000.0 / elapsedNs.toDouble()
        val mbps = encodedBytesInWindow * 8.0 * 1_000_000_000.0 / elapsedNs.toDouble() / 1_000_000.0
        val codecPtsStats =
            formatDeltaStats(
                sampleCount = codecPtsDeltaCount,
                minUs = codecPtsDeltaMinUs,
                maxUs = codecPtsDeltaMaxUs,
                sumUs = codecPtsDeltaSumUs,
                repeatedCount = repeatedCodecPtsCount,
            )
        val senderPtsStats =
            formatDeltaStats(
                sampleCount = senderPtsDeltaCount,
                minUs = senderPtsDeltaMinUs,
                maxUs = senderPtsDeltaMaxUs,
                sumUs = senderPtsDeltaSumUs,
                repeatedCount = repeatedSenderPtsCount,
            )
        SenderDiagnostics.i(
            TAG,
            "编码输出统计: fps=${"%.1f".format(fps)}, bitrate=${"%.1f".format(mbps)}Mbps, keyFrame=$isKeyFrame, profile=${profile.width}x${profile.height}@${profile.fps}",
        )
        SenderDiagnostics.i(
            TAG,
            "编码时间戳统计: codecPts=[$codecPtsStats], senderPts=[$senderPtsStats]",
        )
        statsWindowStartNs = nowNs
        encodedBytesInWindow = 0L
        resetTimingWindow()
    }

    private fun applyLowLatencyHints(format: MediaFormat) {
        runCatching { format.setInteger(MediaFormat.KEY_PRIORITY, 0) }
        runCatching { format.setInteger(MediaFormat.KEY_LATENCY, 1) }
        val operatingRate = targetFps.toFloat()
        runCatching { format.setFloat(MediaFormat.KEY_OPERATING_RATE, operatingRate) }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            runCatching { format.setInteger("vendor.qti-ext-enc-low-latency.enable", 1) }
            runCatching { format.setInteger("vendor.qti-ext-enc-low-latency-mode.value", 1) }
        }
    }
}
