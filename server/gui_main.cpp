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

enum ConnectMode {
    MODE_WIFI = 0,
    MODE_CABLE = 1
};

struct AppState {
    UltraDisplayServer server;
    UsbManager usb;
    ConnectMode currentMode = MODE_WIFI;
    std::vector<std::pair<std::string, std::string>> ipList;
    std::string primaryIp = "127.0.0.1";
    int currentPort = 28443;
    bool isServerRunning = false;
    bool isTray = false;
    NOTIFYICONDATAA nid = {0};
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

static void DrawRoundedRect(Graphics& g, Brush* brush, Pen* pen, RectF rect, float radius) {
    GraphicsPath path;
    path.AddArc(rect.X, rect.Y, radius * 2, radius * 2, 180, 90);
    path.AddArc(rect.X + rect.Width - radius * 2, rect.Y, radius * 2, radius * 2, 270, 90);
    path.AddArc(rect.X + rect.Width - radius * 2, rect.Y + rect.Height - radius * 2, radius * 2, radius * 2, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - radius * 2, radius * 2, radius * 2, 90, 90);
    path.CloseFigure();

    if (brush) g.FillPath(brush, &path);
    if (pen) g.DrawPath(pen, &path);
}

static void RenderUI(HWND hWnd, HDC hdc) {
    RECT cr;
    GetClientRect(hWnd, &cr);
    int width = cr.right - cr.left;
    int height = cr.bottom - cr.top;

    Bitmap backBuffer(width, height);
    Graphics g(&backBuffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Dark Background (#121214)
    SolidBrush bgBrush(Color(255, 18, 18, 20));
    g.FillRectangle(&bgBrush, 0, 0, width, height);

    FontFamily fontFamily(L"Segoe UI");
    Font fontTitle(&fontFamily, 16, FontStyleBold, UnitPixel);
    Font fontHeader(&fontFamily, 12, FontStyleBold, UnitPixel);
    Font fontBody(&fontFamily, 11, FontStyleRegular, UnitPixel);
    Font fontMono(&fontFamily, 11, FontStyleBold, UnitPixel);
    Font fontSmall(&fontFamily, 10, FontStyleRegular, UnitPixel);

    SolidBrush textWhite(Color(255, 255, 255, 255));
    SolidBrush textDim(Color(255, 150, 150, 160));
    SolidBrush textGreen(Color(255, 52, 211, 153));
    SolidBrush textBlue(Color(255, 56, 189, 248));
    SolidBrush textAmber(Color(255, 251, 191, 36));

    // 1. Header Banner
    g.DrawString(L"UltraDisplay Server", -1, &fontTitle, PointF(24, 20), &textWhite);
    g.DrawString(L"High-Precision Windows Ink Tablet Host", -1, &fontSmall, PointF(24, 44), &textDim);

    // Status Pill
    bool connected = g_app.server.IsClientConnected();
    bool running = g_app.server.IsRunning();

    RectF statusPill(width - 150, 22, 126, 26);
    SolidBrush pillBg(connected ? Color(40, 52, 211, 153) : (running ? Color(40, 56, 189, 248) : Color(40, 239, 68, 68)));
    Pen pillBorder(connected ? Color(255, 52, 211, 153) : (running ? Color(255, 56, 189, 248) : Color(255, 239, 68, 68)), 1.0f);
    DrawRoundedRect(g, &pillBg, &pillBorder, statusPill, 12.0f);

    std::wstring statusStr = connected ? L"● STREAMING" : (running ? L"● LISTENING" : L"○ STOPPED");
    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(statusStr.c_str(), -1, &fontSmall, statusPill, &sfCenter, connected ? &textGreen : (running ? &textBlue : &textDim));

    // 2. Mode Selector: [ 🌐 Wi-Fi ]  [ ⚡ USB Cable ]
    int modeY = 80;
    int tabW = (width - 48 - 12) / 2;

    RectF wifiTab(24, modeY, tabW, 40);
    RectF cableTab(24 + tabW + 12, modeY, tabW, 40);

    bool isWifi = (g_app.currentMode == MODE_WIFI);

    SolidBrush activeTabBg(Color(255, 30, 41, 59));
    SolidBrush inactiveTabBg(Color(255, 24, 24, 27));
    Pen activeTabBorder(Color(255, 56, 189, 248), 1.5f);
    Pen inactiveTabBorder(Color(255, 45, 45, 50), 1.0f);

    DrawRoundedRect(g, isWifi ? &activeTabBg : &inactiveTabBg, isWifi ? &activeTabBorder : &inactiveTabBorder, wifiTab, 8.0f);
    DrawRoundedRect(g, !isWifi ? &activeTabBg : &inactiveTabBg, !isWifi ? &activeTabBorder : &inactiveTabBorder, cableTab, 8.0f);

    g.DrawString(L"🌐 Wi-Fi Mode", -1, &fontHeader, wifiTab, &sfCenter, isWifi ? &textWhite : &textDim);
    g.DrawString(L"⚡ USB Cable Mode", -1, &fontHeader, cableTab, &sfCenter, !isWifi ? &textWhite : &textDim);

    // 3. Mode Content Card
    int cardY = 132;
    RectF modeCard(24, cardY, width - 48, 140);
    SolidBrush cardBg(Color(255, 24, 24, 27));
    Pen cardBorder(Color(255, 39, 39, 42), 1.0f);
    DrawRoundedRect(g, &cardBg, &cardBorder, modeCard, 10.0f);

    if (isWifi) {
        g.DrawString(L"Wi-Fi Connection Details", -1, &fontHeader, PointF(40, cardY + 16), &textWhite);
        g.DrawString(L"Connect your Android device to the same local network.", -1, &fontSmall, PointF(40, cardY + 38), &textDim);

        // IP Box
        RectF ipBox(40, cardY + 62, width - 170, 34);
        SolidBrush ipBoxBg(Color(255, 15, 15, 18));
        Pen ipBoxBorder(Color(255, 60, 60, 70), 1.0f);
        DrawRoundedRect(g, &ipBoxBg, &ipBoxBorder, ipBox, 6.0f);

        std::string ipLabel = g_app.primaryIp + ":" + std::to_string(g_app.currentPort);
        std::wstring wIp(ipLabel.begin(), ipLabel.end());
        StringFormat sfLeft;
        sfLeft.SetLineAlignment(StringAlignmentCenter);
        RectF ipTextRect(52, cardY + 62, width - 182, 34);
        g.DrawString(wIp.c_str(), -1, &fontMono, ipTextRect, &sfLeft, &textGreen);

        // Copy Button Box
        RectF copyBtn(width - 120, cardY + 62, 96, 34);
        SolidBrush copyBg(Color(255, 37, 99, 235));
        DrawRoundedRect(g, &copyBg, nullptr, copyBtn, 6.0f);
        g.DrawString(L"Copy IP", -1, &fontHeader, copyBtn, &sfCenter, &textWhite);

        g.DrawString(L"Auto-Discovery: Active (Android app will find this PC automatically)", -1, &fontSmall, PointF(40, cardY + 108), &textDim);
    } else {
        bool usbPlugged = g_app.usb.IsDeviceConnected();
        std::string model = g_app.usb.GetConnectedDeviceModel();

        g.DrawString(L"USB Cable (Zero Latency Bridge)", -1, &fontHeader, PointF(40, cardY + 16), &textWhite);
        g.DrawString(L"High-bandwidth direct USB tunnel via reverse ADB (0ms network jitter).", -1, &fontSmall, PointF(40, cardY + 38), &textDim);

        RectF usbBox(40, cardY + 62, width - 80, 36);
        SolidBrush usbBoxBg(usbPlugged ? Color(40, 52, 211, 153) : Color(40, 251, 191, 36));
        Pen usbBoxBorder(usbPlugged ? Color(255, 52, 211, 153) : Color(255, 251, 191, 36), 1.0f);
        DrawRoundedRect(g, &usbBoxBg, &usbBoxBorder, usbBox, 6.0f);

        std::wstring usbStatusStr = usbPlugged 
            ? (L"✔ Cable Ready: " + std::wstring(model.begin(), model.end()) + L" (Port Forwarded: 28443)")
            : L"⏳ Plug phone into PC via USB-C (ensure USB debugging is on)";
        StringFormat sfUsb;
        sfUsb.SetLineAlignment(StringAlignmentCenter);
        RectF usbTextRect(52, cardY + 62, width - 92, 36);
        g.DrawString(usbStatusStr.c_str(), -1, &fontHeader, usbTextRect, &sfUsb, usbPlugged ? &textGreen : &textAmber);

        g.DrawString(L"In UltraDisplay on Android, tap 'USB Cable' to connect with 1 click.", -1, &fontSmall, PointF(40, cardY + 108), &textDim);
    }

    // 4. Live Session Telemetry Card
    int teleY = 286;
    RectF teleCard(24, teleY, width - 48, 180);
    DrawRoundedRect(g, &cardBg, &cardBorder, teleCard, 10.0f);

    g.DrawString(L"Live Session Telemetry", -1, &fontHeader, PointF(40, teleY + 16), &textWhite);

    std::string devName = g_app.server.GetClientDeviceName();
    if (devName.empty()) devName = "Waiting for device connection...";
    std::wstring wDev(devName.begin(), devName.end());

    int fps = g_app.server.GetCurrentFps();
    int bitrate = g_app.server.GetCurrentBitrate() / 1000000;
    uint64_t penPackets = g_app.server.GetTotalPenEvents();
    bool winInk = g_app.server.IsWindowsInkActive();

    // 4 Telemetry Metrics in a 2x2 Grid
    int colW = (width - 48 - 32) / 2;

    // Row 1
    g.DrawString(L"Connected Client:", -1, &fontSmall, PointF(40, teleY + 44), &textDim);
    g.DrawString(wDev.c_str(), -1, &fontHeader, PointF(40, teleY + 60), &textWhite);

    g.DrawString(L"Windows Ink Stylus:", -1, &fontSmall, PointF(40 + colW, teleY + 44), &textDim);
    g.DrawString(winInk ? L"✔ Active & Injected (Pure Stylus)" : L"⚠ Restricted", -1, &fontHeader, PointF(40 + colW, teleY + 60), winInk ? &textGreen : &textAmber);

    // Row 2
    g.DrawString(L"Video Stream Performance:", -1, &fontSmall, PointF(40, teleY + 95), &textDim);
    std::wstring streamInfo = std::to_wstring(fps) + L" FPS @ " + std::to_wstring(bitrate) + L" Mbps (GPU H.264)";
    g.DrawString(connected ? streamInfo.c_str() : L"Idle (Standby)", -1, &fontHeader, PointF(40, teleY + 111), connected ? &textBlue : &textDim);

    g.DrawString(L"Stylus Packets Injected:", -1, &fontSmall, PointF(40 + colW, teleY + 95), &textDim);
    std::wstring packetsStr = std::to_wstring(penPackets) + L" packets";
    g.DrawString(packetsStr.c_str(), -1, &fontMono, PointF(40 + colW, teleY + 111), &textWhite);

    // Drawing App Info
    g.DrawString(L"Compatibility: Krita, Photoshop, Clip Studio, Illustrator, OneNote (Windows Ink)", -1, &fontSmall, PointF(40, teleY + 148), &textDim);

    // 5. Action Buttons at Bottom
    int btnY = 484;
    int btnW = (width - 48 - 24) / 3;

    RectF btnServer(24, btnY, btnW, 44);
    RectF btnRestart(24 + btnW + 12, btnY, btnW, 44);
    RectF btnTray(24 + (btnW + 12) * 2, btnY, btnW, 44);

    SolidBrush btnStopBg(Color(255, 220, 38, 38));
    SolidBrush btnStartBg(Color(255, 16, 185, 129));
    SolidBrush btnDarkBg(Color(255, 39, 39, 42));

    DrawRoundedRect(g, running ? &btnStopBg : &btnStartBg, nullptr, btnServer, 8.0f);
    DrawRoundedRect(g, &btnDarkBg, nullptr, btnRestart, 8.0f);
    DrawRoundedRect(g, &btnDarkBg, nullptr, btnTray, 8.0f);

    g.DrawString(running ? L"Stop Server" : L"Start Server", -1, &fontHeader, btnServer, &sfCenter, &textWhite);
    g.DrawString(L"Restart Engine", -1, &fontHeader, btnRestart, &sfCenter, &textWhite);
    g.DrawString(L"Hide to Tray", -1, &fontHeader, btnTray, &sfCenter, &textWhite);

    // Render backBuffer to window HDC
    Graphics windowGraphics(hdc);
    windowGraphics.DrawImage(&backBuffer, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Enable Immersive Dark Mode for Windows 10/11 Titlebar
        BOOL useDarkMode = TRUE;
        DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

        RefreshLocalIps();

        // Setup System Tray Icon
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

        // Setup 250ms Telemetry Timer for smooth live stats
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
        int tabW = (width - 48 - 12) / 2;

        // Mode switch tabs
        if (y >= 80 && y <= 120) {
            if (x >= 24 && x <= 24 + tabW) {
                g_app.currentMode = MODE_WIFI;
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (x >= 24 + tabW + 12 && x <= 24 + tabW * 2 + 12) {
                g_app.currentMode = MODE_CABLE;
                g_app.usb.ForceReversePort(g_app.currentPort);
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }

        // Copy IP Button
        if (g_app.currentMode == MODE_WIFI && y >= 194 && y <= 228) {
            if (x >= width - 120 && x <= width - 24) {
                CopyToClipboard(hWnd, g_app.primaryIp);
                MessageBoxA(hWnd, ("Copied " + g_app.primaryIp + " to clipboard!").c_str(), "UltraDisplay", MB_OK | MB_ICONINFORMATION);
            }
        }

        // Bottom Action Buttons
        int btnY = 484;
        int btnW = (width - 48 - 24) / 3;
        if (y >= btnY && y <= btnY + 44) {
            // Button 1: Start/Stop
            if (x >= 24 && x <= 24 + btnW) {
                if (g_app.server.IsRunning()) {
                    g_app.server.Stop();
                } else {
                    g_app.server.Start(g_app.currentPort);
                }
                InvalidateRect(hWnd, NULL, FALSE);
            }
            // Button 2: Restart
            else if (x >= 24 + btnW + 12 && x <= 24 + btnW * 2 + 12) {
                g_app.server.Stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                g_app.server.Start(g_app.currentPort);
                g_app.usb.ForceReversePort(g_app.currentPort);
                InvalidateRect(hWnd, NULL, FALSE);
            }
            // Button 3: Minimize to Tray
            else if (x >= 24 + (btnW + 12) * 2 && x <= 24 + (btnW + 12) * 2 + btnW) {
                ShowWindow(hWnd, SW_HIDE);
                g_app.isTray = true;
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
        // Hide to tray on close to maintain background drawing sessions
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
    // Initialize GDI+
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

    int winWidth = 540;
    int winHeight = 560;
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
