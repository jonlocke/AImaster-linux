#include "config_loader.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

static void replaceEscapes(std::string &s) {
    size_t pos = 0;
    while ((pos = s.find("\\033", pos)) != std::string::npos) {
        s.replace(pos, 4, "\x1B");
        pos += 1;
    }
}

bool loadConfig(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "[Error] Could not open config file: " << filename << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
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
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        std::string key_lower = key;
        std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);

        if (key_lower == "serial_port") config.serial_port = value;
        else if (key_lower == "baudrate") try { config.baudrate = std::stoi(value); } catch (...) {}
        else if (key_lower == "ollama_url") config.ollama_url = value;
        else if (key_lower == "ollama_model") config.ollama_model = value;
        else if (key_lower == "system_message") config.system_message = value;
        else if (key_lower == "color_enabled") {
            std::string v = value; std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            config.color_enabled = !(v == "false" || v == "0" || v == "no");
        }
        else if (key_lower == "color_user") { replaceEscapes(value); config.color_user = value; }
        else if (key_lower == "color_assistant") { replaceEscapes(value); config.color_assistant = value; }
        else if (key_lower == "color_system") { replaceEscapes(value); config.color_system = value; }
        else if (key_lower == "color_warning") { replaceEscapes(value); config.color_warning = value; }
        else if (key_lower == "color_error") { replaceEscapes(value); config.color_error = value; }
        else if (key_lower == "color_timestamp") { replaceEscapes(value); config.color_timestamp = value; }
        else if (key_lower == "color_reset") { replaceEscapes(value); config.color_reset = value; }
    }

    if (fs::exists("cmds.txt")) loadCommands("cmds.txt", config);
    return true;
}

void loadCommands(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file) return;
    std::string line;
    while (std::getline(file, line)) {
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
