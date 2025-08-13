#include "ollama_client.h"
#include "config_loader.h"        // AppConfig definition + saveConfig/loadConfig
#include "command_exec.h"         // route_output, ScopedSource, CommandSource
#include "serial_handler.h"       // serialSend, serial_available
#include "rag_console_commands.hpp"
#include "rag_int_bridge.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include "route_context.h"


using std::string;

static std::string toupper_copy(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
    return s;
}

// ---------- Prompt helper ----------
std::string modelPrompt(const AppConfig& cfg, const char* suffix) {
    const std::string name = cfg.ollama_model.empty() ? "model" : cfg.ollama_model;
    return name + suffix;
}

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

// ---- Streaming support ----
struct StreamData {
    std::string collected;
    std::string raw_output;
    std::chrono::high_resolution_clock::time_point start_time;
    bool first_chunk_received = false;
};
static bool diagMode = false; // Diagnostic dump mode

namespace {
    void saveCodeBlocks(const std::string& text) {
        namespace fs = std::filesystem;
        const std::string delimiter = "```";
        size_t pos = 0;
        int blockCount = 0;

        while ((pos = text.find(delimiter, pos)) != std::string::npos) {
            size_t start = pos + delimiter.length();
            size_t end = text.find(delimiter, start);
            if (end == std::string::npos) break;

            std::string codeBlock = text.substr(start, end - start);
            // Trim
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
                route_output(std::string("[Saved code block to ") + oss.str() + "]", true);
            }

            blockCount++;
            pos = end + delimiter.length();
        }
    }
}

static size_t StreamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string chunk((char*)contents, totalSize);

    if (diagMode) {
        std::cerr << "\n[DIAG CHUNK] " << chunk << std::endl;
    }

    StreamData* data = (StreamData*)userp;
    if (!data->first_chunk_received) {
        data->first_chunk_received = true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - data->start_time
        ).count();
        route_output(std::string("[Response ") + std::to_string(elapsed) + "ms]", true);
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
            route_output(text);

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
   ::fprintf(stderr, "[DIAG] sendMessage caller src=%d\n", (int)getCurrentCommandSource());
 // Add user message to history
    Json::Value msg;
    msg["role"] = "user";
    msg["content"] = query;
    chatHistory.push_back(msg);

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    // Build streaming payload
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

    // --- Route replies correctly during this call ---
    // If the caller is SERIAL (INT), force serial while streaming; otherwise leave as-is.
const CommandSource prev = getCurrentCommandSource();
setCurrentCommandSource(prev);   // assert caller’s route for this thread
    route_output("[Thinking..]", true);

    // cURL setup
    curl_easy_setopt(curl, CURLOPT_URL,        config.ollama_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,       1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonPayload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,  &streamData);

    struct curl_slist* headers = NULL;
    streamData.start_time = std::chrono::high_resolution_clock::now();
    streamData.first_chunk_received = false;
    setCurlStreamingOptions(curl, headers);

    // Perform request (your write/stream callback should call route_output)
    CURLcode res = curl_easy_perform(curl);

    // Cleanup curl
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Finish line after streaming tokens
    route_output("", true);
setCurrentCommandSource(prev);

    // Determine success and update chat history
    const bool ok = (res == CURLE_OK && !streamData.collected.empty());
    if (ok) {
        Json::Value reply;
        reply["role"] = "assistant";
        reply["content"] = streamData.collected;
        chatHistory.push_back(reply);
        saveCodeBlocks(streamData.collected);
    }

    return ok;
}



// ================= Serial INT state =================
static std::atomic<bool> g_serial_int_active{false};
static std::vector<Json::Value> g_chatHistory;

bool SerialINT_IsActive() {
    return g_serial_int_active.load(std::memory_order_relaxed);
}

void SerialINT_Start(AppConfig& config) {
    g_serial_int_active.store(true, std::memory_order_relaxed);
    fprintf(stderr, "[DIAG] INT called from source=%d\n", (int)getCurrentCommandSource());
    route_output("[Interactive Mode] Type your messages. Type /bye to exit.", true);
    route_output(modelPrompt(config, "-> "), false);
}

void SerialINT_HandleLine(const std::string& line, AppConfig& config) {
    if (line == "/bye") {
        g_serial_int_active.store(false, std::memory_order_relaxed);
        route_output("[Returning to main prompt]", true);
        route_output(modelPrompt(config, "> "), false);
        return;
    }
    // Try RAG first (if active and enabled)
    std::string rag_answer;
    if (rag_int::TryRAGAnswer(line, rag_answer, /*k=*/5, /*threshold=*/0.2)) {
        route_output(rag_answer, true);
        route_output(modelPrompt(config, "-> "), false);
        return;
    }
    // Fall back to normal LLM
    sendMessageToOllama(line, g_chatHistory, config);
    route_output(modelPrompt(config, "-> "), false);
}

// ---- helpers ----
static std::vector<std::string> split_ws(const std::string& s){
    std::istringstream iss(s);
    std::vector<std::string> out; std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}
static std::string ltrim(std::string s){
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch){return !std::isspace(ch);}));
    return s;
}
static std::string rtrim(std::string s){
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch){return !std::isspace(ch);} ).base(), s.end());
    return s;
}
static std::string trim(std::string s){ return rtrim(ltrim(s)); }

// ================= Dispatcher =================
Json::Value processCommand(const std::string& command, AppConfig& config) {
    Json::Value ragOut;
    if (HandleRAGConsoleCommand(command, ragOut)) {
        return ragOut; // handled RAG_INGEST / RAG_ASK / RAG_SESSION / etc
    }

    // One-time Ollama connectivity status on first command
    if (!g_oc_ping_done) {
        g_oc_ping_done = true;
        long http_code = 0;
        bool ok = oc_check_ollama_connectivity(config.ollama_url, config.ollama_timeout_seconds, &http_code);
        if (!ok) {
            route_output(std::string("[Warning] Could not reach Ollama at ") + config.ollama_url +
                         " within " + std::to_string(config.ollama_timeout_seconds) + " seconds. Some commands may not work.", true);
        } else {
            route_output(std::string("[Info] Ollama reachable (HTTP ") + std::to_string(http_code) + ")", true);
        }
    }

    static std::vector<Json::Value> chatHistory;
    Json::Value result;

    const std::string cmd_upper = toupper_copy(command);

    // ===== ASK =====
    if (cmd_upper == "ASK" || cmd_upper.rfind("ASK ", 0) == 0) {
        std::string query;
        if (cmd_upper == "ASK") {
            route_output("What is your Question:", false);
            std::getline(std::cin, query);
        } else {
            query = command.substr(4);
        }

        // Try RAG first (if active)
        std::string rag_answer;
        if (rag_int::TryRAGAnswer(query, rag_answer, /*k=*/5, /*threshold=*/0.2)) {
            route_output(rag_answer, true);
            result["status"] = "success";
        } else {
            // Fall back to normal LLM
            sendMessageToOllama(query, chatHistory, config);
            result["status"] = "success";
        }
        return result;
    }
    // ===== INT (interactive mode) =====
    else if (cmd_upper == "INT") {
        SerialINT_Start(config);
        result["status"] = "success";
        return result;
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
            route_output("Enter context: ", false);
            std::getline(std::cin, context);
            route_output("Enter filename: ", false);
            std::getline(std::cin, filename);
        }

        if (!std::filesystem::exists(filename)) {
            route_output(std::string("[Error] File does not exist: ") + filename, true);
            result["status"] = "error";
            return result;
        }
        if (std::filesystem::is_empty(filename)) {
            route_output(std::string("[Error] File is empty: ") + filename, true);
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
        return result;
    }
    // ===== WHO =====
    else if (cmd_upper == "WHO") {
        result["status"] = "success";
        result["serial_port"] = config.serial_port;
        result["baudrate"] = config.baudrate;
        result["ollama_url"] = config.ollama_url;
        result["ollama_model"] = config.ollama_model;
        result["ollama_timeout_seconds"] = config.ollama_timeout_seconds;
        route_output("Current configuration:", true);
        route_output(std::string("  Serial port: ") + config.serial_port, true);
        route_output(std::string("  Baudrate: ") + std::to_string(config.baudrate), true);
        route_output(std::string("  Ollama URL: ") + config.ollama_url, true);
        route_output(std::string("  Model: ") + config.ollama_model, true);
        route_output(std::string("  Ollama timeout (s): ") + std::to_string(config.ollama_timeout_seconds), true);
        return result;
    }
    // ===== DELAY =====
    else if (cmd_upper.rfind("DELAY", 0) == 0) {
        auto tokens = split_ws(command);
        if (tokens.size() < 2) {
            route_output("Usage: DELAY <ms>", true);
            return Json::Value();
        }
        int ms = -1;
        try { ms = std::stoi(tokens[1]); } catch (...) { ms = -1; }
        if (ms < 0) {
            route_output("[Error] ms must be >= 0", true);
            return Json::Value();
        }
        setSerialSendDelay(ms);
        config.serial_delay_ms = ms;
        if (saveConfig("config.txt", config)) {
            route_output(std::string("[OK] serial_delay_ms=") + std::to_string(ms) + " (saved)", true);
        } else {
            route_output(std::string("[OK] serial_delay_ms=") + std::to_string(ms) + " (save failed)", true);
        }
        return Json::Value();
    }
    // ===== HELP =====
    else if (cmd_upper == "HELP") {
        result["status"] = "success";
        Json::Value cmds(Json::objectValue);
        if (!config.commands.empty()) {
            for (const auto& kv : config.commands) {
                cmds[kv.first] = kv.second;
            }
        } else {
            cmds["ASK"] = "Ask the model a question.";
            cmds["INT"] = "Enter interactive mode with the model.";
            cmds["READ"] = "Send a file with context to the model.";
            cmds["RESET"] = "Clear chat history.";
            cmds["WHO"] = "Show current configuration.";
            cmds["HELP"] = "List available commands.";
            cmds["MODEL"] = "List or set Ollama model.";
            cmds["RAG_INGEST"] = "Ingest a folder into the RAG system.";
            cmds["RAG_SHOW"] = "Show the contents of the RAG ingestion.";
            cmds["RAG_SESSION"] = "Display the session information.";
        }
        result["commands"] = cmds;
        route_output("Available commands:", true);
        for (auto& key : cmds.getMemberNames()) {
            route_output("  " + key + " - " + cmds[key].asString(), true);
        }
        return result;
    }
    // ===== MODEL (list & set) =====
    else if (cmd_upper.rfind("MODEL", 0) == 0) {
        std::string arg = "";
        if (command.size() > 5) {
            arg = trim(command.substr(5));
        }

        std::string err;
        auto models = fetch_ollama_models(config.ollama_url, err);
        if (!err.empty()) {
            route_output(std::string("[Error] ") + err, true);
            result["status"] = "error";
            result["error"] = err;
            return result;
        }

        if (arg.empty()) {
            if (models.empty()) {
                route_output("[Info] No models found.", true);
            } else {
                route_output("Available models:", true);
                for (size_t i = 0; i < models.size(); ++i) {
                    bool isCurrent = (models[i] == config.ollama_model);
                    std::string line = "  [" + std::to_string(i+1) + "] " + models[i];
                    if (isCurrent) line += "  (current)";
                    route_output(line, true);
                }
                route_output("Use: MODEL <#|name> to set the model.", true);
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
            route_output(std::string("[Warn] Model not found: ") + arg, true);
            result["status"] = "not_found";
            result["arg"] = arg;
            return result;
        }

        config.ollama_model = chosen;
        if (saveConfig("config.txt", config)) {
            route_output(std::string("[OK] Model set to: ") + config.ollama_model + " (saved)", true);
        } else {
            route_output(std::string("[OK] Model set to: ") + config.ollama_model + " (save failed)", true);
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

        route_output(std::string("[Diagnostic mode ") + (diagMode ? "ON" : "OFF") + "]", true);
        result["status"] = "success";
        return result;
    }
    // ===== RESET =====
    else if (cmd_upper == "RESET") {
        chatHistory.clear();
        result["status"] = "success";
        result["message"] = "Chat history cleared.";
        return result;
    }
    // ===== QUIT =====
    else if (cmd_upper == "QUIT") {
        route_output("[See Ya!!]", true);
        std::exit(0);
    }

    // ===== Default =====
    result["status"] = "success";
    result["message"] = "Command: " + command;

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
