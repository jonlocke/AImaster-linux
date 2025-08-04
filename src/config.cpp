#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

bool loadConfig(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Error: Could not open config file: " << filename << "\n";
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        line.erase(remove_if(line.begin(), line.end(), ::isspace), line.end());
        if (line.empty() || line[0] == '#') continue;

        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        if (key == "SERIAL_PORT") config.serial_port = value;
        else if (key == "BAUDRATE") config.baudrate = std::stoi(value);
        else if (key == "OLLAMA_URL") config.ollama_url = value;
        else if (key == "MODEL") config.ollama_model = value;
    }
    return true;
}

bool loadCommands(const std::string& filename, AppConfig& config) {
    std::ifstream file(filename);
    if (!file) {
        std::cerr << "Warning: Could not open commands file: " << filename << "\n";
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string cmd = line.substr(0, pos);
        std::string desc = line.substr(pos + 1);

        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
        config.commands[cmd] = desc;
    }
    return true;
}
