# UltraDisplay 🚀

> **High-Performance, Zero-Kernel-Driver SuperDisplay Alternative for Windows & Android**  
> Stream your Windows desktop to your Android device with native **Windows Ink**, ultra-low latency hardware H.264 video encoding, and full support for **Samsung S-Pen** & **external USB OTG drawing tablets** (Huion, XP-Pen, Wacom).

---

## 🎨 Key Features

- **Zero-Kernel-Driver Windows Host**:
  - Direct integration with Windows 10/11 **Synthetic Pointer API** (`InjectSyntheticPointerInput`).
  - No risky unsigned kernel drivers or Virtual Display Driver crashes.
- **Hardware-Accelerated Screen Streaming**:
  - **DXGI Desktop Duplication API** captures monitor frames directly from GPU VRAM in under 1 millisecond.
  - **Media Foundation H.264 Hardware Encoder** (NVIDIA NVENC, Intel QuickSync, AMD AMF) streams with minimal CPU overhead.
- **Natural Windows Ink Pressure & Hover**:
  - **Artistic Pressure Curve ($P^{1.4}$)**: Calibrated specifically for digital art (Krita, Photoshop, Clip Studio Paint), allowing hairline strokes on light touch and gradual build-up without abrupt max-pressure spikes.
  - **Dual-Hover Injection**: Combines synthetic pointer range updates with absolute mouse move events so cursor hover, hit-testing, and brush circles glide effortlessly without clicking.
- **Full Drawing Tablet Support**:
  - **Samsung S-Pen**: 4,096 hardware pressure levels, spherical tilt, barrel button, and eraser.
  - **External USB OTG Drawing Tablets**: Automatically translates generic mouse hover events from USB OTG drawing tablets (Huion, XP-Pen, Gaomon, Wacom) into Windows Ink hover.
- **Native Android Client**:
  - Built with Kotlin and Android's low-latency **MediaCodec** decoding straight to a hardware `SurfaceView`.
  - Supports both local Wi-Fi TCP and USB Android Open Accessory (AOA 2.0) streaming.
- **Zero-Install Web Client**:
  - `client_web.html` provides a zero-install fallback using modern browser WebSockets and Pointer Events.

---

## 📂 Repository Structure

```
UltraDisplay/
├── server/                              # Windows Host C++ Server
│   ├── server_engine.cpp                # Core server, WebSocket & TCP daemon
│   ├── screen_capture.cpp / .h          # DXGI Desktop Duplication capture engine
│   ├── h264_encoder.cpp / .h            # Media Foundation H.264 HW encoder
│   ├── pen_injector.cpp / .h            # Windows Ink synthetic pointer injector
│   ├── app.manifest                     # UAC Administrator manifest
│   ├── Makefile                         # MinGW-w64 build configuration
│   ├── Run_As_Admin.bat                 # 1-Click Administrator Launcher
│   └── UltraDisplayServer_Admin.exe     # Standalone precompiled host binary
├── android/                             # Native Android Client
│   ├── app/src/main/java/com/ultradisplay/client/
│   │   ├── MainActivity.kt              # Fullscreen display activity
│   │   ├── decoder/UltraVideoDecoder.kt # MediaCodec low-latency video decoder
│   │   ├── spen/UltraSPenManager.kt     # S-Pen & OTG tablet digitizer manager
│   │   └── transport/UltraTransport.kt  # TCP & USB AOA networking
│   ├── app/src/main/res/                # Layouts and resources
│   └── build.gradle.kts                 # Modern Gradle build scripts
├── client_web.html                      # Zero-install HTML5 fallback client
├── .gitignore
└── README.md
```

---

## 🚀 Quick Start

### 1. Launch the Windows Host Server
1. Navigate to the `server/` directory.
2. Double-click `Run_As_Admin.bat` (or right-click `UltraDisplayServer_Admin.exe` and select **Run as Administrator**).
   > *Note: Administrator privileges are required by Windows to inject synthetic pointer events into elevated painting applications like Krita.*
3. The server will start listening on port `28443` (TCP & WebSocket).

### 2. Connect Your Android Device

#### Option A: Native Android App
1. Open the **UltraDisplay** app on your Android tablet or phone.
2. Enter your PC's local IP address (e.g., `192.168.0.89`) and tap **Connect** (or plug in via USB cable).
3. The desktop stream will appear immediately with hardware-accelerated rendering.

#### Option B: Web Browser Fallback
1. Open Chrome, Edge, or Samsung Internet on your phone/tablet.
2. Navigate to:
   ```
   http://<YOUR_PC_IP>:28443
   ```
3. Tap **Connect** and switch to Fullscreen for drawing.

---

## 🖌️ Drawing in Krita / Photoshop

1. Open **Krita** -> **Settings** -> **Configure Krita** -> **Tablet Settings**.
2. Select **Windows 8+ Pointer Input (Windows Ink)**.
3. Test your pen:
   - **Hovering**: Moving the pen tip 1-2 cm above the surface will move the cursor and show the brush outline without drawing.
   - **Light Press**: Produces ultra-delicate hairlines.
   - **Firm Press**: Dynamically scales up to maximum stroke width.

---

## 🛠️ Building from Source

### Windows Host (MinGW-w64 GCC)
```powershell
cd server
mingw32-make
```
Dependencies: `d3d11`, `dxgi`, `mfplat`, `mfuuid`, `wmcodecdspuuid`, `user32`, `ws2_32`.

### Android Client (Gradle)
```powershell
cd android
./gradlew assembleRelease
```

---

## 📜 License
MIT License. Feel free to use, modify, and distribute.
