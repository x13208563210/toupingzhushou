package com.example.androidcast.codec

import android.media.MediaCodecInfo
import android.media.MediaCodecList
import com.example.androidcast.model.StreamCodec
import com.example.androidcast.model.StreamProfile

class CapabilityProbe {
    companion object {
        const val BUILD_LABEL = "0.3.16-end-to-end-recovery-2026-04-05"
        private const val FORCE_2K_TEST_MODE = true
    }

    fun chooseProfiles(): List<StreamProfile> =
        candidateProfiles()
            .filter { profile ->
                if (profile.width == 2560 && profile.height == 1440 && profile.fps == 30 && FORCE_2K_TEST_MODE) {
                    true
                } else {
                    supports(profile.codec, profile.width, profile.height, profile.fps.toDouble())
                }
            }
            .distinctBy { "${it.codecWireName}-${it.width}-${it.height}-${it.fps}" }

    private fun candidateProfiles(): List<StreamProfile> =
        listOf(
            createProfile(2560, 1440, 120, 52_000_000),
            createProfile(2560, 1440, 90, 46_000_000),
            createProfile(2560, 1440, 60, 40_000_000),
            createProfile(2560, 1440, 30, 18_000_000),
            createProfile(1920, 1080, 144, 50_000_000),
            createProfile(1920, 1080, 120, 45_000_000),
            createProfile(1920, 1080, 90, 38_000_000),
            createProfile(1920, 1080, 60, 32_000_000),
            createProfile(1920, 1080, 30, 16_000_000),
            createProfile(1280, 720, 60, 12_000_000),
            createProfile(1280, 720, 30, 6_000_000),
        )

    private fun createProfile(
        width: Int,
        height: Int,
        fps: Int,
        bitrate: Int,
    ): StreamProfile =
        StreamProfile(
            codec = StreamCodec.AVC,
            width = width,
            height = height,
            fps = fps,
            bitrate = bitrate,
            videoPort = 0,
        )

    private fun supports(
        codec: StreamCodec,
        width: Int,
        height: Int,
        frameRate: Double,
    ): Boolean {
        val codecInfos = MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos
        return codecInfos.any { info ->
            info.isEncoder &&
                info.supportedTypes.any { it.equals(codec.mimeType, ignoreCase = true) } &&
                hasSurfaceSupport(info, codec.mimeType) &&
                runCatching {
                    info.getCapabilitiesForType(codec.mimeType)
                        .videoCapabilities
                        ?.areSizeAndRateSupported(width, height, frameRate) == true
                }.getOrDefault(false)
        }
    }

    private fun hasSurfaceSupport(info: MediaCodecInfo, mimeType: String): Boolean =
        runCatching {
            val caps = info.getCapabilitiesForType(mimeType)
            caps.colorFormats.contains(MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        }.getOrDefault(false)
}
