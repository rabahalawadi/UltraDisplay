#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <d3d11.h>
#include <vector>
#include <stdint.h>

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool Initialize(ID3D11Device* pDevice, int width, int height, int fps = 60, int bitrateBps = 20000000);
    void Shutdown();

    // Encodes a D3D11 texture into an Annex-B H.264 bitstream (NAL units)
    bool EncodeFrame(ID3D11Texture2D* pTexture, uint64_t timestampUs, std::vector<uint8_t>& outNalData, bool& isKeyframe);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    int GetFps() const { return m_fps; }

private:
    IMFTransform*           m_pTransform;
    IMFDXGIDeviceManager*   m_pDeviceManager;
    UINT                    m_resetToken;
    DWORD                   m_inputStreamId;
    DWORD                   m_outputStreamId;

    int m_width;
    int m_height;
    int m_fps;
    uint32_t m_frameIndex;

    bool SetupMediaTypes(int bitrateBps);
};
