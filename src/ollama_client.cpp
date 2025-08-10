#include "ollama_client.h"
#include <curl/curl.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <ctime>
#include "serial_handler.h"
#include "rag_console_commands.hpp"
#include "rag_int_bridge.hpp"



// ---- One-time connectivity check on first command ----
static size_t ocurl_discard_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    return size * nmemb;
}
static std::string oc_derive_tags_endpoint(const std::string& chat_url) {
    auto pos = chat_url.find("/api/");
    if (pos == std::string::npos) return chat_url;
    return chat_url.substr(0, pos) + "/api/tags";
}
static bool oc_check_ollama_connectivity(const std::string& chat_url, long timeout_seconds, long* http_code_out=nullptr) {
    std::string url = oc_derive_tags_endpoint(chat_url);
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ocurl_discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (http_code_out) *http_code_out = code;
        ok = (code >= 200 && code < 500);
    }
    curl_easy_cleanup(curl);
    return ok;
}
static bool g_oc_ping_done = false;

// ---- MODEL listing helpers (Ollama tags) ----
static size_t ocurl_write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append((char*)contents, total);
    return total;
}

static std::string oderive_tags_endpoint(const std::string& chat_url) {
    auto pos = chat_url.find("/api/");
    if (pos == std::string::npos) return chat_url;
    return chat_url.substr(0, pos) + "/api/tags";
}

static std::vector<std::string> fetch_ollama_models(const std::string& chat_url, std::string& error) {
    std::vector<std::string> models;
    std::string url = oderive_tags_endpoint(chat_url);

    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl init failed"; return models; }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ocurl_write_to_string);
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
        error = std::string("JSON parse error: ") + errs;
        return models;
    }
    if (root.isMember("models") && root["models"].isArray()) {
        for (const auto& m : root["models"]) {
            if (m.isMember("name") && m["name"].isString()) {
                models.push_back(m["name"].asString());
            } else if (m.isMember("model") && m["model"].isString()) {
                models.push_back(m["model"].asString());
            }
        }
    } else {
        error = "unexpected response";
    }
    return models;
}

struct StreamData {
    std::string collected;
    std::string raw_output;
    std::chrono::high_resolution_clock::time_point start_time;
    bool first_chunk_received = false;
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

            // Prepend "# " to the first line
            if (!codeBlock.empty()) {
                size_t newlinePos = codeBlock.find('\n');
                if (newlinePos != std::string::npos) {
                    codeBlock.insert(0, "# ");
                } else {
                    codeBlock = "# " + codeBlock;
                }
            }

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

    if (diagMode) {
        std::cerr << "\n[DIAG CHUNK] " << chunk << std::endl;
    }

    StreamData* data = (StreamData*)userp;
    if (diagMode) {
        std::cerr << "[DEBUG] Got chunk of size " << totalSize << " bytes" << std::endl;
    }
    if (!data->first_chunk_received) {
        data->first_chunk_received = true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - data->start_time
        ).count();    
        std::cerr << "\033[31m[Response: " << elapsed << " ms]\033[0m" << std::endl;
        }
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
            std::cout << "\033[32m" << text << "\033[0m" << std::flush;  // Green output

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
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamCallback);
}

// ---- Send message to Ollama ----
static bool sendMessageToOllama(const std::string& query,
                                std::vector<Json::Value>& chatHistory,
                                const AppConfig& config) {
    Json::Value msg;
    msg["role"] = "user";
    msg["content"] = query;
    chatHistory.push_back(msg);

    CURL* curl = curl_easy_init();
    if (!curl) return false;

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

    std::cout << "\033[38;5;208m[Thinking..]\033[0m" << std::endl;

    curl_easy_setopt(curl, CURLOPT_URL, config.ollama_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &streamData);

    struct curl_slist* headers = NULL;
    streamData.start_time = std::chrono::high_resolution_clock::now();
    streamData.first_chunk_received = false;
    setCurlStreamingOptions(curl, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    std::cout << std::endl;

    if (res == CURLE_OK && !streamData.collected.empty()) {
        Json::Value reply;
        reply["role"] = "assistant";
        reply["content"] = streamData.collected;
        chatHistory.push_back(reply);

        saveCodeBlocks(streamData.collected);
        return true;
    }

    return false;
}

// ---- Process Command ----
Json::Value processCommand(const std::string& command, AppConfig& config) {
    Json::Value ragOut;
if (HandleRAGConsoleCommand(command, ragOut)) {
    return ragOut; // handled RAG_INGEST / RAG_ASK / RAG_SESSION
}
// One-time Ollama connectivity status on first command
    if (!g_oc_ping_done) {
        g_oc_ping_done = true;
        long http_code = 0;
        bool ok = oc_check_ollama_connectivity(config.ollama_url, config.ollama_timeout_seconds, &http_code);
        if (!ok) {
            std::cout << "[Warning] Could not reach Ollama at " << config.ollama_url
                      << " within " << config.ollama_timeout_seconds << " seconds. Some commands may not work.\n";
        } else {
            std::cout << "[Info] Ollama reachable (HTTP " << http_code << ")\n";
        }
    }

    static std::vector<Json::Value> chatHistory;
    Json::Value result;

    std::string cmd_upper = command;
    std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);

    // ===== ASK =====
if (cmd_upper == "ASK" || cmd_upper.rfind("ASK ", 0) == 0) {
    std::string query;
    if (cmd_upper == "ASK") {
        std::cout << "\033[94mWhat is your Question:\033[0m";
        std::getline(std::cin, query);
    } else {
        query = command.substr(4);
    }

    // Try RAG first (if active)
    std::string rag_answer;
    if (rag_int::TryRAGAnswer(query, rag_answer, /*k=*/5, /*threshold=*/0.2)) {
        std::cout << rag_answer << "\n";
        result["status"] = "success";
    } else {
        // Fall back to normal LLM
        sendMessageToOllama(query, chatHistory, config);
        result["status"] = "success";
    }
}

    // ===== INT (interactive mode) =====
else if (cmd_upper == "INT") {
    std::cout << "\033[94m[Interactive Mode]\033[0m Type your messages. Type /bye to exit.\n";
    std::string line;
    while (true) {
        std::cout << "\033[38;2;228;217;111m-> ";
        if (!std::getline(std::cin, line)) break;
        if (line == "/bye") {
            std::cout << "[Returning to main prompt]\033[0m\n";
            break;
        }
        //std::cerr << "[RAG_INT] trying..." << std::endl;
        // Try RAG first (if active and enabled)
        std::string rag_answer;
        if (rag_int::TryRAGAnswer(line, rag_answer, /*k=*/5, /*threshold=*/0.2)) {
            std::cout << rag_answer << "\n";
            continue; // handled via RAG
        }

        // Fall back to normal LLM
        sendMessageToOllama(line, chatHistory, config);
    }
    result["status"] = "success";
}

    // ===== READ =====
    else if (cmd_upper == "READ" || cmd_upper.rfind("READ_CTX:", 0) == 0) {
        std::string context;
        std::string filename;

        if (cmd_upper.rfind("READ_CTX:", 0) == 0) {
            size_t ctxPos = command.find(":") + 1;
            size_t filePos = command.find("|FILE:");
            context = command.substr(ctxPos, filePos - ctxPos);
            filename = command.substr(filePos + 6);
        } else {
            std::cout << "Enter context: ";
            std::getline(std::cin, context);
            std::cout << "Enter filename: ";
            std::getline(std::cin, filename);
        }

        if (!std::filesystem::exists(filename)) {
            std::cout << "[Error] File does not exist: " << filename << std::endl;
            result["status"] = "error";
            return result;
        }
        if (std::filesystem::is_empty(filename)) {
            std::cout << "[Error] File is empty: " << filename << std::endl;
            result["status"] = "error";
            return result;
        }

        std::ifstream inFile(filename);
        std::stringstream buffer;
        buffer << inFile.rdbuf();
        std::string fileContents = buffer.str();

        std::string fullMessage =
            "Context: " + context +
            "\n\nFile contents:\n" + fileContents +
            "\n\nInstruction: Please read and store this content for later reference in our ongoing conversation. "
            "Acknowledge once you have absorbed it.";

        sendMessageToOllama(fullMessage, chatHistory, config);
        result["status"] = "success";
    }

    // ===== WHO =====
    else if (cmd_upper == "WHO") {
        result["status"] = "success";
        result["serial_port"] = config.serial_port;
        result["baudrate"] = config.baudrate;
        result["ollama_url"] = config.ollama_url;
        result["ollama_model"] = config.ollama_model;
        result["ollama_timeout_seconds"] = config.ollama_timeout_seconds;
        std::cout << "\nCurrent configuration:\n";
        std::cout << "  Serial port: " << config.serial_port << "\n";
        std::cout << "  Baudrate: " << config.baudrate << "\n";
        std::cout << "  Ollama URL: " << config.ollama_url << "\n";
        std::cout << "  Model: " << config.ollama_model << "\n";
        std::cout << "  Ollama timeout (s): " << config.ollama_timeout_seconds << "\n";
    }

    // ===== HELP =====
    else if (cmd_upper == "HELP") {
        result["status"] = "success";
        Json::Value cmds(Json::objectValue);
        if (!config.commands.empty()) {
            for (const auto& [cmd, desc] : config.commands) {
                cmds[cmd] = desc;
            
        std::cout << "  MODEL [#|name] - List or set Ollama model\n";}
        } else {
            cmds["ASK"] = "Ask the model a question.";
            cmds["INT"] = "Enter interactive mode with the model.";
            cmds["READ"] = "Send a file with context to the model.";
            cmds["RESET"] = "Clear chat history.";
            cmds["WHO"] = "Show current configuration.";
            cmds["HELP"] = "List available commands.";
            cmds["DIAG"] = "Toggle diagnostic mode.";
        }
        result["commands"] = cmds;
        std::cout << "\nAvailable commands:\n";
        for (auto& key : cmds.getMemberNames()) {
            std::cout << "  " << key << " - " << cmds[key].asString() << "\n";
        }
    }

    
    // ===== MODEL (list only in this layer) =====
    

    // ===== MODEL (list & set) =====
    else if (cmd_upper.rfind("MODEL", 0) == 0) {
        std::string arg = "";
        if (command.size() > 5) {
            arg = command.substr(5);
            auto ltrim = [](std::string& s){ s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){return !std::isspace(ch);})); };
            auto rtrim = [](std::string& s){ s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){return !std::isspace(ch);}).base(), s.end()); };
            ltrim(arg); rtrim(arg);
        }

        std::string err;
        auto models = fetch_ollama_models(config.ollama_url, err);
        if (!err.empty()) {
            std::cout << "[Error] " << err << std::endl;
            result["status"] = "error";
            result["error"] = err;
            return result;
        }

        if (arg.empty()) {
            if (models.empty()) {
                std::cout << "[Info] No models found." << std::endl;
            } else {
                std::cout << "\nAvailable models:\n";
                for (size_t i = 0; i < models.size(); ++i) {
                    bool isCurrent = (models[i] == config.ollama_model);
                    std::cout << "  [" << (i+1) << "] " << models[i];
                    if (isCurrent) std::cout << "  (current)";
                    std::cout << "\n";
                }
                std::cout << "\nUse: MODEL <#|name> to set the model.\n";
            }
            result["status"] = "ok";
            return result;
        }

        int idx = -1;
        try { idx = std::stoi(arg); } catch (...) { idx = -1; }
        std::string chosen;
        if (idx >= 1 && idx <= (int)models.size()) {
            chosen = models[idx-1];
        } else {
            auto eq_ci = [](const std::string& a, const std::string& b){
                if (a.size()!=b.size()) return false;
                for (size_t i=0;i<a.size();++i) if (std::tolower((unsigned char)a[i])!=std::tolower((unsigned char)b[i])) return false;
                return true;
            };
            for (auto& m : models) if (m == arg || eq_ci(m, arg)) { chosen = m; break; }
        }

        if (chosen.empty()) {
            std::cout << "[Warn] Model not found: " << arg << "\n";
            result["status"] = "not_found";
            result["arg"] = arg;
            return result;
        }

        config.ollama_model = chosen;
        if (saveConfig("config.txt", config)) {
            std::cout << "[OK] Model set to: " << config.ollama_model << " (saved)\n";
        } else {
            std::cout << "[OK] Model set to: " << config.ollama_model << " (save failed)\n";
        }
        result["status"] = "ok";
        result["model"] = config.ollama_model;
        return result;
    }
// ===== DIAG =====
    else if (cmd_upper.rfind("DIAG", 0) == 0) {
        std::string arg;
        if (command.size() > 4) {
            arg = command.substr(5);
            std::transform(arg.begin(), arg.end(), arg.begin(), ::toupper);
        }
        if (arg == "ON") diagMode = true;
        else if (arg == "OFF") diagMode = false;
        else if (arg.empty()) diagMode = !diagMode;

        std::cout << "[Diagnostic mode " << (diagMode ? "ON" : "OFF") << "]\n";
        result["status"] = "success";
    }

    // ===== RESET =====
    else if (cmd_upper == "RESET") {
        chatHistory.clear();
        result["status"] = "success";
        result["message"] = "Chat history cleared.";
    }

    // ===== RESET =====
    else if (cmd_upper == "QUIT") {
        std::cout << "[See Ya!!]\n";
        exit(0);
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
        }
    }

    return result;
}
