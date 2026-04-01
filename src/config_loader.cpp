#include "config_loader.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <vector>
#include <cstdlib>

static inline void rtrim(std::string& s) { while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); }
static inline void ltrim(std::string& s) { size_t i=0; while (i<s.size() && std::isspace((unsigned char)s[i])) ++i; if (i) s.erase(0,i); }
static inline std::string trim_copy(std::string s){ ltrim(s); rtrim(s); return s; }

static bool parse_int(const std::string& s, int& out) {
    try { size_t idx=0; int v=std::stoi(s,&idx,10); if (idx!=s.size()) return false; out=v; return true; }
    catch(...) { return false; }
}
static bool parse_long(const std::string& s, long& out) {
    try { size_t idx=0; long v=std::stol(s,&idx,10); if (idx!=s.size()) return false; out=v; return true; }
    catch(...) { return false; }
}
static bool parse_double(const std::string& s, double& out) {
    try { size_t idx=0; double v=std::stod(s,&idx); if (idx!=s.size()) return false; out=v; return true; }
    catch(...) { return false; }
}

static bool parse_bool(const std::string& s, bool& out) {
    std::string v = s;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c){ return std::tolower(c); });
    if (v=="1" || v=="true" || v=="yes" || v=="on") { out=true; return true; }
    if (v=="0" || v=="false" || v=="no" || v=="off") { out=false; return true; }
    return false;
}

static void parse_extra_header(const std::string& input, std::map<std::string,std::string>& out) {
    auto pos = input.find(':');
    if (pos == std::string::npos) return;
    std::string key = trim_copy(input.substr(0, pos));
    std::string value = trim_copy(input.substr(pos + 1));
    if (!key.empty()) out[key] = value;
}

static void load_commands_csv(const std::string& path, std::map<std::string,std::string>& out_map) {
    std::ifstream in(path);
    if (!in) { std::cerr << "[Warning] commands_csv not found: " << path << "\n"; return; }
    std::string line;
    while (std::getline(in, line)) {
        auto cpos = line.find_first_of("#;");
        if (cpos != std::string::npos) line.erase(cpos);
        line = trim_copy(line);
        if (line.empty()) continue;
        std::string cmd, desc; std::stringstream ss(line);
        if (!std::getline(ss, cmd, ',')) continue;
        std::getline(ss, desc);
        cmd = trim_copy(cmd); desc = trim_copy(desc);
        if (!cmd.empty()) out_map[cmd] = desc;
    }
}

bool loadConfig(const std::string& path, AppConfig& out) {
    std::ifstream in(path);
    if (!in) { std::cerr << "[Warning] Could not open config file: " << path << "\n"; return false; }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && (unsigned char)line[0]==0xEF) {
            if (line.size()>=3 && (unsigned char)line[1]==0xBB && (unsigned char)line[2]==0xBF) line.erase(0,3);
        }
        auto comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) line.erase(comment_pos);
        line = trim_copy(line);
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_copy(line.substr(0, eq));
        std::string val = trim_copy(line.substr(eq + 1));

        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c){ return std::tolower(c); });

        if (key == "serial_port")       out.serial_port = val;
        else if (key == "baudrate")    { int v; if (parse_int(val, v)) out.baudrate = v; }
        else if (key == "serial_delay_ms") { int v; if (parse_int(val, v) && v>=0) out.serial_delay_ms = v; }
        else if (key == "serial_newline")  { out.serial_newline = val; }
        else if (key == "welcome_message_file") { out.welcome_message_file = val; }
        else if (key == "user_prompt_file") { out.user_prompt_file = val; }
        else if (key == "ollama_url")  out.ollama_url = val;
        else if (key == "ollama_model") out.ollama_model = val;
        else if (key == "ollama_timeout_seconds") { long v; if (parse_long(val, v) && v>=0) out.ollama_timeout_seconds = v; }
        else if (key == "provider_type") out.provider_type = val;
        else if (key == "api_base") out.api_base = val;
        else if (key == "api_key") out.api_key = val;
        else if (key == "model") out.model = val;
        else if (key == "timeout") { long v; if (parse_long(val, v) && v>=0) out.timeout = v; }
        else if (key == "organization") out.organization = val;
        else if (key == "project") out.project = val;
        else if (key == "temperature") { double v; if (parse_double(val, v)) out.temperature = v; }
        else if (key == "num_predict") { int v; if (parse_int(val, v) && v>0) out.num_predict = v; }
        else if (key == "format") out.format = val;
        else if (key == "weather_plugin_enabled") { bool v; if (parse_bool(val, v)) out.weather_plugin_enabled = v; }
        else if (key == "weather_geocoding_url") out.weather_geocoding_url = val;
        else if (key == "weather_forecast_url") out.weather_forecast_url = val;
        else if (key == "weather_timeout_seconds") { long v; if (parse_long(val, v) && v > 0) out.weather_timeout_seconds = v; }
        else if (key == "tts_endpoint_url") out.tts_endpoint_url = val;
        else if (key == "tts_timeout_seconds") { long v; if (parse_long(val, v) && v > 0) out.tts_timeout_seconds = v; }
        else if (key == "tts_voice") out.tts_voice = val;
        else if (key == "tts_speaker") out.tts_speaker = val;
        else if (key == "tts_output_device") out.tts_output_device = val;
        else if (key == "tts_enabled") { bool v; if (parse_bool(val, v)) out.tts_enabled = v; }
        else if (key == "mic_record_device") out.mic_record_device = val;
        else if (key == "mic_sample_rate") { int v; if (parse_int(val, v) && v > 0) out.mic_sample_rate = v; }
        else if (key == "mic_channels") { int v; if (parse_int(val, v) && v > 0) out.mic_channels = v; }
        else if (key == "stt_endpoint_url") out.stt_endpoint_url = val;
        else if (key == "stt_api_key") out.stt_api_key = val;
        else if (key == "stt_model") out.stt_model = val;
        else if (key == "stt_timeout_seconds") { long v; if (parse_long(val, v) && v > 0) out.stt_timeout_seconds = v; }
        else if (key == "stt_command") out.stt_command = val;
        else if (key == "hid_input_device") out.hid_input_device = val;
        else if (key == "hid_input_name") out.hid_input_name = val;
        else if (key == "hid_button_code") { int v; if (parse_int(val, v) && v >= 0) out.hid_button_code = v; }
        else if (key == "hid_debounce_ms") { int v; if (parse_int(val, v) && v >= 0) out.hid_debounce_ms = v; }
        else if (key == "bluetooth_scan_seconds") { int v; if (parse_int(val, v) && v > 0) out.bluetooth_scan_seconds = v; }
        else if (key == "extra_header") parse_extra_header(val, out.extra_headers);
        else if (key == "rag_chunks") { int v; if (parse_int(val, v) && v > 0) out.rag_chunks = v; }
        else if (key == "rag_threshold") { double v; if (parse_double(val, v) && v >= 0.0) out.rag_threshold = v; }
        else if (key == "commands_csv") { out.commands_csv_path = val; load_commands_csv(val, out.commands); }
        else if (key == "serial_wrap_cols") {
    int v; if (parse_int(val, v)) {
        if (v < 10) v = 10;
        if (v > 240) v = 240;
        out.serial_wrap_cols = v;
    }
}

        else { /* ignore unknown */ }
    }

    if (const char* env = std::getenv("AIMASTER_TTS_ENABLED")) { bool v; if (parse_bool(env, v)) out.tts_enabled = v; }
    if (const char* env = std::getenv("AIMASTER_TTS_ENDPOINT")) out.tts_endpoint_url = env;
    if (const char* env = std::getenv("AIMASTER_TTS_TIMEOUT_SECONDS")) { long v; if (parse_long(env, v) && v > 0) out.tts_timeout_seconds = v; }
    if (const char* env = std::getenv("AIMASTER_TTS_VOICE")) out.tts_voice = env;
    if (const char* env = std::getenv("AIMASTER_TTS_SPEAKER")) out.tts_speaker = env;
    if (const char* env = std::getenv("AIMASTER_TTS_OUTPUT_DEVICE")) out.tts_output_device = env;

    return true;
}

bool saveConfig(const std::string& path, const AppConfig& cfg) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) { std::cerr << "[Error] Could not write config file: " << path << "\n"; return false; }

    out << "# AImaster configuration (key=value)\n";
    out << "serial_port=" << cfg.serial_port << "\n";
    out << "baudrate=" << cfg.baudrate << "\n";
    out << "serial_delay_ms=" << cfg.serial_delay_ms << "\n";
    out << "serial_newline=" << cfg.serial_newline << "\n";
    out << "welcome_message_file=" << cfg.welcome_message_file << "\n";
    out << "user_prompt_file=" << cfg.user_prompt_file << "\n";
    out << "ollama_url=" << cfg.ollama_url << "\n";
    out << "ollama_model=" << cfg.ollama_model << "\n";
    out << "ollama_timeout_seconds=" << cfg.ollama_timeout_seconds << "\n";
    out << "provider_type=" << cfg.provider_type << "\n";
    if (!cfg.api_base.empty()) out << "api_base=" << cfg.api_base << "\n";
    if (!cfg.api_key.empty()) out << "api_key=" << cfg.api_key << "\n";
    if (!cfg.model.empty()) out << "model=" << cfg.model << "\n";
    if (cfg.timeout > 0) out << "timeout=" << cfg.timeout << "\n";
    if (!cfg.organization.empty()) out << "organization=" << cfg.organization << "\n";
    if (!cfg.project.empty()) out << "project=" << cfg.project << "\n";
    if (cfg.temperature >= 0.0) out << "temperature=" << cfg.temperature << "\n";
    if (cfg.num_predict > 0) out << "num_predict=" << cfg.num_predict << "\n";
    if (!cfg.format.empty()) out << "format=" << cfg.format << "\n";
    out << "weather_plugin_enabled=" << (cfg.weather_plugin_enabled ? "true" : "false") << "\n";
    if (!cfg.weather_geocoding_url.empty()) out << "weather_geocoding_url=" << cfg.weather_geocoding_url << "\n";
    if (!cfg.weather_forecast_url.empty()) out << "weather_forecast_url=" << cfg.weather_forecast_url << "\n";
    out << "weather_timeout_seconds=" << cfg.weather_timeout_seconds << "\n";
    out << "tts_enabled=" << (cfg.tts_enabled ? "true" : "false") << "\n";
    out << "tts_endpoint_url=" << cfg.tts_endpoint_url << "\n";
    out << "tts_timeout_seconds=" << cfg.tts_timeout_seconds << "\n";
    if (!cfg.tts_voice.empty()) out << "tts_voice=" << cfg.tts_voice << "\n";
    if (!cfg.tts_speaker.empty()) out << "tts_speaker=" << cfg.tts_speaker << "\n";
    if (!cfg.tts_output_device.empty()) out << "tts_output_device=" << cfg.tts_output_device << "\n";
    if (!cfg.mic_record_device.empty()) out << "mic_record_device=" << cfg.mic_record_device << "\n";
    out << "mic_sample_rate=" << cfg.mic_sample_rate << "\n";
    out << "mic_channels=" << cfg.mic_channels << "\n";
    if (!cfg.stt_endpoint_url.empty()) out << "stt_endpoint_url=" << cfg.stt_endpoint_url << "\n";
    if (!cfg.stt_api_key.empty()) out << "stt_api_key=" << cfg.stt_api_key << "\n";
    if (!cfg.stt_model.empty()) out << "stt_model=" << cfg.stt_model << "\n";
    out << "stt_timeout_seconds=" << cfg.stt_timeout_seconds << "\n";
    if (!cfg.stt_command.empty()) out << "stt_command=" << cfg.stt_command << "\n";
    if (!cfg.hid_input_device.empty()) out << "hid_input_device=" << cfg.hid_input_device << "\n";
    if (!cfg.hid_input_name.empty()) out << "hid_input_name=" << cfg.hid_input_name << "\n";
    if (cfg.hid_button_code >= 0) out << "hid_button_code=" << cfg.hid_button_code << "\n";
    out << "hid_debounce_ms=" << cfg.hid_debounce_ms << "\n";
    out << "bluetooth_scan_seconds=" << cfg.bluetooth_scan_seconds << "\n";
    for (const auto& kv : cfg.extra_headers) out << "extra_header=" << kv.first << ": " << kv.second << "\n";
    out << "rag_chunks=" << cfg.rag_chunks << "\n";
    out << "rag_threshold=" << cfg.rag_threshold << "\n";
    out << "serial_wrap_cols=" << cfg.serial_wrap_cols << "\n";
    if (!cfg.commands_csv_path.empty()) out << "commands_csv=" << cfg.commands_csv_path << "\n";
    else out << "# commands_csv=cmds.csv\n";
    return true;
}
