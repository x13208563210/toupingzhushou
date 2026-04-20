package com.example.androidcast.model

import org.json.JSONArray
import org.json.JSONObject

object ControlMessage {
    private const val MAX_STREAM_FPS = 60
    const val TYPE_HELLO = "HELLO"
    const val TYPE_SELECT_PROFILE = "SELECT_PROFILE"
    const val TYPE_REQUEST_IDR = "REQUEST_IDR"
    const val TYPE_TIME_SYNC_REQUEST = "TIME_SYNC_REQUEST"
    const val TYPE_TIME_SYNC_RESPONSE = "TIME_SYNC_RESPONSE"
    const val TYPE_STOP = "STOP"

    fun buildHello(
        deviceName: String,
        profiles: List<StreamProfile>,
        audioEnabled: Boolean,
        audioSampleRate: Int,
        audioChannels: Int,
    ): JSONObject {
        val profilesArray = JSONArray()
        for (profile in profiles) {
            val profileObj = JSONObject()
            profileObj.put("codec", profile.codecWireName)
            profileObj.put("width", profile.width)
            profileObj.put("height", profile.height)
            profileObj.put("fps", profile.fps.coerceAtMost(MAX_STREAM_FPS))
            profileObj.put("adaptiveFps", profile.adaptiveFps)
            profileObj.put("bitrate", profile.bitrate)
            profilesArray.put(profileObj)
        }

        val codecsArray = JSONArray()
        for (profile in profiles) {
            codecsArray.put(profile.codecWireName)
        }

        val json = JSONObject()
        json.put("type", TYPE_HELLO)
        json.put("deviceName", deviceName)
        json.put("codecs", codecsArray)
        json.put("audio", buildAudioJson(audioEnabled, audioSampleRate, audioChannels))
        json.put("profiles", profilesArray)
        return json
    }

    private fun buildAudioJson(enabled: Boolean, sampleRate: Int, channels: Int): JSONObject {
        val json = JSONObject()
        json.put("enabled", enabled)
        json.put("sampleRate", sampleRate)
        json.put("channels", channels)
        json.put("format", "pcm_s16le")
        return json
    }

    fun parseSelectedProfile(json: JSONObject): StreamProfile? {
        if (json.optString("type") != TYPE_SELECT_PROFILE) {
            return null
        }
        val codec = StreamCodec.fromWireName(json.optString("codec")) ?: return null
        val width = json.optInt("width")
        val height = json.optInt("height")
        val requestedFps = json.optInt("fps")
        val adaptiveFps = json.optBoolean("adaptiveFps", false)
        val bitrate = json.optInt("bitrate")
        val videoPort = json.optInt("videoPort")
        val audioEnabled = json.optBoolean("audioEnabled", false)
        val audioPort = json.optInt("audioPort", 0)
        val audioSampleRate = json.optInt("audioSampleRate", 0)
        val audioChannels = json.optInt("audioChannels", 0)
        if (width <= 0 || height <= 0 || requestedFps <= 0 || bitrate <= 0 || videoPort <= 0) {
            return null
        }
        val fps = requestedFps.coerceAtMost(MAX_STREAM_FPS)
        return StreamProfile(
            codec = codec,
            width = width,
            height = height,
            fps = fps,
            bitrate = bitrate,
            videoPort = videoPort,
            adaptiveFps = adaptiveFps,
            audioEnabled = audioEnabled && audioPort > 0 && audioSampleRate > 0 && audioChannels > 0,
            audioPort = audioPort,
            audioSampleRate = audioSampleRate,
            audioChannels = audioChannels,
        )
    }

    fun buildTimeSyncResponse(
        syncId: Int,
        receiverSendUs: Long,
        senderReceiveUs: Long,
        senderSendUs: Long,
    ): JSONObject =
        JSONObject()
            .put("type", TYPE_TIME_SYNC_RESPONSE)
            .put("syncId", syncId)
            .put("receiverSendUs", receiverSendUs)
            .put("senderReceiveUs", senderReceiveUs)
            .put("senderSendUs", senderSendUs)
}
