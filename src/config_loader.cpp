#include "config_loader.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

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
        else if (key == "ollama_url")  out.ollama_url = val;
        else if (key == "ollama_model") out.ollama_model = val;
        else if (key == "ollama_timeout_seconds") { long v; if (parse_long(val, v) && v>=0) out.ollama_timeout_seconds = v; }
        else if (key == "commands_csv") { out.commands_csv_path = val; load_commands_csv(val, out.commands); }
        else { /* ignore unknown */ }
    }

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
    out << "ollama_url=" << cfg.ollama_url << "\n";
    out << "ollama_model=" << cfg.ollama_model << "\n";
    out << "ollama_timeout_seconds=" << cfg.ollama_timeout_seconds << "\n";
    if (!cfg.commands_csv_path.empty()) out << "commands_csv=" << cfg.commands_csv_path << "\n";
    else out << "# commands_csv=cmds.csv\n";
    return true;
}
