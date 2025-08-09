#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>

struct AppConfig {
    std::string serial_port;
    int baudrate = 0;
    std::string ollama_url;
    std::string ollama_model;
    int ollama_timeout_seconds = 2;
    std::map<std::string, std::string> commands; // command -> description
};

// Loads config.txt into AppConfig
// Returns true if successful, false otherwise
bool loadConfig(const std::string& filename, AppConfig& config);

// Loads commands from cmds.txt into AppConfig
// This is called by loadConfig if cmds.txt exists
void loadCommands(const std::string& filename, AppConfig& config);

#endif


// Saves the AppConfig back to the given filename. Returns true on success.
bool saveConfig(const std::string& filename, const AppConfig& config);
