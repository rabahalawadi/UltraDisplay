#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include "pen_injector.h"
#include <stdio.h>
#include <math.h>

// Dynamic User32 function prototypes for Synthetic Pointer and Touch Injection
typedef HANDLE (WINAPI *pfnCreateSyntheticPointerDevice)(POINTER_INPUT_TYPE pointerType, ULONG maxCount, POINTER_FEEDBACK_MODE mode);
typedef BOOL   (WINAPI *pfnDestroySyntheticPointerDevice)(HANDLE device);
typedef BOOL   (WINAPI *pfnInjectSyntheticPointerInput)(HANDLE device, CONST POINTER_TYPE_INFO* pointerInfo, UINT32 count);
typedef BOOL   (WINAPI *pfnInitializeTouchInjection)(UINT32 maxCount, DWORD dwMode);
typedef BOOL   (WINAPI *pfnInjectTouchInput)(UINT32 count, CONST POINTER_TOUCH_INFO* contacts);

static pfnCreateSyntheticPointerDevice s_CreateSyntheticPointerDevice = nullptr;
static pfnDestroySyntheticPointerDevice s_DestroySyntheticPointerDevice = nullptr;
static pfnInjectSyntheticPointerInput s_InjectSyntheticPointerInput = nullptr;
static pfnInitializeTouchInjection s_InitializeTouchInjection = nullptr;
static pfnInjectTouchInput s_InjectTouchInput = nullptr;

PenInjector::PenInjector()
    : m_hSyntheticPen(nullptr)
    , m_touchInitialized(false)
    , m_monitorLeft(0)
    , m_monitorTop(0)
    , m_monitorWidth(1920)
    , m_monitorHeight(1080)
    , m_wasDown(false)
    , m_wasInRange(false)
    , m_wasBarrel(false)
{
}

PenInjector::~PenInjector() {
    Shutdown();
}

bool PenInjector::Initialize(int targetMonitorLeft, int targetMonitorTop, int targetWidth, int targetHeight) {
    UpdateTargetBounds(targetMonitorLeft, targetMonitorTop, targetWidth, targetHeight);

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) hUser32 = LoadLibraryW(L"user32.dll");

    if (hUser32) {
        s_CreateSyntheticPointerDevice = (pfnCreateSyntheticPointerDevice)GetProcAddress(hUser32, "CreateSyntheticPointerDevice");
        s_DestroySyntheticPointerDevice = (pfnDestroySyntheticPointerDevice)GetProcAddress(hUser32, "DestroySyntheticPointerDevice");
        s_InjectSyntheticPointerInput = (pfnInjectSyntheticPointerInput)GetProcAddress(hUser32, "InjectSyntheticPointerInput");
        s_InitializeTouchInjection = (pfnInitializeTouchInjection)GetProcAddress(hUser32, "InitializeTouchInjection");
        s_InjectTouchInput = (pfnInjectTouchInput)GetProcAddress(hUser32, "InjectTouchInput");
    }

    if (s_CreateSyntheticPointerDevice) {
        m_hSyntheticPen = s_CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_INDIRECT);
        if (!m_hSyntheticPen) {
            m_hSyntheticPen = s_CreateSyntheticPointerDevice(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
        }

        if (m_hSyntheticPen) {
            printf("[PenInjector] Windows Ink Synthetic Pen Device initialized (Handle: %p)\n", m_hSyntheticPen);
        } else {
            printf("[PenInjector] CreateSyntheticPointerDevice: 0x%lu (Fallback SendInput/mouse_event active)\n", GetLastError());
        }
    }

    if (s_InitializeTouchInjection) {
        if (s_InitializeTouchInjection(10, 1 /* TOUCH_FEEDBACK_DEFAULT */)) {
            m_touchInitialized = true;
            printf("[PenInjector] Touch Injection initialized (10 contacts supported)\n");
        }
    }

    return true;
}

void PenInjector::Shutdown() {
    if (m_hSyntheticPen && s_DestroySyntheticPointerDevice) {
        s_DestroySyntheticPointerDevice(m_hSyntheticPen);
        m_hSyntheticPen = nullptr;
    }
    m_touchInitialized = false;
}

void PenInjector::UpdateTargetBounds(int left, int top, int width, int height) {
    m_monitorLeft = left;
    m_monitorTop = top;
    m_monitorWidth = (width > 0) ? width : 1920;
    m_monitorHeight = (height > 0) ? height : 1080;
    printf("[PenInjector] Monitor bounds set: (%d, %d) - %dx%d\n", m_monitorLeft, m_monitorTop, m_monitorWidth, m_monitorHeight);
}

POINT PenInjector::MapCoordinates(float normX, float normY) {
    POINT pt;
    if (normX < 0.0f) normX = 0.0f;
    if (normX > 1.0f) normX = 1.0f;
    if (normY < 0.0f) normY = 0.0f;
    if (normY > 1.0f) normY = 1.0f;

    int spanW = (m_monitorWidth > 1) ? (m_monitorWidth - 1) : 1;
    int spanH = (m_monitorHeight > 1) ? (m_monitorHeight - 1) : 1;

    pt.x = m_monitorLeft + (LONG)(normX * spanW);
    pt.y = m_monitorTop + (LONG)(normY * spanH);
    return pt;
}

void PenInjector::SendMouseAbsolute(int x, int y, DWORD mouseFlags) {
    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vWidth <= 0) vWidth = 1920;
    if (vHeight <= 0) vHeight = 1080;

    int spanW = (vWidth > 1) ? (vWidth - 1) : 1;
    int spanH = (vHeight > 1) ? (vHeight - 1) : 1;

    DWORD dx = (DWORD)(((double)(x - vLeft) * 65535.0) / spanW);
    DWORD dy = (DWORD)(((double)(y - vTop) * 65535.0) / spanH);

    // 1. Direct hardware cursor placement
    SetCursorPos(x, y);

    // 2. High-precision SendInput absolute movement
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | mouseFlags;
    SendInput(1, &input, sizeof(INPUT));

    // 3. Low-level mouse_event fallback (ensures cross-session/UIPI desktop events are delivered)
    mouse_event(MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | mouseFlags, dx, dy, 0, 0);
}

bool PenInjector::InjectSPen(const UDisSPenEvent& ev) {
    POINT pt = MapCoordinates(ev.normX, ev.normY);

    bool isDown = (ev.flags & SPEN_FLAG_INCONTACT) != 0;
    bool isInRange = (ev.flags & SPEN_FLAG_INRANGE) != 0;
    bool barrel = (ev.flags & SPEN_FLAG_BARREL) != 0;

    // 1. In Hover state (!isDown): Always move cursor without clicking
    if (!isDown) {
        if (m_wasDown) {
            // Lifted pen tip off tablet: ensure mouse button release is posted
            SendMouseAbsolute(pt.x, pt.y, MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTUP);
            m_wasDown = false;
        } else {
            // Gliding in air: smooth cursor hover without clicking
            SendMouseAbsolute(pt.x, pt.y, MOUSEEVENTF_MOVE);
        }
    }

    // 2. Windows Ink Synthetic Pen Injection (High-precision pressure, tilt & Ink messages)
    if (m_hSyntheticPen && s_InjectSyntheticPointerInput) {
        POINTER_TYPE_INFO pti = {0};
        pti.type = PT_PEN;

        POINTER_INFO& pi = pti.penInfo.pointerInfo;
        pi.pointerType = PT_PEN;
        pi.pointerId = 0;
        pi.ptPixelLocation = pt;
        pi.ptPixelLocationRaw = pt;
        pi.ptHimetricLocation.x = (LONG)((pt.x * 2540) / 96);
        pi.ptHimetricLocation.y = (LONG)((pt.y * 2540) / 96);
        pi.ptHimetricLocationRaw = pi.ptHimetricLocation;

        UINT32 flags = 0;
        if (isInRange || isDown) {
            flags |= POINTER_FLAG_INRANGE;
        }

        if (isDown) {
            flags |= POINTER_FLAG_INCONTACT;
            if (!m_wasDown) {
                flags |= POINTER_FLAG_DOWN;
            } else {
                flags |= POINTER_FLAG_UPDATE;
            }
        } else {
            if (m_wasDown) {
                flags |= POINTER_FLAG_UP;
            } else if (!m_wasInRange) {
                // First enter into range: POINTER_FLAG_INRANGE only!
            } else {
                // Gliding in hover while already in range
                flags |= POINTER_FLAG_UPDATE;
            }
        }

        if (barrel) {
            flags |= POINTER_FLAG_SECONDBUTTON;
            pti.penInfo.penFlags |= PEN_FLAG_BARREL;
        }

        if (ev.flags & SPEN_FLAG_ERASER) {
            pti.penInfo.penFlags |= PEN_FLAG_ERASER;
        }

        pi.pointerFlags = flags;
        pti.penInfo.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;

        // Windows Ink POINTER_PEN_INFO pressure scale is strictly 0..1024!
        if (isDown && ev.pressure > 0) {
            float normP = (float)ev.pressure / 4095.0f;
            if (normP < 0.0f) normP = 0.0f;
            if (normP > 1.0f) normP = 1.0f;

            // Natural Cintiq response curve: delicate hairline at low end, full control at high end
            float curveP = powf(normP, 1.4f);
            uint32_t winPressure = (uint32_t)(curveP * 1024.0f);
            if (winPressure < 1) winPressure = 1;
            if (winPressure > 1024) winPressure = 1024;
            pti.penInfo.pressure = winPressure;
        } else {
            pti.penInfo.pressure = 0;
        }

        pti.penInfo.tiltX = ev.tiltX;
        pti.penInfo.tiltY = ev.tiltY;
        pti.penInfo.rotation = ev.rotation;

        if (s_InjectSyntheticPointerInput(m_hSyntheticPen, &pti, 1)) {
            m_wasDown = isDown;
            m_wasInRange = isInRange;
            m_wasBarrel = barrel;
            return true;
        }
    }

    // 3. Fallback Mouse Injection for drag and contact
    DWORD mouseFlags = MOUSEEVENTF_MOVE;

    if (barrel && !m_wasBarrel) {
        mouseFlags |= MOUSEEVENTF_RIGHTDOWN;
    } else if (!barrel && m_wasBarrel) {
        mouseFlags |= MOUSEEVENTF_RIGHTUP;
    }

    if (isDown && !m_wasDown) {
        mouseFlags |= MOUSEEVENTF_LEFTDOWN;
    } else if (!isDown && m_wasDown) {
        mouseFlags |= MOUSEEVENTF_LEFTUP;
    }

    m_wasDown = isDown;
    m_wasInRange = isInRange;
    m_wasBarrel = barrel;

    SendMouseAbsolute(pt.x, pt.y, mouseFlags);
    return true;
}

bool PenInjector::InjectTouch(const UDisTouchEvent& ev) {
    if (!m_touchInitialized || !s_InjectTouchInput || ev.count == 0) {
        if (ev.count > 0) {
            POINT pt = MapCoordinates(ev.contacts[0].normX, ev.contacts[0].normY);
            DWORD mouseFlags = MOUSEEVENTF_MOVE;
            if (ev.contacts[0].state == 0) mouseFlags |= MOUSEEVENTF_LEFTDOWN;
            else if (ev.contacts[0].state == 2) mouseFlags |= MOUSEEVENTF_LEFTUP;
            SendMouseAbsolute(pt.x, pt.y, mouseFlags);
        }
        return true;
    }

    POINTER_TOUCH_INFO touches[10] = {0};
    UINT32 validCount = (ev.count <= 10) ? ev.count : 10;

    for (UINT32 i = 0; i < validCount; i++) {
        const auto& c = ev.contacts[i];
        POINT pt = MapCoordinates(c.normX, c.normY);

        touches[i].pointerInfo.pointerType = PT_TOUCH;
        touches[i].pointerInfo.pointerId = c.contactId;
        touches[i].pointerInfo.ptPixelLocation = pt;
        touches[i].pointerInfo.ptPixelLocationRaw = pt;

        if (c.state == 0) {
            touches[i].pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
        } else if (c.state == 1) {
            touches[i].pointerInfo.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
        } else {
            touches[i].pointerInfo.pointerFlags = POINTER_FLAG_UP;
        }

        touches[i].touchMask = TOUCH_MASK_CONTACTAREA;
        LONG radius = (LONG)(c.size > 0 ? c.size : 10);
        touches[i].rcContact.left = pt.x - radius;
        touches[i].rcContact.right = pt.x + radius;
        touches[i].rcContact.top = pt.y - radius;
        touches[i].rcContact.bottom = pt.y + radius;
    }

    return s_InjectTouchInput(validCount, touches);
}
