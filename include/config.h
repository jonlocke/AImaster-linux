#pragma once
#include <string>
#include <map>

struct AppConfig {
    std::string serial_port;
    int baudrate = 115200;
    std::string ollama_url;
    std::string ollama_model;
    std::map<std::string, std::string> commands; // CMD -> Description
};

bool loadConfig(const std::string& filename, AppConfig& config);
bool loadCommands(const std::string& filename, AppConfig& config);
