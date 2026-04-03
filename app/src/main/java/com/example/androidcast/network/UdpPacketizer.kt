package com.example.androidcast.network

import android.os.Process
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

class UdpPacketizer(
    host: String,
    port: Int,
    private val streamId: Int = 1,
    private val onRequestKeyFrame: (() -> Unit)? = null,
) {
    private data class PendingAccessUnit(
        val frameId: Int,
        val ptsUs: Long,
        val flags: Int,
        val payload: ByteArray,
    )

    private val address = InetAddress.getByName(host)
    private val socket = DatagramSocket().apply {
        connect(address, port)
        sendBufferSize = 4 shl 20
        runCatching { trafficClass = 0x10 }
    }
    private val running = AtomicBoolean(true)
    private val queueLock = Object()
    private val pendingVideoQueue = ArrayDeque<PendingAccessUnit>()
    private var latestCodecConfig: PendingAccessUnit? = null
    private var pendingCodecConfig: PendingAccessUnit? = null
    private var waitingForKeyFrame = false
    private val senderThread =
        Thread {
            runCatching {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
            }
            sendLoop()
        }.apply {
            name = "udp-video-sender"
            start()
        }

    fun sendAccessUnit(
        frameId: Int,
        ptsUs: Long,
        flags: Int,
        payload: ByteArray,
    ) {
        if (payload.isEmpty() || !running.get()) {
            return
        }

        val pendingAccessUnit =
            PendingAccessUnit(
                frameId = frameId,
                ptsUs = ptsUs,
                flags = flags,
                payload = payload,
            )
        var shouldRequestKeyFrame = false
        synchronized(queueLock) {
            if (!running.get()) {
                return
            }

            val isCodecConfig = (flags and PacketHeader.FLAG_CODEC_CONFIG) != 0
            val isKeyFrame = (flags and PacketHeader.FLAG_KEYFRAME) != 0
            if (isCodecConfig) {
                latestCodecConfig = pendingAccessUnit
                pendingCodecConfig = pendingAccessUnit
            } else {
                if (waitingForKeyFrame) {
                    if (!isKeyFrame) {
                        return
                    }
                    pendingVideoQueue.clear()
                    latestCodecConfig?.let { pendingCodecConfig = it }
                    waitingForKeyFrame = false
                }

                pendingVideoQueue.addLast(pendingAccessUnit)
                if (pendingVideoQueue.size > MAX_PENDING_ACCESS_UNITS) {
                    pendingVideoQueue.clear()
                    waitingForKeyFrame = true
                    shouldRequestKeyFrame = true
                }
            }
            queueLock.notifyAll()
        }
        if (shouldRequestKeyFrame) {
            onRequestKeyFrame?.invoke()
        }
    }

    fun close() {
        if (!running.getAndSet(false)) {
            return
        }
        synchronized(queueLock) {
            pendingVideoQueue.clear()
            latestCodecConfig = null
            pendingCodecConfig = null
            waitingForKeyFrame = false
            queueLock.notifyAll()
        }
        runCatching { senderThread.join(500) }
        socket.close()
    }

    private fun sendLoop() {
        val packetBuffer = ByteArray(PacketHeader.HEADER_SIZE + MAX_PAYLOAD_SIZE)
        val datagram = DatagramPacket(packetBuffer, 0, address, socket.port)

        while (running.get()) {
            val accessUnit = waitForAccessUnit() ?: break
            sendAccessUnitNow(accessUnit, packetBuffer, datagram)
        }
    }

    private fun waitForAccessUnit(): PendingAccessUnit? {
        synchronized(queueLock) {
            while (running.get() && pendingCodecConfig == null && pendingVideoQueue.isEmpty()) {
                queueLock.wait()
            }
            if (!running.get() && pendingCodecConfig == null && pendingVideoQueue.isEmpty()) {
                return null
            }

            pendingCodecConfig?.let { codecConfig ->
                pendingCodecConfig = null
                return codecConfig
            }

            return pendingVideoQueue.removeFirstOrNull()
        }
    }

    private fun sendAccessUnitNow(
        accessUnit: PendingAccessUnit,
        packetBuffer: ByteArray,
        datagram: DatagramPacket,
    ) {
        val packetCount = (accessUnit.payload.size + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE
        for (index in 0 until packetCount) {
            if (!running.get()) {
                return
            }

            val start = index * MAX_PAYLOAD_SIZE
            val end = min(start + MAX_PAYLOAD_SIZE, accessUnit.payload.size)
            val chunkSize = end - start
            PacketHeader(
                flags = accessUnit.flags,
                streamId = streamId,
                frameId = accessUnit.frameId,
                packetIndex = index,
                packetCount = packetCount,
                payloadSize = chunkSize,
                ptsUs = accessUnit.ptsUs,
            ).writeTo(packetBuffer)
            System.arraycopy(
                accessUnit.payload,
                start,
                packetBuffer,
                PacketHeader.HEADER_SIZE,
                chunkSize,
            )

            datagram.length = PacketHeader.HEADER_SIZE + chunkSize
            socket.send(datagram)
        }
    }

    companion object {
        const val MAX_PAYLOAD_SIZE = 1400
        private const val MAX_PENDING_ACCESS_UNITS = 24
    }
}
