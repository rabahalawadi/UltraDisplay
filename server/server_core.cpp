#include "server_core.h"
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

static void LogToFile(const char* fmt, ...) {
    static FILE* s_log = fopen("server.log", "a");
    if (!s_log) s_log = fopen("server.log", "w");
    if (s_log) {
        va_list args;
        va_start(args, fmt);
        vfprintf(s_log, fmt, args);
        va_end(args);
        fflush(s_log);
    }
}

static std::string ComputeWebSocketAccept(const std::string& key) {
    std::string fullKey = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE sha1Hash[20];
    DWORD hashLen = sizeof(sha1Hash);
    std::string result = "";

    if (CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
            if (CryptHashData(hHash, (BYTE*)fullKey.data(), (DWORD)fullKey.size(), 0)) {
                if (CryptGetHashParam(hHash, HP_HASHVAL, sha1Hash, &hashLen, 0)) {
                    char b64[64] = {0};
                    DWORD b64Len = sizeof(b64);
                    if (CryptBinaryToStringA(sha1Hash, hashLen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64Len)) {
                        result = b64;
                    }
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    return result;
}

UltraDisplayServer::UltraDisplayServer()
    : m_running(false)
    , m_clientConnected(false)
    , m_clientProtocol(PROTO_UNKNOWN)
    , m_port(ULTRADISPLAY_DEFAULT_PORT)
    , m_monitorIndex(0)
    , m_currentFps(0)
    , m_currentBitrate(0)
    , m_totalFramesSent(0)
    , m_totalPenEvents(0)
    , m_clientSocket(INVALID_SOCKET)
    , m_listenSocket(INVALID_SOCKET)
    , m_udpSocket(INVALID_SOCKET)
{
}

UltraDisplayServer::~UltraDisplayServer() {
    Stop();
}

std::string UltraDisplayServer::GetClientDeviceName() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_clientDeviceName;
}

std::string UltraDisplayServer::GetClientIp() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_clientIp;
}

void UltraDisplayServer::SetTargetMonitor(int index) {
    m_monitorIndex = index;
    if (m_running && m_clientConnected) {
        m_capture.Initialize(m_monitorIndex);
        int targetWidth = m_capture.GetWidth();
        int targetHeight = m_capture.GetHeight();
        m_injector.UpdateTargetBounds(0, 0, targetWidth, targetHeight);
    }
}

bool UltraDisplayServer::Start(int port) {
    if (m_running) return true;
    m_port = port;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[Server] WSAStartup failed.\n");
        return false;
    }

    if (!m_injector.Initialize(0, 0, 1920, 1080)) {
        printf("[Server] Warning: Pen injector initialization failed or restricted.\n");
    }

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) return false;

    BOOL opt = TRUE;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(m_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        printf("[Server] Bind failed on port %d (WSA Error: %d).\n", port, err);
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, 1) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    StartDiscoveryResponder(port);

    m_running = true;
    printf("[Server] UltraDisplay Host Server running on port %d\n", port);
    m_acceptThread = std::thread(&UltraDisplayServer::AcceptLoop, this);
    return true;
}

void UltraDisplayServer::Stop() {
    m_running = false;
    m_clientConnected = false;

    if (m_clientSocket != INVALID_SOCKET) {
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
    }
    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }

    if (m_acceptThread.joinable()) m_acceptThread.join();
    if (m_udpThread.joinable()) m_udpThread.join();
    if (m_streamThread.joinable()) m_streamThread.join();
    if (m_inputThread.joinable()) m_inputThread.join();

    m_capture.Shutdown();
    m_encoder.Shutdown();
    m_injector.Shutdown();

    WSACleanup();
}

void UltraDisplayServer::Wait() {
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
}

void UltraDisplayServer::StartDiscoveryResponder(int tcpPort) {
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) return;

    BOOL broadcast = TRUE;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));
    BOOL reuse = TRUE;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in udpAddr = {0};
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_addr.s_addr = INADDR_ANY;
    udpAddr.sin_port = htons(tcpPort);

    if (bind(m_udpSocket, (sockaddr*)&udpAddr, sizeof(udpAddr)) == SOCKET_ERROR) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
        return;
    }

    m_udpThread = std::thread([this, tcpPort]() {
        char buf[512];
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);

        while (m_running) {
            int recvBytes = recvfrom(m_udpSocket, buf, sizeof(buf) - 1, 0, (sockaddr*)&clientAddr, &clientLen);
            if (recvBytes > 0) {
                buf[recvBytes] = '\0';
                if (strstr(buf, "UDIS_DISCOVER")) {
                    char resp[256];
                    char hostname[128];
                    gethostname(hostname, sizeof(hostname));
                    snprintf(resp, sizeof(resp), "UDIS_OFFER|%s|%d", hostname, tcpPort);
                    sendto(m_udpSocket, resp, (int)strlen(resp), 0, (sockaddr*)&clientAddr, clientLen);
                }
            }
        }
    });
}

void UltraDisplayServer::AcceptLoop() {
    while (m_running) {
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET s = accept(m_listenSocket, (sockaddr*)&clientAddr, &clientLen);
        if (s == INVALID_SOCKET) break;

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        printf("[Server] Client connected from %s:%d\n", clientIp, ntohs(clientAddr.sin_port));

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_clientIp = clientIp;
            m_clientDeviceName = "Connecting...";
        }

        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

        m_clientSocket = s;
        m_clientConnected = true;

        if (PerformHandshake()) {
            if (m_streamThread.joinable()) m_streamThread.join();
            if (m_inputThread.joinable()) m_inputThread.join();

            m_streamThread = std::thread(&UltraDisplayServer::StreamLoop, this);
            m_inputThread = std::thread(&UltraDisplayServer::InputLoop, this);

            m_streamThread.join();
            m_inputThread.join();
        }

        m_clientConnected = false;
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
        m_clientProtocol = PROTO_UNKNOWN;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_clientIp = "";
            m_clientDeviceName = "";
        }
        m_currentFps = 0;
        m_currentBitrate = 0;
        printf("[Server] Client disconnected. Waiting for new connection...\n");
    }
}

bool UltraDisplayServer::PerformHandshake() {
    char peekBuf[512] = {0};
    int peekBytes = recv(m_clientSocket, peekBuf, sizeof(peekBuf) - 1, MSG_PEEK);
    if (peekBytes <= 0) return false;

    if (strncmp(peekBuf, "GET ", 4) == 0) {
        return HandleWebSocketHandshake(peekBuf, peekBytes);
    }
    if (strncmp(peekBuf, "KELOCUBE", 8) == 0) {
        return HandleSuperDisplayHandshake();
    }
    uint32_t magic = *(uint32_t*)peekBuf;
    if (magic == ULTRADISPLAY_MAGIC) {
        return HandleNativeHandshake();
    }
    return false;
}

bool UltraDisplayServer::HandleWebSocketHandshake(char* peekBuf, int peekBytes) {
    std::vector<char> httpReq(peekBytes);
    RecvAll(httpReq.data(), peekBytes);
    std::string reqStr(httpReq.data(), peekBytes);

    std::string keyHeader = "Sec-WebSocket-Key: ";
    size_t keyPos = reqStr.find(keyHeader);
    if (keyPos == std::string::npos) {
        FILE* f = fopen("client_web.html", "rb");
        std::string body = "";
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            body.resize(sz);
            fread(&body[0], 1, sz, f);
            fclose(f);
        } else {
            body = "<html><body><h1>UltraDisplay Web Client Missing</h1></body></html>";
        }
        std::string httpResp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html; charset=utf-8\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + body;
        SendAll(httpResp.data(), (int)httpResp.size());
        return false;
    }

    size_t keyEnd = reqStr.find("\r\n", keyPos);
    std::string secKey = reqStr.substr(keyPos + keyHeader.length(), keyEnd - (keyPos + keyHeader.length()));
    std::string acceptKey = ComputeWebSocketAccept(secKey);

    std::string wsResp = "HTTP/1.1 101 Switching Protocols\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
    if (!SendAll(wsResp.data(), (int)wsResp.size())) return false;

    m_clientProtocol = PROTO_WEBSOCKET;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_clientDeviceName = "Web Browser Client";
    }

    if (!m_capture.Initialize(m_monitorIndex)) return false;
    int targetWidth = m_capture.GetWidth();
    int targetHeight = m_capture.GetHeight();
    m_injector.UpdateTargetBounds(0, 0, targetWidth, targetHeight);

    if (!m_encoder.Initialize(m_capture.GetDevice(), targetWidth, targetHeight, 60, 20000000)) {
        return false;
    }
    return true;
}

bool UltraDisplayServer::HandleSuperDisplayHandshake() {
    char clientMagic[18] = {0};
    if (!RecvAll(clientMagic, 18)) return false;

    const char serverMagic[18] = "KELOCUBE_MIRR_004";
    if (!SendAll(serverMagic, 18)) return false;

    m_clientProtocol = PROTO_SUPERDISPLAY;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_clientDeviceName = "SuperDisplay Tablet";
    }

    if (!m_capture.Initialize(m_monitorIndex)) return false;
    int targetWidth = m_capture.GetWidth();
    int targetHeight = m_capture.GetHeight();
    m_injector.UpdateTargetBounds(0, 0, targetWidth, targetHeight);

    if (!m_encoder.Initialize(m_capture.GetDevice(), targetWidth, targetHeight, 60, 25000000)) {
        return false;
    }
    return true;
}

bool UltraDisplayServer::HandleNativeHandshake() {
    UDisHeader hdr;
    if (!RecvAll(&hdr, sizeof(hdr))) return false;

    UDisHandshakeReq req;
    if (!RecvAll(&req, sizeof(req))) return false;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_clientDeviceName = req.deviceName;
    }

    printf("[Server] Native Client: '%s' (%dx%d, %d DPI, %d Hz, Stylus: %s)\n",
           req.deviceName, req.clientWidth, req.clientHeight, req.densityDpi,
           req.refreshRate, req.hasStylus ? "YES" : "NO");

    if (!m_capture.Initialize(m_monitorIndex)) return false;

    int targetWidth = m_capture.GetWidth();
    int targetHeight = m_capture.GetHeight();
    int targetFps = (req.refreshRate >= 120) ? 120 : 60;
    int targetBitrate = 25000000;

    m_injector.UpdateTargetBounds(0, 0, targetWidth, targetHeight);

    if (!m_encoder.Initialize(m_capture.GetDevice(), targetWidth, targetHeight, targetFps, targetBitrate)) {
        return false;
    }

    UDisHeader respHdr;
    respHdr.magic = ULTRADISPLAY_MAGIC;
    respHdr.type = PKT_HANDSHAKE_RESP;
    respHdr.version = ULTRADISPLAY_VERSION;
    respHdr.payloadLen = sizeof(UDisHandshakeResp);

    UDisHandshakeResp resp;
    resp.status = 0;
    resp.selectedCodec = CODEC_H264;
    resp.streamWidth = targetWidth;
    resp.streamHeight = targetHeight;
    resp.targetFps = targetFps;
    resp.targetBitrate = targetBitrate;

    if (!SendAll(&respHdr, sizeof(respHdr))) return false;
    if (!SendAll(&resp, sizeof(resp))) return false;

    m_clientProtocol = PROTO_UDIS;
    return true;
}

void UltraDisplayServer::StreamLoop() {
    std::vector<uint8_t> nalBuffer;
    uint32_t frameSeq = 0;

    auto lastStatTime = std::chrono::steady_clock::now();
    int framesCount = 0;
    size_t bytesCount = 0;

    while (m_running && m_clientConnected) {
        ID3D11Texture2D* pTexture = nullptr;
        if (m_capture.AcquireFrame(&pTexture, 16)) {
            auto now = std::chrono::steady_clock::now();
            uint64_t timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

            bool isKeyframe = false;
            nalBuffer.clear();

            if (m_encoder.EncodeFrame(pTexture, timestampUs, nalBuffer, isKeyframe)) {
                pTexture->Release();

                framesCount++;
                bytesCount += nalBuffer.size();
                m_totalFramesSent++;

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatTime).count();
                if (elapsed >= 1000) {
                    m_currentFps = (int)((framesCount * 1000) / elapsed);
                    m_currentBitrate = (int)((bytesCount * 8 * 1000) / elapsed);
                    framesCount = 0;
                    bytesCount = 0;
                    lastStatTime = now;
                }

                if (m_clientProtocol == PROTO_WEBSOCKET) {
                    SendWebSocketBinary(nalBuffer.data(), (int)nalBuffer.size());
                } else if (m_clientProtocol == PROTO_SUPERDISPLAY) {
                    uint32_t len = (uint32_t)nalBuffer.size();
                    SendAll(&len, 4);
                    SendAll(nalBuffer.data(), (int)nalBuffer.size());
                } else {
                    UDisHeader hdr;
                    hdr.magic = ULTRADISPLAY_MAGIC;
                    hdr.type = PKT_FRAME_DATA;
                    hdr.version = ULTRADISPLAY_VERSION;
                    hdr.payloadLen = (uint16_t)(sizeof(UDisFrameHeader) + nalBuffer.size());

                    UDisFrameHeader fhdr;
                    fhdr.frameIndex = frameSeq++;
                    fhdr.timestampUs = timestampUs;
                    fhdr.isKeyframe = isKeyframe ? 1 : 0;
                    fhdr.nalSize = (uint32_t)nalBuffer.size();

                    if (!SendAll(&hdr, sizeof(hdr))) break;
                    if (!SendAll(&fhdr, sizeof(fhdr))) break;
                    if (!SendAll(nalBuffer.data(), (int)nalBuffer.size())) break;
                }
            } else {
                pTexture->Release();
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

void UltraDisplayServer::InputLoop() {
    if (m_clientProtocol == PROTO_WEBSOCKET) {
        InputLoopWebSocket();
    } else {
        InputLoopBinary();
    }
}

void UltraDisplayServer::InputLoopWebSocket() {
    while (m_running && m_clientConnected) {
        uint8_t b0 = 0, b1 = 0;
        if (!RecvAll(&b0, 1) || !RecvAll(&b1, 1)) break;

        uint8_t opcode = b0 & 0x0F;
        if (opcode == 0x8) break;

        bool masked = (b1 & 0x80) != 0;
        uint64_t payloadLen = b1 & 0x7F;

        if (payloadLen == 126) {
            uint16_t extLen = 0;
            if (!RecvAll(&extLen, 2)) break;
            payloadLen = ntohs(extLen);
        } else if (payloadLen == 127) {
            uint64_t extLen = 0;
            if (!RecvAll(&extLen, 8)) break;
            payloadLen = ((extLen & 0xFF) << 56) | ((extLen >> 56) & 0xFF);
        }

        uint8_t maskKey[4] = {0};
        if (masked) {
            if (!RecvAll(maskKey, 4)) break;
        }

        std::vector<uint8_t> payload(payloadLen);
        if (payloadLen > 0) {
            if (!RecvAll(payload.data(), (int)payloadLen)) break;
            if (masked) {
                for (size_t i = 0; i < payloadLen; i++) {
                    payload[i] ^= maskKey[i % 4];
                }
            }
        }

        if (payload.size() >= sizeof(UDisHeader)) {
            UDisHeader* pHdr = (UDisHeader*)payload.data();
            if (pHdr->magic == ULTRADISPLAY_MAGIC) {
                if (pHdr->type == PKT_INPUT_SPEN && payload.size() >= sizeof(UDisHeader) + sizeof(UDisSPenEvent)) {
                    UDisSPenEvent* pSpen = (UDisSPenEvent*)(payload.data() + sizeof(UDisHeader));
                    m_totalPenEvents++;
                    m_injector.InjectSPen(*pSpen);
                } else if (pHdr->type == PKT_INPUT_TOUCH && payload.size() >= sizeof(UDisHeader) + sizeof(UDisTouchEvent)) {
                    UDisTouchEvent* pTouch = (UDisTouchEvent*)(payload.data() + sizeof(UDisHeader));
                    m_injector.InjectTouch(*pTouch);
                }
            }
        }
    }
}

void UltraDisplayServer::InputLoopBinary() {
    while (m_running && m_clientConnected) {
        UDisHeader hdr;
        if (!RecvAll(&hdr, sizeof(hdr))) break;
        if (hdr.magic != ULTRADISPLAY_MAGIC) continue;

        if (hdr.type == PKT_INPUT_SPEN) {
            UDisSPenEvent spen;
            if (!RecvAll(&spen, sizeof(spen))) break;
            m_totalPenEvents++;
            m_injector.InjectSPen(spen);
        } else if (hdr.type == PKT_INPUT_TOUCH) {
            UDisTouchEvent touch;
            if (!RecvAll(&touch, sizeof(touch))) break;
            m_injector.InjectTouch(touch);
        } else {
            std::vector<char> drain(hdr.payloadLen);
            if (hdr.payloadLen > 0) {
                RecvAll(drain.data(), hdr.payloadLen);
            }
        }
    }
}

bool UltraDisplayServer::SendWebSocketBinary(const void* data, int bytes) {
    std::vector<uint8_t> frame;
    frame.push_back(0x82);

    if (bytes < 126) {
        frame.push_back((uint8_t)bytes);
    } else if (bytes <= 65535) {
        frame.push_back(126);
        frame.push_back((bytes >> 8) & 0xFF);
        frame.push_back(bytes & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((bytes >> (i * 8)) & 0xFF);
        }
    }

    const uint8_t* p = (const uint8_t*)data;
    frame.insert(frame.end(), p, p + bytes);
    return SendAll(frame.data(), (int)frame.size());
}

bool UltraDisplayServer::SendAll(const void* data, int bytes) {
    const char* p = (const char*)data;
    while (bytes > 0) {
        int sent = send(m_clientSocket, p, bytes, 0);
        if (sent <= 0) return false;
        p += sent;
        bytes -= sent;
    }
    return true;
}

bool UltraDisplayServer::RecvAll(void* data, int bytes) {
    char* p = (char*)data;
    while (bytes > 0) {
        int recvd = recv(m_clientSocket, p, bytes, 0);
        if (recvd <= 0) return false;
        p += recvd;
        bytes -= recvd;
    }
    return true;
}
