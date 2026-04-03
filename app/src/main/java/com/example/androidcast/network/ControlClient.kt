package com.example.androidcast.network

import android.util.Log
import android.os.SystemClock
import com.example.androidcast.model.ControlMessage
import com.example.androidcast.model.StreamProfile
import org.json.JSONObject
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

class ControlClient(
    private val receiverHost: String,
    private val controlPort: Int,
    private val deviceName: String,
    private val supportedProfiles: List<StreamProfile>,
) {
    private var socket: Socket? = null
    private var reader: BufferedReader? = null
    private var writer: BufferedWriter? = null
    private var listenerThread: Thread? = null
    private val running = AtomicBoolean(false)

    fun connectAndSelectProfile(
        onRequestIdr: () -> Unit,
    ): StreamProfile {
        require(supportedProfiles.isNotEmpty()) { "At least one stream profile is required." }

        val currentSocket = Socket(receiverHost, controlPort).apply {
            tcpNoDelay = true
            keepAlive = true
        }
        val currentReader = BufferedReader(InputStreamReader(currentSocket.getInputStream()))
        val currentWriter = BufferedWriter(OutputStreamWriter(currentSocket.getOutputStream()))
        socket = currentSocket
        reader = currentReader
        writer = currentWriter

        writeJson(ControlMessage.buildHello(deviceName, supportedProfiles))
        val response = currentReader.readLine() ?: error("Receiver closed control channel during setup.")
        val selected = ControlMessage.parseSelectedProfile(JSONObject(response))
            ?: error("Receiver did not send SELECT_PROFILE.")

        val matched = supportedProfiles.firstOrNull { it.isSameCoreFormat(selected) }
            ?: error("Receiver selected an unsupported stream profile: $selected")

        startListener(onRequestIdr)
        return matched.copy(videoPort = selected.videoPort, bitrate = selected.bitrate)
    }

    fun sendStop(reason: String) {
        if (!running.get() && socket == null) {
            return
        }
        runCatching {
            writeJson(
                JSONObject()
                    .put("type", ControlMessage.TYPE_STOP)
                    .put("reason", reason),
            )
        }
    }

    fun close() {
        running.set(false)
        runCatching { socket?.close() }
        runCatching { listenerThread?.join(250) }
        socket = null
        reader = null
        writer = null
    }

    private fun startListener(onRequestIdr: () -> Unit) {
        running.set(true)
        listenerThread = Thread {
            while (running.get()) {
                val line = runCatching { reader?.readLine() }.getOrNull() ?: break
                val json = runCatching { JSONObject(line) }.getOrNull() ?: continue
                when (json.optString("type")) {
                    ControlMessage.TYPE_REQUEST_IDR -> onRequestIdr()
                    ControlMessage.TYPE_TIME_SYNC_REQUEST -> handleTimeSyncRequest(json)
                }
            }
            running.set(false)
        }.apply {
            name = "control-listener"
            start()
        }
    }

    @Synchronized
    private fun writeJson(jsonObject: JSONObject) {
        val currentWriter = writer ?: error("Control writer is not ready.")
        currentWriter.write(jsonObject.toString())
        currentWriter.newLine()
        currentWriter.flush()
        Log.d(TAG, "Sent control message: $jsonObject")
    }

    private fun handleTimeSyncRequest(json: JSONObject) {
        val syncId = json.optInt("syncId", -1)
        val receiverSendUs = json.optLong("receiverSendUs", -1L)
        if (syncId < 0 || receiverSendUs < 0L) {
            return
        }

        val senderReceiveUs = SystemClock.elapsedRealtimeNanos() / 1_000L
        val response = ControlMessage.buildTimeSyncResponse(
            syncId = syncId,
            receiverSendUs = receiverSendUs,
            senderReceiveUs = senderReceiveUs,
            senderSendUs = SystemClock.elapsedRealtimeNanos() / 1_000L,
        )
        runCatching { writeJson(response) }
    }

    companion object {
        private const val TAG = "ControlClient"
    }
}
