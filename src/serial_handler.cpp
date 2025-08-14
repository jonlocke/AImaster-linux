// serial_handler.cpp — rate-limited send + serial line listener + newline policy
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>

#include <libserialport.h>
#include "serial_handler.h"
#include "io_sink.h"

bool serial_available = false;
static sp_port* serial_port = nullptr;
static std::atomic<int> serial_send_delay_ms{50};

// Newline policy: 0=CRLF, 1=LFCR, 2=LF, 3=CR
static std::atomic<int> serial_newline_policy{0};

static std::thread serial_thread;
static std::atomic<bool> serial_thread_running{false};
static std::function<void(const std::string&)> line_callback = nullptr;
static std::string rx_buffer;

static int to_policy(const std::string& s) {
    std::string k; k.reserve(s.size());
    for (unsigned char c : s) k.push_back(std::toupper(c));
    if (k == "CRLF") return 0;
    if (k == "LFCR") return 1;
    if (k == "LF")   return 2;
    if (k == "CR")   return 3;
    return 0;
}

void setSerialNewlinePolicy(const std::string& policy) {
    serial_newline_policy.store(to_policy(policy), std::memory_order_relaxed);
}

void setSerialSendDelay(int delay_ms) {
    if (delay_ms < 0) delay_ms = 0;
    serial_send_delay_ms.store(delay_ms, std::memory_order_relaxed);
}

static bool set_port_config(sp_port* port_handle, int baudrate) {
    if (sp_set_baudrate(port_handle, baudrate) != SP_OK) return false;
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
    //outln("AImaster: Serial link active");
    //emit_prompt();

    // Optional: announce
    serialSend("\f");
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1234ms);
    serialSend("Welcome to AImaster\n\n");
    std::this_thread::sleep_for(1234ms);
    serialSend("AImaster: Link active\n");
    std::this_thread::sleep_for(1234ms);
    serialSend("AImaster: Online\n");
    std::this_thread::sleep_for(1234ms);
    serialSend(": --> Type help for help!\n");
    std::this_thread::sleep_for(1234ms);
    serialSend("AImaster: AIinterface active\n\n");
    std::this_thread::sleep_for(1234ms);
    emit_prompt();

    return true;
}

// Write one byte with a few retries. Returns true on success.
static bool write_byte(char c) {
    int attempts = 0;
    while (attempts < 3) {
        int written = sp_nonblocking_write(serial_port, &c, 1);
        if (written == 1) return true;
        if (written < 0) {
            char* msg = sp_last_error_message();
            if (msg) {
                std::cerr << "[Warning] serialSend write error: " << msg << std::endl;
                sp_free_error_message(msg);
            } else {
                std::cerr << "[Warning] serialSend write error (no message)" << std::endl;
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        attempts++;
    }
    return false;
}

void serialSend(const std::string& data) {
    if (!serial_available || serial_port == nullptr) return;

    const int delay_ms = serial_send_delay_ms.load(std::memory_order_relaxed);
    const int policy   = serial_newline_policy.load(std::memory_order_relaxed);
    const size_t n = data.size();

    for (size_t i = 0; i < n; ++i) {
        char c = data[i];

        if (c == '\n') {
            // Expand LF according to policy
            switch (policy) {
                case 0: // CRLF
                    if (!(i > 0 && data[i-1] == '\r')) (void)write_byte('\r');
                    (void)write_byte('\n');
                    break;
                case 1: // LFCR
                    (void)write_byte('\n');
                    (void)write_byte('\r');
                    break;
                case 2: // LF
                    (void)write_byte('\n');
                    break;
                case 3: // CR
                    (void)write_byte('\r');
                    break;
            }
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            continue;
        }

        // Normal byte
        (void)write_byte(c);

        // If user provided CRLF literally, avoid splitting delay between them
        if (c == '\r' && (i + 1 < n) && data[i+1] == '\n') {
            continue;
        }

        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
}

static inline void rstrip_crlf(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
}

bool serialReadLine(std::string& out, int timeout_ms) {
    out.clear();
    if (!serial_available || serial_port == nullptr) return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char byte = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        int waiting = sp_input_waiting(serial_port);
        if (waiting < 0) {
            char* msg = sp_last_error_message();
            if (msg) {
                std::cerr << "[Warning] serialReadLine error: " << msg << std::endl;
                sp_free_error_message(msg);
            }
            return false;
        }
        if (waiting == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        int n = sp_nonblocking_read(serial_port, &byte, 1);
        if (n == 1) {
            rx_buffer.push_back(byte);
            if (byte == '\n' || byte == '\r') {
                out = rx_buffer;
                rstrip_crlf(out);
                rx_buffer.clear();
                return true;
            }
            if (rx_buffer.size() > 4096) {
                out = rx_buffer;
                rstrip_crlf(out);
                rx_buffer.clear();
                return true;
            }
        } else if (n < 0) {
            char* msg = sp_last_error_message();
            if (msg) {
                std::cerr << "[Warning] serialReadLine read error: " << msg << std::endl;
                sp_free_error_message(msg);
            }
            return false;
        }
    }
    return false;
}

void startSerialListener(const std::function<void(const std::string&)>& on_line, int timeout_ms) {
    if (!serial_available || serial_port == nullptr) return;
    if (serial_thread_running.load()) return;

    line_callback = on_line;
    serial_thread_running.store(true);
    serial_thread = std::thread([timeout_ms]() {
        while (serial_thread_running.load()) {
            std::string line;
            if (serialReadLine(line, timeout_ms)) {
                if (!line.empty() && line_callback) {
                    try { line_callback(line); } catch (...) {}
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    });
    serial_thread.detach();
}

void stopSerialListener() { serial_thread_running.store(false); }

void closeSerial() {
    stopSerialListener();
    if (serial_port) {
        sp_close(serial_port);
        sp_free_port(serial_port);
        serial_port = nullptr;
    }
    serial_available = false;
}