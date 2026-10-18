package com.ultradisplay.client.spen

import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import com.ultradisplay.client.transport.UltraTransport
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.cos
import kotlin.math.sin

class UltraSPenManager(
    private val transport: UltraTransport,
    private val surfaceView: View
) : View.OnTouchListener, View.OnHoverListener, View.OnGenericMotionListener {

    private var wasTouching = false
    private var wasHovering = false

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
        val toolType = event.getToolType(0)
        // Accept S-Pen, Stylus, Eraser, and OTG Drawing Tablets (which Android classifies as Mouse or Unknown)
        if (toolType != MotionEvent.TOOL_TYPE_FINGER) {
            handleSPenHover(event)
            return true
        }
        return false
    }

    override fun onGenericMotion(v: View, event: MotionEvent): Boolean {
        val action = event.actionMasked
        if (action == MotionEvent.ACTION_HOVER_MOVE ||
            action == MotionEvent.ACTION_HOVER_ENTER ||
            action == MotionEvent.ACTION_HOVER_EXIT) {
            val toolType = event.getToolType(0)
            if (toolType != MotionEvent.TOOL_TYPE_FINGER) {
                handleSPenHover(event)
                return true
            }
        }
        return false
    }

    override fun onTouch(v: View, event: MotionEvent): Boolean {
        val toolType = event.getToolType(0)
        // If not a capacitive finger touch, treat as precision stylus / drawing tablet
        if (toolType != MotionEvent.TOOL_TYPE_FINGER) {
            handleSPenTouch(event)
            return true
        } else {
            handleMultitouch(event)
            return true
        }
    }

    private fun handleSPenHover(event: MotionEvent) {
        val width = surfaceView.width.toFloat().coerceAtLeast(1f)
        val height = surfaceView.height.toFloat().coerceAtLeast(1f)

        val normX = (event.x / width).coerceIn(0f, 1f)
        val normY = (event.y / height).coerceIn(0f, 1f)

        val tiltRad = event.getAxisValue(MotionEvent.AXIS_TILT)
        val orientRad = event.getAxisValue(MotionEvent.AXIS_ORIENTATION)

        // Convert spherical tilt/orientation to Cartesian tiltX and tiltY (-90..+90 degrees)
        val tiltDeg = Math.toDegrees(tiltRad.toDouble())
        val tiltX = (tiltDeg * sin(orientRad.toDouble())).toInt().coerceIn(-90, 90).toByte()
        val tiltY = (-tiltDeg * cos(orientRad.toDouble())).toInt().coerceIn(-90, 90).toByte()

        val rotation = ((Math.toDegrees(orientRad.toDouble()) + 360) % 360).toInt().toShort()
        val barrel = event.isButtonPressed(MotionEvent.BUTTON_STYLUS_PRIMARY) ||
                     event.isButtonPressed(MotionEvent.BUTTON_SECONDARY)
        val isEraser = event.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER ||
                       event.isButtonPressed(MotionEvent.BUTTON_STYLUS_SECONDARY) ||
                       event.isButtonPressed(MotionEvent.BUTTON_TERTIARY)

        var flags: Short = SPEN_FLAG_INRANGE
        if (barrel) flags = (flags.toInt() or SPEN_FLAG_BARREL.toInt()).toShort()
        if (isEraser) flags = (flags.toInt() or SPEN_FLAG_ERASER.toInt()).toShort()

        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                flags = (flags.toInt() or SPEN_FLAG_MOVE.toInt()).toShort()
                wasHovering = true
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                flags = SPEN_FLAG_NONE
                wasHovering = false
            }
        }

        // Pressure is strictly 0 when gliding in air during hover
        sendSPenPacket(flags, normX, normY, 0, tiltX, tiltY, rotation, event.eventTime)
    }

    private fun handleSPenTouch(event: MotionEvent) {
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

        // Process batch historical points for 120Hz/240Hz ultra-smooth stroke recording
        val historySize = event.historySize
        for (h in 0 until historySize) {
            val hx = (event.getHistoricalX(0, h) / width).coerceIn(0f, 1f)
            val hy = (event.getHistoricalY(0, h) / height).coerceIn(0f, 1f)
            val rawHp = event.getHistoricalPressure(0, h)
            val hp = if (rawHp > 0f) {
                (rawHp * 4095f).toInt().coerceIn(1, 4095).toShort()
            } else {
                2048.toShort()
            }

            var hFlags: Short = (SPEN_FLAG_INRANGE.toInt() or SPEN_FLAG_INCONTACT.toInt() or SPEN_FLAG_MOVE.toInt()).toShort()
            if (barrel) hFlags = (hFlags.toInt() or SPEN_FLAG_BARREL.toInt()).toShort()
            if (isEraser) hFlags = (hFlags.toInt() or SPEN_FLAG_ERASER.toInt()).toShort()

            sendSPenPacket(hFlags, hx, hy, hp, tiltX, tiltY, rotation, event.getHistoricalEventTime(h))
        }

        // Current point
        val normX = (event.x / width).coerceIn(0f, 1f)
        val normY = (event.y / height).coerceIn(0f, 1f)
        val rawP = event.pressure
        val pressure = if (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL) {
            0.toShort()
        } else if (rawP > 0f) {
            (rawP * 4095f).toInt().coerceIn(1, 4095).toShort()
        } else {
            2048.toShort() // Fallback if binary button press
        }

        var flags: Short = (SPEN_FLAG_INRANGE.toInt() or SPEN_FLAG_INCONTACT.toInt()).toShort()
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
                flags = (flags.toInt() or SPEN_FLAG_UP.toInt()).toShort()
                wasTouching = false
            }
        }

        sendSPenPacket(flags, normX, normY, pressure, tiltX, tiltY, rotation, event.eventTime)
    }

    private fun handleMultitouch(event: MotionEvent) {
        val width = surfaceView.width.toFloat().coerceAtLeast(1f)
        val height = surfaceView.height.toFloat().coerceAtLeast(1f)
        val pointerCount = event.pointerCount.coerceAtMost(10)

        // Header: 8 bytes, TouchEvent: 1 byte count + 10 * 10 bytes contacts + 4 bytes timestamp = 105 bytes
        val buf = ByteBuffer.allocate(8 + 105).order(ByteOrder.LITTLE_ENDIAN)

        // UDisHeader
        buf.putInt(0x55444953) // MAGIC
        buf.put(PKT_INPUT_TOUCH)
        buf.put(1) // VERSION
        buf.putShort(105) // payloadLen

        // UDisTouchEvent
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
                // Empty padding contact
                buf.put(0); buf.put(0); buf.putFloat(0f); buf.putFloat(0f); buf.putFloat(0f)
            }
        }

        buf.putInt(event.eventTime.toInt())
        transport.sendAsync(buf.array())
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
        // UDisHeader: 8 bytes
        // UDisSPenEvent: 2 (flags) + 4 (normX) + 4 (normY) + 2 (pressure) + 1 (tiltX) + 1 (tiltY) + 2 (rotation) + 4 (timestamp) = 20 bytes
        val buf = ByteBuffer.allocate(28).order(ByteOrder.LITTLE_ENDIAN)

        // Header
        buf.putInt(0x55444953) // MAGIC
        buf.put(PKT_INPUT_SPEN)
        buf.put(1) // VERSION
        buf.putShort(20) // payloadLen

        // Payload
        buf.putShort(flags)
        buf.putFloat(normX)
        buf.putFloat(normY)
        buf.putShort(pressure)
        buf.put(tiltX)
        buf.put(tiltY)
        buf.putShort(rotation)
        buf.putInt(timestampMs.toInt())

        transport.sendAsync(buf.array())
    }
}
