#include "usb_manager.h"
#include <windows.h>
#include <stdio.h>
#include <sstream>
#include <chrono>

UsbManager::UsbManager()
    : m_adbAvailable(false)
    , m_monitoring(false)
    , m_deviceConnected(false)
    , m_reverseActive(false)
    , m_targetPort(28443)
{
}

UsbManager::~UsbManager() {
    StopMonitoring();
}

std::string UsbManager::FindAdbExecutable() {
    char localApp[MAX_PATH] = {0};
    GetEnvironmentVariableA("LOCALAPPDATA", localApp, sizeof(localApp));

    std::vector<std::string> candidates = {
        std::string(localApp) + "\\Android\\Sdk\\platform-tools\\adb.exe",
        std::string(localApp) + "\\Microsoft\\WinGet\\Packages\\Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe\\platform-tools\\adb.exe",
        "adb.exe"
    };

    for (const auto& path : candidates) {
        DWORD attrib = GetFileAttributesA(path.c_str());
        if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            return path;
        }
    }

    // Try finding in PATH
    char foundPath[MAX_PATH] = {0};
    if (SearchPathA(NULL, "adb.exe", NULL, MAX_PATH, foundPath, NULL) > 0) {
        return std::string(foundPath);
    }

    return "";
}

bool UsbManager::Initialize() {
    m_adbPath = FindAdbExecutable();
    m_adbAvailable = !m_adbPath.empty();
    if (m_adbAvailable) {
        printf("[UsbManager] Found ADB at: %s\n", m_adbPath.c_str());
    } else {
        printf("[UsbManager] Warning: ADB not found on system.\n");
    }
    return m_adbAvailable;
}

bool UsbManager::RunAdbCommand(const std::string& args, std::string* output) {
    if (m_adbPath.empty()) return false;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi = {0};
    std::string cmd = "\"" + m_adbPath + "\" " + args;

    char cmdBuf[1024];
    strncpy(cmdBuf, cmd.c_str(), sizeof(cmdBuf) - 1);
    cmdBuf[sizeof(cmdBuf) - 1] = '\0';

    BOOL success = CreateProcessA(NULL, cmdBuf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hWrite);

    if (!success) {
        CloseHandle(hRead);
        return false;
    }

    if (output) {
        char buf[512];
        DWORD bytesRead = 0;
        output->clear();
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            output->append(buf, bytesRead);
        }
    }

    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, 3000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode == 0);
}

bool UsbManager::ForceReversePort(int port) {
    m_targetPort = port;
    if (!m_adbAvailable) return false;

    std::string args = "reverse tcp:" + std::to_string(port) + " tcp:" + std::to_string(port);
    std::string out;
    bool ok = RunAdbCommand(args, &out);
    if (ok) {
        m_reverseActive = true;
        printf("[UsbManager] Active reverse port forwarding: tcp:%d -> tcp:%d\n", port, port);
    }
    return ok;
}

void UsbManager::StartMonitoring(int targetPort) {
    m_targetPort = targetPort;
    if (m_monitoring) return;

    if (!m_adbAvailable) {
        Initialize();
    }

    m_monitoring = true;
    m_monitorThread = std::thread(&UsbManager::MonitoringLoop, this, targetPort);
}

void UsbManager::StopMonitoring() {
    if (m_monitoring) {
        m_monitoring = false;
        if (m_monitorThread.joinable()) {
            m_monitorThread.join();
        }
    }
}

std::string UsbManager::GetConnectedDeviceModel() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connectedModel;
}

std::string UsbManager::GetConnectedDeviceSerial() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connectedSerial;
}

void UsbManager::MonitoringLoop(int targetPort) {
    while (m_monitoring) {
        if (m_adbAvailable) {
            std::string out;
            if (RunAdbCommand("devices -l", &out)) {
                std::istringstream stream(out);
                std::string line;
                bool foundDevice = false;
                std::string devSerial;
                std::string devModel = "Android Device";

                while (std::getline(stream, line)) {
                    if (line.find("List of devices") != std::string::npos || line.empty()) continue;

                    // Exclude network ADB connections (e.g. 192.168.0.54:5555) from being marked as USB Cable
                    if (line.find(":5555") != std::string::npos) continue;

                    if (line.find("device") != std::string::npos) {
                        foundDevice = true;
                        std::istringstream lineStream(line);
                        lineStream >> devSerial;

                        size_t modelPos = line.find("model:");
                        if (modelPos != std::string::npos) {
                            size_t spacePos = line.find(' ', modelPos);
                            devModel = line.substr(modelPos + 6, spacePos - (modelPos + 6));
                        }
                        break;
                    }
                }

                if (foundDevice) {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_connectedSerial = devSerial;
                        m_connectedModel = devModel;
                    }
                    m_deviceConnected = true;

                    // Automatically setup ADB reverse over USB cable
                    if (!m_reverseActive) {
                        ForceReversePort(targetPort);
                    }
                } else {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_connectedSerial = "";
                        m_connectedModel = "";
                    }
                    m_deviceConnected = false;
                    m_reverseActive = false;
                }
            }
        }

        // Sleep 1.5 seconds between checks
        for (int i = 0; i < 15 && m_monitoring; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
