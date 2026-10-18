#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include "../protocol/protocol.h"

class PenInjector {
public:
    PenInjector();
    ~PenInjector();

    bool Initialize(int targetMonitorLeft = 0, int targetMonitorTop = 0, int targetWidth = 1920, int targetHeight = 1080);
    void Shutdown();

    // Injects high-precision S-Pen events (pressure, tilt, hover, barrel button)
    bool InjectSPen(const UDisSPenEvent& ev);

    // Injects standard multitouch events (fingers)
    bool InjectTouch(const UDisTouchEvent& ev);

    void UpdateTargetBounds(int left, int top, int width, int height);

private:
    HANDLE m_hSyntheticPen;
    bool m_touchInitialized;
    int m_monitorLeft;
    int m_monitorTop;
    int m_monitorWidth;
    int m_monitorHeight;

    bool m_wasDown;
    bool m_wasInRange;
    bool m_wasBarrel;

    // Helper to calculate target pixel coordinates
    POINT MapCoordinates(float normX, float normY);

    // Hardware-accurate absolute mouse injector for multi-monitor desktop
    void SendMouseAbsolute(int x, int y, DWORD mouseFlags);
};
