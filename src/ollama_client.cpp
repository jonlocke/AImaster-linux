#include "ollama_client.h"
#include "config_loader.h"
#include "chat_session.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>
#include <curl/curl.h>
#include <jsoncpp/json/json.h>
#include <readline/readline.h>
#include <readline/history.h>

extern AppConfig config;

static std::string get_timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm_local{};
    localtime_r(&t, &tm_local);
    char buf[20];
    std::strftime(buf, sizeof(buf), "[%I:%M:%S %p]", &tm_local);
    return std::string(buf);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

void start_console_mode(AppConfig &cfg) {
    std::string input;
    while (true) {
        std::string prompt_str = "8[" + current_model + "]-> ";
        char *raw_input = readline(prompt_str.c_str());
        if (!raw_input) break; // NULL from readline means EOF / Ctrl-D
        input = raw_input;
        free(raw_input);

        if (input.empty()) continue;
        add_history(input.c_str());

        std::string cmd_lower = input;
        std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);

        if (cmd_lower == "quit") {
            std::cout << get_timestamp() << " [Exiting...]" << std::endl;
            break;
        } else if (cmd_lower == "showchat") {
            if (conversation_history.empty()) {
                std::cout << "[Conversation history is empty]" << std::endl;
            } else {
                for (auto &msg : conversation_history) {
                    std::string role_color = cfg.color_system;
                    if (msg.role == "user") role_color = cfg.color_user;
                    else if (msg.role == "assistant") role_color = cfg.color_assistant;
                    std::cout << get_timestamp() << " ";
                    if (cfg.color_enabled)
                        std::cout << "\033[1m" << role_color << msg.role << ":" << cfg.color_reset << " " << role_color << msg.content << cfg.color_reset << "\n";
                    else
                        std::cout << msg.role << ": " << msg.content << "\n";
                }
            }
        } else {
            // Send to LLM
            conversation_history.push_back({"user", input});
            std::string assistant_reply;
            send_llm_request(cfg, input, assistant_reply);
            conversation_history.push_back({"assistant", assistant_reply});
        }
    }
}

void send_llm_request(AppConfig &cfg, const std::string &prompt, std::string &full_response) {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    Json::Value root;
    root["model"] = current_model;
    root["messages"] = Json::arrayValue;
    for (auto &msg : conversation_history) {
        Json::Value m;
        m["role"] = msg.role;
        m["content"] = msg.content;
        root["messages"].append(m);
    }

    Json::StreamWriterBuilder writer;
    std::string json_data = Json::writeString(writer, root);

    std::string response_string;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, cfg.ollama_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    Json::CharReaderBuilder reader;
    Json::Value root_resp;
    std::string errs;
    std::istringstream s(response_string);
    if (Json::parseFromStream(reader, s, &root_resp, &errs)) {
        if (root_resp.isMember("choices") && root_resp["choices"].isArray() && !root_resp["choices"].empty()) {
            full_response = root_resp["choices"][0]["message"]["content"].asString();
            if (cfg.color_enabled)
                std::cout << cfg.color_assistant << full_response << cfg.color_reset << std::endl;
            else
                std::cout << full_response << std::endl;
        }
    }
}
