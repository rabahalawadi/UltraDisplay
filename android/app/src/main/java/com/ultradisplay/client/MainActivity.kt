package com.ultradisplay.client

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.DisplayMetrics
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Switch
import android.widget.TextView
import com.ultradisplay.client.decoder.UltraVideoDecoder
import com.ultradisplay.client.spen.UltraSPenManager
import com.ultradisplay.client.transport.UltraTransport

class MainActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var surfaceView: SurfaceView
    private lateinit var overlayLayout: LinearLayout
    private lateinit var statusText: TextView
    private lateinit var hostIpInput: EditText
    private lateinit var connectButton: Button
    private lateinit var statsOverlay: TextView
    private lateinit var debugOverlay: TextView
    private lateinit var cursorIndicator: View
    private lateinit var headerPill: TextView

    // Tab Containers
    private lateinit var connectContainer: LinearLayout
    private lateinit var settingsContainer: LinearLayout
    private lateinit var aboutContainer: LinearLayout

    // Tab Buttons
    private lateinit var tabConnectBtn: Button
    private lateinit var tabSettingsBtn: Button
    private lateinit var tabAboutBtn: Button

    // Settings Switches
    private lateinit var switch120Hz: Switch
    private lateinit var switchKeepAwake: Switch
    private lateinit var switchDebugOverlay: Switch
    private lateinit var switchCursorDot: Switch

    private var transport: UltraTransport? = null
    private var decoder: UltraVideoDecoder? = null
    private var spenManager: UltraSPenManager? = null

    private var isConnected = false
    private var screenWidth = 1920
    private var screenHeight = 1080
    private var screenDpi = 280
    private var refreshRate = 60

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        hideSystemUI()

        surfaceView = findViewById(R.id.displaySurface)
        overlayLayout = findViewById(R.id.overlayLayout)
        statusText = findViewById(R.id.statusText)
        hostIpInput = findViewById(R.id.hostIpInput)
        connectButton = findViewById(R.id.connectButton)
        statsOverlay = findViewById(R.id.statsOverlay)
        debugOverlay = findViewById(R.id.debugOverlay)
        cursorIndicator = findViewById(R.id.cursorIndicator)
        headerPill = findViewById(R.id.headerPill)

        connectContainer = findViewById(R.id.connectContainer)
        settingsContainer = findViewById(R.id.settingsContainer)
        aboutContainer = findViewById(R.id.aboutContainer)

        tabConnectBtn = findViewById(R.id.tabConnectBtn)
        tabSettingsBtn = findViewById(R.id.tabSettingsBtn)
        tabAboutBtn = findViewById(R.id.tabAboutBtn)

        switch120Hz = findViewById(R.id.switch120Hz)
        switchKeepAwake = findViewById(R.id.switchKeepAwake)
        switchDebugOverlay = findViewById(R.id.switchDebugOverlay)
        switchCursorDot = findViewById(R.id.switchCursorDot)

        val githubLink: TextView = findViewById(R.id.githubLink)

        surfaceView.holder.addCallback(this)
        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true

        val metrics = DisplayMetrics()
        windowManager.defaultDisplay.getRealMetrics(metrics)
        screenWidth = metrics.widthPixels
        screenHeight = metrics.heightPixels
        screenDpi = metrics.densityDpi
        val displayRefresh = windowManager.defaultDisplay.refreshRate.toInt()
        refreshRate = if (displayRefresh >= 90) displayRefresh else 120

        // Initialize S-Pen & Huion manager immediately so user can debug input before connecting
        spenManager = UltraSPenManager(null, surfaceView, debugOverlay, cursorIndicator)

        setupTransport()

        // Tab Navigation
        tabConnectBtn.setOnClickListener { selectTab(0) }
        tabSettingsBtn.setOnClickListener { selectTab(1) }
        tabAboutBtn.setOnClickListener { selectTab(2) }

        // Mode Selector: Wi-Fi vs USB Cable
        val wifiModeBtn: Button = findViewById(R.id.wifiModeBtn)
        val cableModeBtn: Button = findViewById(R.id.cableModeBtn)
        var lastWifiIp = "192.168.0.89"

        wifiModeBtn.setOnClickListener {
            wifiModeBtn.setBackgroundColor(Color.parseColor("#E0E0E0"))
            wifiModeBtn.setTextColor(Color.parseColor("#121212"))
            cableModeBtn.setBackgroundColor(Color.parseColor("#1C1C1C"))
            cableModeBtn.setTextColor(Color.parseColor("#888888"))
            hostIpInput.setText(lastWifiIp)
            hostIpInput.isEnabled = true
            statusText.text = "Mode: Wi-Fi (Local Network)"
        }

        cableModeBtn.setOnClickListener {
            cableModeBtn.setBackgroundColor(Color.parseColor("#E0E0E0"))
            cableModeBtn.setTextColor(Color.parseColor("#121212"))
            wifiModeBtn.setBackgroundColor(Color.parseColor("#1C1C1C"))
            wifiModeBtn.setTextColor(Color.parseColor("#888888"))
            val current = hostIpInput.text.toString().trim()
            if (current != "127.0.0.1" && current.isNotEmpty()) {
                lastWifiIp = current
            }
            hostIpInput.setText("127.0.0.1")
            hostIpInput.isEnabled = false
            statusText.text = "Mode: USB Cable (Direct Tunnel via 127.0.0.1)"
        }

        // Settings switches logic
        switch120Hz.setOnCheckedChangeListener { _, isChecked ->
            refreshRate = if (isChecked) 120 else 60
        }

        switchKeepAwake.setOnCheckedChangeListener { _, isChecked ->
            if (isChecked) {
                window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            } else {
                window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }

        switchDebugOverlay.setOnCheckedChangeListener { _, isChecked ->
            debugOverlay.visibility = if (isChecked) View.VISIBLE else View.GONE
        }

        switchCursorDot.setOnCheckedChangeListener { _, isChecked ->
            spenManager?.showCursor = isChecked
            cursorIndicator.visibility = if (isChecked) View.VISIBLE else View.GONE
        }

        // About Us GitHub link
        githubLink.setOnClickListener {
            try {
                val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/rabahalawadi/UltraDisplay"))
                startActivity(intent)
            } catch (e: Exception) {
                // Ignore if browser not found
            }
        }

        // Stats pill click toggles overlay dashboard while streaming
        statsOverlay.setOnClickListener {
            overlayLayout.visibility = if (overlayLayout.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }

        connectButton.setOnClickListener {
            if (!isConnected) {
                val ip = hostIpInput.text.toString().trim()
                val targetIp = if (ip.isNotEmpty()) ip else "192.168.0.89"
                statusText.text = "Connecting to $targetIp..."
                headerPill.text = "CONNECTING"
                headerPill.setTextColor(Color.parseColor("#E0E0E0"))
                transport?.connectTcp(targetIp, 28443, screenWidth, screenHeight, screenDpi, refreshRate)
            } else {
                transport?.disconnect()
                onDisconnectedUI("Disconnected by user")
            }
        }

        // Auto-discover host on local network
        transport?.discoverHost { hostIp, hostname, port ->
            runOnUiThread {
                lastWifiIp = hostIp
                if (hostIpInput.text.toString() != "127.0.0.1") {
                    hostIpInput.setText(hostIp)
                    statusText.text = "Found PC: $hostname ($hostIp)"
                }
            }
        }

        // Check if launched by USB Accessory attachment
        handleUsbIntent(intent)
    }

    private fun selectTab(tabIndex: Int) {
        val activeBg = Color.parseColor("#E0E0E0")
        val inactiveBg = Color.parseColor("#1C1C1C")
        val activeText = Color.parseColor("#121212")
        val inactiveText = Color.parseColor("#888888")

        // Tab buttons styling
        tabConnectBtn.setBackgroundColor(if (tabIndex == 0) activeBg else inactiveBg)
        tabConnectBtn.setTextColor(if (tabIndex == 0) activeText else inactiveText)

        tabSettingsBtn.setBackgroundColor(if (tabIndex == 1) activeBg else inactiveBg)
        tabSettingsBtn.setTextColor(if (tabIndex == 1) activeText else inactiveText)

        tabAboutBtn.setBackgroundColor(if (tabIndex == 2) activeBg else inactiveBg)
        tabAboutBtn.setTextColor(if (tabIndex == 2) activeText else inactiveText)

        // Tab containers visibility
        connectContainer.visibility = if (tabIndex == 0) View.VISIBLE else View.GONE
        settingsContainer.visibility = if (tabIndex == 1) View.VISIBLE else View.GONE
        aboutContainer.visibility = if (tabIndex == 2) View.VISIBLE else View.GONE
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleUsbIntent(intent)
    }

    private fun handleUsbIntent(intent: Intent?) {
        if (intent?.action == UsbManager.ACTION_USB_ACCESSORY_ATTACHED) {
            val accessory = intent.getParcelableExtra<UsbAccessory>(UsbManager.EXTRA_ACCESSORY)
            if (accessory != null) {
                statusText.text = "Connecting via USB AOA..."
                transport?.connectUsb(accessory, screenWidth, screenHeight, screenDpi, refreshRate)
            }
        }
    }

    private fun setupTransport() {
        transport = UltraTransport(
            context = this,
            onFrameReceived = { nalData, timestampUs, isKeyframe ->
                decoder?.decodeNalUnit(nalData, timestampUs, isKeyframe)
            },
            onConnected = { streamWidth, streamHeight, fps ->
                runOnUiThread {
                    isConnected = true
                    overlayLayout.visibility = View.GONE
                    statsOverlay.visibility = View.VISIBLE
                    statsOverlay.text = "Stream: ${streamWidth}x${streamHeight} @ ${fps}fps"
                    connectButton.text = "Disconnect"
                    headerPill.text = "LIVE STREAM"
                    headerPill.setTextColor(Color.parseColor("#E0E0E0"))

                    // Start hardware video decoder
                    decoder = UltraVideoDecoder(surfaceView.holder.surface, streamWidth, streamHeight)
                    decoder?.start()

                    // Link transport to S-Pen / Huion manager
                    spenManager?.transport = transport

                    surfaceView.requestFocus()
                }
            },
            onDisconnected = { reason ->
                runOnUiThread {
                    onDisconnectedUI(reason)
                }
            }
        )
    }

    /**
     * Intercepts all hover and generic mouse movements across the entire Activity window.
     * Guarantees external Huion tablets are captured on Xiaomi MIUI/HyperOS.
     */
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (spenManager?.handleGenericMotion("dispatchGeneric", event) == true) {
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    /**
     * Intercepts all touch and pen clicks across the Activity window.
     */
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (isConnected && spenManager?.handleTouchEvent("dispatchTouch", event) == true) {
            return true
        }
        return super.dispatchTouchEvent(event)
    }

    private fun onDisconnectedUI(reason: String) {
        isConnected = false
        overlayLayout.visibility = View.VISIBLE
        statsOverlay.visibility = View.GONE
        headerPill.text = "STANDBY"
        headerPill.setTextColor(Color.parseColor("#E0E0E0"))
        statusText.text = reason
        connectButton.text = "Connect"
        decoder?.stop()
        decoder = null
        spenManager?.transport = null
    }

    override fun surfaceCreated(holder: SurfaceHolder) {}
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        decoder?.stop()
    }

    private fun hideSystemUI() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let { controller ->
                controller.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_FULLSCREEN
            )
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        transport?.disconnect()
        decoder?.stop()
    }
}
