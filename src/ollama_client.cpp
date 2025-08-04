#include "ollama_client.h"
#include <curl/curl.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <ctime>
#include "serial_handler.h"

struct StreamData {
    std::string collected;
    std::string raw_output;
};

static bool diagMode = false; // Diagnostic dump mode

// ---- Save Code Blocks ----
namespace {
    void saveCodeBlocks(const std::string& text) {
        namespace fs = std::filesystem;
        std::string delimiter = "```";
        size_t pos = 0;
        int blockCount = 0;

        while ((pos = text.find(delimiter, pos)) != std::string::npos) {
            size_t start = pos + delimiter.length();
            size_t end = text.find(delimiter, start);
            if (end == std::string::npos) break;

            std::string codeBlock = text.substr(start, end - start);
            codeBlock.erase(0, codeBlock.find_first_not_of(" \t\n\r"));
            codeBlock.erase(codeBlock.find_last_not_of(" \t\n\r") + 1);

            fs::create_directories("code");

            std::time_t t = std::time(nullptr);
            std::tm tm{};
        #if defined(_WIN32)
            localtime_s(&tm, &t);
        #else
            localtime_r(&t, &tm);
        #endif
            char filename[64];
            std::strftime(filename, sizeof(filename), "code/%Y%m%d_%H%M%S", &tm);

            std::ostringstream oss;
            oss << filename;
            if (blockCount > 0) oss << "_" << blockCount;
            oss << ".txt";

            std::ofstream outFile(oss.str());
            if (outFile.is_open()) {
                outFile << codeBlock;
                outFile.close();
                std::cout << "[Saved code block to " << oss.str() << "]\n";
            }

            blockCount++;
            pos = end + delimiter.length();
        }
    }
}

// ---- Streaming Callback ----
static size_t StreamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string chunk((char*)contents, totalSize);

    // Show raw chunk in DIAG mode
    if (diagMode) {
        std::cerr << "\n[DIAG CHUNK] " << chunk << std::endl;
    }

    StreamData* data = (StreamData*)userp;
    data->raw_output += chunk;

    Json::CharReaderBuilder reader;
    Json::Value parsed;
    std::string errs;
    std::istringstream ss(chunk);

    if (Json::parseFromStream(reader, ss, &parsed, &errs)) {
        if (parsed.isObject() &&
            parsed.isMember("message") &&
            parsed["message"].isObject() &&
            parsed["message"].isMember("content")) {

            std::string text = parsed["message"]["content"].asString();

            // Always show streaming output in console
            std::cout << text << std::flush;

            // Append to log file in console mode
            if (!serial_available) {
                std::ofstream log("log.txt", std::ios::app);
                if (log.is_open()) {
                    log << text;
                    log.flush();
                }
            }

            data->collected += text;
        }
    }
    return totalSize;
}

// ---- Helper for common curl options ----
static void setCurlStreamingOptions(CURL* curl, struct curl_slist*& headers) {
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:"); // disable Expect: 100-continue
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamCallback);
}

// ---- Process Command ----
Json::Value processCommand(const std::string& command, const AppConfig& config) {
    static std::vector<Json::Value> chatHistory; // multi-turn chat state
    Json::Value result;

    std::string cmd_upper = command;
    std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);

    // ===== ASK Command =====
    if (cmd_upper == "ASK" || cmd_upper.rfind("ASK ", 0) == 0) {
        std::string query;
        if (cmd_upper == "ASK") {
            std::cout << "Enter your question: ";
            std::getline(std::cin, query);
        } else {
            query = command.substr(4);
        }

        Json::Value msg;
        msg["role"] = "user";
        msg["content"] = query;
        chatHistory.push_back(msg);

        CURL* curl = curl_easy_init();
        if (curl) {
            StreamData streamData;
            Json::Value payload;
            payload["model"] = config.ollama_model;
            payload["messages"] = Json::arrayValue;
            for (auto& m : chatHistory) payload["messages"].append(m);
            payload["stream"] = true;

            Json::StreamWriterBuilder wbuilder;
            std::string jsonPayload = Json::writeString(wbuilder, payload);

            if (diagMode) {
                std::cerr << "\n[DIAG URL] " << config.ollama_url << "\n";
                std::cerr << "[DIAG PAYLOAD] " << jsonPayload << "\n";
            }
            std::cout << "[Waiting for response from Ollama...]" << std::endl;

            curl_easy_setopt(curl, CURLOPT_URL, config.ollama_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &streamData);

            struct curl_slist* headers = NULL;
            setCurlStreamingOptions(curl, headers);

            CURLcode res = curl_easy_perform(curl);

            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            std::cout << std::endl; // newline after streaming

            if (res == CURLE_OK) {
                if (!streamData.collected.empty()) {
                    result["status"] = "success";
                    result["ollama_output"] = streamData.collected;

                    Json::Value reply;
                    reply["role"] = "assistant";
                    reply["content"] = streamData.collected;
                    chatHistory.push_back(reply);

                    saveCodeBlocks(streamData.collected);
                } else {
                    std::cout << "[HTTP Status] " << http_code << std::endl;
                    Json::CharReaderBuilder rbuilder;
                    Json::Value parsed;
                    std::string errs;
                    std::istringstream sraw(streamData.raw_output);
                    if (Json::parseFromStream(rbuilder, sraw, &parsed, &errs) &&
                        parsed.isMember("error")) {
                        std::string errMsg = parsed["error"].asString();
                        std::cout << "[Error] " << errMsg << std::endl;
                        result["status"] = "error";
                        result["message"] = errMsg;
                    } else {
                        std::string noOutputMsg = "[No output received from model]";
                        std::cout << noOutputMsg << std::endl;
                        result["status"] = "warning";
                        result["message"] = noOutputMsg;
                    }
                }
            } else {
                std::cout << "[CURL Error] " << curl_easy_strerror(res) << std::endl;
                result["status"] = "error";
                result["message"] = curl_easy_strerror(res);
            }
        }
    }

    // ===== READ Command =====
    else if (cmd_upper == "READ") {
        std::string context;
        std::cout << "Enter context: ";
        std::getline(std::cin, context);

        std::string filename;
        std::cout << "Enter filename: ";
        std::getline(std::cin, filename);

        std::ifstream inFile(filename);
        if (!inFile) {
            result["status"] = "error";
            result["message"] = "Could not open file: " + filename;
            return result;
        }
        std::stringstream buffer;
        buffer << inFile.rdbuf();
        std::string fileContents = buffer.str();

        std::string fullMessage =
            "Context: " + context +
            "\n\nFile contents:\n" + fileContents +
            "\n\nInstruction: Please read and store this content for later reference in our ongoing conversation. "
            "Acknowledge once you have absorbed it.";

        Json::Value msg;
        msg["role"] = "user";
        msg["content"] = fullMessage;
        chatHistory.push_back(msg);

        CURL* curl = curl_easy_init();
        if (curl) {
            StreamData streamData;
            Json::Value payload;
            payload["model"] = config.ollama_model;
            payload["messages"] = Json::arrayValue;
            for (auto& m : chatHistory) payload["messages"].append(m);
            payload["stream"] = true;

            Json::StreamWriterBuilder wbuilder;
            std::string jsonPayload = Json::writeString(wbuilder, payload);

            if (diagMode) {
                std::cerr << "\n[DIAG URL] " << config.ollama_url << "\n";
                std::cerr << "[DIAG PAYLOAD] " << jsonPayload << "\n";
            }
            std::cout << "[Waiting for response from Ollama...]" << std::endl;

            curl_easy_setopt(curl, CURLOPT_URL, config.ollama_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &streamData);

            struct curl_slist* headers = NULL;
            setCurlStreamingOptions(curl, headers);

            CURLcode res = curl_easy_perform(curl);

            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            std::cout << std::endl; // newline after streaming

            if (res == CURLE_OK) {
                if (!streamData.collected.empty()) {
                    result["status"] = "success";
                    result["ollama_output"] = streamData.collected;

                    Json::Value reply;
                    reply["role"] = "assistant";
                    reply["content"] = streamData.collected;
                    chatHistory.push_back(reply);

                    saveCodeBlocks(streamData.collected);
                } else {
                    if (diagMode) {
                        std::cerr << "[No chunks received from Ollama]\n";
                    }
                    std::cout << "[HTTP Status] " << http_code << std::endl;
                    Json::CharReaderBuilder rbuilder;
                    Json::Value parsed;
                    std::string errs;
                    std::istringstream sraw(streamData.raw_output);
                    if (Json::parseFromStream(rbuilder, sraw, &parsed, &errs) &&
                        parsed.isMember("error")) {
                        std::string errMsg = parsed["error"].asString();
                        std::cout << "[Error] " << errMsg << std::endl;
                        result["status"] = "error";
                        result["message"] = errMsg;
                    } else {
                        std::string noOutputMsg = "[No output received from model]";
                        std::cout << noOutputMsg << std::endl;
                        result["status"] = "warning";
                        result["message"] = noOutputMsg;
                    }
                }
            } else {
                std::cout << "[CURL Error] " << curl_easy_strerror(res) << std::endl;
                result["status"] = "error";
                result["message"] = curl_easy_strerror(res);
            }
        }
    }

    // ===== WHO Command =====
    else if (cmd_upper == "WHO") {
        result["status"] = "success";
        result["serial_port"] = config.serial_port;
        result["baudrate"] = config.baudrate;
        result["ollama_url"] = config.ollama_url;
        result["ollama_model"] = config.ollama_model;
    }

    // ===== HELP Command =====
    else if (cmd_upper == "HELP") {
        result["status"] = "success";
        Json::Value cmds(Json::objectValue);

        if (!config.commands.empty()) {
            for (const auto& [cmd, desc] : config.commands) {
                cmds[cmd] = desc;
            }
        } else {
            cmds["ASK"]   = "Ask the model a question and receive an answer.";
            cmds["READ"]  = "Provide context and send a file's contents to the model.";
            cmds["RESET"] = "Clear the current chat history.";
            cmds["WHO"]   = "Show current configuration.";
            cmds["HELP"]  = "List available commands.";
            cmds["DIAG"]  = "Toggle raw chunk dumping from Ollama (DIAG, DIAG ON, DIAG OFF).";
        }
        result["commands"] = cmds;

        std::cout << "\nAvailable commands:\n";
        for (auto& key : cmds.getMemberNames()) {
            std::cout << "  " << key << " - " << cmds[key].asString() << "\n";
        }
        std::cout << std::endl;
    }

    // ===== DIAG Command =====
    else if (cmd_upper.rfind("DIAG", 0) == 0) {
        std::string arg;
        if (command.size() > 4) {
            arg = command.substr(5);
            std::transform(arg.begin(), arg.end(), arg.begin(), ::toupper);
        }

        if (arg == "ON") {
            diagMode = true;
        } else if (arg == "OFF") {
            diagMode = false;
        } else if (arg.empty()) {
            diagMode = !diagMode;
        }

        result["status"] = "success";
        result["message"] = std::string("Diagnostic mode ") + (diagMode ? "ON" : "OFF");
        std::cout << "[Diagnostic mode " << (diagMode ? "ON" : "OFF") << "]\n";
    }

    // ===== RESET Command =====
    else if (cmd_upper == "RESET") {
        chatHistory.clear();
        result["status"] = "success";
        result["message"] = "Chat history cleared.";
    }

    // ===== Default =====
    else {
        result["status"] = "success";
        result["message"] = "Command: " + command;
    }

    if (!serial_available) {
        std::ofstream log("log.txt", std::ios::app);
        if (log.is_open()) {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "  ";
            log << Json::writeString(writer, result) << std::endl;
            log.flush();
        }
    }

    return result;
}
