package com.example.androidcast

import android.app.Activity
import android.content.Context
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

    private var supportedProfiles: List<StreamProfile> = emptyList()
    private var resolutionOptions: List<ResolutionOption> = emptyList()

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode != Activity.RESULT_OK || result.data == null) {
                statusText.text = getString(R.string.status_permission_denied)
                return@registerForActivityResult
            }

            val receiverIp = receiverIpInput.text.toString().trim()
            val controlPort = controlPortInput.text.toString().trim().toIntOrNull()
            if (receiverIp.isEmpty() || controlPort == null) {
                statusText.text = getString(R.string.status_invalid_target)
                return@registerForActivityResult
            }

            if (persistStreamSettingsFromUi(showError = true) == null) {
                statusText.text = getString(R.string.status_invalid_stream_settings)
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
            statusText.text = getString(R.string.status_starting_stream)
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
        statusText.text = getString(R.string.status_idle_with_version, BuildConfig.VERSION_NAME)
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
            statusText.text = getString(R.string.status_invalid_stream_settings)
        }

        startButton.setOnClickListener {
            if (persistStreamSettingsFromUi(showError = true) == null) {
                statusText.text = getString(R.string.status_invalid_stream_settings)
                return@setOnClickListener
            }
            statusText.text = getString(R.string.status_requesting_permission)
            projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
        }

        findViewById<Button>(R.id.stopButton).setOnClickListener {
            ContextCompat.startForegroundService(
                this,
                ProjectionForegroundService.createStopIntent(this),
            )
            statusText.text = getString(R.string.status_stopping_stream)
        }
    }

    override fun onPause() {
        persistStreamSettingsFromUi(showError = false)
        super.onPause()
    }

    private fun setupStreamSettingsInputs() {
        if (supportedProfiles.isEmpty()) {
            streamSettingsSummaryText.text = getString(R.string.status_invalid_stream_settings)
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
            streamSettingsSummaryText.text = getString(R.string.status_invalid_stream_settings)
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
                streamSettingsSummaryText.text = getString(R.string.status_invalid_stream_settings)
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
