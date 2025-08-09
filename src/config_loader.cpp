#include "config_loader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

bool loadConfig(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "[Error] Could not open config file: " << filename << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Remove comments and whitespace
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos)
            line = line.substr(0, commentPos);

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty()) continue;

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);

        // Trim
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        // Lowercase key
        std::string key_lower = key;
        std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);

        if (key_lower == "serial_port") {
            config.serial_port = value;
        } else if (key_lower == "baudrate") {
            try {
                config.baudrate = std::stoi(value);
            } catch (...) {
                std::cerr << "[Warning] Invalid baudrate value: " << value << std::endl;
            }
        } else if (key_lower == "ollama_url") {
            config.ollama_url = value;
        } else if (key_lower == "ollama_model") {
            config.ollama_model = value;
        }
    }

    // Load commands from cmds.txt if present
    if (fs::exists("cmds.txt")) {
        loadCommands("cmds.txt", config);
    }

    return true;
}

void loadCommands(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "[Warning] Could not open commands file: " << filename << std::endl;
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Remove comments
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos)
            line = line.substr(0, commentPos);

        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty()) continue;

        size_t commaPos = line.find(',');
        if (commaPos == std::string::npos) continue;

        std::string cmd = line.substr(0, commaPos);
        std::string desc = line.substr(commaPos + 1);

        cmd.erase(0, cmd.find_first_not_of(" \t\r\n"));
        cmd.erase(cmd.find_last_not_of(" \t\r\n") + 1);
        desc.erase(0, desc.find_first_not_of(" \t\r\n"));
        desc.erase(desc.find_last_not_of(" \t\r\n") + 1);

        config.commands[cmd] = desc;
    }
}


bool saveConfig(const std::string& filename, const AppConfig& config) {
    // Read existing lines to preserve ordering/comments
    std::ifstream in(filename);
    std::vector<std::string> lines;
    if (in) {
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    } else {
        lines = {
            "serial_port=" + config.serial_port,
            "baudrate=" + std::to_string(config.baudrate),
            "ollama_url=" + config.ollama_url,
            "ollama_model=" + config.ollama_model
        };
        std::ofstream out_new(filename);
        if (!out_new) return false;
        for (auto& l : lines) out_new << l << "\n";
        return true;
    }

    auto set_or_add = [&](const std::string& key, const std::string& value){
        std::string prefix = key + "=";
        bool found = false;
        for (auto& l : lines) {
            std::string t = l;
            auto notspace = [](int ch){ return !std::isspace(ch); };
            t.erase(t.begin(), std::find_if(t.begin(), t.end(), notspace));
            if (!t.empty() && t[0] == '#') continue;
            if (t.rfind(prefix, 0) == 0) {
                l = prefix + value;
                found = true;
                break;
            }
        }
        if (!found) lines.push_back(prefix + value);
    };

    set_or_add("serial_port", config.serial_port);
    set_or_add("baudrate", std::to_string(config.baudrate));
    set_or_add("ollama_url", config.ollama_url);
    set_or_add("ollama_model", config.ollama_model);
    set_or_add("ollama_timeout_seconds", std::to_string(config.ollama_timeout_seconds));

    std::ofstream out(filename, std::ios::trunc);
    if (!out) return false;
    for (auto& l : lines) out << l << "\n";
    return true;
}
