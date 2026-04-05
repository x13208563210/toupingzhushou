package com.example.androidcast.settings

import android.content.Context
import com.example.androidcast.model.StreamProfile

data class SenderStreamSettings(
    val width: Int,
    val height: Int,
    val fpsUpperBound: Int,
    val bitrateMbps: Int,
    val adaptiveFps: Boolean,
) {
    val bitrateBps: Int
        get() = bitrateMbps * 1_000_000
}

class StreamSettingsStore(context: Context) {
    private val preferences =
        context.getSharedPreferences(PREFERENCE_NAME, Context.MODE_PRIVATE)

    fun load(supportedProfiles: List<StreamProfile>): SenderStreamSettings {
        val defaultSettings = defaultSettings(supportedProfiles)
        val savedWidth = preferences.getInt(KEY_WIDTH, defaultSettings.width)
        val savedHeight = preferences.getInt(KEY_HEIGHT, defaultSettings.height)
        val resolutionProfiles =
            supportedProfiles.filter {
                it.width == savedWidth &&
                    it.height == savedHeight
            }
        val matchedResolutionProfiles =
            if (resolutionProfiles.isNotEmpty()) {
                resolutionProfiles
            } else {
                supportedProfiles.filter {
                    it.width == defaultSettings.width &&
                        it.height == defaultSettings.height
                }
            }
        val matchedProfile =
            matchedResolutionProfiles.maxByOrNull { it.fps }
                ?: supportedProfiles.first()

        val savedBitrateMbps =
            preferences.getInt(
                KEY_BITRATE_MBPS,
                (matchedProfile.bitrate / 1_000_000).coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
            ).coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS)
        val recommendedBitrateMbps = recommendedBitrateMbps(matchedProfile)
        val migrationLevel = preferences.getInt(KEY_QUALITY_MIGRATION_LEVEL, 0)
        val shouldMigrateLegacyQuality =
            migrationLevel < QUALITY_MIGRATION_LEVEL &&
                matchedProfile.width >= 2560 &&
                matchedProfile.height >= 1440 &&
                savedBitrateMbps < recommendedBitrateMbps
        val bitrateMbps =
            if (shouldMigrateLegacyQuality) {
                preferences.edit()
                    .putInt(KEY_BITRATE_MBPS, recommendedBitrateMbps)
                    .putInt(KEY_QUALITY_MIGRATION_LEVEL, QUALITY_MIGRATION_LEVEL)
                    .apply()
                recommendedBitrateMbps
            } else {
                savedBitrateMbps
            }

        return SenderStreamSettings(
            width = matchedProfile.width,
            height = matchedProfile.height,
            fpsUpperBound = matchedProfile.fps,
            bitrateMbps = bitrateMbps,
            adaptiveFps = true,
        )
    }

    fun save(settings: SenderStreamSettings) {
        preferences.edit()
            .putInt(KEY_WIDTH, settings.width)
            .putInt(KEY_HEIGHT, settings.height)
            .putInt(KEY_BITRATE_MBPS, settings.bitrateMbps.coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS))
            .putInt(KEY_QUALITY_MIGRATION_LEVEL, QUALITY_MIGRATION_LEVEL)
            .remove(KEY_FPS)
            .remove(KEY_ADAPTIVE_FPS)
            .apply()
    }

    fun resolveSelectedProfile(supportedProfiles: List<StreamProfile>): StreamProfile {
        val settings = load(supportedProfiles)
        val resolutionProfiles =
            supportedProfiles.filter {
                it.width == settings.width &&
                    it.height == settings.height
            }
        val matchedProfile =
            resolutionProfiles.maxByOrNull { it.fps }
                ?: defaultProfile(supportedProfiles)

        return matchedProfile.copy(
            bitrate = settings.bitrateBps,
            adaptiveFps = true,
        )
    }

    private fun defaultProfile(supportedProfiles: List<StreamProfile>): StreamProfile =
        supportedProfiles
            .maxWithOrNull(
                compareBy<StreamProfile> { it.width * it.height }
                    .thenBy { it.fps },
            )
            ?: supportedProfiles.first()

    private fun defaultSettings(supportedProfiles: List<StreamProfile>): SenderStreamSettings {
        val profile = defaultProfile(supportedProfiles)
        return SenderStreamSettings(
            width = profile.width,
            height = profile.height,
            fpsUpperBound = profile.fps,
            bitrateMbps = (profile.bitrate / 1_000_000).coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS),
            adaptiveFps = true,
        )
    }

    companion object {
        const val MIN_BITRATE_MBPS = 4
        const val MAX_BITRATE_MBPS = 80
        private const val QUALITY_MIGRATION_LEVEL = 1

        private const val PREFERENCE_NAME = "stream_settings"
        private const val KEY_WIDTH = "width"
        private const val KEY_HEIGHT = "height"
        private const val KEY_FPS = "fps"
        private const val KEY_ADAPTIVE_FPS = "adaptive_fps"
        private const val KEY_BITRATE_MBPS = "bitrate_mbps"
        private const val KEY_QUALITY_MIGRATION_LEVEL = "quality_migration_level"
    }

    private fun recommendedBitrateMbps(profile: StreamProfile): Int =
        (profile.bitrate / 1_000_000).coerceIn(MIN_BITRATE_MBPS, MAX_BITRATE_MBPS)
}
