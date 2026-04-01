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
    std::string user_prompt_file = "/usr/share/aimaster/user_prompt.txt";

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
    bool weather_plugin_enabled = true;
    std::string weather_geocoding_url = "https://geocoding-api.open-meteo.com/v1/search";
    std::string weather_forecast_url = "https://api.open-meteo.com/v1/forecast";
    long weather_timeout_seconds = 15;

    // Text-to-speech
    bool tts_enabled = false;
    std::string tts_endpoint_url = "http://127.0.0.1:8092/speak";
    long tts_timeout_seconds = 10;
    std::string tts_voice;
    std::string tts_speaker;
    std::string tts_output_device = "plughw:0,0";

    // Microphone / speech-to-text
    std::string mic_record_device;
    int mic_sample_rate = 16000;
    int mic_channels = 1;
    std::string stt_endpoint_url;
    std::string stt_api_key;
    std::string stt_model;
    long stt_timeout_seconds = 60;
    std::string stt_command;
    std::string hid_input_device;
    std::string hid_input_name;
    int hid_button_code = -1;
    int hid_debounce_ms = 500;
    int bluetooth_scan_seconds = 8;

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
