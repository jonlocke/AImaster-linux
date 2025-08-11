#include "endpoint_utils.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <readline/readline.h>
#include <readline/history.h>
#include "config_loader.h"
#include "serial_handler.h"
#include "command_exec.h"
#include "ollama_client.h"
#include "utils.h"
#include <jsoncpp/json/json.h>
#include <curl/curl.h>
#include "rag_adapter.hpp"
#include "rag_state.hpp"
#include "rag_int_bridge.hpp"


namespace fs = std::filesystem;
static const char* HISTORY_FILE = "~/.ollama_cli_history";



// ---- Startup connectivity check for Ollama ----
static size_t curl_discard_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    // We don't need the body for a ping check
    return size * nmemb;
}

static bool check_ollama_connectivity(const std::string& chat_url, long timeout_seconds, long* http_code_out=nullptr) {
    std::string url = EndpointResolver::deriveTagsEndpoint(chat_url);
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds); // overall timeout
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (http_code_out) *http_code_out = code;
        ok = (code >= 200 && code < 500); // if server responds at all, consider reachable
    }
    curl_easy_cleanup(curl);
    return ok;
}


// ---- MODEL command helpers ----
static size_t curl_write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append((char*)contents, total);
    return total;
}

static std::vector<std::string> fetch_ollama_models(const std::string& chat_url, std::string& error) {
    std::vector<std::string> models;
    std::string url = EndpointResolver::deriveTagsEndpoint(chat_url);

    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl init failed"; return models; }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        error = std::string("curl error: ") + curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return models;
    }
    curl_easy_cleanup(curl);

    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    std::istringstream iss(response);
    if (!Json::parseFromStream(b, iss, &root, &errs)) {
        error = "JSON parse error: " + errs;
        return models;
    }
    if (root.isMember("models") && root["models"].isArray()) {
        for (const auto& m : root["models"]) {
            if (m.isMember("name") && m["name"].isString()) {
                models.push_back(m["name"].asString());
            }
        }
    } else {
        error = "unexpected response";
    }
    return models;
}


// Expand tilde in file paths
std::string expandTilde(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

// --- Completion function ---
char* command_generator(const char* text, int state) {
    static size_t list_index;
    static std::vector<std::string> matches;

    if (state == 0) {
        matches.clear();
        list_index = 0;

        std::string buffer(rl_line_buffer);
        size_t spacePos = buffer.find(' ');

        if (spacePos != std::string::npos) {
            std::string cmd = buffer.substr(0, spacePos);
            if (cmd == "READ") {
                std::string partial(text);
                std::string dir = ".";
                std::string prefix = partial;

                size_t slashPos = partial.find_last_of('/');
                if (slashPos != std::string::npos) {
                    dir = partial.substr(0, slashPos);
                    prefix = partial.substr(slashPos + 1);
                }

                if (fs::exists(dir) && fs::is_directory(dir)) {
                    for (const auto& entry : fs::directory_iterator(dir)) {
                        std::string fname = entry.path().filename().string();
                        if (fname.find(prefix) == 0) {
                            matches.push_back(dir + "/" + fname);
                        }
                    }
                }
            }
        }
    }

    if (list_index < matches.size()) {
        return strdup(matches[list_index++].c_str());
    }
    return nullptr;
}

char** custom_completion(const char* text, int start, int end) {
    (void)end;
    if (start > 0) {
        return rl_completion_matches(text, command_generator);
    }
    return nullptr;
}

int main() {
    AppConfig config;
    if (!loadConfig("config.txt", config)) {
        std::cerr << "Error loading config.txt" << std::endl;
        return 1;
    }
    // Ping Ollama server with 2s timeout (non-fatal)
    {
        long http_code = 0;
        bool ok = check_ollama_connectivity(config.ollama_url, 2, &http_code);
        if (!ok) {
            std::cout << "[Warning] Could not reach Ollama at " << config.ollama_url
                      << " within 2 seconds. Some commands may not work.\n";
        }
    }
setSerialNewlinePolicy(config.serial_newline);  // NEW (CRLF/LFCR/LF/CR)
setSerialSendDelay(config.serial_delay_ms);      // <- make sure this line exists


    if (!initSerial(config.serial_port, config.baudrate)) {
        std::cerr << "Warning: No serial port available. Using console mode." << std::endl;
    }
if (serial_available) {
    startSerialListener([&](const std::string& line) {
        try {
            (void)execute_command(line, config, CommandSource::SERIAL);
        } catch (...) {
            std::cerr << "[Warning] Exception while processing serial command\n";
        }
    }, /*timeout_ms=*/50);
}

    // Set up tab completion
    rl_attempted_completion_function = custom_completion;

    // Load persistent history
    std::string histFile = expandTilde(HISTORY_FILE);
    read_history(histFile.c_str());

    std::cout << "\033[38;2;255;215;0mAImaster CLI\033[0m\n\033[38;2;255;239;184mType HELP for a list of commands.\033[0m\n";

    while (true) {
        std::string prompt = "\033[38;2;255;239;184m" + config.ollama_model + "> \033[0m";
        char* input = readline(prompt.c_str());
        if (!input) break;

        if (*input) {
            add_history(input);
            write_history(histFile.c_str());
        }

        std::string command(input);
        free(input);

        std::string cmd_upper = command;
        std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);

        // If READ entered with no args → interactive picker
        if (cmd_upper == "READ") {
            std::string context;
            std::cout << "Enter context: ";
            std::getline(std::cin, context);

            std::string filename = pickFile("code");
            if (filename.empty()) continue;

            command = "READ_CTX:" + context + "|FILE:" + filename;
        }

        Json::Value result = execute_command(command, config, CommandSource::CONSOLE);
    }

    // Save history on exit
    write_history(histFile.c_str());

    return 0;
}
