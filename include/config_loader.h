#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>

struct AppConfig {
    // Serial
    std::string serial_port;
    int baudrate = 2400;
    int serial_delay_ms = 50;
    std::string serial_newline = "CRLF"; // NEW: CRLF (default), LFCR, LF, CR

    // Ollama
    std::string ollama_url = "http://localhost:11434";
    std::string ollama_model = "gemma3:4b";
    long ollama_timeout_seconds = 5;

    // Commands
    std::map<std::string, std::string> commands;
    std::string commands_csv_path;
};

bool loadConfig(const std::string& path, AppConfig& out);
bool saveConfig(const std::string& path, const AppConfig& cfg);

#endif
