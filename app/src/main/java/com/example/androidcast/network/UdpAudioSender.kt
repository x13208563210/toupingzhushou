package com.example.androidcast.network

import android.os.Process
import com.example.androidcast.audio.AudioStreamConfig
import com.example.androidcast.diagnostics.SenderDiagnostics
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

class UdpAudioSender(
    host: String,
    port: Int,
    private val streamId: Int = AudioStreamConfig.STREAM_ID,
) {
    private data class PendingAudioFrame(
        val frameId: Int,
        val ptsUs: Long,
        val payload: ByteArray,
    )

    private val address = InetAddress.getByName(host)
    private val socket = DatagramSocket().apply {
        connect(address, port)
        sendBufferSize = AUDIO_UDP_SEND_BUFFER_BYTES
        runCatching { trafficClass = 0x10 }
    }
    private val running = AtomicBoolean(true)
    private val queueLock = Object()
    private val pendingFrames = ArrayDeque<PendingAudioFrame>()
    private var droppedFrames = 0L
    private var sentFrames = 0L
    private var sentPackets = 0L
    private var sentBytes = 0L
    private val senderThread =
        Thread {
            runCatching {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            }
            sendLoop()
        }.apply {
            name = "udp-audio-sender"
            start()
        }

    fun sendPcmFrame(
        frameId: Int,
        ptsUs: Long,
        payload: ByteArray,
    ) {
        if (!running.get() || payload.isEmpty()) {
            return
        }

        synchronized(queueLock) {
            if (!running.get()) {
                return
            }

            if (pendingFrames.size >= MAX_PENDING_AUDIO_FRAMES) {
                pendingFrames.removeFirstOrNull()
                droppedFrames += 1
                if (droppedFrames <= 5 || (droppedFrames % 20L) == 0L) {
                    SenderDiagnostics.w(
                        TAG,
                        "闊抽闃熷垪绉帇锛屽凡涓㈠純鏃ц抚: dropped=$droppedFrames, pending=${pendingFrames.size}",
                    )
                }
            }

            pendingFrames.addLast(
                PendingAudioFrame(
                    frameId = frameId,
                    ptsUs = ptsUs,
                    payload = payload,
                ),
            )
            queueLock.notifyAll()
        }
    }

    fun close() {
        if (!running.getAndSet(false)) {
            return
        }
        synchronized(queueLock) {
            pendingFrames.clear()
            queueLock.notifyAll()
        }
        runCatching { senderThread.join(500) }
        socket.close()
        SenderDiagnostics.i(
            TAG,
            "安卓声音 UDP 已关闭: frames=$sentFrames, packets=$sentPackets, payloadBytes=$sentBytes, dropped=$droppedFrames",
        )
    }

    private fun sendLoop() {
        val packetBuffer = ByteArray(PacketHeader.HEADER_SIZE + MAX_PAYLOAD_SIZE)
        val datagram = DatagramPacket(packetBuffer, 0, address, socket.port)

        while (running.get()) {
            val frame = waitForFrame() ?: break
            sendFrameNow(frame, packetBuffer, datagram)
        }
    }

    private fun waitForFrame(): PendingAudioFrame? {
        synchronized(queueLock) {
            while (running.get() && pendingFrames.isEmpty()) {
                queueLock.wait()
            }
            if (!running.get() && pendingFrames.isEmpty()) {
                return null
            }
            return pendingFrames.removeFirstOrNull()
        }
    }

    private fun sendFrameNow(
        frame: PendingAudioFrame,
        packetBuffer: ByteArray,
        datagram: DatagramPacket,
    ) {
        val packetCount = (frame.payload.size + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE
        for (index in 0 until packetCount) {
            if (!running.get()) {
                return
            }

            val start = index * MAX_PAYLOAD_SIZE
            val end = min(start + MAX_PAYLOAD_SIZE, frame.payload.size)
            val chunkSize = end - start
            PacketHeader(
                flags = 0,
                streamId = streamId,
                frameId = frame.frameId,
                packetIndex = index,
                packetCount = packetCount,
                payloadSize = chunkSize,
                ptsUs = frame.ptsUs,
            ).writeTo(packetBuffer)
            System.arraycopy(
                frame.payload,
                start,
                packetBuffer,
                PacketHeader.HEADER_SIZE,
                chunkSize,
            )

            datagram.length = PacketHeader.HEADER_SIZE + chunkSize
            socket.send(datagram)
            sentPackets += 1
        }

        sentFrames += 1
        sentBytes += frame.payload.size.toLong()
        if (sentFrames <= 3L || (sentFrames % AUDIO_SEND_STATS_LOG_EVERY_FRAMES) == 0L) {
            val pendingCount =
                synchronized(queueLock) {
                    pendingFrames.size
                }
            SenderDiagnostics.i(
                TAG,
                "安卓声音 UDP 发送中: frames=$sentFrames, packets=$sentPackets, payloadBytes=$sentBytes, queue=$pendingCount",
            )
        }
    }

    companion object {
        private const val AUDIO_UDP_SEND_BUFFER_BYTES = 256 shl 10
        private const val MAX_PAYLOAD_SIZE = 1400
        private const val MAX_PENDING_AUDIO_FRAMES = 4
        private const val AUDIO_SEND_STATS_LOG_EVERY_FRAMES = 100L
        private const val TAG = "UdpAudioSender"
    }
}
