package com.example.androidcast.diagnostics

import android.content.Context
import android.util.Log
import java.io.BufferedWriter
import java.io.File
import java.io.OutputStreamWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object SenderDiagnostics {
    private const val TAG = "SenderDiagnostics"
    private const val LOG_DIR_NAME = "sender-logs"
    private const val MAX_LOG_FILES = 8

    private val lineTimeFormat = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)
    private val fileTimeFormat = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)

    @Volatile
    private var currentLogFile: File? = null
    private var writer: BufferedWriter? = null

    @Synchronized
    fun startSession(context: Context, buildLabel: String): File {
        closeWriterLocked()

        val logDir = File(context.filesDir, LOG_DIR_NAME).apply { mkdirs() }
        trimOldFiles(logDir)

        val file = File(logDir, "sender-${fileTimeFormat.format(Date())}.log")
        writer = BufferedWriter(OutputStreamWriter(file.outputStream(), Charsets.UTF_8))
        currentLogFile = file
        writeLineLocked("I", TAG, "发送端诊断日志已开启，版本=$buildLabel，文件=${file.absolutePath}")
        return file
    }

    @Synchronized
    fun stopSession(reason: String) {
        writeLineLocked("I", TAG, "发送端诊断日志关闭，原因=$reason")
        closeWriterLocked()
    }

    fun currentLogFilePath(): String? = currentLogFile?.absolutePath

    fun d(tag: String, message: String) {
        Log.d(tag, message)
        writeLine("D", tag, message)
    }

    fun i(tag: String, message: String) {
        Log.i(tag, message)
        writeLine("I", tag, message)
    }

    fun w(tag: String, message: String, throwable: Throwable? = null) {
        if (throwable != null) {
            Log.w(tag, message, throwable)
        } else {
            Log.w(tag, message)
        }
        writeLine("W", tag, message, throwable)
    }

    fun e(tag: String, message: String, throwable: Throwable? = null) {
        if (throwable != null) {
            Log.e(tag, message, throwable)
        } else {
            Log.e(tag, message)
        }
        writeLine("E", tag, message, throwable)
    }

    @Synchronized
    private fun writeLine(level: String, tag: String, message: String, throwable: Throwable? = null) {
        writeLineLocked(level, tag, message)
        if (throwable != null) {
            Log.getStackTraceString(throwable)
                .lineSequence()
                .forEach { line -> writeLineLocked(level, tag, line) }
        }
    }

    private fun trimOldFiles(logDir: File) {
        val oldFiles =
            logDir.listFiles()
                ?.sortedByDescending { it.lastModified() }
                ?.drop(MAX_LOG_FILES - 1)
                .orEmpty()
        oldFiles.forEach { file ->
            runCatching { file.delete() }
        }
    }

    @Synchronized
    private fun writeLineLocked(level: String, tag: String, message: String) {
        val currentWriter = writer ?: return
        runCatching {
            currentWriter.write("${lineTimeFormat.format(Date())} $level/$tag: $message")
            currentWriter.newLine()
            currentWriter.flush()
        }.onFailure {
            Log.e(TAG, "写入发送端诊断日志失败", it)
        }
    }

    @Synchronized
    private fun closeWriterLocked() {
        runCatching {
            writer?.flush()
            writer?.close()
        }
        writer = null
    }
}
