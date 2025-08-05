#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <readline/readline.h>
#include <readline/history.h>
#include "config_loader.h"
#include "serial_handler.h"
#include "ollama_client.h"
#include "utils.h"
#include "chat_session.h"

namespace fs = std::filesystem;

int main() {
    AppConfig config;
    if (!loadConfig("config.txt", config)) {
        return 1;
    }

    // Initialize current model and seed system message
    current_model = config.ollama_model;
    if (!config.system_message.empty()) {
        conversation_history.push_back({"system", config.system_message});
    }

    // Start serial if available, otherwise console
    if (!config.serial_port.empty()) {
        if (!start_serial_mode(config)) {
            std::cerr << "[Warning] Could not open serial port: " << config.serial_port << std::endl;
            std::cout << "Warning: No serial port available. Using console mode." << std::endl;
            start_console_mode(config);
        }
    } else {
        start_console_mode(config);
    }

    return 0;
}
