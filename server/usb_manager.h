#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

struct UsbDeviceInfo {
    std::string serial;
    std::string model;
    std::string product;
    bool isAuthorized;
};

class UsbManager {
public:
    UsbManager();
    ~UsbManager();

    bool Initialize();
    void StartMonitoring(int targetPort = 28443);
    void StopMonitoring();

    bool IsAdbAvailable() const { return m_adbAvailable; }
    std::string GetAdbPath() const { return m_adbPath; }
    bool IsDeviceConnected() const { return m_deviceConnected; }
    bool IsReversePortActive() const { return m_reverseActive; }
    std::string GetConnectedDeviceModel() const;
    std::string GetConnectedDeviceSerial() const;

    bool ForceReversePort(int port = 28443);

private:
    std::string FindAdbExecutable();
    void MonitoringLoop(int targetPort);
    bool RunAdbCommand(const std::string& args, std::string* output = nullptr);

    std::string m_adbPath;
    std::atomic<bool> m_adbAvailable;
    std::atomic<bool> m_monitoring;
    std::atomic<bool> m_deviceConnected;
    std::atomic<bool> m_reverseActive;
    mutable std::mutex m_mutex;
    std::string m_connectedModel;
    std::string m_connectedSerial;
    std::thread m_monitorThread;
    int m_targetPort;
};
