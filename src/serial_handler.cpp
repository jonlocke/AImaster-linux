#include "serial_handler.h"
#include "chat_session.h"
#include "ollama_client.h"
#include <thread>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <libserialport.h>
#include <iostream>

bool serial_available = false;
static sp_port* serial_port = nullptr;

bool initSerial(const std::string& port, int baudrate) {
    if (port.empty() || baudrate <= 0) {
        serial_available = false;
        return false;
    }

    if (sp_get_port_by_name(port.c_str(), &serial_port) != SP_OK) {
        std::cerr << "[Warning] Could not open serial port: " << port << std::endl;
        serial_available = false;
        return false;
    }

    if (sp_open(serial_port, SP_MODE_READ_WRITE) != SP_OK) {
        std::cerr << "[Warning] Failed to open serial port: " << port << std::endl;
        sp_free_port(serial_port);
        serial_available = false;
        return false;
    }

    sp_set_baudrate(serial_port, baudrate);
    sp_set_bits(serial_port, 8);
    sp_set_parity(serial_port, SP_PARITY_NONE);
    sp_set_stopbits(serial_port, 1);
    sp_set_flowcontrol(serial_port, SP_FLOWCONTROL_NONE);

    serial_available = true;
    return true;
}

void serialSend(const std::string& data) {
    if (!serial_available || !serial_port) return;
    sp_nonblocking_write(serial_port, data.c_str(), data.size());
}

void closeSerial() {
    if (serial_port) {
        sp_close(serial_port);
        sp_free_port(serial_port);
        serial_port = nullptr;
    }
    serial_available = false;
}





bool serialReadLine(std::string &line_out) {
    static std::string buffer;
    char buf[256];
    int n = read(serial_fd, buf, sizeof(buf));
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!buffer.empty()) {
                    line_out = buffer;
                    buffer.clear();
                    return true;
                }
            } else {
                buffer.push_back(c);
            }
        }
    }
    return false;
}

bool start_serial_mode(AppConfig &config) {
    if (!initSerial(config.serial_port, config.baudrate)) {
        std::cerr << "[Error] Failed to open serial port: " << config.serial_port << std::endl;
        return false;
    }
    serial_available = true;
    std::cout << "[Serial Mode] Listening on " << config.serial_port << " at " << config.baudrate << " baud." << std::endl;

    std::string line;
    while (true) {
        if (!serialReadLine(line)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Trim whitespace
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        std::string cmd_lower = line;
        std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);

        if (cmd_lower == "quit") {
            serialSend("[Error] Can't quit on serial\n");
            continue;
        } else if (cmd_lower == "showchat") {
            if (conversation_history.empty()) {
                serialSend("[Conversation history is empty]\n");
            } else {
                for (auto &msg : conversation_history) {
                    serialSend(msg.role + ": " + msg.content + "\n");
                }
            }
        } else {
            conversation_history.push_back({"user", line});
            std::string assistant_reply;
            send_llm_request(config, line, assistant_reply);
            conversation_history.push_back({"assistant", assistant_reply});
            serialSend(assistant_reply + "\n");
        }
    }
    return true;
}
