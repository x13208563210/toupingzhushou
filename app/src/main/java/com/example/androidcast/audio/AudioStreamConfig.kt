package com.example.androidcast.audio

object AudioStreamConfig {
    const val SAMPLE_RATE = 48_000
    const val CHANNEL_COUNT = 2
    const val BYTES_PER_SAMPLE = 2
    const val FRAME_DURATION_MS = 10
    const val STREAM_ID = 2
    const val FORMAT_WIRE_NAME = "pcm_s16le"
    const val FRAME_BYTES =
        SAMPLE_RATE * CHANNEL_COUNT * BYTES_PER_SAMPLE * FRAME_DURATION_MS / 1_000
}
