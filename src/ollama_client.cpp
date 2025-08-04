#include "ollama_client.h"
#include <curl/curl.h>
#include <algorithm>
#include <iostream>
#include <sstream>

struct StreamData {
    std::string collected;
    std::string raw_output; // store all chunks just in case
};

// Callback for streamed data from Ollama
static size_t StreamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string chunk((char*)contents, totalSize);
    StreamData* data = (StreamData*)userp;

    // Keep raw output for fallback parsing
    data->raw_output += chunk;

    Json::CharReaderBuilder reader;
    Json::Value parsed;
    std::string errs;
    std::istringstream ss(chunk);

    if (Json::parseFromStream(reader, ss, &parsed, &errs)) {
        if (parsed.isMember("response")) {
            std::string text = parsed["response"].asString();
            std::cout << text << std::flush; // live output
            data->collected += text;
        }
    }
    return totalSize;
}

Json::Value processCommand(const std::string& command, const AppConfig& config) {
    Json::Value result;

    // Normalize command to uppercase for matching
    std::string cmd_upper = command;
    std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);

    if (cmd_upper == "ASK" || cmd_upper.rfind("ASK ", 0) == 0) {
        std::string query;
        if (cmd_upper == "ASK") {
            std::cout << "Enter your question: ";
            std::getline(std::cin, query);
        } else {
            query = command.substr(4);
        }

        CURL* curl = curl_easy_init();
        if (curl) {
            StreamData streamData;

            curl_easy_setopt(curl, CURLOPT_URL, config.ollama_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);

            // Keep streaming true for live output
            std::string jsonPayload = "{\"model\":\"" + config.ollama_model +
                                      "\",\"prompt\":\"" + query + "\",\"stream\":true}";
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());

            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &streamData);

            CURLcode res = curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            std::cout << std::endl; // end of live output

            if (res == CURLE_OK) {
                if (!streamData.collected.empty()) {
                    // We streamed some output successfully
                    result["status"] = "success";
                    result["ollama_output"] = streamData.collected;
                } else {
                    // Fallback: parse full raw_output for "response"
                    Json::CharReaderBuilder reader;
                    Json::Value parsed;
                    std::string errs;
                    std::istringstream ss(streamData.raw_output);
                    if (Json::parseFromStream(reader, ss, &parsed, &errs)) {
                        if (parsed.isMember("response")) {
                            result["status"] = "success";
                            result["ollama_output"] = parsed["response"].asString();
                        } else {
                            result["status"] = "error";
                            result["message"] = "No 'response' field in Ollama output.";
                        }
                    } else {
                        result["status"] = "error";
                        result["message"] = "Failed to parse Ollama output: " + errs;
                    }
                }
            } else {
                result["status"] = "error";
                result["message"] = curl_easy_strerror(res);
            }
        }
    }
    else if (cmd_upper == "WHO") {
        result["status"] = "success";
        result["serial_port"] = config.serial_port;
        result["baudrate"] = config.baudrate;
        result["ollama_url"] = config.ollama_url;
        result["ollama_model"] = config.ollama_model;
    }
    else if (cmd_upper == "HELP") {
        result["status"] = "success";
        Json::Value cmds(Json::objectValue);
        for (const auto& [cmd, desc] : config.commands) {
            cmds[cmd] = desc;
        }
        result["commands"] = cmds;
    }
    else {
        result["status"] = "success";
        result["message"] = "Command: " + command;
    }

    return result;
}
