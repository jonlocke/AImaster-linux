#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>

struct AppConfig {
    std::string serial_port;
    int baudrate = 0;
    std::string ollama_url;
    std::string ollama_model;
    std::string system_message;
    bool color_enabled = true;
    std::string color_user = "\033[38;5;208m";
    std::string color_assistant = "\033[32m";
    std::string color_system = "\033[34m";
    std::string color_warning = "\033[33m";
    std::string color_error = "\033[31m";
    std::string color_timestamp = "\033[90m";
    std::string color_reset = "\033[0m";
    std::map<std::string, std::string> commands;
};

bool loadConfig(const std::string& filename, AppConfig& config);
void loadCommands(const std::string& filename, AppConfig& config);

#endif
