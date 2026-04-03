package com.example.androidcast.network

import java.nio.ByteBuffer
import java.nio.ByteOrder

data class PacketHeader(
    val version: Int = 1,
    val flags: Int,
    val streamId: Int,
    val frameId: Int,
    val packetIndex: Int,
    val packetCount: Int,
    val payloadSize: Int,
    val ptsUs: Long,
) {
    fun toByteArray(): ByteArray =
        ByteBuffer.allocate(HEADER_SIZE)
            .order(ByteOrder.BIG_ENDIAN)
            .putShort(MAGIC.toShort())
            .put(version.toByte())
            .put(flags.toByte())
            .putInt(streamId)
            .putInt(frameId)
            .putShort(packetIndex.toShort())
            .putShort(packetCount.toShort())
            .putInt(payloadSize)
            .putLong(ptsUs)
            .putInt(0)
            .array()

    fun writeTo(target: ByteArray, offset: Int = 0) {
        require(target.size - offset >= HEADER_SIZE) { "目标缓冲区空间不足，无法写入包头。" }
        ByteBuffer.wrap(target, offset, HEADER_SIZE)
            .order(ByteOrder.BIG_ENDIAN)
            .putShort(MAGIC.toShort())
            .put(version.toByte())
            .put(flags.toByte())
            .putInt(streamId)
            .putInt(frameId)
            .putShort(packetIndex.toShort())
            .putShort(packetCount.toShort())
            .putInt(payloadSize)
            .putLong(ptsUs)
            .putInt(0)
    }

    companion object {
        const val MAGIC = 0x5343
        const val HEADER_SIZE = 32

        const val FLAG_KEYFRAME = 1 shl 0
        const val FLAG_CODEC_CONFIG = 1 shl 1
    }
}
