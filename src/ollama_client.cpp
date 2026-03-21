#include "ollama_client.h"
#include "config_loader.h"        // AppConfig definition + saveConfig/loadConfig
#include "command_exec.h"         // route_output, ScopedSource, CommandSource
#include "serial_handler.h"       // serialSend, serial_available
#include "rag_console_commands.hpp"
#include "rag_int_bridge.hpp"
#include "utils.h"
#include "chat_provider.hpp"
#include "tts.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdarg>
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
    const std::string name = effectiveModel(cfg).empty() ? "model" : effectiveModel(cfg);
    return name + suffix;
}

// ---- One-time connectivity check on first command ----
static size_t ocurl_discard_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    return size * nmemb;
}
static bool oc_check_provider_connectivity(const AppConfig& config, long timeout_seconds, long* http_code_out=nullptr) {
    std::string url = deriveModelsUrl(config);
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ocurl_discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    struct curl_slist* headers = nullptr;
    if (!config.api_key.empty()) headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + config.api_key).c_str());
    if (!config.organization.empty()) headers = curl_slist_append(headers, (std::string("OpenAI-Organization: ") + config.organization).c_str());
    if (!config.project.empty()) headers = curl_slist_append(headers, (std::string("OpenAI-Project: ") + config.project).c_str());
    for (const auto& kv : config.extra_headers) headers = curl_slist_append(headers, (kv.first + ": " + kv.second).c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (http_code_out) *http_code_out = code;
        ok = (code >= 200 && code < 500);
    }
    if (headers) curl_slist_free_all(headers);
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
static std::vector<std::string> fetch_provider_models(const AppConfig& config, std::string& error) {
    std::vector<std::string> models;
    std::string url = deriveModelsUrl(config);

    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl init failed"; return models; }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ocurl_write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    struct curl_slist* headers = nullptr;
    if (!config.api_key.empty()) headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + config.api_key).c_str());
    if (!config.organization.empty()) headers = curl_slist_append(headers, (std::string("OpenAI-Organization: ") + config.organization).c_str());
    if (!config.project.empty()) headers = curl_slist_append(headers, (std::string("OpenAI-Project: ") + config.project).c_str());
    for (const auto& kv : config.extra_headers) headers = curl_slist_append(headers, (kv.first + ": " + kv.second).c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        error = std::string("curl error: ") + curl_easy_strerror(res);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return models;
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return parseModelListResponse(config, response, error);
}

// ---- Streaming support ----
struct StreamData {
    std::string collected;
    std::chrono::high_resolution_clock::time_point start_time;
    bool first_chunk_received = false;
};
static bool diagMode = false; // Diagnostic dump mode

static void diag_log(const char* fmt, ...) {
    if (!diagMode) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

bool IsDiagnosticModeEnabled() {
    return diagMode;
}

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

// ---- Send message to configured provider ----
static bool sendMessageToOllama(const std::string& query,
                                std::vector<Json::Value>& chatHistory,
                                const AppConfig& config) {
   diag_log("[DIAG] sendMessage caller src=%d\n", (int)getCurrentCommandSource());
    Json::Value msg;
    msg["role"] = "user";
    msg["content"] = query;
    chatHistory.push_back(msg);

    StreamData streamData;
    const CommandSource prev = getCurrentCommandSource();
    setCurrentCommandSource(prev);
    route_output("[Thinking.....:-).......]", true);
    streamData.start_time = std::chrono::high_resolution_clock::now();

    ChatProviderResult providerResult;
    const bool ok = executeProviderChat(
        config,
        chatHistory,
        true,
        [&](const std::string& text) {
            if (!streamData.first_chunk_received) {
                streamData.first_chunk_received = true;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - streamData.start_time
                ).count();
                route_output(std::string("[Response ") + std::to_string(elapsed) + "ms]", true);
            }
            route_output(text);
            if (!serial_available) {
                std::ofstream log("log.txt", std::ios::app);
                if (log.is_open()) { log << text; log.flush(); }
            }
            streamData.collected += text;
        },
        [&](const std::string& line) {
            diag_log("%s\n", line.c_str());
        },
        providerResult
    );

    route_output("", true);
    setCurrentCommandSource(prev);

    if (!ok) {
        route_output(std::string("[Error] ") + providerResult.error_message, true);
        if (providerResult.stream_interrupted) route_output("[Warning] Upstream stream ended early; partial output may be incomplete.", true);
        chatHistory.pop_back();
        return false;
    }

    Json::Value reply = providerResult.assistant_message;
    if (!reply.isObject()) reply = Json::Value(Json::objectValue);
    reply["role"] = reply.get("role", "assistant");
    reply["content"] = providerResult.assistant_content;
    if (!providerResult.usage.isNull()) reply["usage"] = providerResult.usage;
    chatHistory.push_back(reply);
    saveCodeBlocks(providerResult.assistant_content);
    maybeSpeakText(providerResult.assistant_content, config);
    return true;
}


// ================= Serial INT state =================
static std::atomic<bool> g_serial_int_active{false};
static std::vector<Json::Value> g_chatHistory;
enum class ReadStage { Idle=0, WaitingContext, WaitingFilename, WaitingContextPresetFile, WaitingPickIndex_ContextKnown, WaitingPickIndex_ThenAskContext };
static std::atomic<ReadStage> g_read_stage{ReadStage::Idle};
static std::string g_read_context;
static std::string g_read_preset_filename;
static std::vector<std::string> g_read_pick_files;
static std::string g_read_pick_dir;
static bool list_dir_and_prompt(const std::string& dir) {
    namespace fs = std::filesystem;
    g_read_pick_files.clear();
    g_read_pick_dir.clear();
    if (!fs::exists(dir) || !fs::is_directory(dir)) { route_output(std::string("[Error] Not a directory: ") + dir, true); return false; }
    for (auto& entry : fs::directory_iterator(dir)) if (entry.is_regular_file()) g_read_pick_files.push_back(entry.path().filename().string());
    if (g_read_pick_files.empty()) { route_output("[Error] No files found in " + dir, true); return false; }
    std::sort(g_read_pick_files.begin(), g_read_pick_files.end());
    route_output("Files in " + dir + ":", true);
    for (size_t i = 0; i < g_read_pick_files.size(); ++i) route_output(std::to_string(i+1) + ") " + g_read_pick_files[i], true);
    route_output("Choose file number (or 0 to cancel):", true);
    if (getCurrentCommandSource() == CommandSource::SERIAL) route_output(": ");
    g_read_pick_dir = dir;
    return true;
}

static AppConfig* g_read_cfg = nullptr;


bool SerialINT_IsActive() {
    return g_serial_int_active.load(std::memory_order_relaxed);
}

void SerialINT_Start(AppConfig& config) {
    g_serial_int_active.store(true, std::memory_order_relaxed);
    diag_log("[DIAG] INT called from source=%d\n", (int)getCurrentCommandSource());
    route_output("[Interactive Mode] Type your messages. Type /bye to exit.\n", true);
    route_output("-> ");
}

void SerialINT_HandleLine(const std::string& line, AppConfig& config) {
    AIMaster_RAG_ConfigureRemote(config.ollama_url, config.ollama_model);
    if (line == "/bye") {
        g_serial_int_active.store(false, std::memory_order_relaxed);
        route_output("[Returning to main prompt]", true);
        route_output(modelPrompt(config, "> "), false);
        return;
    }
    if (line == "/n") {
        g_serial_int_active.store(false, std::memory_order_relaxed);
        route_output(modelPrompt(config, "> "), false);
        return;
    }

    SpeakCommandResult speak;
    if (applySpeakCommand(line, config, speak)) {
        const std::string upper = toupper_copy(line);
        if (upper == "/SPEAK ON" || upper == "/SPEAK OFF") {
            if (saveConfig("config.txt", config)) speak.message += " (saved)";
        }
        route_output(speak.message, true);
        route_output("-> ", false);
        return;
    }

    // Try RAG first (if active and enabled)
    std::string rag_answer;
    if (rag_int::TryRAGAnswer(line, rag_answer, /*k=*/config.rag_chunks, /*threshold=*/config.rag_threshold)) {
        route_output(rag_answer, true);
        route_output("-> ", false);
        return;
    }
    // Fall back to normal LLM
    sendMessageToOllama(line, g_chatHistory, config);
    route_output("-> ");
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

    AIMaster_RAG_ConfigureRemote(config.ollama_url, config.ollama_model);
    diag_log("[processCommand] src=%d raw='%s'\n",
             (int)getCurrentCommandSource(), command.c_str());
    if (diagMode) std::fflush(stderr);

    if (getCurrentCommandSource() == CommandSource::SERIAL) {
        diag_log("[RX_CMD] '%s'\n", command.c_str());
        if (diagMode) std::fflush(stderr);
    }

    Json::Value ragOut;
    if (HandleRAGConsoleCommand(command, ragOut)) {
        return ragOut; // handled RAG_INGEST / RAG_ASK / RAG_SESSION / etc
    }

    Json::Value result;
    const std::string cmd_upper = toupper_copy(command);

    SpeakCommandResult speak;
    if (applySpeakCommand(command, config, speak)) {
        if (cmd_upper == "/SPEAK ON" || cmd_upper == "/SPEAK OFF") {
            if (saveConfig("config.txt", config)) speak.message += " (saved)";
        }
        route_output(speak.message, true);
        result["status"] = (cmd_upper == "/SPEAK ON" || cmd_upper == "/SPEAK OFF") ? "success" : "error";
        result["tts_enabled"] = config.tts_enabled;
        return result;
    }

    // One-time Ollama connectivity status on first command
    if (!g_oc_ping_done) {
        g_oc_ping_done = true;
        long http_code = 0;
        bool ok = oc_check_provider_connectivity(config, effectiveTimeoutSeconds(config), &http_code);
        if (!ok) {
            route_output(std::string("[Warning] Could not reach provider at ") + effectiveChatUrl(config) +
                         " within " + std::to_string(effectiveTimeoutSeconds(config)) + " seconds. Some commands may not work.", true);
        } else {
            route_output(std::string("[Info] Provider reachable (HTTP ") + std::to_string(http_code) + ")", true);
        }
    }

    static std::vector<Json::Value> chatHistory;

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
        if (rag_int::TryRAGAnswer(query, rag_answer, /*k=*/config.rag_chunks, /*threshold=*/config.rag_threshold)) {
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
    else if (cmd_upper == "CFG") {
        result["status"] = "success";
        result["serial_port"] = config.serial_port;
        result["baudrate"] = config.baudrate;
        result["provider_type"] = effectiveProviderType(config);
        result["api_base"] = effectiveApiBase(config);
        result["model"] = effectiveModel(config);
        result["timeout"] = Json::Value(static_cast<Json::UInt64>(effectiveTimeoutSeconds(config)));
        result["ollama_url"] = config.ollama_url;
        result["ollama_model"] = config.ollama_model;
        result["ollama_timeout_seconds"] = Json::Value(static_cast<Json::UInt64>(config.ollama_timeout_seconds));
        result["rag_chunks"] = config.rag_chunks;
        result["rag_threshold"] = config.rag_threshold;
        result["tts_enabled"] = config.tts_enabled;
        result["tts_endpoint_url"] = config.tts_endpoint_url;
        result["tts_timeout_seconds"] = Json::Value(static_cast<Json::UInt64>(config.tts_timeout_seconds));
        result["tts_voice"] = config.tts_voice;
        result["tts_speaker"] = config.tts_speaker;
        route_output("Current configuration:", true);
        route_output(std::string("\tSerial port: ") + config.serial_port, true);
        route_output(std::string("\tBaudrate: ") + std::to_string(config.baudrate), true);
        route_output(std::string("\tProvider type: ") + effectiveProviderType(config), true);
        route_output(std::string("\tAPI base: ") + effectiveApiBase(config), true);
        route_output(std::string("\tModel: ") + effectiveModel(config), true);
        route_output(std::string("\tTimeout (s): ") + std::to_string(effectiveTimeoutSeconds(config)), true);
        route_output(std::string("\tLegacy Ollama URL: ") + config.ollama_url, true);
        route_output(std::string("\tRAG chunks (ASK/INT): ") + std::to_string(config.rag_chunks), true);
        route_output(std::string("\tRAG threshold (ASK/INT): ") + std::to_string(config.rag_threshold), true);
        route_output(std::string("\tChar delay: ") + std::to_string(config.serial_delay_ms), true);
        route_output(std::string("\tNewline: ") + config.serial_newline, true);
        route_output("->", false);
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
        route_output("->", false);
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
            cmds["/speak on"] = "Enable text-to-speech output for assistant replies.";
            cmds["/speak off"] = "Disable text-to-speech output.";
            cmds["CFG"] = "Show current configuration.";
            cmds["HELP"] = "List available commands.";
            cmds["MODEL"] = "List or set Ollama model.";
            cmds["RAG_INGEST"] = "Ingest a folder into the RAG system.";
            cmds["RAG_SHOW"] = "Show the contents of the RAG ingestion.";
            cmds["RAG_ASK"] = "Ask RAG: RAG_ASK [--k N] [--thr T] <question...> (ASK/INT defaults use config: rag_chunks, rag_threshold).";
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
        auto models = fetch_provider_models(config, err);
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
                    bool isCurrent = (models[i] == effectiveModel(config));
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
        config.model = chosen;
        if (saveConfig("config.txt", config)) {
            route_output(std::string("[OK] Model set to: ") + effectiveModel(config) + " (saved)", true);
        } else {
            route_output(std::string("[OK] Model set to: ") + effectiveModel(config) + " (save failed)", true);
        }
        result["status"] = "ok";
        result["model"] = effectiveModel(config);
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
bool ReadAwait_IsActive() { return g_read_stage.load(std::memory_order_relaxed) != ReadStage::Idle; }
void ReadAwait_Start(AppConfig& config) { g_read_cfg = &config; g_read_context.clear(); g_read_preset_filename.clear(); g_read_stage.store(ReadStage::WaitingContext, std::memory_order_relaxed); route_output("Enter context:", true); if (getCurrentCommandSource() == CommandSource::SERIAL) route_output(": "); }
void ReadAwait_StartWithFile(AppConfig& config, const std::string& filename) { g_read_cfg = &config; g_read_context.clear(); g_read_preset_filename = filename; g_read_stage.store(ReadStage::WaitingContextPresetFile, std::memory_order_relaxed); route_output("Enter context:", true); if (getCurrentCommandSource() == CommandSource::SERIAL) route_output(": "); }
void ReadAwait_StartFolderPickWithContext(AppConfig& config, const std::string& ctx, const std::string& dir) { g_read_cfg=&config; g_read_context=ctx; g_read_preset_filename.clear(); if (!list_dir_and_prompt(dir)) return; g_read_stage.store(ReadStage::WaitingPickIndex_ContextKnown, std::memory_order_relaxed); }
void ReadAwait_StartFolderPickThenAskContext(AppConfig& config, const std::string& dir) { g_read_cfg=&config; g_read_context.clear(); g_read_preset_filename.clear(); if (!list_dir_and_prompt(dir)) return; g_read_stage.store(ReadStage::WaitingPickIndex_ThenAskContext, std::memory_order_relaxed); }
static void ReadAwait_Reset() { g_read_stage.store(ReadStage::Idle, std::memory_order_relaxed); g_read_context.clear(); g_read_preset_filename.clear(); g_read_pick_files.clear(); g_read_pick_dir.clear(); g_read_cfg=nullptr; }
void ReadAwait_HandleLine(const std::string& line, AppConfig& config) {
    ReadStage st = g_read_stage.load(std::memory_order_relaxed);
    if (st == ReadStage::WaitingContext) {
        g_read_context = line;
        g_read_stage.store(ReadStage::WaitingFilename, std::memory_order_relaxed);
        route_output("Enter filename:", true);
        if (getCurrentCommandSource() == CommandSource::SERIAL) route_output(": ");
        return;
    }
    if (st == ReadStage::WaitingFilename) {
        std::string filename = line;
        if (std::filesystem::is_directory(filename)) { if (list_dir_and_prompt(filename)) g_read_stage.store(ReadStage::WaitingPickIndex_ContextKnown, std::memory_order_relaxed); return; }
        if (!std::filesystem::exists(filename) || std::filesystem::is_empty(filename)) { route_output("[Error] Invalid file.", true); ReadAwait_Reset(); return; }
        std::ifstream inFile(filename); std::stringstream buffer; buffer << inFile.rdbuf(); std::string fileContents = buffer.str();
        std::string fullMessage = "Context: " + g_read_context + "\n\nFile contents:\n" + fileContents + "\n\nInstruction: Please read and store this content for later reference in our ongoing conversation. " "Acknowledge once you have absorbed it.";
        sendMessageToOllama(fullMessage, g_chatHistory, config);
        ReadAwait_Reset(); route_output(modelPrompt(config, SerialINT_IsActive() ? "-> " : "> ")); return;
    }
    if (st == ReadStage::WaitingContextPresetFile) {
        g_read_context = line;
        std::string filename = g_read_preset_filename;
        if (std::filesystem::is_directory(filename)) { if (list_dir_and_prompt(filename)) g_read_stage.store(ReadStage::WaitingPickIndex_ContextKnown, std::memory_order_relaxed); return; }
        if (!std::filesystem::exists(filename) || std::filesystem::is_empty(filename)) { route_output("[Error] Invalid file.", true); ReadAwait_Reset(); return; }
        std::ifstream inFile(filename); std::stringstream buffer; buffer << inFile.rdbuf(); std::string fileContents = buffer.str();
        std::string fullMessage = "Context: " + g_read_context + "\n\nFile contents:\n" + fileContents + "\n\nInstruction: Please read and store this content for later reference in our ongoing conversation. " "Acknowledge once you have absorbed it.";
        sendMessageToOllama(fullMessage, g_chatHistory, config);
        ReadAwait_Reset(); route_output(modelPrompt(config, SerialINT_IsActive() ? "-> " : "> ")); return;
    }
    if (st == ReadStage::WaitingPickIndex_ThenAskContext || st == ReadStage::WaitingPickIndex_ContextKnown) {
        int idx = -1; try { idx = std::stoi(line); } catch (...) { idx = -1; }
        if (idx <= 0 || static_cast<size_t>(idx) > g_read_pick_files.size()) { route_output("[Cancelled]", true); ReadAwait_Reset(); route_output(modelPrompt(config, SerialINT_IsActive() ? "-> " : "> ")); return; }
        std::string chosen = g_read_pick_files[idx-1]; std::string filename = (std::filesystem::path(g_read_pick_dir) / chosen).string();
        if (st == ReadStage::WaitingPickIndex_ThenAskContext) {
            g_read_preset_filename = filename; g_read_stage.store(ReadStage::WaitingContextPresetFile, std::memory_order_relaxed);
            route_output("Enter context:", true); if (getCurrentCommandSource() == CommandSource::SERIAL) route_output(": "); return;
        } else {
            std::ifstream inFile(filename); std::stringstream buffer; buffer << inFile.rdbuf(); std::string fileContents = buffer.str();
            std::string fullMessage = "Context: " + g_read_context + "\n\nFile contents:\n" + fileContents + "\n\nInstruction: Please read and store this content for later reference in our ongoing conversation. " "Acknowledge once you have absorbed it.";
            sendMessageToOllama(fullMessage, g_chatHistory, config);
            ReadAwait_Reset(); route_output(modelPrompt(config, SerialINT_IsActive() ? "-> " : "> ")); return;
        }
    }
}


bool ReadAwait_TryHandleLine(const std::string& line, AppConfig& config) {
    ReadStage st = g_read_stage.load(std::memory_order_relaxed);
    if (st == ReadStage::Idle) return false; // not active, let caller handle normally
    // Allow user to cancel the flow
    if (line == "/cancel" || line == "/CANCEL") {
        ReadAwait_Reset();
        route_output(modelPrompt(config, SerialINT_IsActive() ? "-> " : "> "));
        return true;
    }
    ReadAwait_HandleLine(line, config);
    return true;
}
