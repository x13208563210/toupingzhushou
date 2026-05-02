package com.example.androidcast.projection

import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.view.Display
import android.view.Surface
import com.example.androidcast.audio.AudioStreamConfig
import com.example.androidcast.audio.ProjectionAudioStreamer
import com.example.androidcast.codec.CapabilityProbe
import com.example.androidcast.codec.VideoEncoder
import com.example.androidcast.diagnostics.SenderDiagnostics
import com.example.androidcast.model.StreamProfile
import com.example.androidcast.network.ControlClient
import com.example.androidcast.network.PacketHeader
import com.example.androidcast.network.UdpAudioSender
import com.example.androidcast.network.UdpPacketizer
import com.example.androidcast.settings.StreamSettingsStore
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

class ProjectionSession(
    private val context: Context,
    private val resultCode: Int,
    private val resultData: Intent,
    private val receiverHost: String,
    private val controlPort: Int,
) {
    private val projectionManager =
        context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
    private val displayManager =
        context.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val frameCounter = AtomicInteger(1)
    private val monitorRunning = AtomicBoolean(false)

    private var mediaProjection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var controlClient: ControlClient? = null
    private var packetizer: UdpPacketizer? = null
    private var encoder: VideoEncoder? = null
    private var audioStreamer: ProjectionAudioStreamer? = null
    private var projectionCallback: MediaProjection.Callback? = null
    private var displayMonitorThread: Thread? = null
    private var activeVideoPort: Int = 0
    private var activeAudioPort: Int = 0

    val videoPort: Int
        get() = activeVideoPort

    fun start() {
        val logFile = SenderDiagnostics.startSession(context, CapabilityProbe.BUILD_LABEL)
        SenderDiagnostics.i(TAG, "发送端诊断日志文件=${logFile.absolutePath}")

        val availableProfiles = CapabilityProbe().chooseProfiles()
        require(availableProfiles.isNotEmpty()) { "没有找到可用的 AVC 编码配置。" }

        val configuredProfile =
            StreamSettingsStore(context).resolveSelectedProfile(availableProfiles)
        val supportedProfiles = listOf(configuredProfile)
        val audioRequested = configuredProfile.audioEnabled
        SenderDiagnostics.i(
            TAG,
            "启动版本=${CapabilityProbe.BUILD_LABEL}，当前上报配置=${
                supportedProfiles.joinToString { "${it.width}x${it.height}@${it.frameRateDebugLabel}/${it.bitrate}" }
            }，音频开关=${if (audioRequested) "开启" else "关闭"}",
        )
        logPhysicalDisplayState()
        ProjectionStatusBridge.publish(
            context,
            ProjectionStatusBridge.connecting(receiverHost, controlPort),
        )

        val selected =
            createControlClient(supportedProfiles, audioRequested).connectAndSelectProfile {
                encoder?.requestKeyFrame()
            }
        activeVideoPort = selected.videoPort
        activeAudioPort = 0
        ProjectionStatusBridge.publish(context, ProjectionStatusBridge.preparingStream())

        packetizer =
            UdpPacketizer(
                host = receiverHost,
                port = selected.videoPort,
                onRequestKeyFrame = {
                    encoder?.requestKeyFrame()
                },
            )

        val projection =
            projectionManager.getMediaProjection(resultCode, resultData)
                ?: error("没有拿到屏幕录制权限。")
        mediaProjection = projection
        projectionCallback =
            object : MediaProjection.Callback() {
                override fun onStop() {
                    stop("投屏权限已停止")
                }
            }.also { callback ->
                projection.registerCallback(callback, null)
            }

        val currentEncoder = createEncoder(selected)
        encoder = currentEncoder
        currentEncoder.start()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching {
                currentEncoder.getInputSurface().setFrameRate(
                    selected.fps.toFloat(),
                    Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                )
                SenderDiagnostics.i(
                    TAG,
                    "已向投屏输入 Surface 声明采集上限 ${selected.fps} fps，当前模式=${selected.frameRateLabel}",
                )
            }.onFailure { error ->
                SenderDiagnostics.w(
                    TAG,
                    "设置投屏输入 Surface 帧率提示失败: ${error.message}",
                    error,
                )
            }
        }

        virtualDisplay =
            projection.createVirtualDisplay(
                "安卓投屏发送端",
                selected.width,
                selected.height,
                context.resources.displayMetrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                currentEncoder.getInputSurface(),
                null,
                null,
            )

        if (selected.audioEnabled && selected.audioPort > 0) {
            startAudioStreaming(projection, selected)
        } else if (audioRequested) {
            SenderDiagnostics.w(
                TAG,
                "本次协商未启用安卓声音独立投送，画面仍会按原链路继续发送。",
            )
        }

        currentEncoder.requestKeyFrame()
        requestStartupKeyFrames()

        logVirtualDisplayState("创建后")
        startDisplayMonitor()
        ProjectionStatusBridge.publish(
            context,
            ProjectionStatusBridge.streaming(receiverHost, activeVideoPort),
        )
        SenderDiagnostics.i(
            TAG,
            "投屏会话已启动，版本=${CapabilityProbe.BUILD_LABEL}，机型=${Build.MODEL}，配置=$selected，音频=${if (activeAudioPort > 0) "UDP/$activeAudioPort" else "关闭"}",
        )
    }

    fun stop(reason: String, publishStoppedStatus: Boolean = true) {
        SenderDiagnostics.i(TAG, "正在停止投屏会话，原因=$reason")
        monitorRunning.set(false)
        runCatching { displayMonitorThread?.interrupt() }
        runCatching { displayMonitorThread?.join(300) }
        displayMonitorThread = null
        logVirtualDisplayState("停止前")
        runCatching { controlClient?.sendStop(reason) }
        runCatching { audioStreamer?.stop() }
        audioStreamer = null
        runCatching {
            projectionCallback?.let { mediaProjection?.unregisterCallback(it) }
        }
        projectionCallback = null
        runCatching { virtualDisplay?.release() }
        virtualDisplay = null
        runCatching { encoder?.stop() }
        encoder = null
        runCatching { packetizer?.close() }
        packetizer = null
        runCatching { mediaProjection?.stop() }
        mediaProjection = null
        runCatching { controlClient?.close() }
        controlClient = null
        activeVideoPort = 0
        activeAudioPort = 0
        if (publishStoppedStatus) {
            ProjectionStatusBridge.publish(
                context,
                ProjectionStatusBridge.stopped(reason),
            )
        }
        SenderDiagnostics.stopSession(reason)
    }

    private fun createControlClient(
        supportedProfiles: List<StreamProfile>,
        audioRequested: Boolean,
    ): ControlClient =
        ControlClient(
            receiverHost = receiverHost,
            controlPort = controlPort,
            deviceName = "${Build.MANUFACTURER} ${Build.MODEL} [${CapabilityProbe.BUILD_LABEL}]",
            supportedProfiles = supportedProfiles,
            audioEnabled = audioRequested,
            audioSampleRate = if (audioRequested) AudioStreamConfig.SAMPLE_RATE else 0,
            audioChannels = if (audioRequested) AudioStreamConfig.CHANNEL_COUNT else 0,
        ).also {
            controlClient = it
        }

    private fun startAudioStreaming(
        projection: MediaProjection,
        selected: StreamProfile,
    ) {
        runCatching {
            val sender =
                UdpAudioSender(
                    host = receiverHost,
                    port = selected.audioPort,
                )
            ProjectionAudioStreamer(
                mediaProjection = projection,
                audioSender = sender,
            ).also { streamer ->
                streamer.start()
                audioStreamer = streamer
                activeAudioPort = selected.audioPort
            }
        }.onSuccess {
            SenderDiagnostics.i(
                TAG,
                "安卓声音独立投送已启动: UDP/${selected.audioPort}, ${selected.audioSampleRate}Hz/${selected.audioChannels}ch",
            )
        }.onFailure { error ->
            activeAudioPort = 0
            audioStreamer = null
            SenderDiagnostics.w(
                TAG,
                "安卓声音独立投送启动失败，已保持原视频链路继续工作: ${error.message}",
                error,
            )
        }
    }

    private fun createEncoder(selected: StreamProfile): VideoEncoder =
        VideoEncoder(
            selected,
            object : VideoEncoder.Listener {
                override fun onEncodedAccessUnit(
                    data: ByteArray,
                    ptsUs: Long,
                    isKeyFrame: Boolean,
                    isCodecConfig: Boolean,
                ) {
                    val flags =
                        (if (isKeyFrame) PacketHeader.FLAG_KEYFRAME else 0) or
                            (if (isCodecConfig) PacketHeader.FLAG_CODEC_CONFIG else 0)
                    packetizer?.sendAccessUnit(
                        frameId = frameCounter.getAndIncrement(),
                        ptsUs = ptsUs,
                        flags = flags,
                        payload = data,
                    )
                }
            },
        )

    private fun logPhysicalDisplayState() {
        val display = displayManager.getDisplay(Display.DEFAULT_DISPLAY)
        val currentMode = display?.mode
        val supportedModes =
            display?.supportedModes?.joinToString { mode ->
                "${mode.physicalWidth}x${mode.physicalHeight}@${"%.2f".format(mode.refreshRate)}"
            }.orEmpty()
        SenderDiagnostics.i(
            TAG,
            "物理屏幕: displayId=${display?.displayId ?: -1}, refresh=${"%.2f".format(display?.refreshRate ?: 0f)}Hz, mode=${currentMode?.physicalWidth ?: 0}x${currentMode?.physicalHeight ?: 0}@${"%.2f".format(currentMode?.refreshRate ?: 0f)}, supported=[$supportedModes]",
        )
    }

    private fun logVirtualDisplayState(prefix: String) {
        val display = virtualDisplay?.display
        if (display == null) {
            SenderDiagnostics.w(TAG, "$prefix 虚拟显示不可用")
            return
        }
        val mode = display.mode
        val supportedModes =
            display.supportedModes.joinToString { supportedMode ->
                "${supportedMode.physicalWidth}x${supportedMode.physicalHeight}@${"%.2f".format(supportedMode.refreshRate)}"
            }
        SenderDiagnostics.i(
            TAG,
            "$prefix 虚拟显示: displayId=${display.displayId}, refresh=${"%.2f".format(display.refreshRate)}Hz, mode=${mode.physicalWidth}x${mode.physicalHeight}@${"%.2f".format(mode.refreshRate)}, rotation=${display.rotation}, supported=[$supportedModes]",
        )
    }

    private fun startDisplayMonitor() {
        monitorRunning.set(true)
        displayMonitorThread =
            Thread {
                var sampleCount = 0
                while (monitorRunning.get()) {
                    logVirtualDisplayState("运行中#$sampleCount")
                    sampleCount += 1
                    try {
                        Thread.sleep(1_000)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            }.apply {
                name = "projection-display-monitor"
                start()
            }
    }

    private fun requestStartupKeyFrames() {
        Thread {
            val retryDelaysMs = longArrayOf(80L, 180L, 320L)
            for (delayMs in retryDelaysMs) {
                try {
                    Thread.sleep(delayMs)
                } catch (_: InterruptedException) {
                    return@Thread
                }

                if (mediaProjection == null || virtualDisplay == null) {
                    return@Thread
                }

                encoder?.requestKeyFrame()
                SenderDiagnostics.d(TAG, "启动阶段追加请求关键帧: ${delayMs}ms")
            }
        }.apply {
            name = "projection-startup-keyframe"
            start()
        }
    }

    companion object {
        private const val TAG = "ProjectionSession"
    }
}
