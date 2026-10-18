#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <sstream>

#include "server_core.h"
#include "usb_manager.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

#define WM_TRAYICON (WM_USER + 101)
#define ID_TRAY_SHOW 2001
#define ID_TRAY_RESTART 2002
#define ID_TRAY_EXIT 2003

#define ID_TIMER_TELEMETRY 1001

enum MainTab {
    TAB_CONNECT = 0,
    TAB_SETTINGS = 1,
    TAB_ABOUT = 2
};

enum ConnectMode {
    MODE_WIFI = 0,
    MODE_CABLE = 1
};

struct AppState {
    UltraDisplayServer server;
    UsbManager usb;
    MainTab currentTab = TAB_CONNECT;
    ConnectMode currentMode = MODE_WIFI;
    std::vector<std::pair<std::string, std::string>> ipList;
    std::string primaryIp = "127.0.0.1";
    int currentPort = 28443;
    bool isServerRunning = false;
    bool isTray = false;
    NOTIFYICONDATAA nid = {0};

    // Settings
    int targetFps = 120;
    int targetBitrateMbps = 25;
};

static AppState g_app;

static void CopyToClipboard(HWND hWnd, const std::string& text) {
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.length() + 1);
        if (hMem) {
            char* p = (char*)GlobalLock(hMem);
            if (p) {
                memcpy(p, text.c_str(), text.length() + 1);
                GlobalUnlock(hMem);
                SetClipboardData(CF_TEXT, hMem);
            }
        }
        CloseClipboard();
    }
}

static void RefreshLocalIps() {
    g_app.ipList.clear();
    ULONG bufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(bufLen);
    }

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) continue;
            if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                sockaddr_in* sa_in = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(sa_in->sin_addr), ipStr, sizeof(ipStr));

                std::wstring wDesc(pCurr->FriendlyName);
                std::string desc(wDesc.begin(), wDesc.end());

                g_app.ipList.push_back({ ipStr, desc });
                if (g_app.primaryIp == "127.0.0.1") {
                    g_app.primaryIp = ipStr;
                }
            }
        }
    }
    if (pAddresses) free(pAddresses);
    if (g_app.ipList.empty()) {
        g_app.ipList.push_back({ "127.0.0.1", "Local Loopback" });
    }
}

// Crisp 90-degree sharp rectangular renderer (zero rounded edges)
static void DrawSharpRect(Graphics& g, Brush* brush, Pen* pen, const RectF& rect) {
    if (brush) g.FillRectangle(brush, rect);
    if (pen) g.DrawRectangle(pen, rect);
}

static void RenderUI(HWND hWnd, HDC hdc) {
    RECT cr;
    GetClientRect(hWnd, &cr);
    int width = cr.right - cr.left;
    int height = cr.bottom - cr.top;

    Bitmap backBuffer(width, height);
    Graphics g(&backBuffer);
    g.SetSmoothingMode(SmoothingModeNone); // Crisp pixel-aligned geometric edges
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Deep Obsidian Background (#121214)
    SolidBrush bgBrush(Color(255, 18, 18, 20));
    g.FillRectangle(&bgBrush, 0, 0, width, height);

    // Typography
    FontFamily fontFamily(L"Segoe UI");
    Font fontTitle(&fontFamily, 16, FontStyleBold, UnitPixel);
    Font fontSubtitle(&fontFamily, 11, FontStyleRegular, UnitPixel);
    Font fontTabActive(&fontFamily, 12, FontStyleBold, UnitPixel);
    Font fontTabInactive(&fontFamily, 12, FontStyleRegular, UnitPixel);
    Font fontSection(&fontFamily, 10, FontStyleBold, UnitPixel);
    Font fontCardTitle(&fontFamily, 13, FontStyleBold, UnitPixel);
    Font fontBody(&fontFamily, 11, FontStyleRegular, UnitPixel);
    Font fontBold(&fontFamily, 11, FontStyleBold, UnitPixel);
    Font fontMono(&fontFamily, 11, FontStyleBold, UnitPixel);
    Font fontPill(&fontFamily, 10, FontStyleBold, UnitPixel);
    Font fontSmall(&fontFamily, 9.5f, FontStyleRegular, UnitPixel);

    // Theme Brushes & Pens
    SolidBrush textWhite(Color(255, 255, 255, 255));
    SolidBrush textMuted(Color(255, 161, 161, 170));   // #A1A1AA
    SolidBrush textSection(Color(255, 113, 113, 122)); // #71717A
    SolidBrush textCyan(Color(255, 56, 189, 248));     // #38BDF8
    SolidBrush textGreen(Color(255, 52, 211, 153));    // #34D399
    SolidBrush textAmber(Color(255, 251, 191, 36));    // #FBBF24
    SolidBrush textRed(Color(255, 239, 68, 68));       // #EF4444

    SolidBrush headerBg(Color(255, 24, 24, 27));       // #18181B
    SolidBrush cardBg(Color(255, 30, 30, 36));         // #1E1E24
    SolidBrush innerBoxBg(Color(255, 39, 39, 42));     // #27272A
    SolidBrush activeBlue(Color(255, 37, 99, 235));    // #2563EB
    SolidBrush btnStart(Color(255, 16, 185, 129));     // #10B981
    SolidBrush btnStop(Color(255, 239, 68, 68));       // #EF4444

    Pen borderSubtle(Color(255, 46, 46, 56), 1.0f);    // #2E2E38
    Pen borderInner(Color(255, 63, 63, 70), 1.0f);     // #3F3F46
    Pen borderActiveBlue(Color(255, 59, 130, 246), 1.0f); // #3B82F6

    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);

    StringFormat sfLeft;
    sfLeft.SetAlignment(StringAlignmentNear);
    sfLeft.SetLineAlignment(StringAlignmentCenter);

    // ==========================================
    // 1. TOP HEADER BAR (#18181B)
    // ==========================================
    RectF headerRect(0, 0, (float)width, 68.0f);
    DrawSharpRect(g, &headerBg, &borderSubtle, headerRect);

    g.DrawString(L"UltraDisplay", -1, &fontTitle, PointF(24, 15), &textWhite);
    g.DrawString(L"High-Performance Virtual Tablet Display", -1, &fontSubtitle, PointF(24, 38), &textMuted);

    // Status Pill in Top Right
    bool connected = g_app.server.IsClientConnected();
    bool running = g_app.server.IsRunning();

    RectF statusPill(width - 134.0f, 20.0f, 110.0f, 26.0f);
    SolidBrush pillBg(Color(255, 39, 39, 42));
    Pen pillBorder(connected ? Color(255, 52, 211, 153) : (running ? Color(255, 56, 189, 248) : Color(255, 239, 68, 68)), 1.0f);
    DrawSharpRect(g, &pillBg, &pillBorder, statusPill);

    std::wstring statusStr = connected ? L"STREAMING" : (running ? L"STANDBY" : L"STOPPED");
    g.DrawString(statusStr.c_str(), -1, &fontPill, statusPill, &sfCenter, 
                 connected ? &textGreen : (running ? &textCyan : &textRed));

    // ==========================================
    // 2. SEGMENTED TAB NAVIGATION BAR: [ Connect ] [ Settings ] [ About Us ]
    // ==========================================
    RectF tabBarRect(0, 68.0f, (float)width, 54.0f);
    DrawSharpRect(g, &headerBg, &borderSubtle, tabBarRect);

    float tabW = (width - 48.0f - 16.0f) / 3.0f;
    RectF tab0(24.0f, 78.0f, tabW, 34.0f);
    RectF tab1(24.0f + tabW + 8.0f, 78.0f, tabW, 34.0f);
    RectF tab2(24.0f + (tabW + 8.0f) * 2.0f, 78.0f, tabW, 34.0f);

    // Draw Tab 0: Connect
    bool isTab0 = (g_app.currentTab == TAB_CONNECT);
    DrawSharpRect(g, isTab0 ? &activeBlue : &innerBoxBg, isTab0 ? &borderActiveBlue : &borderInner, tab0);
    g.DrawString(L"Connect", -1, isTab0 ? &fontTabActive : &fontTabInactive, tab0, &sfCenter, isTab0 ? &textWhite : &textMuted);

    // Draw Tab 1: Settings
    bool isTab1 = (g_app.currentTab == TAB_SETTINGS);
    DrawSharpRect(g, isTab1 ? &activeBlue : &innerBoxBg, isTab1 ? &borderActiveBlue : &borderInner, tab1);
    g.DrawString(L"Settings", -1, isTab1 ? &fontTabActive : &fontTabInactive, tab1, &sfCenter, isTab1 ? &textWhite : &textMuted);

    // Draw Tab 2: About Us
    bool isTab2 = (g_app.currentTab == TAB_ABOUT);
    DrawSharpRect(g, isTab2 ? &activeBlue : &innerBoxBg, isTab2 ? &borderActiveBlue : &borderInner, tab2);
    g.DrawString(L"About Us", -1, isTab2 ? &fontTabActive : &fontTabInactive, tab2, &sfCenter, isTab2 ? &textWhite : &textMuted);

    float contentY = 138.0f;
    float contentW = width - 48.0f;

    // ==========================================
    // TAB 0: CONNECT
    // ==========================================
    if (g_app.currentTab == TAB_CONNECT) {
        // Section: CONNECTION MODE
        g.DrawString(L"CONNECTION MODE", -1, &fontSection, PointF(24, contentY), &textSection);

        // Segmented Mode Buttons: [ Wi-Fi Mode ] [ USB Cable Mode ]
        float modeW = (contentW - 8.0f) / 2.0f;
        RectF wifiBtn(24.0f, contentY + 20.0f, modeW, 38.0f);
        RectF cableBtn(24.0f + modeW + 8.0f, contentY + 20.0f, modeW, 38.0f);

        bool isWifi = (g_app.currentMode == MODE_WIFI);
        DrawSharpRect(g, isWifi ? &activeBlue : &innerBoxBg, isWifi ? &borderActiveBlue : &borderInner, wifiBtn);
        DrawSharpRect(g, !isWifi ? &activeBlue : &innerBoxBg, !isWifi ? &borderActiveBlue : &borderInner, cableBtn);

        g.DrawString(L"Wi-Fi Mode", -1, isWifi ? &fontTabActive : &fontTabInactive, wifiBtn, &sfCenter, isWifi ? &textWhite : &textMuted);
        g.DrawString(L"USB Cable Mode", -1, !isWifi ? &fontTabActive : &fontTabInactive, cableBtn, &sfCenter, !isWifi ? &textWhite : &textMuted);

        // Connection Card (#1E1E24)
        float card1Y = contentY + 70.0f;
        RectF connCard(24.0f, card1Y, contentW, 134.0f);
        DrawSharpRect(g, &cardBg, &borderSubtle, connCard);

        if (isWifi) {
            g.DrawString(L"Mode: Wi-Fi (Local Network)", -1, &fontCardTitle, PointF(40, card1Y + 16), &textCyan);
            g.DrawString(L"HOST PC ADDRESS", -1, &fontSection, PointF(40, card1Y + 38), &textSection);

            // IP Display Box
            RectF ipBox(40.0f, card1Y + 54.0f, contentW - 130.0f, 36.0f);
            DrawSharpRect(g, &innerBoxBg, &borderInner, ipBox);

            std::string ipLabel = g_app.primaryIp + ":" + std::to_string(g_app.currentPort);
            std::wstring wIp(ipLabel.begin(), ipLabel.end());
            RectF ipTextRect(52.0f, card1Y + 54.0f, contentW - 146.0f, 36.0f);
            g.DrawString(wIp.c_str(), -1, &fontMono, ipTextRect, &sfLeft, &textGreen);

            // Copy IP Button
            RectF copyBtn(width - 138.0f, card1Y + 54.0f, 98.0f, 36.0f);
            DrawSharpRect(g, &activeBlue, &borderActiveBlue, copyBtn);
            g.DrawString(L"Copy IP", -1, &fontTabActive, copyBtn, &sfCenter, &textWhite);

            g.DrawString(L"Auto-Discovery: Active (Android app detects this PC automatically)", -1, &fontSmall, PointF(40, card1Y + 102), &textSection);
        } else {
            bool usbPlugged = g_app.usb.IsDeviceConnected();
            std::string model = g_app.usb.GetConnectedDeviceModel();

            g.DrawString(L"Mode: USB Cable (Direct Tunnel via 127.0.0.1)", -1, &fontCardTitle, PointF(40, card1Y + 16), &textCyan);
            g.DrawString(L"ADB REVERSE TUNNEL STATUS", -1, &fontSection, PointF(40, card1Y + 38), &textSection);

            RectF usbBox(40.0f, card1Y + 54.0f, contentW - 32.0f, 36.0f);
            DrawSharpRect(g, &innerBoxBg, &borderInner, usbBox);

            std::wstring usbStatusStr = usbPlugged 
                ? (L"Cable Ready: " + std::wstring(model.begin(), model.end()) + L" (Port Forwarded: 28443)")
                : L"Waiting for USB device (ensure USB debugging is on)";
            RectF usbTextRect(52.0f, card1Y + 54.0f, contentW - 52.0f, 36.0f);
            g.DrawString(usbStatusStr.c_str(), -1, &fontBold, usbTextRect, &sfLeft, usbPlugged ? &textGreen : &textAmber);

            g.DrawString(L"In UltraDisplay on Android, tap 'USB Cable Mode' to connect with 0ms latency.", -1, &fontSmall, PointF(40, card1Y + 102), &textSection);
        }

        // Live Session Telemetry Card (#1E1E24)
        float card2Y = card1Y + 146.0f;
        RectF teleCard(24.0f, card2Y, contentW, 164.0f);
        DrawSharpRect(g, &cardBg, &borderSubtle, teleCard);

        g.DrawString(L"LIVE SESSION TELEMETRY", -1, &fontSection, PointF(40, card2Y + 14), &textSection);

        std::string devName = g_app.server.GetClientDeviceName();
        if (devName.empty()) devName = "Waiting for client connection...";
        std::wstring wDev(devName.begin(), devName.end());

        int fps = g_app.server.GetCurrentFps();
        int bitrate = g_app.server.GetCurrentBitrate() / 1000000;
        uint64_t penPackets = g_app.server.GetTotalPenEvents();
        bool winInk = g_app.server.IsWindowsInkActive();

        float colW = (contentW - 32.0f) / 2.0f;

        // Metric 1: Client
        g.DrawString(L"Connected Client:", -1, &fontSmall, PointF(40, card2Y + 36), &textMuted);
        g.DrawString(wDev.c_str(), -1, &fontBold, PointF(40, card2Y + 52), &textWhite);

        // Metric 2: Windows Ink
        g.DrawString(L"Windows Ink Stylus:", -1, &fontSmall, PointF(40 + colW, card2Y + 36), &textMuted);
        g.DrawString(winInk ? L"Active & Injected (Pure Stylus)" : L"Standby", -1, &fontBold, PointF(40 + colW, card2Y + 52), winInk ? &textGreen : &textMuted);

        // Metric 3: Video Stream
        g.DrawString(L"Video Stream Performance:", -1, &fontSmall, PointF(40, card2Y + 84), &textMuted);
        std::wstring streamInfo = std::to_wstring(fps) + L" FPS @ " + std::to_wstring(bitrate) + L" Mbps (GPU H.264)";
        g.DrawString(connected ? streamInfo.c_str() : L"Idle (Standby)", -1, &fontBold, PointF(40, card2Y + 100), connected ? &textCyan : &textMuted);

        // Metric 4: Packets
        g.DrawString(L"Stylus Packets Injected:", -1, &fontSmall, PointF(40 + colW, card2Y + 84), &textMuted);
        std::wstring packetsStr = std::to_wstring(penPackets) + L" packets";
        g.DrawString(packetsStr.c_str(), -1, &fontMono, PointF(40 + colW, card2Y + 100), &textWhite);

        // Compatibility footer
        g.DrawString(L"Compatibility: Krita, Photoshop, Clip Studio, Illustrator, OneNote (Windows Ink)", -1, &fontSmall, PointF(40, card2Y + 136), &textSection);

        // Action Buttons at Bottom
        float btnY = card2Y + 176.0f;
        float btnW = (contentW - 16.0f) / 3.0f;

        RectF btnServer(24.0f, btnY, btnW, 42.0f);
        RectF btnRestart(24.0f + btnW + 8.0f, btnY, btnW, 42.0f);
        RectF btnTray(24.0f + (btnW + 8.0f) * 2.0f, btnY, btnW, 42.0f);

        DrawSharpRect(g, running ? &btnStop : &btnStart, nullptr, btnServer);
        DrawSharpRect(g, &innerBoxBg, &borderInner, btnRestart);
        DrawSharpRect(g, &innerBoxBg, &borderInner, btnTray);

        g.DrawString(running ? L"Stop Server" : L"Start Server", -1, &fontTabActive, btnServer, &sfCenter, &textWhite);
        g.DrawString(L"Restart Engine", -1, &fontTabActive, btnRestart, &sfCenter, &textWhite);
        g.DrawString(L"Hide to Tray", -1, &fontTabActive, btnTray, &sfCenter, &textWhite);
    }
    // ==========================================
    // TAB 1: SETTINGS
    // ==========================================
    else if (g_app.currentTab == TAB_SETTINGS) {
        g.DrawString(L"DISPLAY & STREAMING", -1, &fontSection, PointF(24, contentY), &textSection);

        // Card 1: Display & Streaming
        float sCard1Y = contentY + 20.0f;
        RectF sCard1(24.0f, sCard1Y, contentW, 140.0f);
        DrawSharpRect(g, &cardBg, &borderSubtle, sCard1);

        g.DrawString(L"TARGET FRAME RATE", -1, &fontSection, PointF(40, sCard1Y + 14), &textSection);
        float fpsBtnW = (contentW - 40.0f) / 2.0f;
        RectF fps120(40.0f, sCard1Y + 30.0f, fpsBtnW, 32.0f);
        RectF fps60(40.0f + fpsBtnW + 8.0f, sCard1Y + 30.0f, fpsBtnW, 32.0f);

        bool is120 = (g_app.targetFps == 120);
        DrawSharpRect(g, is120 ? &activeBlue : &innerBoxBg, is120 ? &borderActiveBlue : &borderInner, fps120);
        DrawSharpRect(g, !is120 ? &activeBlue : &innerBoxBg, !is120 ? &borderActiveBlue : &borderInner, fps60);
        g.DrawString(L"Ultra 120 FPS", -1, is120 ? &fontTabActive : &fontTabInactive, fps120, &sfCenter, is120 ? &textWhite : &textMuted);
        g.DrawString(L"Standard 60 FPS", -1, !is120 ? &fontTabActive : &fontTabInactive, fps60, &sfCenter, !is120 ? &textWhite : &textMuted);

        g.DrawString(L"HARDWARE ENCODER BITRATE", -1, &fontSection, PointF(40, sCard1Y + 74), &textSection);
        float bBtnW = (contentW - 48.0f) / 3.0f;
        RectF br25(40.0f, sCard1Y + 90.0f, bBtnW, 32.0f);
        RectF br15(40.0f + bBtnW + 8.0f, sCard1Y + 90.0f, bBtnW, 32.0f);
        RectF br8(40.0f + (bBtnW + 8.0f) * 2.0f, sCard1Y + 90.0f, bBtnW, 32.0f);

        DrawSharpRect(g, g_app.targetBitrateMbps == 25 ? &activeBlue : &innerBoxBg, g_app.targetBitrateMbps == 25 ? &borderActiveBlue : &borderInner, br25);
        DrawSharpRect(g, g_app.targetBitrateMbps == 15 ? &activeBlue : &innerBoxBg, g_app.targetBitrateMbps == 15 ? &borderActiveBlue : &borderInner, br15);
        DrawSharpRect(g, g_app.targetBitrateMbps == 8 ? &activeBlue : &innerBoxBg, g_app.targetBitrateMbps == 8 ? &borderActiveBlue : &borderInner, br8);

        g.DrawString(L"25 Mbps (Ultra)", -1, g_app.targetBitrateMbps == 25 ? &fontTabActive : &fontTabInactive, br25, &sfCenter, g_app.targetBitrateMbps == 25 ? &textWhite : &textMuted);
        g.DrawString(L"15 Mbps (High)", -1, g_app.targetBitrateMbps == 15 ? &fontTabActive : &fontTabInactive, br15, &sfCenter, g_app.targetBitrateMbps == 15 ? &textWhite : &textMuted);
        g.DrawString(L"8 Mbps (Balanced)", -1, g_app.targetBitrateMbps == 8 ? &fontTabActive : &fontTabInactive, br8, &sfCenter, g_app.targetBitrateMbps == 8 ? &textWhite : &textMuted);

        // Card 2: Stylus & Digitizer
        float sCard2Y = sCard1Y + 154.0f;
        g.DrawString(L"STYLUS & WINDOWS INK", -1, &fontSection, PointF(24, sCard2Y - 14), &textSection);

        RectF sCard2(24.0f, sCard2Y, contentW, 114.0f);
        DrawSharpRect(g, &cardBg, &borderSubtle, sCard2);

        g.DrawString(L"DIGITIZER INJECTION ENGINE", -1, &fontSection, PointF(40, sCard2Y + 14), &textSection);
        RectF engBox(40.0f, sCard2Y + 30.0f, contentW - 32.0f, 28.0f);
        DrawSharpRect(g, &innerBoxBg, &borderInner, engBox);
        RectF engText(52.0f, sCard2Y + 30.0f, contentW - 52.0f, 28.0f);
        g.DrawString(L"InjectSyntheticPointerInput (Native Windows Ink Indirect Tablet)", -1, &fontBold, engText, &sfLeft, &textGreen);

        g.DrawString(L"PRESSURE & TILT RESOLUTION", -1, &fontSection, PointF(40, sCard2Y + 68), &textSection);
        RectF resBox(40.0f, sCard2Y + 84.0f, contentW - 32.0f, 28.0f);
        DrawSharpRect(g, &innerBoxBg, &borderInner, resBox);
        RectF resText(52.0f, sCard2Y + 84.0f, contentW - 52.0f, 28.0f);
        g.DrawString(L"8192 Pressure Levels & Hardware Tilt X/Y (-90 to +90 deg)", -1, &fontBold, resText, &sfLeft, &textWhite);

        // Card 3: Host Environment
        float sCard3Y = sCard2Y + 128.0f;
        g.DrawString(L"HOST ENVIRONMENT & NETWORK", -1, &fontSection, PointF(24, sCard3Y - 14), &textSection);

        RectF sCard3(24.0f, sCard3Y, contentW, 80.0f);
        DrawSharpRect(g, &cardBg, &borderSubtle, sCard3);

        RectF netBox(40.0f, sCard3Y + 16.0f, contentW - 32.0f, 48.0f);
        DrawSharpRect(g, &innerBoxBg, &borderInner, netBox);
        RectF netText(52.0f, sCard3Y + 16.0f, contentW - 52.0f, 48.0f);
        g.DrawString(L"DXGI Desktop Duplication: Monitor 0 Primary Display\nTCP Stream Port: 28443  |  UDP Auto-Discovery: 28444  |  ADB: 28443", -1, &fontBold, netText, &sfLeft, &textMuted);
    }
    // ==========================================
    // TAB 2: ABOUT US
    // ==========================================
    else if (g_app.currentTab == TAB_ABOUT) {
        // Card 1: App Info
        RectF aCard1(24.0f, contentY, contentW, 206.0f);
        DrawSharpRect(g, &cardBg, &borderSubtle, aCard1);

        g.DrawString(L"UltraDisplay v2.0", -1, &fontTitle, PointF(40, contentY + 18), &textCyan);
        g.DrawString(L"Created by Rabah Alawadi", -1, &fontBold, PointF(40, contentY + 44), &textWhite);

        RectF descRect(40.0f, contentY + 70.0f, contentW - 32.0f, 64.0f);
        g.DrawString(
            L"UltraDisplay turns your Android phone or tablet into a high-precision, zero-latency graphics drawing display for Windows.\n\nOptimized for Krita, Photoshop, Clip Studio, Illustrator, and OneNote with native Windows Ink pressure and tilt.",
            -1, &fontBody, descRect, &sfLeft, &textMuted
        );

        // Clickable GitHub Button
        RectF gitBtn(40.0f, contentY + 148.0f, contentW - 32.0f, 38.0f);
        DrawSharpRect(g, &activeBlue, &borderActiveBlue, gitBtn);
        g.DrawString(L"GitHub: github.com/rabahalawadi/UltraDisplay", -1, &fontTabActive, gitBtn, &sfCenter, &textWhite);

        // Card 2: Key Technologies
        float aCard2Y = contentY + 220.0f;
        RectF aCard2(24.0f, aCard2Y, contentW, 140.0f);
        DrawSharpRect(g, &innerBoxBg, &borderInner, aCard2);

        g.DrawString(L"KEY TECHNOLOGIES", -1, &fontSection, PointF(40, aCard2Y + 14), &textSection);

        std::wstring techs = 
            L"• Pure Windows Ink Synthetic Pointer (InjectSyntheticPointerInput)\n"
            L"• DXGI Desktop Duplication & Hardware Media Foundation H.264\n"
            L"• High-precision 120 FPS / 25 Mbps streaming engine\n"
            L"• Native Huion & S-Pen OTG Digitizer support";

        RectF techRect(40.0f, aCard2Y + 34.0f, contentW - 32.0f, 96.0f);
        g.DrawString(techs.c_str(), -1, &fontBody, techRect, &sfLeft, &textMuted);
    }

    // Blit to screen
    Graphics windowGraphics(hdc);
    windowGraphics.DrawImage(&backBuffer, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        BOOL useDarkMode = TRUE;
        DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

        RefreshLocalIps();

        // System Tray Icon
        g_app.nid.cbSize = sizeof(NOTIFYICONDATAA);
        g_app.nid.hWnd = hWnd;
        g_app.nid.uID = 1;
        g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_app.nid.uCallbackMessage = WM_TRAYICON;
        g_app.nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        strcpy(g_app.nid.szTip, "UltraDisplay Server (Running)");
        Shell_NotifyIconA(NIM_ADD, &g_app.nid);

        // Start Server Engine
        g_app.server.Start(g_app.currentPort);
        g_app.isServerRunning = true;

        // Start USB Cable Manager
        g_app.usb.Initialize();
        g_app.usb.StartMonitoring(g_app.currentPort);

        // 250ms Telemetry Timer
        SetTimer(hWnd, ID_TIMER_TELEMETRY, 250, NULL);
        break;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER_TELEMETRY) {
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RenderUI(hWnd, hdc);
        EndPaint(hWnd, &ps);
        break;
    }

    case WM_ERASEBKGND:
        return 1; // Prevent flicker

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        RECT cr;
        GetClientRect(hWnd, &cr);
        int width = cr.right - cr.left;
        float contentW = width - 48.0f;

        // 1. Tab Bar Navigation (Y: 78 to 112)
        if (y >= 78 && y <= 112) {
            float tabW = (width - 48.0f - 16.0f) / 3.0f;
            if (x >= 24 && x <= 24 + tabW) {
                g_app.currentTab = TAB_CONNECT;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            } else if (x >= 24 + tabW + 8.0f && x <= 24 + (tabW + 8.0f) + tabW) {
                g_app.currentTab = TAB_SETTINGS;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            } else if (x >= 24 + (tabW + 8.0f) * 2.0f && x <= 24 + (tabW + 8.0f) * 2.0f + tabW) {
                g_app.currentTab = TAB_ABOUT;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
        }

        // 2. Tab 0: CONNECT clicks
        if (g_app.currentTab == TAB_CONNECT) {
            float contentY = 138.0f;

            // Mode Selector: Wi-Fi Mode vs USB Cable Mode (Y: contentY + 20 to contentY + 58)
            float modeW = (contentW - 8.0f) / 2.0f;
            if (y >= contentY + 20 && y <= contentY + 58) {
                if (x >= 24 && x <= 24 + modeW) {
                    g_app.currentMode = MODE_WIFI;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else if (x >= 24 + modeW + 8.0f && x <= 24 + modeW * 2.0f + 8.0f) {
                    g_app.currentMode = MODE_CABLE;
                    g_app.usb.ForceReversePort(g_app.currentPort);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }

            // Copy IP Button (if Wi-Fi mode)
            float card1Y = contentY + 70.0f;
            if (g_app.currentMode == MODE_WIFI && y >= card1Y + 54 && y <= card1Y + 90) {
                if (x >= width - 138 && x <= width - 40) {
                    CopyToClipboard(hWnd, g_app.primaryIp);
                    MessageBoxA(hWnd, ("Copied " + g_app.primaryIp + " to clipboard!").c_str(), "UltraDisplay", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
            }

            // Bottom Action Buttons (Start/Stop, Restart, Tray)
            float card2Y = card1Y + 146.0f;
            float btnY = card2Y + 176.0f;
            float btnW = (contentW - 16.0f) / 3.0f;

            if (y >= btnY && y <= btnY + 42) {
                // Button 1: Start/Stop
                if (x >= 24 && x <= 24 + btnW) {
                    if (g_app.server.IsRunning()) {
                        g_app.server.Stop();
                    } else {
                        g_app.server.Start(g_app.currentPort);
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                // Button 2: Restart
                else if (x >= 24 + btnW + 8.0f && x <= 24 + btnW * 2.0f + 8.0f) {
                    g_app.server.Stop();
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    g_app.server.Start(g_app.currentPort);
                    g_app.usb.ForceReversePort(g_app.currentPort);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                // Button 3: Hide to Tray
                else if (x >= 24 + (btnW + 8.0f) * 2.0f && x <= 24 + (btnW + 8.0f) * 2.0f + btnW) {
                    ShowWindow(hWnd, SW_HIDE);
                    g_app.isTray = true;
                    return 0;
                }
            }
        }
        // 3. Tab 1: SETTINGS clicks
        else if (g_app.currentTab == TAB_SETTINGS) {
            float contentY = 138.0f;
            float sCard1Y = contentY + 20.0f;

            // Target FPS (120 vs 60)
            if (y >= sCard1Y + 30 && y <= sCard1Y + 62) {
                float fpsBtnW = (contentW - 40.0f) / 2.0f;
                if (x >= 40 && x <= 40 + fpsBtnW) {
                    g_app.targetFps = 120;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else if (x >= 40 + fpsBtnW + 8.0f && x <= 40 + fpsBtnW * 2.0f + 8.0f) {
                    g_app.targetFps = 60;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }

            // Bitrate (25 vs 15 vs 8)
            if (y >= sCard1Y + 90 && y <= sCard1Y + 122) {
                float bBtnW = (contentW - 48.0f) / 3.0f;
                if (x >= 40 && x <= 40 + bBtnW) {
                    g_app.targetBitrateMbps = 25;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else if (x >= 40 + bBtnW + 8.0f && x <= 40 + (bBtnW + 8.0f) + bBtnW) {
                    g_app.targetBitrateMbps = 15;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else if (x >= 40 + (bBtnW + 8.0f) * 2.0f && x <= 40 + (bBtnW + 8.0f) * 2.0f + bBtnW) {
                    g_app.targetBitrateMbps = 8;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }
        }
        // 4. Tab 2: ABOUT US clicks
        else if (g_app.currentTab == TAB_ABOUT) {
            float contentY = 138.0f;
            // GitHub Button (Y: contentY + 148 to contentY + 186)
            if (y >= contentY + 148 && y <= contentY + 186) {
                if (x >= 40 && x <= 40 + contentW - 32.0f) {
                    ShellExecuteA(NULL, "open", "https://github.com/rabahalawadi/UltraDisplay", NULL, NULL, SW_SHOWNORMAL);
                    return 0;
                }
            }
        }
        break;
    }

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_SHOW, "Open UltraDisplay Dashboard");
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_RESTART, "Restart Server");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit Server");

            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);

            if (cmd == ID_TRAY_SHOW) {
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
                g_app.isTray = false;
            } else if (cmd == ID_TRAY_RESTART) {
                g_app.server.Stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                g_app.server.Start(g_app.currentPort);
                g_app.usb.ForceReversePort(g_app.currentPort);
            } else if (cmd == ID_TRAY_EXIT) {
                DestroyWindow(hWnd);
            }
        }
        break;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        g_app.isTray = true;
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconA(NIM_DELETE, &g_app.nid);
        KillTimer(hWnd, ID_TIMER_TELEMETRY);
        g_app.usb.StopMonitoring();
        g_app.server.Stop();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    const char CLASS_NAME[] = "UltraDisplayServerGUI";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = NULL;

    RegisterClassA(&wc);

    int winWidth = 580;
    int winHeight = 650;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winWidth) / 2;
    int posY = (screenH - winHeight) / 2;

    HWND hWnd = CreateWindowExA(
        0,
        CLASS_NAME,
        "UltraDisplay - Host Server & Control Dashboard",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winWidth, winHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}
