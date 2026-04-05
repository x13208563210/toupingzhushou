package com.example.androidcast.projection

import android.content.Context
import android.content.Intent

object ProjectionStatusBridge {
    const val ACTION_STATUS_CHANGED = "com.example.androidcast.action.PROJECTION_STATUS_CHANGED"

    private const val EXTRA_STATUS_TEXT = "extra_status_text"
    private const val PREF_NAME = "projection_status"
    private const val KEY_STATUS_TEXT = "status_text"

    fun idle(versionName: String): String = "\u7A7A\u95F2\n\u7248\u672C\uFF1A$versionName"

    fun requestingPermission(): String = "\u6B63\u5728\u8BF7\u6C42\u5F55\u5C4F\u6743\u9650\u2026"

    fun permissionDenied(): String = "\u4F60\u53D6\u6D88\u4E86\u5F55\u5C4F\u6388\u6743\u3002"

    fun invalidTarget(): String = "\u8BF7\u5148\u586B\u5199\u6B63\u786E\u7684\u7535\u8111 IP \u548C\u63A7\u5236\u7AEF\u53E3\u3002"

    fun invalidStreamSettings(): String =
        "\u8BF7\u5148\u9009\u62E9\u6709\u6548\u7684\u5206\u8FA8\u7387\u3001\u5E27\u7387\u548C\u7801\u7387\u3002"

    fun starting(): String = "\u6B63\u5728\u542F\u52A8\u6295\u5C4F\u2026"

    fun connecting(receiverHost: String, controlPort: Int): String =
        "\u6B63\u5728\u8FDE\u63A5\u7535\u8111\u63A5\u6536\u7AEF\u2026\n\u76EE\u6807\uFF1A$receiverHost:$controlPort"

    fun preparingStream(): String =
        "\u7535\u8111\u7AEF\u5DF2\u8FDE\u4E0A\uFF0C\u6B63\u5728\u914D\u7F6E\u7F16\u7801\u4E0E\u6295\u5C4F\u2026"

    fun streaming(receiverHost: String, videoPort: Int? = null): String {
        val target = if (videoPort != null && videoPort > 0) "$receiverHost:$videoPort" else receiverHost
        return "\u6B63\u5728\u6295\u5C4F\u5230\u7535\u8111\n\u89C6\u9891\u76EE\u6807\uFF1A$target"
    }

    fun stopping(): String = "\u6B63\u5728\u505C\u6B62\u6295\u5C4F\u2026"

    fun stopped(reason: String? = null): String {
        val normalized = reason?.trim().orEmpty()
        return if (normalized.isEmpty()) {
            "\u6295\u5C4F\u5DF2\u505C\u6B62"
        } else {
            "\u6295\u5C4F\u5DF2\u505C\u6B62\n\u539F\u56E0\uFF1A$normalized"
        }
    }

    fun error(message: String? = null): String {
        val normalized = message?.trim().orEmpty()
        return if (normalized.isEmpty()) {
            "\u542F\u52A8\u6295\u5C4F\u5931\u8D25"
        } else {
            "\u542F\u52A8\u6295\u5C4F\u5931\u8D25\n$normalized"
        }
    }

    fun publish(context: Context, statusText: String) {
        context
            .getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_STATUS_TEXT, statusText)
            .apply()

        context.sendBroadcast(
            Intent(ACTION_STATUS_CHANGED)
                .setPackage(context.packageName)
                .putExtra(EXTRA_STATUS_TEXT, statusText),
        )
    }

    fun currentStatus(context: Context): String? =
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE).getString(KEY_STATUS_TEXT, null)

    fun readStatus(intent: Intent?): String? = intent?.getStringExtra(EXTRA_STATUS_TEXT)
}
