// serial_handler.cpp — rate-limited per-character send (build-fixed)
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <vector>
#include <atomic>           // FIX: for std::atomic

#include <libserialport.h>

#include "serial_handler.h"

// Public flag expected by the rest of the app.
bool serial_available = false;

// Internal handle
static sp_port* serial_port = nullptr;

// Default per-character delay (ms). Can be changed via setSerialSendDelay().
static std::atomic<int> serial_send_delay_ms{50};

void setSerialSendDelay(int delay_ms) {
    if (delay_ms < 0) delay_ms = 0;
    serial_send_delay_ms.store(delay_ms, std::memory_order_relaxed);
}

static bool set_port_config(sp_port* port_handle, int baudrate) {
    if (sp_set_baudrate(port_handle, baudrate) != SP_OK) return false;
    // Typical 8N1; adjust if your project requires different settings.
    if (sp_set_bits(port_handle, 8) != SP_OK) return false;
    if (sp_set_parity(port_handle, SP_PARITY_NONE) != SP_OK) return false;
    if (sp_set_stopbits(port_handle, 1) != SP_OK) return false;
    if (sp_set_flowcontrol(port_handle, SP_FLOWCONTROL_NONE) != SP_OK) return false;
    return true;
}

bool initSerial(const std::string& port, int baudrate) {
    sp_port* handle = nullptr;

    if (sp_get_port_by_name(port.c_str(), &handle) != SP_OK) {
        std::cerr << "[Warning] Could not open serial port: " << port << std::endl;
        serial_available = false;
        return false;
    }

    // FIX: cast flags to enum sp_mode to placate -fpermissive
    if (sp_open(handle, static_cast<sp_mode>(SP_MODE_READ | SP_MODE_WRITE)) != SP_OK) {
        std::cerr << "[Error] Failed to open serial port: " << port << std::endl;
        serial_available = false;
        sp_free_port(handle);
        return false;
    }

    if (!set_port_config(handle, baudrate)) {
        std::cerr << "[Error] Failed to configure serial port: " << port << std::endl;
        sp_close(handle);
        sp_free_port(handle);
        return false;
    }

    serial_port = handle;
    serial_available = true;

    // Optional: initial banner to confirm link-up.
    serialSend("AImaster: Serial link active\r\n");

    return true;
}

void serialSend(const std::string& data) {
    if (!serial_available || serial_port == nullptr) return;

    const int delay_ms = serial_send_delay_ms.load(std::memory_order_relaxed);

    for (size_t i = 0; i < data.size(); ++i) {
        const char c = data[i];
        // Try to write this single byte. Retry a few times if needed.
        int attempts = 0;
        while (attempts < 3) {
            int written = sp_nonblocking_write(serial_port, &c, 1);
            if (written == 1) break; // success
            if (written < 0) {
                // FIX: capture and free the error message correctly
                char* msg = sp_last_error_message();
                if (msg) {
                    std::cerr << "[Warning] serialSend write error: " << msg << std::endl;
                    sp_free_error_message(msg);
                } else {
                    std::cerr << "[Warning] serialSend write error (no message)" << std::endl;
                }
                break;
            }
            // If written == 0, port not ready; brief backoff
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            attempts++;
        }

        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
}

void closeSerial() {
    if (serial_port) {
        sp_close(serial_port);
        sp_free_port(serial_port);
        serial_port = nullptr;
    }
    serial_available = false;
}
