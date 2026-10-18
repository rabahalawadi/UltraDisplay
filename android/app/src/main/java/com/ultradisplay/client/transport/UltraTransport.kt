package com.ultradisplay.client.transport

import android.content.Context
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.ParcelFileDescriptor
import android.util.Log
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentLinkedQueue

class UltraTransport(
    private val context: Context,
    private val onFrameReceived: (nalData: ByteArray, timestampUs: Long, isKeyframe: Boolean) -> Unit,
    private val onConnected: (streamWidth: Int, streamHeight: Int, fps: Int) -> Unit,
    private val onDisconnected: (reason: String) -> Unit
) {
    private var tcpSocket: Socket? = null
    private var usbFileDescriptor: ParcelFileDescriptor? = null
    private var inputStream: InputStream? = null
    private var outputStream: OutputStream? = null

    private var isRunning = false
    private val sendQueue = ConcurrentLinkedQueue<ByteArray>()

    private var workerThread: Thread? = null
    private var senderThread: Thread? = null

    companion object {
        private const val TAG = "UltraTransport"
        const val ULTRADISPLAY_MAGIC = 0x55444953
        const val PKT_HANDSHAKE_REQ: Byte = 0x01
        const val PKT_HANDSHAKE_RESP: Byte = 0x02
        const val PKT_FRAME_DATA: Byte = 0x03
    }

    // Auto-discover PC on local Wi-Fi via UDP broadcast
    fun discoverHost(onFound: (hostIp: String, hostname: String, port: Int) -> Unit) {
        Thread {
            try {
                val udpSocket = DatagramSocket()
                udpSocket.broadcast = true
                udpSocket.soTimeout = 3000

                val msg = "UDIS_DISCOVER".toByteArray()
                val broadcastAddr = InetAddress.getByName("255.255.255.255")
                val packet = DatagramPacket(msg, msg.size, broadcastAddr, 28444)
                udpSocket.send(packet)

                val buf = ByteArray(256)
                val respPacket = DatagramPacket(buf, buf.size)
                udpSocket.receive(respPacket)

                val respStr = String(respPacket.data, 0, respPacket.length)
                val parts = respStr.split("|")
                if (parts.size >= 3 && parts[0] == "UDIS_OFFER") {
                    val hostname = parts[1]
                    val port = parts[2].toIntOrNull() ?: 28443
                    val hostIp = respPacket.address.hostAddress ?: ""
                    onFound(hostIp, hostname, port)
                }
                udpSocket.close()
            } catch (e: Exception) {
                Log.w(TAG, "Discovery timed out or error: ${e.message}")
            }
        }.start()
    }

    // Connect via Wi-Fi TCP
    fun connectTcp(host: String, port: Int = 28443, width: Int, height: Int, dpi: Int, refreshRate: Int) {
        disconnect()
        isRunning = true

        workerThread = Thread {
            try {
                Log.i(TAG, "Connecting to TCP $host:$port...")
                val socket = Socket(host, port)
                socket.tcpNoDelay = true
                socket.receiveBufferSize = 1024 * 1024
                socket.sendBufferSize = 64 * 1024

                tcpSocket = socket
                inputStream = socket.getInputStream()
                outputStream = socket.getOutputStream()

                startSession(width, height, dpi, refreshRate)
            } catch (e: Exception) {
                Log.e(TAG, "TCP connection failed", e)
                onDisconnected("Connection failed: ${e.message}")
            }
        }.apply { start() }
    }

    // Connect via USB Android Open Accessory (AOAP)
    fun connectUsb(accessory: UsbAccessory, width: Int, height: Int, dpi: Int, refreshRate: Int) {
        disconnect()
        isRunning = true

        workerThread = Thread {
            try {
                val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
                val fd = usbManager.openAccessory(accessory)
                if (fd == null) {
                    onDisconnected("Failed to open USB accessory.")
                    return@Thread
                }

                usbFileDescriptor = fd
                val fileDesc = fd.fileDescriptor
                inputStream = java.io.FileInputStream(fileDesc)
                outputStream = java.io.FileOutputStream(fileDesc)

                Log.i(TAG, "USB Accessory connected: ${accessory.description}")
                startSession(width, height, dpi, refreshRate)
            } catch (e: Exception) {
                Log.e(TAG, "USB connection failed", e)
                onDisconnected("USB Error: ${e.message}")
            }
        }.apply { start() }
    }

    private fun startSession(width: Int, height: Int, dpi: Int, refreshRate: Int) {
        // Start sender thread
        senderThread = Thread {
            try {
                while (isRunning) {
                    val pkt = sendQueue.poll()
                    if (pkt != null) {
                        outputStream?.write(pkt)
                        outputStream?.flush()
                    } else {
                        Thread.sleep(1)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Sender thread error: ${e.message}")
            }
        }.apply { start() }

        // Send Handshake
        if (!sendHandshake(width, height, dpi, refreshRate)) {
            onDisconnected("Handshake failed")
            return
        }

        // Receive Loop
        val headerBuf = ByteArray(8)
        while (isRunning) {
            readExact(headerBuf, 8)
            val hBuf = ByteBuffer.wrap(headerBuf).order(ByteOrder.LITTLE_ENDIAN)
            val magic = hBuf.int
            val type = hBuf.get()
            val version = hBuf.get()
            val payloadLen = hBuf.short.toInt() and 0xFFFF

            if (magic != ULTRADISPLAY_MAGIC) {
                Log.e(TAG, "Invalid magic 0x${Integer.toHexString(magic)}")
                break
            }

            if (type == PKT_FRAME_DATA) {
                // Read Frame Header (20 bytes: 4 seq + 8 timestamp + 1 key + 3 reserved + 4 nalSize)
                val fhdrBuf = ByteArray(20)
                readExact(fhdrBuf, 20)
                val fBuf = ByteBuffer.wrap(fhdrBuf).order(ByteOrder.LITTLE_ENDIAN)
                val frameIndex = fBuf.int
                val timestampUs = fBuf.long
                val isKeyframe = fBuf.get() != 0.toByte()
                fBuf.position(fBuf.position() + 3) // padding
                val nalSize = fBuf.int

                // Read NAL payload
                val nalData = ByteArray(nalSize)
                readExact(nalData, nalSize)

                onFrameReceived(nalData, timestampUs, isKeyframe)
            } else {
                // Skip payload
                skipExact(payloadLen)
            }
        }

        onDisconnected("Session ended")
    }

    private fun sendHandshake(width: Int, height: Int, dpi: Int, refreshRate: Int): Boolean {
        // UDisHeader: 8 bytes, UDisHandshakeReq: 74 bytes = 82 bytes
        val buf = ByteBuffer.allocate(82).order(ByteOrder.LITTLE_ENDIAN)

        // Header
        buf.putInt(ULTRADISPLAY_MAGIC)
        buf.put(PKT_HANDSHAKE_REQ)
        buf.put(1) // version
        buf.putShort(74) // payloadLen (74 bytes)

        // Req (74 bytes: 2+2+2+1+1+1+1 + 64)
        buf.putShort(width.toShort())
        buf.putShort(height.toShort())
        buf.putShort(dpi.toShort())
        buf.put(refreshRate.toByte())
        buf.put(1) // codec bitmask: 1=H264
        buf.put(1) // hasStylus: 1 (S-Pen active)
        buf.put(10) // maxTouchPoints: 10

        val deviceNameBytes = (android.os.Build.MODEL ?: "Android Device").toByteArray()
        val namePadded = ByteArray(64)
        System.arraycopy(deviceNameBytes, 0, namePadded, 0, deviceNameBytes.size.coerceAtMost(63))
        buf.put(namePadded)

        outputStream?.write(buf.array())
        outputStream?.flush()

        // Read Handshake Response Header (8 bytes)
        val respHdrBuf = ByteArray(8)
        readExact(respHdrBuf, 8)
        val respHdr = ByteBuffer.wrap(respHdrBuf).order(ByteOrder.LITTLE_ENDIAN)
        if (respHdr.int != ULTRADISPLAY_MAGIC || respHdr.get() != PKT_HANDSHAKE_RESP) {
            return false
        }
        val respVersion = respHdr.get()
        val respLen = respHdr.short.toInt() and 0xFFFF

        // Read Handshake Response (respLen bytes, exactly 11 bytes)
        val respBuf = ByteArray(respLen)
        readExact(respBuf, respLen)
        val resp = ByteBuffer.wrap(respBuf).order(ByteOrder.LITTLE_ENDIAN)
        val status = resp.get()
        val codec = resp.get()
        val streamWidth = resp.short.toInt() and 0xFFFF
        val streamHeight = resp.short.toInt() and 0xFFFF
        val targetFps = resp.get().toInt() and 0xFF

        if (status == 0.toByte()) {
            onConnected(streamWidth, streamHeight, targetFps)
            return true
        }
        return false
    }

    fun sendAsync(packet: ByteArray) {
        if (isRunning) {
            sendQueue.offer(packet)
        }
    }

    private fun readExact(dest: ByteArray, count: Int) {
        var readTotal = 0
        while (readTotal < count && isRunning) {
            val r = inputStream?.read(dest, readTotal, count - readTotal) ?: -1
            if (r == -1) throw java.io.EOFException("Connection closed")
            readTotal += r
        }
    }

    private fun skipExact(count: Int) {
        var skipped = 0L
        while (skipped < count && isRunning) {
            val s = inputStream?.skip(count - skipped) ?: -1L
            if (s <= 0) break
            skipped += s
        }
    }

    fun disconnect() {
        isRunning = false
        try {
            tcpSocket?.close()
            usbFileDescriptor?.close()
            inputStream?.close()
            outputStream?.close()
        } catch (ignored: Exception) {}
        tcpSocket = null
        usbFileDescriptor = null
        inputStream = null
        outputStream = null
    }
}
