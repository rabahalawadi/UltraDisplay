#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

#include "../protocol/protocol.h"
#include "pen_injector.h"
#include "screen_capture.h"
#include "encoder.h"

enum ClientProtocol {
    PROTO_UNKNOWN,
    PROTO_UDIS,
    PROTO_WEBSOCKET,
    PROTO_SUPERDISPLAY
};

class UltraDisplayServer {
public:
    UltraDisplayServer();
    ~UltraDisplayServer();

    bool Start(int port = ULTRADISPLAY_DEFAULT_PORT);
    void Stop();
    void Wait();

    // Telemetry & Status
    bool IsRunning() const { return m_running; }
    bool IsClientConnected() const { return m_clientConnected; }
    int GetCurrentFps() const { return m_currentFps; }
    int GetCurrentBitrate() const { return m_currentBitrate; }
    int GetClientProtocol() const { return m_clientProtocol; }
    int GetPort() const { return m_port; }
    std::string GetClientDeviceName() const;
    std::string GetClientIp() const;
    uint64_t GetTotalFramesSent() const { return m_totalFramesSent; }
    uint64_t GetTotalPenEvents() const { return m_totalPenEvents; }
    bool IsWindowsInkActive() const { return m_injector.IsWindowsInkActive(); }

    void SetTargetMonitor(int index);
    int GetTargetMonitor() const { return m_monitorIndex; }

private:
    void StartDiscoveryResponder(int tcpPort);
    void AcceptLoop();
    bool PerformHandshake();
    bool HandleWebSocketHandshake(char* peekBuf, int peekBytes);
    bool HandleSuperDisplayHandshake();
    bool HandleNativeHandshake();
    void StreamLoop();
    void InputLoop();
    void InputLoopWebSocket();
    void InputLoopBinary();
    bool SendWebSocketBinary(const void* data, int bytes);
    bool SendAll(const void* data, int bytes);
    bool RecvAll(void* data, int bytes);

    std::atomic<bool> m_running;
    std::atomic<bool> m_clientConnected;
    std::atomic<int> m_clientProtocol;
    std::atomic<int> m_port;
    std::atomic<int> m_monitorIndex;

    std::atomic<int> m_currentFps;
    std::atomic<int> m_currentBitrate;
    std::atomic<uint64_t> m_totalFramesSent;
    std::atomic<uint64_t> m_totalPenEvents;

    mutable std::mutex m_stateMutex;
    std::string m_clientDeviceName;
    std::string m_clientIp;

    SOCKET m_clientSocket;
    SOCKET m_listenSocket;
    SOCKET m_udpSocket;

    ScreenCapture m_capture;
    VideoEncoder m_encoder;
    PenInjector m_injector;

    std::thread m_acceptThread;
    std::thread m_udpThread;
    std::thread m_streamThread;
    std::thread m_inputThread;
};
