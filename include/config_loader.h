#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>

struct AppConfig {
    // Serial
    std::string serial_port;        // e.g. /dev/ttyUSB0 or COM3
    int baudrate = 2400;
    int serial_delay_ms = 50;       // per-char delay for serialSend()

    // Ollama
    std::string ollama_url = "http://localhost:11434";
    std::string ollama_model = "gemma3:4b";
    long ollama_timeout_seconds = 5; // default network timeout

    // Commands (optional; populated from CSV if provided)
    std::map<std::string, std::string> commands; // cmd -> description
    std::string commands_csv_path; // where we loaded from (if any)
};

// Load configuration from key=value "config.txt".
// Recognized keys (case-insensitive):
//   serial_port, baudrate, serial_delay_ms,
//   ollama_url, ollama_model, ollama_timeout_seconds,
//   commands_csv (optional: path to CSV "cmd,description")
bool loadConfig(const std::string& path, AppConfig& out);

// Save a minimal config back to disk in key=value format.
bool saveConfig(const std::string& path, const AppConfig& cfg);

#endif // CONFIG_LOADER_H
