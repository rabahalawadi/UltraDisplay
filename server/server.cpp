#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

#include "../protocol/protocol.h"
#include "pen_injector.h"
#include "screen_capture.h"
#include "encoder.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

enum ClientProtocol {
    PROTO_UNKNOWN,
    PROTO_UDIS,
    PROTO_WEBSOCKET,
    PROTO_SUPERDISPLAY
};

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

class UltraDisplayServer {
public:
    UltraDisplayServer()
        : m_running(false)
        , m_clientConnected(false)
        , m_clientProtocol(PROTO_UNKNOWN)
        , m_clientSocket(INVALID_SOCKET)
        , m_listenSocket(INVALID_SOCKET)
        , m_udpSocket(INVALID_SOCKET)
    {
    }

    ~UltraDisplayServer() {
        Stop();
    }

    bool Start(int port = ULTRADISPLAY_DEFAULT_PORT) {
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
            if (err == WSAEADDRINUSE) {
                printf("[Server] Port %d is already in use by another instance or process.\n", port);
            }
            printf("[Server] Closing in 5 seconds...\n");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            closesocket(m_listenSocket);
            return false;
        }

        if (listen(m_listenSocket, 1) == SOCKET_ERROR) {
            closesocket(m_listenSocket);
            return false;
        }

        StartDiscoveryResponder(port);

        m_running = true;
        printf("\n======================================================\n");
        printf("   UltraDisplay Host Server Running on Port %d\n", port);
        printf("   - Zero-driver Windows Synthetic Pen & S-Pen Active\n");
        printf("   - Desktop Duplication + Low Latency H.264 Encoder Ready\n");
        printf("   - Multi-Protocol: Native UDIS, WebSocket & SuperDisplay\n");
        printf("   - Waiting for Android client connection...\n");
        printf("======================================================\n\n");

        m_acceptThread = std::thread(&UltraDisplayServer::AcceptLoop, this);
        return true;
    }

    void Stop() {
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
        if (m_streamThread.joinable()) m_streamThread.join();
        if (m_inputThread.joinable()) m_inputThread.join();
        if (m_discoveryThread.joinable()) m_discoveryThread.join();

        m_injector.Shutdown();
        m_encoder.Shutdown();
        m_capture.Shutdown();

        WSACleanup();
        printf("[Server] Server stopped.\n");
    }

    void Wait() {
        if (m_acceptThread.joinable()) {
            m_acceptThread.join();
        }
    }

private:
    std::atomic<bool> m_running;
    std::atomic<bool> m_clientConnected;
    ClientProtocol    m_clientProtocol;
    SOCKET m_listenSocket;
    SOCKET m_clientSocket;
    SOCKET m_udpSocket;

    PenInjector   m_injector;
    ScreenCapture m_capture;
    VideoEncoder  m_encoder;

    std::thread m_acceptThread;
    std::thread m_streamThread;
    std::thread m_inputThread;
    std::thread m_discoveryThread;

    void StartDiscoveryResponder(int tcpPort) {
        m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_udpSocket == INVALID_SOCKET) return;

        BOOL opt = TRUE;
        setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in udpAddr = {0};
        udpAddr.sin_family = AF_INET;
        udpAddr.sin_addr.s_addr = INADDR_ANY;
        udpAddr.sin_port = htons(tcpPort + 1); // 28444

        if (bind(m_udpSocket, (sockaddr*)&udpAddr, sizeof(udpAddr)) == SOCKET_ERROR) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
            return;
        }

        m_discoveryThread = std::thread([this, tcpPort]() {
            char buf[256];
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

    void AcceptLoop() {
        while (m_running) {
            sockaddr_in clientAddr;
            int clientLen = sizeof(clientAddr);
            SOCKET s = accept(m_listenSocket, (sockaddr*)&clientAddr, &clientLen);
            if (s == INVALID_SOCKET) break;

            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
            printf("[Server] Client connected from %s:%d\n", clientIp, ntohs(clientAddr.sin_port));

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
            printf("[Server] Client disconnected. Waiting for new connection...\n");
        }
    }

    bool PerformHandshake() {
        // Peek incoming bytes to detect protocol
        char peekBuf[512] = {0};
        int peekBytes = recv(m_clientSocket, peekBuf, sizeof(peekBuf) - 1, MSG_PEEK);
        if (peekBytes <= 0) {
            printf("[Server] No initial data received from client.\n");
            return false;
        }

        printf("[Server] Received %d handshake probe bytes:\n", peekBytes);
        printf("         HEX:   ");
        for (int i = 0; i < peekBytes && i < 32; i++) {
            printf("%02X ", (unsigned char)peekBuf[i]);
        }
        printf("\n         ASCII: '");
        for (int i = 0; i < peekBytes && i < 32; i++) {
            char c = peekBuf[i];
            putchar((c >= 32 && c <= 126) ? c : '.');
        }
        printf("'\n");

        // 1. Detect WebSocket Client (HTTP GET / Upgrade: websocket)
        if (strncmp(peekBuf, "GET ", 4) == 0) {
            return HandleWebSocketHandshake(peekBuf, peekBytes);
        }

        // 2. Detect SuperDisplay Client ("KELOCUBE_MIRR_004")
        if (strncmp(peekBuf, "KELOCUBE", 8) == 0) {
            return HandleSuperDisplayHandshake();
        }

        // 3. Detect Native UltraDisplay Client (MAGIC 0x55444953)
        uint32_t magic = *(uint32_t*)peekBuf;
        if (magic == ULTRADISPLAY_MAGIC) {
            return HandleNativeHandshake();
        }

        printf("[Server] Unknown handshake format from client.\n");
        return false;
    }

    bool HandleWebSocketHandshake(char* peekBuf, int peekBytes) {
        // Consume the HTTP request from socket
        std::vector<char> httpReq(peekBytes);
        RecvAll(httpReq.data(), peekBytes);
        std::string reqStr(httpReq.data(), peekBytes);

        // Extract Sec-WebSocket-Key
        std::string keyHeader = "Sec-WebSocket-Key: ";
        size_t keyPos = reqStr.find(keyHeader);
        if (keyPos == std::string::npos) {
            // Standard browser navigation: Serve client_web.html directly
            FILE* f = fopen("C:\\Users\\rabah\\UltraDisplay\\client_web.html", "rb");
            if (!f) f = fopen("client_web.html", "rb");
            if (!f) f = fopen("..\\client_web.html", "rb");
            std::string htmlContent;
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                std::vector<char> buf(sz);
                fread(buf.data(), 1, sz, f);
                fclose(f);
                htmlContent.assign(buf.data(), sz);
            } else {
                htmlContent = "<html><body><h2>UltraDisplay Server Online</h2></body></html>";
            }

            std::string httpResp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + std::to_string(htmlContent.size()) + "\r\n"
                "Connection: close\r\n\r\n" + htmlContent;

            SendAll(httpResp.data(), (int)httpResp.size());
            printf("[Server] Served UltraDisplay Web Client HTML to browser!\n");
            return false; // Closes socket cleanly; browser will now connect via WebSocket
        }
        size_t keyEnd = reqStr.find("\r\n", keyPos);
        std::string wsKey = reqStr.substr(keyPos + keyHeader.length(), keyEnd - (keyPos + keyHeader.length()));

        std::string acceptKey = ComputeWebSocketAccept(wsKey);

        std::string httpResp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";

        if (!SendAll(httpResp.data(), (int)httpResp.size())) return false;

        m_clientProtocol = PROTO_WEBSOCKET;
        printf("[Server] WebSocket Handshake Established with Browser Client!\n");

        if (!m_capture.Initialize(0)) return false;
        int targetWidth = m_capture.GetWidth();
        int targetHeight = m_capture.GetHeight();
        m_injector.UpdateTargetBounds(0, 0, targetWidth, targetHeight);

        if (!m_encoder.Initialize(m_capture.GetDevice(), targetWidth, targetHeight, 60, 20000000)) {
            return false;
        }

        printf("[Server] Streaming %dx%d via WebSocket to Browser/S-Pen...\n", targetWidth, targetHeight);
        return true;
    }

    bool HandleSuperDisplayHandshake() {
        char clientMagic[18] = {0};
        if (!RecvAll(clientMagic, 18)) return false;

        printf("[Server] SuperDisplay Handshake: '%s'\n", clientMagic);

        // Send Server Handshake
        const char serverMagic[18] = "KELOCUBE_MIRR_004";
        if (!SendAll(serverMagic, 18)) return false;

        m_clientProtocol = PROTO_SUPERDISPLAY;
        printf("[Server] SuperDisplay protocol established with tablet!\n");

        if (!m_capture.Initialize(0)) return false;
        int targetWidth = m_capture.GetWidth();
        int targetHeight = m_capture.GetHeight();
        m_injector.UpdateTargetBounds(0, 0, targetWidth, targetHeight);

        if (!m_encoder.Initialize(m_capture.GetDevice(), targetWidth, targetHeight, 60, 25000000)) {
            return false;
        }

        return true;
    }

    bool HandleNativeHandshake() {
        UDisHeader hdr;
        if (!RecvAll(&hdr, sizeof(hdr))) return false;

        UDisHandshakeReq req;
        if (!RecvAll(&req, sizeof(req))) return false;

        printf("[Server] Native Client Handshake: '%s' (%dx%d, %d DPI, %d Hz, S-Pen: %s)\n",
               req.deviceName, req.clientWidth, req.clientHeight, req.densityDpi,
               req.refreshRate, req.hasStylus ? "YES" : "NO");

        if (!m_capture.Initialize(0)) return false;

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
        printf("[Server] Native Handshake complete! Streaming %dx%d @ %d fps...\n", targetWidth, targetHeight, targetFps);
        return true;
    }

    void StreamLoop() {
        std::vector<uint8_t> nalBuffer;
        uint32_t frameSeq = 0;

        while (m_running && m_clientConnected) {
            ID3D11Texture2D* pTexture = nullptr;
            if (m_capture.AcquireFrame(&pTexture, 16)) {
                auto now = std::chrono::steady_clock::now();
                uint64_t timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

                bool isKeyframe = false;
                if (m_encoder.EncodeFrame(pTexture, timestampUs, nalBuffer, isKeyframe)) {
                    pTexture->Release();

                    if (m_clientProtocol == PROTO_WEBSOCKET) {
                        SendWebSocketBinary(nalBuffer.data(), (int)nalBuffer.size());
                    } else if (m_clientProtocol == PROTO_SUPERDISPLAY) {
                        // SuperDisplay framing: [uint32 payload_len][payload]
                        uint32_t len = (uint32_t)nalBuffer.size();
                        SendAll(&len, 4);
                        SendAll(nalBuffer.data(), (int)nalBuffer.size());
                    } else {
                        // Native UltraDisplay framing
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

    void InputLoop() {
        if (m_clientProtocol == PROTO_WEBSOCKET) {
            InputLoopWebSocket();
        } else {
            InputLoopBinary();
        }
    }

    void InputLoopWebSocket() {
        while (m_running && m_clientConnected) {
            // Read WebSocket Frame Header
            uint8_t b0 = 0, b1 = 0;
            if (!RecvAll(&b0, 1) || !RecvAll(&b1, 1)) break;

            uint8_t opcode = b0 & 0x0F;
            if (opcode == 0x08) break; // Close frame

            bool isMasked = (b1 & 0x80) != 0;
            uint64_t payloadLen = b1 & 0x7F;

            if (payloadLen == 126) {
                uint16_t extLen = 0;
                if (!RecvAll(&extLen, 2)) break;
                payloadLen = ntohs(extLen);
            } else if (payloadLen == 127) {
                uint64_t extLen = 0;
                if (!RecvAll(&extLen, 8)) break;
                // Reverse 64-bit big endian
                payloadLen = ((extLen & 0xFF) << 56) | ((extLen >> 56) & 0xFF);
            }

            uint8_t mask[4] = {0};
            if (isMasked) {
                if (!RecvAll(mask, 4)) break;
            }

            std::vector<uint8_t> payload(payloadLen);
            if (payloadLen > 0) {
                if (!RecvAll(payload.data(), (int)payloadLen)) break;
                if (isMasked) {
                    for (size_t i = 0; i < payloadLen; i++) {
                        payload[i] ^= mask[i % 4];
                    }
                }
            }

            // Parse S-Pen / Touch event from unmasked WebSocket frame
            if (payload.size() >= sizeof(UDisHeader)) {
                UDisHeader* pHdr = (UDisHeader*)payload.data();
                if (pHdr->magic == ULTRADISPLAY_MAGIC) {
                    if (pHdr->type == PKT_INPUT_SPEN && payload.size() >= sizeof(UDisHeader) + sizeof(UDisSPenEvent)) {
                        UDisSPenEvent* pSpen = (UDisSPenEvent*)(payload.data() + sizeof(UDisHeader));
                        static int s_logCounter = 0;
                        if ((++s_logCounter % 25 == 1) || (pSpen->flags & (SPEN_FLAG_DOWN | SPEN_FLAG_UP | SPEN_FLAG_BARREL))) {
                            printf("[S-Pen WS] norm(%.3f, %.3f) pressure:%4u flags:0x%04X%s%s\n",
                                   pSpen->normX, pSpen->normY, pSpen->pressure, pSpen->flags,
                                   (pSpen->flags & SPEN_FLAG_INCONTACT) ? " [TOUCH]" : " [HOVER]",
                                   (pSpen->flags & SPEN_FLAG_BARREL) ? " [BARREL]" : "");
                        }
                        LogToFile("[S-Pen WS] norm(%.3f, %.3f) p=%u flags=0x%04X%s\n",
                                  pSpen->normX, pSpen->normY, pSpen->pressure, pSpen->flags,
                                  (pSpen->flags & SPEN_FLAG_INCONTACT) ? " [TOUCH]" : " [HOVER]");
                        m_injector.InjectSPen(*pSpen);
                    } else if (pHdr->type == PKT_INPUT_TOUCH && payload.size() >= sizeof(UDisHeader) + sizeof(UDisTouchEvent)) {
                        UDisTouchEvent* pTouch = (UDisTouchEvent*)(payload.data() + sizeof(UDisHeader));
                        m_injector.InjectTouch(*pTouch);
                    }
                }
            }
        }
    }

    void InputLoopBinary() {
        while (m_running && m_clientConnected) {
            UDisHeader hdr;
            if (!RecvAll(&hdr, sizeof(hdr))) break;

            if (hdr.magic != ULTRADISPLAY_MAGIC) continue;

            if (hdr.type == PKT_INPUT_SPEN) {
                UDisSPenEvent spen;
                if (!RecvAll(&spen, sizeof(spen))) break;
                static int s_logBinCounter = 0;
                if ((++s_logBinCounter % 25 == 1) || (spen.flags & (SPEN_FLAG_DOWN | SPEN_FLAG_UP | SPEN_FLAG_BARREL))) {
                    printf("[S-Pen Native] norm(%.3f, %.3f) pressure:%4u flags:0x%04X\n",
                           spen.normX, spen.normY, spen.pressure, spen.flags);
                }
                LogToFile("[S-Pen Native] norm(%.3f, %.3f) p=%u flags=0x%04X%s\n",
                          spen.normX, spen.normY, spen.pressure, spen.flags,
                          (spen.flags & SPEN_FLAG_INCONTACT) ? " [TOUCH]" : " [HOVER]");
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

    bool SendWebSocketBinary(const void* data, int bytes) {
        std::vector<uint8_t> frame;
        frame.push_back(0x82); // FIN = 1, Binary Frame

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

    bool SendAll(const void* data, int bytes) {
        const char* p = (const char*)data;
        while (bytes > 0) {
            int sent = send(m_clientSocket, p, bytes, 0);
            if (sent <= 0) return false;
            p += sent;
            bytes -= sent;
        }
        return true;
    }

    bool RecvAll(void* data, int bytes) {
        char* p = (char*)data;
        while (bytes > 0) {
            int recvd = recv(m_clientSocket, p, bytes, 0);
            if (recvd <= 0) return false;
            p += recvd;
            bytes -= recvd;
        }
        return true;
    }
};

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Initializing UltraDisplay Multi-Protocol Host Engine...\n");
    UltraDisplayServer server;
    if (server.Start()) {
        server.Wait();
    }
    return 0;
}
