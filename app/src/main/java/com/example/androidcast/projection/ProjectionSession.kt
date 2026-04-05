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
import com.example.androidcast.codec.CapabilityProbe
import com.example.androidcast.codec.VideoEncoder
import com.example.androidcast.diagnostics.SenderDiagnostics
import com.example.androidcast.model.StreamProfile
import com.example.androidcast.network.ControlClient
import com.example.androidcast.network.PacketHeader
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
    private var projectionCallback: MediaProjection.Callback? = null
    private var displayMonitorThread: Thread? = null
    private var activeVideoPort: Int = 0

    val videoPort: Int
        get() = activeVideoPort

    fun start() {
        val logFile = SenderDiagnostics.startSession(context, CapabilityProbe.BUILD_LABEL)
        SenderDiagnostics.i(TAG, "\u53D1\u9001\u7AEF\u8BCA\u65AD\u65E5\u5FD7\u6587\u4EF6=${logFile.absolutePath}")

        val availableProfiles = CapabilityProbe().chooseProfiles()
        require(availableProfiles.isNotEmpty()) { "\u6CA1\u6709\u627E\u5230\u53EF\u7528\u7684 AVC \u7F16\u7801\u914D\u7F6E\u3002" }

        val configuredProfile = StreamSettingsStore(context).resolveSelectedProfile(availableProfiles)
        val supportedProfiles = listOf(configuredProfile)
        SenderDiagnostics.i(
            TAG,
            "\u542F\u52A8\u7248\u672C=${CapabilityProbe.BUILD_LABEL}\uFF0C\u5F53\u524D\u4E0A\u62A5\u914D\u7F6E=${
                supportedProfiles.joinToString { "${it.width}x${it.height}@${it.frameRateDebugLabel}/${it.bitrate}" }
            }",
        )
        logPhysicalDisplayState()
        ProjectionStatusBridge.publish(
            context,
            ProjectionStatusBridge.connecting(receiverHost, controlPort),
        )

        val selected = createControlClient(supportedProfiles).connectAndSelectProfile {
            encoder?.requestKeyFrame()
        }
        activeVideoPort = selected.videoPort
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
                ?: error("\u6CA1\u6709\u62FF\u5230\u5C4F\u5E55\u5F55\u5236\u6743\u9650\u3002")
        mediaProjection = projection
        projectionCallback =
            object : MediaProjection.Callback() {
                override fun onStop() {
                    stop("\u6295\u5C4F\u6743\u9650\u5DF2\u505C\u6B62")
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
                    "\u5DF2\u5411\u6295\u5C4F\u8F93\u5165 Surface \u7533\u660E\u91C7\u96C6\u4E0A\u9650 ${selected.fps} fps\uFF0C\u5F53\u524D\u6A21\u5F0F=${selected.frameRateLabel}",
                )
            }.onFailure { error ->
                SenderDiagnostics.w(
                    TAG,
                    "\u8BBE\u7F6E\u6295\u5C4F\u8F93\u5165 Surface \u5E27\u7387\u63D0\u793A\u5931\u8D25: ${error.message}",
                    error,
                )
            }
        }

        virtualDisplay =
            projection.createVirtualDisplay(
                "\u5B89\u5353\u6295\u5C4F\u53D1\u9001\u7AEF",
                selected.width,
                selected.height,
                context.resources.displayMetrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                currentEncoder.getInputSurface(),
                null,
                null,
            )

        currentEncoder.requestKeyFrame()
        requestStartupKeyFrames()

        logVirtualDisplayState("\u521B\u5EFA\u540E")
        startDisplayMonitor()
        ProjectionStatusBridge.publish(
            context,
            ProjectionStatusBridge.streaming(receiverHost, activeVideoPort),
        )
        SenderDiagnostics.i(
            TAG,
            "\u6295\u5C4F\u4F1A\u8BDD\u5DF2\u542F\u52A8\uFF0C\u7248\u672C=${CapabilityProbe.BUILD_LABEL}\uFF0C\u673A\u578B=${Build.MODEL}\uFF0C\u914D\u7F6E=$selected",
        )
    }

    fun stop(reason: String, publishStoppedStatus: Boolean = true) {
        SenderDiagnostics.i(TAG, "\u6B63\u5728\u505C\u6B62\u6295\u5C4F\u4F1A\u8BDD\uFF0C\u539F\u56E0=$reason")
        monitorRunning.set(false)
        runCatching { displayMonitorThread?.interrupt() }
        runCatching { displayMonitorThread?.join(300) }
        displayMonitorThread = null
        logVirtualDisplayState("\u505C\u6B62\u524D")
        runCatching { controlClient?.sendStop(reason) }
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
        if (publishStoppedStatus) {
            ProjectionStatusBridge.publish(
                context,
                ProjectionStatusBridge.stopped(reason),
            )
        }
        SenderDiagnostics.stopSession(reason)
    }

    private fun createControlClient(supportedProfiles: List<StreamProfile>): ControlClient =
        ControlClient(
            receiverHost = receiverHost,
            controlPort = controlPort,
            deviceName = "${Build.MANUFACTURER} ${Build.MODEL} [${CapabilityProbe.BUILD_LABEL}]",
            supportedProfiles = supportedProfiles,
        ).also {
            controlClient = it
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
            "\u7269\u7406\u5C4F\u5E55: displayId=${display?.displayId ?: -1}, refresh=${"%.2f".format(display?.refreshRate ?: 0f)}Hz, mode=${currentMode?.physicalWidth ?: 0}x${currentMode?.physicalHeight ?: 0}@${"%.2f".format(currentMode?.refreshRate ?: 0f)}, supported=[$supportedModes]",
        )
    }

    private fun logVirtualDisplayState(prefix: String) {
        val display = virtualDisplay?.display
        if (display == null) {
            SenderDiagnostics.w(TAG, "$prefix \u865A\u62DF\u663E\u793A\u4E0D\u53EF\u7528")
            return
        }
        val mode = display.mode
        val supportedModes =
            display.supportedModes.joinToString { supportedMode ->
                "${supportedMode.physicalWidth}x${supportedMode.physicalHeight}@${"%.2f".format(supportedMode.refreshRate)}"
            }
        SenderDiagnostics.i(
            TAG,
            "$prefix \u865A\u62DF\u663E\u793A: displayId=${display.displayId}, refresh=${"%.2f".format(display.refreshRate)}Hz, mode=${mode.physicalWidth}x${mode.physicalHeight}@${"%.2f".format(mode.refreshRate)}, rotation=${display.rotation}, supported=[$supportedModes]",
        )
    }

    private fun startDisplayMonitor() {
        monitorRunning.set(true)
        displayMonitorThread =
            Thread {
                var sampleCount = 0
                while (monitorRunning.get()) {
                    logVirtualDisplayState("\u8FD0\u884C\u4E2D#$sampleCount")
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
                SenderDiagnostics.d(TAG, "\u542F\u52A8\u9636\u6BB5\u8FFD\u52A0\u8BF7\u6C42\u5173\u952E\u5E27: ${delayMs}ms")
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
