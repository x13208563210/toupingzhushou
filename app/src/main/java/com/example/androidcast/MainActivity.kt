package com.example.androidcast

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.view.MotionEvent
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.widget.doAfterTextChanged
import com.example.androidcast.codec.CapabilityProbe
import com.example.androidcast.model.StreamProfile
import com.example.androidcast.projection.ProjectionForegroundService
import com.example.androidcast.projection.ProjectionStatusBridge
import com.example.androidcast.settings.SenderStreamSettings
import com.example.androidcast.settings.StreamSettingsStore

class MainActivity : AppCompatActivity() {
    companion object {
        private const val ADAPTIVE_FRAME_RATE_LABEL = "\u81ea\u9002\u5e94\u5237\u65b0\u7387"
    }

    private data class ResolutionOption(
        val width: Int,
        val height: Int,
    ) {
        val label: String
            get() = "${width}x${height}"
    }

    private lateinit var projectionManager: MediaProjectionManager
    private lateinit var receiverIpInput: EditText
    private lateinit var controlPortInput: EditText
    private lateinit var resolutionInput: AutoCompleteTextView
    private lateinit var fpsInput: EditText
    private lateinit var bitrateInput: EditText
    private lateinit var statusText: TextView
    private lateinit var versionText: TextView
    private lateinit var streamSettingsSummaryText: TextView
    private lateinit var settingsStore: StreamSettingsStore
    private var statusReceiverRegistered = false

    private var supportedProfiles: List<StreamProfile> = emptyList()
    private var resolutionOptions: List<ResolutionOption> = emptyList()

    private val idleStatusText: String
        get() = ProjectionStatusBridge.idle(BuildConfig.VERSION_NAME)

    private val projectionStatusReceiver =
        object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                ProjectionStatusBridge.readStatus(intent)?.let { status ->
                    statusText.text = status
                }
            }
        }

    private fun updateStatus(status: String, persist: Boolean = true) {
        statusText.text = status
        if (persist) {
            ProjectionStatusBridge.publish(this, status)
        }
    }

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode != Activity.RESULT_OK || result.data == null) {
                updateStatus(ProjectionStatusBridge.permissionDenied())
                return@registerForActivityResult
            }

            val receiverIp = receiverIpInput.text.toString().trim()
            val controlPort = controlPortInput.text.toString().trim().toIntOrNull()
            if (receiverIp.isEmpty() || controlPort == null) {
                updateStatus(ProjectionStatusBridge.invalidTarget())
                return@registerForActivityResult
            }

            if (persistStreamSettingsFromUi(showError = true) == null) {
                updateStatus(ProjectionStatusBridge.invalidStreamSettings())
                return@registerForActivityResult
            }

            val serviceIntent = ProjectionForegroundService.createStartIntent(
                context = this,
                resultCode = result.resultCode,
                resultData = result.data!!,
                receiverHost = receiverIp,
                controlPort = controlPort,
            )
            ContextCompat.startForegroundService(this, serviceIntent)
            updateStatus(ProjectionStatusBridge.starting())
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        projectionManager =
            getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        settingsStore = StreamSettingsStore(this)
        receiverIpInput = findViewById(R.id.receiverIpInput)
        controlPortInput = findViewById(R.id.controlPortInput)
        resolutionInput = findViewById(R.id.resolutionInput)
        fpsInput = findViewById(R.id.fpsInput)
        bitrateInput = findViewById(R.id.bitrateInput)
        statusText = findViewById(R.id.statusText)
        versionText = findViewById(R.id.versionText)
        streamSettingsSummaryText = findViewById(R.id.streamSettingsSummaryText)

        title = getString(R.string.app_title_with_version, BuildConfig.VERSION_NAME)
        versionText.text = getString(R.string.label_version, BuildConfig.VERSION_NAME)
        statusText.text = ProjectionStatusBridge.currentStatus(this) ?: idleStatusText
        supportedProfiles =
            CapabilityProbe()
                .chooseProfiles()
                .sortedWith(
                    compareByDescending<StreamProfile> { it.width * it.height }
                        .thenByDescending { it.fps },
                )
        configureDropdownInput(resolutionInput)
        setupStreamSettingsInputs()

        val startButton = findViewById<Button>(R.id.startButton)
        startButton.isEnabled = supportedProfiles.isNotEmpty()
        if (supportedProfiles.isEmpty()) {
            updateStatus(ProjectionStatusBridge.invalidStreamSettings())
        }

        startButton.setOnClickListener {
            if (persistStreamSettingsFromUi(showError = true) == null) {
                updateStatus(ProjectionStatusBridge.invalidStreamSettings())
                return@setOnClickListener
            }
            updateStatus(ProjectionStatusBridge.requestingPermission())
            projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
        }

        findViewById<Button>(R.id.stopButton).setOnClickListener {
            ContextCompat.startForegroundService(
                this,
                ProjectionForegroundService.createStopIntent(this),
            )
            updateStatus(ProjectionStatusBridge.stopping())
        }
    }

    override fun onStart() {
        super.onStart()
        if (!statusReceiverRegistered) {
            ContextCompat.registerReceiver(
                this,
                projectionStatusReceiver,
                IntentFilter(ProjectionStatusBridge.ACTION_STATUS_CHANGED),
                ContextCompat.RECEIVER_NOT_EXPORTED,
            )
            statusReceiverRegistered = true
        }
        statusText.text = ProjectionStatusBridge.currentStatus(this) ?: idleStatusText
    }

    override fun onStop() {
        if (statusReceiverRegistered) {
            unregisterReceiver(projectionStatusReceiver)
            statusReceiverRegistered = false
        }
        super.onStop()
    }

    override fun onPause() {
        persistStreamSettingsFromUi(showError = false)
        super.onPause()
    }

    private fun setupStreamSettingsInputs() {
        if (supportedProfiles.isEmpty()) {
            streamSettingsSummaryText.text = ProjectionStatusBridge.invalidStreamSettings()
            return
        }

        val savedSettings = settingsStore.load(supportedProfiles)
        resolutionOptions =
            supportedProfiles
                .map { ResolutionOption(it.width, it.height) }
                .distinctBy { "${it.width}x${it.height}" }

        resolutionInput.setAdapter(
            ArrayAdapter(
                this,
                android.R.layout.simple_list_item_1,
                resolutionOptions.map { it.label },
            ),
        )

        val selectedResolution =
            resolutionOptions.firstOrNull {
                it.width == savedSettings.width && it.height == savedSettings.height
            } ?: resolutionOptions.first()
        resolutionInput.setText(selectedResolution.label, false)
        updateFrameRateDisplay()
        bitrateInput.setText(savedSettings.bitrateMbps.toString())
        updateStreamSettingsSummary()

        resolutionInput.setOnItemClickListener { _, _, position, _ ->
            val resolution = resolutionOptions.getOrNull(position) ?: return@setOnItemClickListener
            resolutionInput.setText(resolution.label, false)
            updateFrameRateDisplay()
            updateStreamSettingsSummary()
        }
        bitrateInput.doAfterTextChanged {
            updateStreamSettingsSummary()
        }
    }

    private fun updateFrameRateDisplay() {
        fpsInput.setText(ADAPTIVE_FRAME_RATE_LABEL)
    }

    private fun configureDropdownInput(input: AutoCompleteTextView) {
        input.threshold = 0
        input.keyListener = null
        input.isCursorVisible = false
        input.setOnClickListener {
            input.showDropDown()
        }
        input.setOnFocusChangeListener { _, hasFocus ->
            if (hasFocus) {
                input.post { input.showDropDown() }
            }
        }
        input.setOnTouchListener { _, event ->
            if (event.action == MotionEvent.ACTION_UP) {
                input.post { input.showDropDown() }
            }
            false
        }
    }

    private fun updateStreamSettingsSummary() {
        val settings = readStreamSettingsFromUi()
        if (settings == null) {
            streamSettingsSummaryText.text = ProjectionStatusBridge.invalidStreamSettings()
            return
        }

        streamSettingsSummaryText.text =
            getString(R.string.stream_settings_summary_prefix) +
                "${settings.width}x${settings.height} / ${formatFrameRateSummary()} / ${settings.bitrateMbps} Mbps"
    }

    private fun persistStreamSettingsFromUi(showError: Boolean): SenderStreamSettings? {
        val settings = readStreamSettingsFromUi()
        if (settings == null) {
            if (showError) {
                streamSettingsSummaryText.text = ProjectionStatusBridge.invalidStreamSettings()
            }
            return null
        }
        settingsStore.save(settings)
        return settings
    }

    private fun readStreamSettingsFromUi(): SenderStreamSettings? {
        if (supportedProfiles.isEmpty()) {
            return null
        }

        val resolutionText = resolutionInput.text?.toString()?.trim().orEmpty()
        val bitrateMbps =
            bitrateInput.text?.toString()?.trim()?.toIntOrNull()
                ?.coerceIn(StreamSettingsStore.MIN_BITRATE_MBPS, StreamSettingsStore.MAX_BITRATE_MBPS)
                ?: return null

        val resolution =
            resolutionOptions.firstOrNull { it.label == resolutionText }
                ?: return null
        val matchedProfile =
            supportedProfiles
                .filter {
                it.width == resolution.width &&
                    it.height == resolution.height
                }
                .maxByOrNull { it.fps }
                ?: return null

        return SenderStreamSettings(
            width = matchedProfile.width,
            height = matchedProfile.height,
            fpsUpperBound = matchedProfile.fps,
            bitrateMbps = bitrateMbps,
            adaptiveFps = true,
        )
    }

    private fun formatFrameRateSummary(): String =
        ADAPTIVE_FRAME_RATE_LABEL
}
