#include "encoder.h"
#include <stdio.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#ifndef MF_LOW_LATENCY
static const GUID MF_LOW_LATENCY = { 0x9c27891a, 0xed89, 0x405f, { 0x81, 0x24, 0x56, 0x98, 0xba, 0x05, 0x76, 0x97 } };
#endif

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

VideoEncoder::VideoEncoder()
    : m_pTransform(nullptr)
    , m_pDeviceManager(nullptr)
    , m_resetToken(0)
    , m_inputStreamId(0)
    , m_outputStreamId(0)
    , m_width(0)
    , m_height(0)
    , m_fps(60)
    , m_frameIndex(0)
{
}

VideoEncoder::~VideoEncoder() {
    Shutdown();
}

bool VideoEncoder::Initialize(ID3D11Device* pDevice, int width, int height, int fps, int bitrateBps) {
    Shutdown();
    MFStartup(MF_VERSION);

    m_width = width;
    m_height = height;
    m_fps = fps;

    // 1. Create DXGI Device Manager for hardware encoding
    HRESULT hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_pDeviceManager);
    if (FAILED(hr)) {
        printf("[Encoder] MFCreateDXGIDeviceManager failed: 0x%08lX\n", hr);
        return false;
    }

    hr = m_pDeviceManager->ResetDevice(pDevice, m_resetToken);
    if (FAILED(hr)) return false;

    // 2. Discover hardware H.264 Encoder MFT
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** ppActivates = nullptr;
    UINT32 count = 0;

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo,
        &outputInfo,
        &ppActivates,
        &count
    );

    if (FAILED(hr) || count == 0) {
        printf("[Encoder] Hardware H.264 encoder not found, checking software encoders...\n");
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputInfo,
            &outputInfo,
            &ppActivates,
            &count
        );
    }

    if (FAILED(hr) || count == 0) {
        printf("[Encoder] No suitable H.264 encoder found on system.\n");
        return false;
    }

    hr = ppActivates[0]->ActivateObject(IID_PPV_ARGS(&m_pTransform));
    for (UINT32 i = 0; i < count; i++) ppActivates[i]->Release();
    CoTaskMemFree(ppActivates);

    if (FAILED(hr)) return false;

    // Unlock Asynchronous MFT (required for hardware encoders on Windows 10/11)
    IMFAttributes* pAttributes = nullptr;
    if (SUCCEEDED(m_pTransform->GetAttributes(&pAttributes))) {
        pAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        pAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        pAttributes->Release();
    }

    // 3. Set D3D Device Manager on MFT
    m_pTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_pDeviceManager);

    // 4. Configure Low-Latency Codec Attributes
    static const GUID IID_ICodecAPI_Local = { 0x901d859e, 0xfcf7, 0x4144, { 0xb2, 0xa3, 0x61, 0x94, 0xce, 0xee, 0x68, 0x8f } };
    ICodecAPI* pCodecApi = nullptr;
    hr = m_pTransform->QueryInterface(IID_ICodecAPI_Local, (void**)&pCodecApi);
    if (SUCCEEDED(hr)) {
        // Low Latency Mode
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        pCodecApi->SetValue(&CODECAPI_AVEncCommonLowLatency, &var);

        // CBR Mode
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_CBR;
        pCodecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);

        // Quality / Bitrate
        var.ulVal = bitrateBps;
        pCodecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

        // Zero B-Frames (immediate decoding, sub-5ms latency)
        var.ulVal = 0;
        pCodecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);

        // Keyframe interval (GOP size = 120 frames / 2 seconds)
        var.ulVal = (fps > 0) ? fps * 2 : 120;
        pCodecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

        pCodecApi->Release();
    }

    // 5. Setup Input and Output Media Types
    if (!SetupMediaTypes(bitrateBps)) {
        Shutdown();
        return false;
    }

    hr = m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    printf("[Encoder] Media Foundation Encoder initialized (%dx%d @ %d fps, %d Mbps)\n",
           m_width, m_height, m_fps, bitrateBps / 1000000);
    return true;
}

bool VideoEncoder::SetupMediaTypes(int bitrateBps) {
    // 1. Output Media Type (H.264)
    IMFMediaType* pOutputType = nullptr;
    HRESULT hr = MFCreateMediaType(&pOutputType);
    if (FAILED(hr)) return false;

    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    pOutputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(pOutputType, MF_MT_FRAME_RATE, m_fps, 1);
    MFSetAttributeRatio(pOutputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    pOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    pOutputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);

    hr = m_pTransform->SetOutputType(m_outputStreamId, pOutputType, 0);
    pOutputType->Release();
    if (FAILED(hr)) {
        printf("[Encoder] SetOutputType failed: 0x%08lX\n", hr);
        return false;
    }

    // 2. Input Media Type (NV12 or ARGB)
    IMFMediaType* pInputType = nullptr;
    hr = MFCreateMediaType(&pInputType);
    if (FAILED(hr)) return false;

    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, m_fps, 1);
    MFSetAttributeRatio(pInputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    pInputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = m_pTransform->SetInputType(m_inputStreamId, pInputType, 0);
    pInputType->Release();
    if (FAILED(hr)) {
        DWORD typeIdx = 0;
        IMFMediaType* pAvailType = nullptr;
        bool setOk = false;
        while (SUCCEEDED(m_pTransform->GetInputAvailableType(m_inputStreamId, typeIdx++, &pAvailType))) {
            MFSetAttributeSize(pAvailType, MF_MT_FRAME_SIZE, m_width, m_height);
            MFSetAttributeRatio(pAvailType, MF_MT_FRAME_RATE, m_fps, 1);
            hr = m_pTransform->SetInputType(m_inputStreamId, pAvailType, 0);
            pAvailType->Release();
            if (SUCCEEDED(hr)) {
                setOk = true;
                break;
            }
        }
        if (!setOk) {
            printf("[Encoder] SetInputType failed: 0x%08lX\n", hr);
            return false;
        }
    }

    return true;
}

void VideoEncoder::Shutdown() {
    if (m_pTransform) {
        m_pTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_pTransform->Release();
        m_pTransform = nullptr;
    }
    if (m_pDeviceManager) {
        m_pDeviceManager->Release();
        m_pDeviceManager = nullptr;
    }
    MFShutdown();
    m_frameIndex = 0;
}

bool VideoEncoder::EncodeFrame(ID3D11Texture2D* pTexture, uint64_t timestampUs, std::vector<uint8_t>& outNalData, bool& isKeyframe) {
    if (!m_pTransform || !pTexture) return false;

    outNalData.clear();
    isKeyframe = false;

    // 1. Create DXGI Surface Buffer from Texture
    IDXGISurface* pSurface = nullptr;
    HRESULT hr = pTexture->QueryInterface(IID_PPV_ARGS(&pSurface));
    if (FAILED(hr)) return false;

    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pSurface, 0, FALSE, &pBuffer);
    pSurface->Release();
    if (FAILED(hr)) return false;

    // 2. Create Input Sample
    IMFSample* pSample = nullptr;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) { pBuffer->Release(); return false; }

    pSample->AddBuffer(pBuffer);
    pBuffer->Release();

    // 100ns units for MF sample time
    LONGLONG sampleTime = (LONGLONG)(timestampUs * 10);
    pSample->SetSampleTime(sampleTime);
    pSample->SetSampleDuration(10000000 / m_fps);

    // 3. Feed Sample into MFT
    hr = m_pTransform->ProcessInput(m_inputStreamId, pSample, 0);
    pSample->Release();
    if (FAILED(hr)) return false;

    // 4. Retrieve Output NAL packets
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {0};
    outputBuffer.dwStreamID = m_outputStreamId;

    MFT_OUTPUT_STREAM_INFO streamInfo;
    m_pTransform->GetOutputStreamInfo(m_outputStreamId, &streamInfo);

    IMFSample* pOutSample = nullptr;
    IMFMediaBuffer* pOutBuffer = nullptr;
    hr = MFCreateSample(&pOutSample);
    hr = MFCreateMemoryBuffer(streamInfo.cbSize > 0 ? streamInfo.cbSize : 1024 * 1024, &pOutBuffer);
    pOutSample->AddBuffer(pOutBuffer);
    outputBuffer.pSample = pOutSample;

    DWORD status = 0;
    hr = m_pTransform->ProcessOutput(0, 1, &outputBuffer, &status);

    if (SUCCEEDED(hr)) {
        // Check if keyframe
        UINT32 cleanPoint = 0;
        if (SUCCEEDED(pOutSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint) {
            isKeyframe = true;
        }

        BYTE* pData = nullptr;
        DWORD curLen = 0;
        pOutBuffer->Lock(&pData, nullptr, &curLen);
        if (pData && curLen > 0) {
            outNalData.assign(pData, pData + curLen);
        }
        pOutBuffer->Unlock();
    }

    pOutBuffer->Release();
    pOutSample->Release();

    m_frameIndex++;
    return !outNalData.empty();
}
