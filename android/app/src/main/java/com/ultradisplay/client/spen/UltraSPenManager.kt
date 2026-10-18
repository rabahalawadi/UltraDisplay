package com.ultradisplay.client.spen

import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.widget.TextView
import com.ultradisplay.client.transport.UltraTransport
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.cos
import kotlin.math.sin

class UltraSPenManager(
    var transport: UltraTransport?,
    private val surfaceView: View,
    private val debugOverlay: TextView? = null,
    private val cursorIndicator: View? = null
) : View.OnTouchListener, View.OnHoverListener, View.OnGenericMotionListener {

    private var wasTouching = false
    private var wasHovering = false
    private var eventCount = 0

    companion object {
        const val PKT_INPUT_SPEN: Byte = 0x10
        const val PKT_INPUT_TOUCH: Byte = 0x11

        const val SPEN_FLAG_NONE: Short      = 0x0000
        const val SPEN_FLAG_INRANGE: Short   = 0x0001
        const val SPEN_FLAG_INCONTACT: Short = 0x0002
        const val SPEN_FLAG_DOWN: Short      = 0x0004
        const val SPEN_FLAG_MOVE: Short      = 0x0008
        const val SPEN_FLAG_UP: Short        = 0x0010
        const val SPEN_FLAG_BARREL: Short    = 0x0020
        const val SPEN_FLAG_ERASER: Short    = 0x0040
    }

    init {
        surfaceView.setOnTouchListener(this)
        surfaceView.setOnHoverListener(this)
        surfaceView.setOnGenericMotionListener(this)
    }

    override fun onHover(v: View, event: MotionEvent): Boolean {
        return handleGenericMotion("onHover", event)
    }

    override fun onGenericMotion(v: View, event: MotionEvent): Boolean {
        return handleGenericMotion("onGenericMotion", event)
    }

    override fun onTouch(v: View, event: MotionEvent): Boolean {
        return handleTouchEvent("onTouch", event)
    }

    fun handleGenericMotion(callerMethod: String, event: MotionEvent): Boolean {
        eventCount++
        val isButtonDown = event.buttonState != 0
        val hasPressure = event.pressure > 0.015f

        if (isButtonDown || hasPressure) {
            handleSPenTouch(callerMethod, event)
            return true
        }

        handleSPenHover(callerMethod, event)
        return true
    }

    fun handleTouchEvent(callerMethod: String, event: MotionEvent): Boolean {
        eventCount++
        val toolType = event.getToolType(0)

        // Route finger touches strictly to multitouch (never convert to S-Pen / Cintiq drawing clicks)
        if (toolType == MotionEvent.TOOL_TYPE_FINGER) {
            handleMultitouch(event)
            return true
        }

        val isRelease = (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL)
        val isContact = (event.buttonState != 0 || event.pressure > 0.015f)

        if (!isRelease && !isContact && event.actionMasked != MotionEvent.ACTION_DOWN) {
            handleSPenHover(callerMethod, event)
            return true
        }

        handleSPenTouch(callerMethod, event)
        return true
    }

    private fun handleSPenHover(callerMethod: String, event: MotionEvent) {
        val width = surfaceView.width.toFloat().coerceAtLeast(1f)
        val height = surfaceView.height.toFloat().coerceAtLeast(1f)

        val normX = (event.x / width).coerceIn(0f, 1f)
        val normY = (event.y / height).coerceIn(0f, 1f)

        val tiltRad = event.getAxisValue(MotionEvent.AXIS_TILT)
        val orientRad = event.getAxisValue(MotionEvent.AXIS_ORIENTATION)
        val tiltDeg = Math.toDegrees(tiltRad.toDouble())
        val tiltX = (tiltDeg * sin(orientRad.toDouble())).toInt().coerceIn(-90, 90).toByte()
        val tiltY = (-tiltDeg * cos(orientRad.toDouble())).toInt().coerceIn(-90, 90).toByte()
        val rotation = ((Math.toDegrees(orientRad.toDouble()) + 360) % 360).toInt().toShort()

        val barrel = event.isButtonPressed(MotionEvent.BUTTON_STYLUS_PRIMARY) ||
                     event.isButtonPressed(MotionEvent.BUTTON_SECONDARY)
        val isEraser = event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER ||
                       event.isButtonPressed(MotionEvent.BUTTON_STYLUS_SECONDARY) ||
                       event.isButtonPressed(MotionEvent.BUTTON_TERTIARY)

        var flags: Short = (SPEN_FLAG_INRANGE.toInt() or SPEN_FLAG_MOVE.toInt()).toShort()
        if (barrel) flags = (flags.toInt() or SPEN_FLAG_BARREL.toInt()).toShort()
        if (isEraser) flags = (flags.toInt() or SPEN_FLAG_ERASER.toInt()).toShort()

        if (event.actionMasked == MotionEvent.ACTION_HOVER_EXIT) {
            flags = SPEN_FLAG_NONE
            wasHovering = false
        } else {
            wasHovering = true
        }

        updateHUD(callerMethod, event, normX, normY, isHover = true, rawPressure = 0f)

        sendSPenPacket(flags, normX, normY, 0, tiltX, tiltY, rotation, event.eventTime)
    }

    private fun handleSPenTouch(callerMethod: String, event: MotionEvent) {
        val width = surfaceView.width.toFloat().coerceAtLeast(1f)
        val height = surfaceView.height.toFloat().coerceAtLeast(1f)

        val tiltRad = event.getAxisValue(MotionEvent.AXIS_TILT)
        val orientRad = event.getAxisValue(MotionEvent.AXIS_ORIENTATION)
        val tiltDeg = Math.toDegrees(tiltRad.toDouble())
        val tiltX = (tiltDeg * sin(orientRad.toDouble())).toInt().coerceIn(-90, 90).toByte()
        val tiltY = (-tiltDeg * cos(orientRad.toDouble())).toInt().coerceIn(-90, 90).toByte()
        val rotation = ((Math.toDegrees(orientRad.toDouble()) + 360) % 360).toInt().toShort()

        val barrel = event.isButtonPressed(MotionEvent.BUTTON_STYLUS_PRIMARY) ||
                     event.isButtonPressed(MotionEvent.BUTTON_SECONDARY)
        val isEraser = event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER ||
                       event.isButtonPressed(MotionEvent.BUTTON_STYLUS_SECONDARY) ||
                       event.isButtonPressed(MotionEvent.BUTTON_TERTIARY)

        // Process batch historical points
        val historySize = event.historySize
        for (h in 0 until historySize) {
            val hx = (event.getHistoricalX(0, h) / width).coerceIn(0f, 1f)
            val hy = (event.getHistoricalY(0, h) / height).coerceIn(0f, 1f)
            val rawHp = event.getHistoricalPressure(0, h)
            val hp = if (rawHp > 0.015f) {
                (rawHp * 4095f).toInt().coerceIn(1, 4095).toShort()
            } else {
                2048.toShort()
            }

            var hFlags: Short = (SPEN_FLAG_INRANGE.toInt() or SPEN_FLAG_INCONTACT.toInt() or SPEN_FLAG_MOVE.toInt()).toShort()
            if (barrel) hFlags = (hFlags.toInt() or SPEN_FLAG_BARREL.toInt()).toShort()
            if (isEraser) hFlags = (hFlags.toInt() or SPEN_FLAG_ERASER.toInt()).toShort()

            sendSPenPacket(hFlags, hx, hy, hp, tiltX, tiltY, rotation, event.getHistoricalEventTime(h))
        }

        val normX = (event.x / width).coerceIn(0f, 1f)
        val normY = (event.y / height).coerceIn(0f, 1f)
        val rawP = event.pressure

        val isRelease = (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL)
        val pressure = if (isRelease) {
            0.toShort()
        } else if (rawP > 0.015f) {
            (rawP * 4095f).toInt().coerceIn(1, 4095).toShort()
        } else {
            2048.toShort()
        }

        var flags: Short = if (isRelease) {
            (SPEN_FLAG_INRANGE.toInt() or SPEN_FLAG_UP.toInt()).toShort()
        } else {
            (SPEN_FLAG_INRANGE.toInt() or SPEN_FLAG_INCONTACT.toInt()).toShort()
        }

        if (barrel) flags = (flags.toInt() or SPEN_FLAG_BARREL.toInt()).toShort()
        if (isEraser) flags = (flags.toInt() or SPEN_FLAG_ERASER.toInt()).toShort()

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                flags = (flags.toInt() or SPEN_FLAG_DOWN.toInt()).toShort()
                wasTouching = true
            }
            MotionEvent.ACTION_MOVE -> {
                flags = (flags.toInt() or SPEN_FLAG_MOVE.toInt()).toShort()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                wasTouching = false
            }
        }

        updateHUD(callerMethod, event, normX, normY, isHover = isRelease, rawPressure = rawP)

        sendSPenPacket(flags, normX, normY, pressure, tiltX, tiltY, rotation, event.eventTime)
    }

    private fun updateHUD(callerMethod: String, event: MotionEvent, normX: Float, normY: Float, isHover: Boolean, rawPressure: Float) {
        debugOverlay?.post {
            val actionStr = when (event.actionMasked) {
                MotionEvent.ACTION_HOVER_MOVE -> "HOVER_MOVE"
                MotionEvent.ACTION_HOVER_ENTER -> "HOVER_ENTER"
                MotionEvent.ACTION_HOVER_EXIT -> "HOVER_EXIT"
                MotionEvent.ACTION_MOVE -> "MOVE"
                MotionEvent.ACTION_DOWN -> "DOWN"
                MotionEvent.ACTION_UP -> "UP"
                MotionEvent.ACTION_CANCEL -> "CANCEL"
                MotionEvent.ACTION_SCROLL -> "SCROLL"
                else -> "ACT_${event.actionMasked}"
            }

            val toolStr = when (event.getToolType(0)) {
                MotionEvent.TOOL_TYPE_STYLUS -> "STYLUS"
                MotionEvent.TOOL_TYPE_MOUSE -> "MOUSE"
                MotionEvent.TOOL_TYPE_ERASER -> "ERASER"
                MotionEvent.TOOL_TYPE_FINGER -> "FINGER"
                MotionEvent.TOOL_TYPE_UNKNOWN -> "UNKNOWN"
                else -> "TOOL_${event.getToolType(0)}"
            }

            val sourceStr = buildString {
                val s = event.source
                if (s and InputDevice.SOURCE_MOUSE == InputDevice.SOURCE_MOUSE) append("MOUSE ")
                if (s and InputDevice.SOURCE_STYLUS == InputDevice.SOURCE_STYLUS) append("STYLUS ")
                if (s and InputDevice.SOURCE_TOUCHSCREEN == InputDevice.SOURCE_TOUCHSCREEN) append("TOUCHSCREEN ")
                if (s and InputDevice.SOURCE_CLASS_POINTER != 0) append("POINTER ")
                if (isEmpty()) append("0x${Integer.toHexString(s)}")
            }

            val buttonsStr = buildString {
                val b = event.buttonState
                if (b and MotionEvent.BUTTON_PRIMARY != 0) append("PRIM ")
                if (b and MotionEvent.BUTTON_SECONDARY != 0) append("SEC ")
                if (b and MotionEvent.BUTTON_TERTIARY != 0) append("TERT ")
                if (b and MotionEvent.BUTTON_STYLUS_PRIMARY != 0) append("STYLUS1 ")
                if (b and MotionEvent.BUTTON_STYLUS_SECONDARY != 0) append("STYLUS2 ")
                if (isEmpty()) append("NONE")
            }

            val isStreaming = (transport != null)
            debugOverlay.text = """
                [HUION / MOUSE DEBUG - XIAOMI 12 LITE]
                Caller:       $callerMethod
                Action:       $actionStr (${event.actionMasked})
                Mode:         ${if (isHover) "HOVER (In Air)" else "DRAWING (Contact)"}
                Screen Pos:   X: ${event.x.toInt()}, Y: ${event.y.toInt()}
                Normalized:   (${"%.3f".format(normX)}, ${"%.3f".format(normY)})
                Tool Type:    $toolStr (${event.getToolType(0)})
                Source:       $sourceStr
                Pressure:     ${"%.4f".format(rawPressure)}
                Buttons:      $buttonsStr
                Total Events: $eventCount ${if (isStreaming) "[STREAMING -> PC]" else "[STANDBY]"}
            """.trimIndent()

            cursorIndicator?.let { indicator ->
                indicator.visibility = View.VISIBLE
                indicator.x = event.x - (indicator.width / 2f)
                indicator.y = event.y - (indicator.height / 2f)
            }
        }
    }

    private fun handleMultitouch(event: MotionEvent) {
        val width = surfaceView.width.toFloat().coerceAtLeast(1f)
        val height = surfaceView.height.toFloat().coerceAtLeast(1f)
        val pointerCount = event.pointerCount.coerceAtMost(10)

        val buf = ByteBuffer.allocate(8 + 105).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(0x55444953) // MAGIC
        buf.put(PKT_INPUT_TOUCH)
        buf.put(1) // VERSION
        buf.putShort(105) // payloadLen

        buf.put(pointerCount.toByte())

        for (i in 0 until 10) {
            if (i < pointerCount) {
                val pointerId = event.getPointerId(i).toByte()
                val normX = (event.getX(i) / width).coerceIn(0f, 1f)
                val normY = (event.getY(i) / height).coerceIn(0f, 1f)
                val size = event.getSize(i)

                val state: Byte = when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> if (event.actionIndex == i) 0 else 1
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> if (event.actionIndex == i) 2 else 1
                    else -> 1 // MOVE
                }

                buf.put(pointerId)
                buf.put(state)
                buf.putFloat(normX)
                buf.putFloat(normY)
                buf.putFloat(size)
            } else {
                buf.put(0); buf.put(0); buf.putFloat(0f); buf.putFloat(0f); buf.putFloat(0f)
            }
        }

        buf.putInt(event.eventTime.toInt())
        transport?.sendAsync(buf.array())
    }

    private fun sendSPenPacket(
        flags: Short,
        normX: Float,
        normY: Float,
        pressure: Short,
        tiltX: Byte,
        tiltY: Byte,
        rotation: Short,
        timestampMs: Long
    ) {
        val t = transport ?: return
        val buf = ByteBuffer.allocate(28).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(0x55444953) // MAGIC
        buf.put(PKT_INPUT_SPEN)
        buf.put(1) // VERSION
        buf.putShort(20) // payloadLen

        buf.putShort(flags)
        buf.putFloat(normX)
        buf.putFloat(normY)
        buf.putShort(pressure)
        buf.put(tiltX)
        buf.put(tiltY)
        buf.putShort(rotation)
        buf.putInt(timestampMs.toInt())

        t.sendAsync(buf.array())
    }
}
