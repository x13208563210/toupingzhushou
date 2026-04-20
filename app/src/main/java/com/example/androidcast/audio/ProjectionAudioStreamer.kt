package com.example.androidcast.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.os.Process
import android.os.SystemClock
import com.example.androidcast.diagnostics.SenderDiagnostics
import com.example.androidcast.network.UdpAudioSender
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.abs
import kotlin.math.max

class ProjectionAudioStreamer(
    private val mediaProjection: MediaProjection,
    private val audioSender: UdpAudioSender,
) {
    private val running = AtomicBoolean(false)
    private val frameCounter = AtomicInteger(1)

    private var audioRecord: AudioRecord? = null
    private var captureThread: Thread? = null

    fun start() {
        val audioFormat =
            AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(AudioStreamConfig.SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                .build()
        val captureConfig =
            AudioPlaybackCaptureConfiguration.Builder(mediaProjection).apply {
                CAPTURE_USAGES.forEach { usage ->
                    addMatchingUsage(usage)
                }
            }.build()
        val minBufferSize =
            AudioRecord.getMinBufferSize(
                AudioStreamConfig.SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_STEREO,
                AudioFormat.ENCODING_PCM_16BIT,
            )
        val audioRecord =
            AudioRecord.Builder()
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setAudioFormat(audioFormat)
                .setBufferSizeInBytes(max(minBufferSize, AudioStreamConfig.FRAME_BYTES * 3))
                .build()
        require(audioRecord.state == AudioRecord.STATE_INITIALIZED) { "系统声音采集初始化失败" }

        this.audioRecord = audioRecord
        audioRecord.startRecording()
        running.set(true)
        captureThread =
            Thread {
                runCatching {
                    Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
                }
                captureLoop(audioRecord)
            }.apply {
                name = "projection-audio-capture"
                start()
            }
        SenderDiagnostics.i(
            TAG,
            "安卓声音独立投送已启动，${AudioStreamConfig.SAMPLE_RATE}Hz/${AudioStreamConfig.CHANNEL_COUNT}ch/${AudioStreamConfig.FORMAT_WIRE_NAME}，匹配 usage=$CAPTURE_USAGE_LABEL",
        )
    }

    fun stop() {
        running.set(false)
        runCatching { audioRecord?.stop() }
        runCatching { captureThread?.join(500) }
        runCatching { audioRecord?.release() }
        audioRecord = null
        captureThread = null
        audioSender.close()
    }

    private fun captureLoop(record: AudioRecord) {
        val buffer = ByteArray(AudioStreamConfig.FRAME_BYTES)
        var capturedFrames = 0L
        var capturedBytes = 0L
        var silentFrameStreak = 0
        var zeroReadStreak = 0
        while (running.get()) {
            val read =
                record.read(
                    buffer,
                    0,
                    buffer.size,
                    AudioRecord.READ_BLOCKING,
                )
            when {
                read > 0 -> {
                    zeroReadStreak = 0
                    val peakLevel = computePeakPcm16(buffer, read)
                    val looksSilent = peakLevel <= SILENT_PCM_PEAK_THRESHOLD
                    silentFrameStreak = if (looksSilent) silentFrameStreak + 1 else 0

                    audioSender.sendPcmFrame(
                        frameId = frameCounter.getAndIncrement(),
                        ptsUs = SystemClock.elapsedRealtimeNanos() / 1_000L,
                        payload =
                            if (read == buffer.size) {
                                buffer.clone()
                            } else {
                                buffer.copyOf(read)
                            },
                    )

                    capturedFrames += 1
                    capturedBytes += read.toLong()

                    if (capturedFrames <= 3L || (capturedFrames % AUDIO_STATS_LOG_EVERY_FRAMES) == 0L) {
                        SenderDiagnostics.i(
                            TAG,
                            "安卓声音采集中: frames=$capturedFrames, bytes=$capturedBytes, lastRead=$read, peak=$peakLevel, silentStreak=$silentFrameStreak",
                        )
                    } else if (silentFrameStreak == SILENT_FRAME_WARN_THRESHOLD) {
                        SenderDiagnostics.w(
                            TAG,
                            "安卓声音采集连续静音，已累计 $silentFrameStreak 帧，最近峰值=$peakLevel；如果此时手机明明在放声音，说明该 App 的音频没有被当前 playback capture usage 抓到。",
                        )
                    }
                }

                read == 0 -> {
                    zeroReadStreak += 1
                    if (zeroReadStreak == 1 || (zeroReadStreak % ZERO_READ_WARN_INTERVAL) == 0) {
                        SenderDiagnostics.w(
                            TAG,
                            "安卓声音采集暂时没有读到数据: zeroReads=$zeroReadStreak, capturedFrames=$capturedFrames, capturedBytes=$capturedBytes",
                        )
                    }
                }

                else -> {
                    if (running.get()) {
                        SenderDiagnostics.w(TAG, "安卓声音采集读取失败: code=$read")
                    }
                    break
                }
            }
        }
        SenderDiagnostics.i(
            TAG,
            "安卓声音采集线程结束: frames=$capturedFrames, bytes=$capturedBytes, running=${running.get()}",
        )
        running.set(false)
    }

    private fun computePeakPcm16(buffer: ByteArray, size: Int): Int {
        var peak = 0
        var index = 0
        while (index + 1 < size) {
            val sample =
                (((buffer[index + 1].toInt()) shl 8) or (buffer[index].toInt() and 0xFF))
                    .toShort()
                    .toInt()
            val amplitude = abs(sample)
            if (amplitude > peak) {
                peak = amplitude
            }
            index += 2
        }
        return peak
    }

    companion object {
        private const val TAG = "ProjectionAudio"
        private const val AUDIO_STATS_LOG_EVERY_FRAMES = 100L
        private const val ZERO_READ_WARN_INTERVAL = 25
        private const val SILENT_FRAME_WARN_THRESHOLD = 50
        private const val SILENT_PCM_PEAK_THRESHOLD = 8
        private val CAPTURE_USAGES =
            intArrayOf(
                AudioAttributes.USAGE_MEDIA,
                AudioAttributes.USAGE_GAME,
                AudioAttributes.USAGE_UNKNOWN,
                AudioAttributes.USAGE_ALARM,
                AudioAttributes.USAGE_NOTIFICATION,
                AudioAttributes.USAGE_NOTIFICATION_RINGTONE,
                AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE,
                AudioAttributes.USAGE_ASSISTANCE_SONIFICATION,
                AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY,
                AudioAttributes.USAGE_ASSISTANT,
                AudioAttributes.USAGE_VOICE_COMMUNICATION_SIGNALLING,
            )
        private val CAPTURE_USAGE_LABEL = CAPTURE_USAGES.joinToString()
    }
}
