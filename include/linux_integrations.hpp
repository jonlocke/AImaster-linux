#pragma once

#include <functional>
#include <string>
#include <vector>

struct AppConfig;

struct InputDeviceInfo {
    std::string path;
    std::string name;
};

struct HidCaptureResult {
    std::string path;
    std::string name;
    int code = -1;
};

struct BluetoothDeviceInfo {
    std::string mac;
    std::string name;
};

struct CaptureDeviceInfo {
    std::string id;
    std::string description;
    bool is_default = false;
};

using MicTranscriptCallback = std::function<void(const std::string&)>;

std::vector<InputDeviceInfo> listInputDevices(std::string& error);
bool captureHidButton(const AppConfig& config, HidCaptureResult& result, std::string& error);

bool configureMicHotkeyService(AppConfig& config, MicTranscriptCallback callback, std::string& status);
void stopMicHotkeyService();
bool toggleMicRecording(AppConfig& config, std::string& status);
bool startMicRecording(AppConfig& config, std::string& status);
bool stopMicRecording(AppConfig& config, std::string& status);
std::string micServiceStatus(const AppConfig& config);

std::vector<BluetoothDeviceInfo> scanBluetoothDevices(const AppConfig& config, std::string& error);
std::vector<BluetoothDeviceInfo> listKnownBluetoothDevices(std::string& error);
std::vector<BluetoothDeviceInfo> listConnectedBluetoothDevices(std::string& error);
const std::vector<BluetoothDeviceInfo>& lastScannedBluetoothDevices();
bool pairBluetoothDevice(const AppConfig& config, const std::string& target, std::string& status);
std::vector<CaptureDeviceInfo> listCaptureDevices(std::string& error);
