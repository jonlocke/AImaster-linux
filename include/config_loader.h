#ifndef CONFIG_LOADER_H
#define CONFIG_LOADER_H

#include <string>
#include <map>
#include <json/json.h>

struct AppConfig {
    // Serial
    std::string serial_port;
    int baudrate = 2400;
    int serial_wrap_cols = 80; // wrap width for serial routing

    int serial_delay_ms = 50;
    std::string serial_newline = "CRLF"; // NEW: CRLF (default), LFCR, LF, CR
    std::string welcome_message_file = "/usr/share/aimaster/welcome.txt";

    // Ollama / provider compatibility
    std::string ollama_url = "http://localhost:11434";
    std::string ollama_model = "gemma3:4b";
    long ollama_timeout_seconds = 5;

    std::string provider_type = "ollama_native";
    std::string api_base;
    std::string api_key;
    std::string model;
    long timeout = 0;
    std::map<std::string, std::string> extra_headers;
    std::string organization;
    std::string project;
    double temperature = -1.0;
    int num_predict = -1;
    std::string format;
    Json::Value tools;
    Json::Value tool_choice;

    // Text-to-speech
    bool tts_enabled = false;
    std::string tts_endpoint_url = "http://127.0.0.1:8092/speak";
    long tts_timeout_seconds = 10;
    std::string tts_voice;
    std::string tts_speaker;

    // RAG retrieval defaults (used by ASK/INT when RAG is active)
    int rag_chunks = 25;
    double rag_threshold = 0.2;

    // Commands
    std::map<std::string, std::string> commands;
    std::string commands_csv_path;
};

bool loadConfig(const std::string& path, AppConfig& out);
bool saveConfig(const std::string& path, const AppConfig& cfg);

#endif
