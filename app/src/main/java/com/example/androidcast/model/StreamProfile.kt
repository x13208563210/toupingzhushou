package com.example.androidcast.model

import android.media.MediaFormat

enum class StreamCodec(val mimeType: String, val wireName: String) {
    AVC(MediaFormat.MIMETYPE_VIDEO_AVC, "avc"),
    HEVC(MediaFormat.MIMETYPE_VIDEO_HEVC, "hevc"),
    ;

    companion object {
        fun fromWireName(value: String): StreamCodec? =
            entries.firstOrNull { it.wireName.equals(value, ignoreCase = true) }
    }
}

data class StreamProfile(
    val codec: StreamCodec,
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val videoPort: Int,
    val adaptiveFps: Boolean = false,
    val audioEnabled: Boolean = false,
    val audioPort: Int = 0,
    val audioSampleRate: Int = 0,
    val audioChannels: Int = 0,
) {
    val codecWireName: String
        get() = codec.wireName

    val frameRateModeWireName: String
        get() = if (adaptiveFps) "adaptive" else "fixed"

    val frameRateLabel: String
        get() = if (adaptiveFps) "\u81ea\u9002\u5e94\u5237\u65b0\u7387" else "${fps} fps"

    val frameRateDebugLabel: String
        get() =
            if (adaptiveFps) {
                "\u81ea\u9002\u5e94\u5237\u65b0\u7387\uff08\u4e0a\u9650 ${fps} fps\uff09"
            } else {
                "${fps} fps"
            }

    fun isSameCoreFormat(other: StreamProfile): Boolean =
        codec == other.codec &&
            width == other.width &&
            height == other.height &&
            fps == other.fps &&
            adaptiveFps == other.adaptiveFps
}
