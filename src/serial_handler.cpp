#include "serial_handler.h"
#include <iostream>
#include <fstream>

#if !defined(_WIN32)
#include <filesystem>
#include <vector> // ✅ Remembered fix
#endif

struct sp_port* port = nullptr;
bool serial_available = false;
std::string detected_port;

bool initSerial(const AppConfig& config) {
#if defined(_WIN32)
    if (!config.serial_port.empty() &&
        sp_get_port_by_name(config.serial_port.c_str(), &port) == SP_OK &&
        sp_open(port, SP_MODE_READ_WRITE) == SP_OK &&
        sp_set_baudrate(port, config.baudrate) == SP_OK) {
        serial_available = true;
        std::cout << "Serial port opened: " << config.serial_port
                  << " at " << config.baudrate << " baud.\n";
        return true;
    }
#else
    if (!config.serial_port.empty()) {
        detected_port = config.serial_port;
    } else {
        detected_port = autoDetectSerialPort();
    }

    if (!detected_port.empty()) {
        if (sp_get_port_by_name(detected_port.c_str(), &port) == SP_OK &&
            sp_open(port, SP_MODE_READ_WRITE) == SP_OK &&
            sp_set_baudrate(port, config.baudrate) == SP_OK) {
            serial_available = true;
            std::cout << "Serial port opened: " << detected_port
                      << " at " << config.baudrate << " baud.\n";
            return true;
        }
    }
#endif
    std::cerr << "Warning: No serial port available. Using console mode.\n";
    return false;
}

void closeSerial() {
    if (serial_available && port) {
        sp_close(port);
        sp_free_port(port);
    }
}

void sendCommand(const std::string& command) {
    if (serial_available) {
        sp_nonblocking_write(port, command.c_str(), command.size());
    } else {
        std::ofstream log("log.txt", std::ios::app);
        if (log.is_open()) {
            log << "[Console Mode] Sending: " << command << std::endl;
        }
    }
}

std::string receiveCommand() {
    if (serial_available) {
        char buf[256];
        int bytes_read = sp_nonblocking_read(port, buf, sizeof(buf) - 1);
        if (bytes_read > 0) {
            buf[bytes_read] = '\0';
            return std::string(buf);
        }
    }
    std::string command;
    std::cout << "> ";
    std::getline(std::cin, command);
    return command;
}

#ifndef _WIN32
std::string autoDetectSerialPort() {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;

    for (const auto& entry : fs::directory_iterator("/dev")) {
        std::string path = entry.path().string();
        if (path.find("/dev/ttyS") == 0) {
            candidates.push_back(path);
        }
    }

    if (!candidates.empty()) {
        std::cout << "Detected serial devices:\n";
        for (size_t i = 0; i < candidates.size(); i++) {
            std::cout << " [" << i << "] " << candidates[i] << "\n";
        }
        std::cout << "Select device index or press Enter for default [0]: ";

        std::string choice;
        std::getline(std::cin, choice);
        if (choice.empty()) return candidates[0];
        try {
            int idx = std::stoi(choice);
            if (idx >= 0 && idx < (int)candidates.size()) {
                return candidates[idx];
            }
        } catch (...) {}
    }
    return "";
}
#endif
