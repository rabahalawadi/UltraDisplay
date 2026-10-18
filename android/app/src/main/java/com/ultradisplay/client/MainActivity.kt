package com.ultradisplay.client

import android.app.Activity
import android.content.Intent
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
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

        surfaceView.holder.addCallback(this)
        surfaceView.isFocusable = true
        surfaceView.isFocusableInTouchMode = true

        val metrics = DisplayMetrics()
        windowManager.defaultDisplay.getRealMetrics(metrics)
        screenWidth = metrics.widthPixels
        screenHeight = metrics.heightPixels
        screenDpi = metrics.densityDpi
        refreshRate = windowManager.defaultDisplay.refreshRate.toInt()

        // Initialize S-Pen & Huion manager immediately so user can debug input before connecting
        spenManager = UltraSPenManager(null, surfaceView, debugOverlay, cursorIndicator)

        // Attach listeners to window decorView to catch all Xiaomi/MIUI pointer events
        window.decorView.setOnGenericMotionListener { _, event ->
            spenManager?.handleGenericMotion("decorGeneric", event) ?: false
        }
        window.decorView.setOnHoverListener { _, event ->
            spenManager?.handleGenericMotion("decorHover", event) ?: false
        }
        window.decorView.setOnTouchListener { _, event ->
            spenManager?.handleTouchEvent("decorTouch", event) ?: false
        }

        setupTransport()

        connectButton.setOnClickListener {
            if (!isConnected) {
                val ip = hostIpInput.text.toString().trim()
                if (ip.isNotEmpty()) {
                    statusText.text = "Connecting to $ip..."
                    transport?.connectTcp(ip, 28443, screenWidth, screenHeight, screenDpi, refreshRate)
                }
            } else {
                transport?.disconnect()
                onDisconnectedUI("Disconnected by user")
            }
        }

        // Auto-discover host on local network
        transport?.discoverHost { hostIp, hostname, port ->
            runOnUiThread {
                hostIpInput.setText(hostIp)
                statusText.text = "Found PC: $hostname ($hostIp)"
            }
        }

        // Check if launched by USB Accessory attachment
        handleUsbIntent(intent)
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
        spenManager?.handleGenericMotion("dispatchGeneric", event)
        return super.dispatchGenericMotionEvent(event)
    }

    /**
     * Intercepts all touch and pen clicks across the Activity window.
     */
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        spenManager?.handleTouchEvent("dispatchTouch", event)
        return super.dispatchTouchEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        spenManager?.handleGenericMotion("onGenericMotion", event)
        return super.onGenericMotionEvent(event)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        spenManager?.handleTouchEvent("onTouchEvent", event)
        return super.onTouchEvent(event)
    }

    private fun onDisconnectedUI(reason: String) {
        isConnected = false
        overlayLayout.visibility = View.VISIBLE
        statsOverlay.visibility = View.GONE
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
