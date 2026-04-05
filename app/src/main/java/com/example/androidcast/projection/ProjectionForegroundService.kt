package com.example.androidcast.projection

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.Parcelable
import android.util.Log
import androidx.core.app.NotificationCompat
import com.example.androidcast.R
import com.example.androidcast.diagnostics.SenderDiagnostics
import kotlin.concurrent.thread

class ProjectionForegroundService : Service() {
    private var session: ProjectionSession? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        createNotificationChannel()
        val notification = buildNotification(getString(R.string.notification_running))
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION,
            )
        } else {
            startForeground(
                NOTIFICATION_ID,
                notification,
            )
        }

        when (intent?.action) {
            ACTION_START -> handleStart(intent)
            ACTION_STOP -> stopActiveSession("\u7528\u6237\u505C\u6B62\u6295\u5C4F")
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        if (session != null) {
            stopActiveSession(
                reason = "\u524D\u53F0\u670D\u52A1\u9500\u6BC1",
                stopSelfService = false,
            )
        }
        super.onDestroy()
    }

    private fun handleStart(intent: Intent) {
        if (session != null) {
            stopActiveSession("\u91CD\u65B0\u5F00\u59CB\u6295\u5C4F")
        }
        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
        val resultData = intent.getParcelableExtraCompat(EXTRA_RESULT_DATA, Intent::class.java)
            ?: return
        val receiverHost = intent.getStringExtra(EXTRA_RECEIVER_HOST).orEmpty()
        val controlPort = intent.getIntExtra(EXTRA_CONTROL_PORT, -1)
        if (resultCode == 0 || receiverHost.isBlank() || controlPort <= 0) {
            ProjectionStatusBridge.publish(
                applicationContext,
                ProjectionStatusBridge.invalidTarget(),
            )
            stopSelf()
            return
        }

        ProjectionStatusBridge.publish(
            applicationContext,
            ProjectionStatusBridge.connecting(receiverHost, controlPort),
        )

        thread(name = "projection-session-start") {
            val newSession = ProjectionSession(
                context = applicationContext,
                resultCode = resultCode,
                resultData = resultData,
                receiverHost = receiverHost,
                controlPort = controlPort,
            )
            runCatching {
                newSession.start()
                session = newSession
                ProjectionStatusBridge.publish(
                    applicationContext,
                    ProjectionStatusBridge.streaming(receiverHost, newSession.videoPort),
                )
            }.onFailure {
                Log.e(TAG, "启动投屏会话失败", it)
                SenderDiagnostics.e(TAG, "启动投屏会话失败", it)
                ProjectionStatusBridge.publish(
                    applicationContext,
                    ProjectionStatusBridge.error(it.message),
                )
                runCatching { newSession.stop("\u542F\u52A8\u5931\u8D25", publishStoppedStatus = false) }
                session = null
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
        }
    }

    private fun stopActiveSession(
        reason: String,
        stopSelfService: Boolean = true,
        publishStoppedStatus: Boolean = true,
    ) {
        val currentSession = session
        session = null
        if (currentSession != null) {
            runCatching { currentSession.stop(reason, publishStoppedStatus) }
        } else if (publishStoppedStatus) {
            ProjectionStatusBridge.publish(
                applicationContext,
                ProjectionStatusBridge.stopped(reason),
            )
        }
        stopForeground(STOP_FOREGROUND_REMOVE)
        if (stopSelfService) {
            stopSelf()
        }
    }

    private fun buildNotification(contentText: String): Notification =
        NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(contentText)
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setOngoing(true)
            .build()

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return
        }
        val manager = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            NOTIFICATION_CHANNEL_ID,
            getString(R.string.notification_channel_name),
            NotificationManager.IMPORTANCE_LOW,
        )
        manager.createNotificationChannel(channel)
    }

    private fun <T : Parcelable> Intent.getParcelableExtraCompat(name: String, clazz: Class<T>): T? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getParcelableExtra(name, clazz)
        } else {
            @Suppress("DEPRECATION")
            getParcelableExtra(name) as? T
        }

    companion object {
        private const val TAG = "ProjectionService"
        private const val ACTION_START = "com.example.androidcast.action.START"
        private const val ACTION_STOP = "com.example.androidcast.action.STOP"
        private const val EXTRA_RESULT_CODE = "extra_result_code"
        private const val EXTRA_RESULT_DATA = "extra_result_data"
        private const val EXTRA_RECEIVER_HOST = "extra_receiver_host"
        private const val EXTRA_CONTROL_PORT = "extra_control_port"
        private const val NOTIFICATION_CHANNEL_ID = "projection_stream"
        private const val NOTIFICATION_ID = 2001

        fun createStartIntent(
            context: Context,
            resultCode: Int,
            resultData: Intent,
            receiverHost: String,
            controlPort: Int,
        ): Intent =
            Intent(context, ProjectionForegroundService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_RESULT_CODE, resultCode)
                putExtra(EXTRA_RESULT_DATA, resultData)
                putExtra(EXTRA_RECEIVER_HOST, receiverHost)
                putExtra(EXTRA_CONTROL_PORT, controlPort)
            }

        fun createStopIntent(context: Context): Intent =
            Intent(context, ProjectionForegroundService::class.java).apply {
                action = ACTION_STOP
            }
    }
}
