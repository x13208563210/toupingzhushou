package com.example.androidcast.model

import org.json.JSONArray
import org.json.JSONObject

object ControlMessage {
    const val TYPE_HELLO = "HELLO"
    const val TYPE_SELECT_PROFILE = "SELECT_PROFILE"
    const val TYPE_REQUEST_IDR = "REQUEST_IDR"
    const val TYPE_TIME_SYNC_REQUEST = "TIME_SYNC_REQUEST"
    const val TYPE_TIME_SYNC_RESPONSE = "TIME_SYNC_RESPONSE"
    const val TYPE_STOP = "STOP"

    fun buildHello(deviceName: String, profiles: List<StreamProfile>): JSONObject =
        JSONObject()
            .put("type", TYPE_HELLO)
            .put("deviceName", deviceName)
            .put("codecs", JSONArray(profiles.map { it.codecWireName }))
            .put(
                "profiles",
                JSONArray(profiles.map { profile ->
                    JSONObject()
                        .put("codec", profile.codecWireName)
                        .put("width", profile.width)
                        .put("height", profile.height)
                        .put("fps", profile.fps)
                        .put("adaptiveFps", profile.adaptiveFps)
                        .put("bitrate", profile.bitrate)
                }),
            )

    fun parseSelectedProfile(json: JSONObject): StreamProfile? {
        if (json.optString("type") != TYPE_SELECT_PROFILE) {
            return null
        }
        val codec = StreamCodec.fromWireName(json.optString("codec")) ?: return null
        val width = json.optInt("width")
        val height = json.optInt("height")
        val fps = json.optInt("fps")
        val adaptiveFps = json.optBoolean("adaptiveFps", false)
        val bitrate = json.optInt("bitrate")
        val videoPort = json.optInt("videoPort")
        if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || videoPort <= 0) {
            return null
        }
        return StreamProfile(codec, width, height, fps, bitrate, videoPort, adaptiveFps)
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
