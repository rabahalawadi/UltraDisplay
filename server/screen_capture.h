#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool Initialize(int displayIndex = 0);
    void Shutdown();

    // Captures the next desktop frame with timeout (ms)
    // Returns D3D11 Texture containing BGRA8 desktop image in VRAM
    bool AcquireFrame(ID3D11Texture2D** ppTextureOut, int timeoutMs = 16);
    void ReleaseFrame();

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    ID3D11Device* GetDevice() const { return m_pDevice; }
    ID3D11DeviceContext* GetContext() const { return m_pContext; }

private:
    ID3D11Device*           m_pDevice;
    ID3D11DeviceContext*    m_pContext;
    IDXGIOutputDuplication* m_pDuplication;
    ID3D11Texture2D*        m_pStagingTexture;

    int m_width;
    int m_height;
    bool m_hasAcquiredFrame;
};
