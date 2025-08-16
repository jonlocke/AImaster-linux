#include "serial_handler.h"

#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>

#include <libserialport.h>

using namespace std::chrono_literals;

// ---- state ----
bool serial_available = false;
static sp_port* g_port = nullptr;
static std::atomic<bool> g_run{false};
static std::thread g_thread;
static std::mutex g_write_mx;

// ---- helpers ----
static bool set_port_cfg(sp_port* p, int baud)
{
    if (sp_set_baudrate(p, baud) != SP_OK) return false;
    if (sp_set_bits(p, 8) != SP_OK) return false;
    if (sp_set_parity(p, SP_PARITY_NONE) != SP_OK) return false;
    if (sp_set_stopbits(p, 1) != SP_OK) return false;
    if (sp_set_flowcontrol(p, SP_FLOWCONTROL_NONE) != SP_OK) return false;
    // Short timeouts keep the read loop responsive.
    if (sp_set_read_timeout(p, 50) != SP_OK) return false;   // ms
    if (sp_set_write_timeout(p, 200) != SP_OK) return false; // ms
    return true;
}

bool initSerial(const std::string& port, int baudrate)
{
    // Clean up any previous instance
    if (g_port) closeSerial();

    if (sp_get_port_by_name(port.c_str(), &g_port) != SP_OK) {
        std::fprintf(stderr, "[Serial] Port not found: %s\n", port.c_str());
        g_port = nullptr;
        serial_available = false;
        return false;
    }
    if (sp_open(g_port, SP_MODE_READ_WRITE) != SP_OK) {
        std::fprintf(stderr, "[Serial] Failed to open: %s\n", port.c_str());
        sp_free_port(g_port);
        g_port = nullptr;
        serial_available = false;
        return false;
    }
    if (!set_port_cfg(g_port, baudrate)) {
        std::fprintf(stderr, "[Serial] Failed to configure %s @ %d\n", port.c_str(), baudrate);
        sp_close(g_port);
        sp_free_port(g_port);
        g_port = nullptr;
        serial_available = false;
        return false;
    }

    serial_available = true;
    std::fprintf(stderr, "[Serial] Opened %s @ %d  (8N1, no flow)\n", port.c_str(), baudrate);
    // Friendly init prompt to peer
    serialSend("AImaster: Serial link active\r\n-> ");
    return true;
}

bool serialSend(const std::string& data)
{
    if (!serial_available || !g_port) return false;
    std::lock_guard<std::mutex> lock(g_write_mx);
    int wrote = sp_blocking_write(g_port, data.data(), (int)data.size(), 200);
    if (wrote < 0) {
        std::fprintf(stderr, "[Serial] Write error: %d\n", wrote);
        return false;
    }
    return true;
}

static inline void strip_crlf(std::string& s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
}

bool serialReadLine(std::string& out, int timeout_ms)
{
    out.clear();
    if (!serial_available || !g_port) return false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char ch = 0;
    bool got_any = false;

    while (std::chrono::steady_clock::now() < deadline) {
        int r = sp_blocking_read(g_port, &ch, 1, 50); // poll in 50ms slices
        if (r == 1) {
            got_any = true;
            // Treat either CR or LF as end-of-line, collapse CRLF into one
            if (ch == '\n' || ch == '\r') {
                // Drain a possible partner in CRLF
                char peek = 0;
                int r2 = sp_blocking_read(g_port, &peek, 1, 5);
                if (r2 == 1) {
                    if (!((ch == '\r' && peek == '\n') || (ch == '\n' && peek == '\r'))) {
                        // Not part of CRLF pair; push back into buffer by stashing in a static lookahead
                        // (libserialport has no unread, so we keep it in out and it will be at start of next line)
                        out.push_back(peek);
                    }
                }
                strip_crlf(out);
                return true;
            } else {
                out.push_back(ch);
            }
        } else if (r == 0) {
            // timeout slice, continue until overall deadline
        } else {
            // error
            std::fprintf(stderr, "[Serial] Read error: %d\n", r);
            return false;
        }
    }
    // If we collected bytes but no terminator, still return the partial line
    if (got_any && !out.empty()) {
        strip_crlf(out);
        return true;
    }
    return false;
}

void startSerialListener(const std::function<void(const std::string&)>& cb)
{
    if (!serial_available || !g_port) {
        std::fprintf(stderr, "[Serial] startSerialListener called before initSerial.\n");
        return;
    }
    // Stop any previous thread
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();

    g_run.store(true);
    g_thread = std::thread([cb](){
        std::fprintf(stderr, "[Serial] Listener started.\n");
        std::string line;
        while (g_run.load()) {
            if (serialReadLine(line, 250)) {
                if (!line.empty()) {
                    std::fprintf(stderr, "[SER->RX] line='%s'\n", line.c_str());
                    // Optional ACK so you can see life on the device
                    serialSend("[ACK RX]\r\n");
                    try {
                        cb(line);
                    } catch (...) {
                        std::fprintf(stderr, "[Serial] Listener callback threw.\n");
                    }
                    serialSend("-> ");
                }
            }
            // small breather to avoid tight loop when idle
            std::this_thread::sleep_for(2ms);
        }
        std::fprintf(stderr, "[Serial] Listener stopped.\n");
    });
}

void closeSerial()
{
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();

    if (g_port) {
        sp_close(g_port);
        sp_free_port(g_port);
        g_port = nullptr;
    }
    serial_available = false;
    std::fprintf(stderr, "[Serial] Closed.\n");
}
