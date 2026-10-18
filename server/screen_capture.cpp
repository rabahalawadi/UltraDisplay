#include "screen_capture.h"
#include <stdio.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

ScreenCapture::ScreenCapture()
    : m_pDevice(nullptr)
    , m_pContext(nullptr)
    , m_pDuplication(nullptr)
    , m_pStagingTexture(nullptr)
    , m_width(0)
    , m_height(0)
    , m_hasAcquiredFrame(false)
{
}

ScreenCapture::~ScreenCapture() {
    Shutdown();
}

bool ScreenCapture::Initialize(int displayIndex) {
    Shutdown();

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_pDevice,
        &featureLevel,
        &m_pContext
    );

    if (FAILED(hr)) {
        printf("[ScreenCapture] D3D11CreateDevice failed: 0x%08lX\n", hr);
        return false;
    }

    IDXGIDevice* pDXGIDevice = nullptr;
    hr = m_pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
    if (FAILED(hr)) return false;

    IDXGIAdapter* pDXGIAdapter = nullptr;
    hr = pDXGIDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&pDXGIAdapter);
    pDXGIDevice->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput* pDXGIOutput = nullptr;
    hr = pDXGIAdapter->EnumOutputs(displayIndex, &pDXGIOutput);
    pDXGIAdapter->Release();
    if (FAILED(hr)) {
        printf("[ScreenCapture] EnumOutputs(%d) failed: 0x%08lX\n", displayIndex, hr);
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc;
    pDXGIOutput->GetDesc(&outputDesc);
    m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    IDXGIOutput1* pDXGIOutput1 = nullptr;
    hr = pDXGIOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pDXGIOutput1);
    pDXGIOutput->Release();
    if (FAILED(hr)) return false;

    hr = pDXGIOutput1->DuplicateOutput(m_pDevice, &m_pDuplication);
    pDXGIOutput1->Release();

    if (FAILED(hr)) {
        printf("[ScreenCapture] DuplicateOutput failed: 0x%08lX (0x887A0004 = Access Denied / UAC / Lockscreen)\n", hr);
        return false;
    }

    printf("[ScreenCapture] Desktop Duplication initialized: %dx%d (Monitor %d)\n", m_width, m_height, displayIndex);
    return true;
}

void ScreenCapture::Shutdown() {
    ReleaseFrame();

    if (m_pStagingTexture) {
        m_pStagingTexture->Release();
        m_pStagingTexture = nullptr;
    }
    if (m_pDuplication) {
        m_pDuplication->Release();
        m_pDuplication = nullptr;
    }
    if (m_pContext) {
        m_pContext->Release();
        m_pContext = nullptr;
    }
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    m_width = 0;
    m_height = 0;
}

bool ScreenCapture::AcquireFrame(ID3D11Texture2D** ppTextureOut, int timeoutMs) {
    if (!m_pDuplication) return false;

    ReleaseFrame();

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* pDesktopResource = nullptr;

    HRESULT hr = m_pDuplication->AcquireNextFrame(timeoutMs, &frameInfo, &pDesktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // No new frame rendered within timeout
    }

    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            printf("[ScreenCapture] Desktop Duplication access lost (mode change/UAC). Reinitializing...\n");
            Initialize(0);
        }
        return false;
    }

    m_hasAcquiredFrame = true;

    hr = pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)ppTextureOut);
    pDesktopResource->Release();

    return SUCCEEDED(hr);
}

void ScreenCapture::ReleaseFrame() {
    if (m_hasAcquiredFrame && m_pDuplication) {
        m_pDuplication->ReleaseFrame();
        m_hasAcquiredFrame = false;
    }
}
