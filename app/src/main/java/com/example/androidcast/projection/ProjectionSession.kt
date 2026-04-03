package com.example.androidcast.projection

import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.util.Log
import android.view.Surface
import com.example.androidcast.codec.CapabilityProbe
import com.example.androidcast.codec.VideoEncoder
import com.example.androidcast.model.StreamProfile
import com.example.androidcast.network.ControlClient
import com.example.androidcast.network.PacketHeader
import com.example.androidcast.network.UdpPacketizer
import com.example.androidcast.settings.StreamSettingsStore
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
    private val frameCounter = AtomicInteger(1)

    private var mediaProjection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var controlClient: ControlClient? = null
    private var packetizer: UdpPacketizer? = null
    private var encoder: VideoEncoder? = null
    private var projectionCallback: MediaProjection.Callback? = null

    fun start() {
        val availableProfiles = CapabilityProbe().chooseProfiles()
        require(availableProfiles.isNotEmpty()) { "没有找到可用的 AVC 编码配置。" }
        val configuredProfile = StreamSettingsStore(context).resolveSelectedProfile(availableProfiles)
        val supportedProfiles = listOf(configuredProfile)
        Log.i(
            TAG,
            "启动版本=${CapabilityProbe.BUILD_LABEL}，当前上报档位=${
                supportedProfiles.joinToString { "${it.width}x${it.height}@${it.frameRateDebugLabel}/${it.bitrate}" }
            }",
        )

        val selected = createControlClient(supportedProfiles).connectAndSelectProfile {
            encoder?.requestKeyFrame()
        }

        packetizer =
            UdpPacketizer(
                host = receiverHost,
                port = selected.videoPort,
                onRequestKeyFrame = {
                    encoder?.requestKeyFrame()
                },
            )
        val projection = projectionManager.getMediaProjection(resultCode, resultData)
            ?: error("没有拿到屏幕录制权限。")
        mediaProjection = projection
        projectionCallback = object : MediaProjection.Callback() {
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
                if (selected.adaptiveFps) {
                    currentEncoder.getInputSurface().setFrameRate(
                        0f,
                        Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,
                    )
                } else {
                    currentEncoder.getInputSurface().setFrameRate(
                        selected.fps.toFloat(),
                        Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                    )
                }
            }
        }

        virtualDisplay = projection.createVirtualDisplay(
            "安卓投屏发送端",
            selected.width,
            selected.height,
            context.resources.displayMetrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            currentEncoder.getInputSurface(),
            null,
            null,
        )

        Log.i(
            TAG,
            "投屏会话已启动，版本=${CapabilityProbe.BUILD_LABEL}，机型=${Build.MODEL}，配置=$selected",
        )
    }

    fun stop(reason: String) {
        Log.i(TAG, "正在停止投屏会话，原因=$reason")
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
        VideoEncoder(selected, object : VideoEncoder.Listener {
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
        })

    companion object {
        private const val TAG = "ProjectionSession"
    }
}
