#include "ollama_client.h"
#include "config_loader.h"        // AppConfig definition + saveConfig/loadConfig
#include "command_exec.h"         // route_output, ScopedSource, CommandSource
#include "serial_handler.h"       // serialSend, serial_available
#include "rag_console_commands.hpp"
#include "rag_int_bridge.hpp"
#include "utils.h"
#include "chat_provider.hpp"
#include "tool_prompting.hpp"
#include "tool_plugins.hpp"
#include "tts.hpp"
#include "linux_integrations.hpp"
#include "hid_button.h"

#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdarg>
#include <sstream>
#include <memory>
#include <mutex>
#include <thread>
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
    std::string load_prompt_file_contents(const std::string& configured_path) {
        std::vector<std::string> candidates;
        if (!configured_path.empty()) candidates.push_back(configured_path);
        candidates.push_back("/usr/share/aimaster/user_prompt.txt");
        candidates.push_back("assets/user_prompt.txt");

        for (const auto& path : candidates) {
            if (path.empty()) continue;
            std::ifstream in(path);
            if (!in) continue;
            std::stringstream buffer;
            buffer << in.rdbuf();
            const std::string content = buffer.str();
            if (!content.empty()) return content;
        }
        return "";
    }

    Json::Value merge_tool_definitions(const Json::Value& configured_tools, const Json::Value& registered_tools) {
        Json::Value merged(Json::arrayValue);
        if (configured_tools.isArray()) {
            for (const auto& tool : configured_tools) merged.append(tool);
        }
        if (registered_tools.isArray()) {
            for (const auto& tool : registered_tools) merged.append(tool);
        }
        return merged;
    }

    Json::Value make_message(const std::string& role, const std::string& content) {
        Json::Value msg(Json::objectValue);
        msg["role"] = role;
        msg["content"] = content;
        return msg;
    }

    void ensure_session_prompt(std::vector<Json::Value>& chat_history, const AppConfig& config) {
        if (!chat_history.empty()) {
            const Json::Value& first = chat_history.front();
            if (first.isObject() && first.get("role", "").asString() == "system") return;
        }

        const std::string prompt = load_prompt_file_contents(config.user_prompt_file);
        if (prompt.empty()) return;
        chat_history.insert(chat_history.begin(), make_message("system", prompt));
    }

    Json::Value make_tool_call_assistant_message(const ToolPromptDecision& decision, const std::string& tool_call_id) {
        Json::Value msg(Json::objectValue);
        msg["role"] = "assistant";
        msg["content"] = "";
        msg["tool_calls"] = Json::arrayValue;
        Json::Value call(Json::objectValue);
        call["id"] = tool_call_id;
        call["type"] = "function";
        call["function"] = Json::Value(Json::objectValue);
        call["function"]["name"] = decision.tool_name;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        call["function"]["arguments"] = Json::writeString(writer, decision.arguments);
        msg["tool_calls"].append(call);
        return msg;
    }

    Json::Value plugin_result_to_json(const PluginResult& result, const std::string& tool_name) {
        Json::Value payload(Json::objectValue);
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream ss(result.content);
        if (Json::parseFromStream(reader, ss, &payload, &errors) && payload.isObject()) {
            if (!payload.isMember("tool_name")) payload["tool_name"] = tool_name;
            return payload;
        }
        payload = Json::Value(Json::objectValue);
        payload["ok"] = result.success;
        payload["tool_name"] = tool_name;
        if (result.success) payload["content"] = result.content;
        else payload["error"] = result.error_message.empty() ? result.content : result.error_message;
        return payload;
    }

    ToolPromptDecision select_tool_with_prompt(const std::vector<Json::Value>& chat_history,
                                               const std::string& user_query,
                                               const AppConfig& config,
                                               const Json::Value& merged_tools,
                                               const ChatLogEmitter& on_log) {
        ToolPromptDecision decision;
        const Json::Value tool_specs = buildToolSelectionSpecs(merged_tools);
        if (!tool_specs.isArray() || tool_specs.empty()) return decision;

        std::vector<Json::Value> selector_history = chat_history;
        selector_history.push_back(make_message("user", buildToolSelectionPrompt(tool_specs, user_query)));

        AppConfig selector_config = config;
        selector_config.tools = Json::Value();
        selector_config.tool_choice = Json::Value();

        ChatProviderResult selector_result;
        if (!executeProviderChat(selector_config, selector_history, false, ChatStreamEmitter(), on_log, selector_result)) {
            if (on_log) on_log("[Debug] Tool selector prompt failed; falling back to normal chat flow.");
            return decision;
        }

        decision = parseToolSelectionResponse(selector_result.assistant_content);
        if (on_log) {
            on_log(std::string("[Debug] Tool selector result valid=") + (decision.valid ? "true" : "false") +
                   ", use_tool=" + (decision.use_tool ? "true" : "false") +
                   (decision.tool_name.empty() ? "" : ", tool=" + decision.tool_name));
        }
        return decision;
    }

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

    void writeThinkingStatusRaw(const std::string& text, bool use_serial) {
        if (use_serial && serial_available) {
            serialSend(text);
        } else {
            std::cout << text;
            std::cout.flush();
        }
    }

    class ThinkingSpinner {
    public:
        explicit ThinkingSpinner(bool use_serial) : use_serial_(use_serial) {}

        void start() {
            active_.store(true, std::memory_order_relaxed);
            writeThinkingStatusRaw("[Thinking -]", use_serial_);
            worker_ = std::thread([this]() {
                static constexpr char frames[] = {'-', '/', '-', '\\'};
                std::size_t idx = 1;
                using namespace std::chrono_literals;
                while (active_.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(150ms);
                    if (!active_.load(std::memory_order_relaxed)) break;
                    std::string update = "\b\b";
                    update.push_back(frames[idx]);
                    update.push_back(']');
                    writeThinkingStatusRaw(update, use_serial_);
                    idx = (idx + 1) % 4;
                }
            });
        }

        void stop(bool newline) {
            const bool was_active = active_.exchange(false, std::memory_order_relaxed);
            if (worker_.joinable()) worker_.join();
            if (was_active && newline) writeThinkingStatusRaw("\n", use_serial_);
        }

        ~ThinkingSpinner() {
            stop(false);
        }

    private:
        bool use_serial_ = false;
        std::atomic<bool> active_{false};
        std::thread worker_;
    };
}

// ---- Send message to configured provider ----
static bool sendMessageToOllama(const std::string& query,
                                std::vector<Json::Value>& chatHistory,
                                const AppConfig& config) {
    diag_log("[DIAG] sendMessage caller src=%d\n", (int)getCurrentCommandSource());
    ensure_session_prompt(chatHistory, config);
    const std::size_t history_start = chatHistory.size();
    Json::Value msg;
    msg["role"] = "user";
    msg["content"] = query;
    chatHistory.push_back(msg);

    AppConfig request_config = config;
    request_config.tools = merge_tool_definitions(config.tools, buildRegisteredToolDefinitions(config));
    const bool has_tools = request_config.tools.isArray() && !request_config.tools.empty();

    StreamData streamData;
    const CommandSource prev = getCurrentCommandSource();
    setCurrentCommandSource(prev);
    const bool use_serial_spinner = (prev == CommandSource::SERIAL);
    ThinkingSpinner spinner(use_serial_spinner);
    spinner.start();
    streamData.start_time = std::chrono::high_resolution_clock::now();

    std::unique_ptr<StreamingTTSPlayer> tts_player;
    if (config.tts_enabled) tts_player = std::make_unique<StreamingTTSPlayer>(config);

    bool ok = false;
    bool history_committed = false;
    constexpr int kMaxToolRounds = 8;

    for (int round = 0; round < kMaxToolRounds; ++round) {
        if (round == 0 && has_tools) {
            const ToolPromptDecision decision = select_tool_with_prompt(
                std::vector<Json::Value>(chatHistory.begin(), chatHistory.end() - 1),
                query,
                config,
                request_config.tools,
                [&](const std::string& line) {
                    diag_log("%s\n", line.c_str());
                }
            );

            if (decision.valid && decision.use_tool) {
                const std::string tool_call_id = "call_local_" + std::to_string(history_start + 1);
                const PluginResult tool_result = executePluginInvocation(
                    PluginInvocation{tool_call_id, decision.tool_name, decision.arguments},
                    config
                );
                chatHistory.push_back(make_tool_call_assistant_message(decision, tool_call_id));
                chatHistory.push_back(buildPluginResultMessage(PluginInvocation{tool_call_id, decision.tool_name, decision.arguments}, tool_result));

                std::vector<Json::Value> followup_history = chatHistory;
                followup_history.push_back(make_message(
                    "user",
                    buildToolResultPrompt(query, decision.tool_name, decision.arguments, plugin_result_to_json(tool_result, decision.tool_name))
                ));

                AppConfig followup_config = request_config;
                followup_config.tools = Json::Value();
                followup_config.tool_choice = Json::Value();

                ChatProviderResult providerResult;
                ok = executeProviderChat(
                    followup_config,
                    followup_history,
                    true,
                    [&](const std::string& text) {
                        if (!streamData.first_chunk_received) {
                            streamData.first_chunk_received = true;
                            spinner.stop(true);
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
                        if (tts_player) tts_player->pushText(text);
                    },
                    [&](const std::string& line) {
                        diag_log("%s\n", line.c_str());
                    },
                    providerResult
                );

                if (!ok) {
                    if (!streamData.first_chunk_received) spinner.stop(true);
                    route_output(std::string("[Error] ") + providerResult.error_message, true);
                    if (providerResult.stream_interrupted) route_output("[Warning] Upstream stream ended early; partial output may be incomplete.", true);
                    chatHistory.resize(history_start);
                    setCurrentCommandSource(prev);
                    if (tts_player) tts_player->finish();
                    return false;
                }

                Json::Value reply = providerResult.assistant_message;
                if (!reply.isObject()) reply = Json::Value(Json::objectValue);
                reply["role"] = reply.get("role", "assistant");
                reply["content"] = providerResult.assistant_content;
                if (!providerResult.usage.isNull()) reply["usage"] = providerResult.usage;

                if (!streamData.first_chunk_received) spinner.stop(true);
                route_output("", true);
                setCurrentCommandSource(prev);
                if (tts_player) tts_player->finish();
                chatHistory.push_back(reply);
                saveCodeBlocks(providerResult.assistant_content);
                return true;
            }
        }

        ChatProviderResult providerResult;
        const bool stream_response = !has_tools && round == 0;
        ok = executeProviderChat(
            request_config,
            chatHistory,
            stream_response,
            [&](const std::string& text) {
                if (!streamData.first_chunk_received) {
                    streamData.first_chunk_received = true;
                    spinner.stop(true);
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
                if (tts_player) tts_player->pushText(text);
            },
            [&](const std::string& line) {
                diag_log("%s\n", line.c_str());
            },
            providerResult
        );

        if (!ok) {
            if (!streamData.first_chunk_received) spinner.stop(true);
            route_output(std::string("[Error] ") + providerResult.error_message, true);
            if (providerResult.stream_interrupted) route_output("[Warning] Upstream stream ended early; partial output may be incomplete.", true);
            if (!history_committed) chatHistory.resize(history_start);
            setCurrentCommandSource(prev);
            if (tts_player) tts_player->finish();
            return false;
        }

        Json::Value reply = providerResult.assistant_message;
        if (!reply.isObject()) reply = Json::Value(Json::objectValue);
        reply["role"] = reply.get("role", "assistant");
        reply["content"] = providerResult.assistant_content;
        if (!providerResult.usage.isNull()) reply["usage"] = providerResult.usage;

        const std::vector<PluginInvocation> invocations = extractPluginInvocations(reply);
        if (!invocations.empty()) {
            if (!streamData.first_chunk_received) {
                streamData.first_chunk_received = true;
                spinner.stop(true);
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - streamData.start_time
                ).count();
                route_output(std::string("[Response ") + std::to_string(elapsed) + "ms]", true);
            }

            chatHistory.push_back(reply);
            history_committed = true;
            for (const auto& invocation : invocations) {
                route_output(std::string("[Tool] ") + invocation.name, true);
                const PluginResult tool_result = executePluginInvocation(invocation, config);
                chatHistory.push_back(buildPluginResultMessage(invocation, tool_result));
            }
            continue;
        }

        if (!stream_response) {
            if (!streamData.first_chunk_received) {
                streamData.first_chunk_received = true;
                spinner.stop(true);
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - streamData.start_time
                ).count();
                route_output(std::string("[Response ") + std::to_string(elapsed) + "ms]", true);
            }
            if (!providerResult.assistant_content.empty()) {
                route_output(providerResult.assistant_content);
                if (!serial_available) {
                    std::ofstream log("log.txt", std::ios::app);
                    if (log.is_open()) {
                        log << providerResult.assistant_content;
                        log.flush();
                    }
                }
                if (tts_player) tts_player->pushText(providerResult.assistant_content);
            }
        }

        if (!streamData.first_chunk_received) spinner.stop(true);
        route_output("", true);
        setCurrentCommandSource(prev);
        if (tts_player) tts_player->finish();

        chatHistory.push_back(reply);
        saveCodeBlocks(providerResult.assistant_content);
        return true;
    }

    if (!streamData.first_chunk_received) spinner.stop(true);
    route_output("[Error] Tool execution exceeded maximum rounds.", true);
    chatHistory.resize(history_start);
    setCurrentCommandSource(prev);
    if (tts_player) tts_player->finish();
    return false;
}


// ================= Serial INT state =================
static std::atomic<bool> g_serial_int_active{false};
static std::atomic<bool> g_button_monitor_active{false};
static std::vector<Json::Value> g_chatHistory;
static std::mutex g_chatHistoryMutex;
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

bool ButtonMonitor_IsActive() {
    return g_button_monitor_active.load(std::memory_order_relaxed);
}

void ButtonMonitor_SetActive(bool enabled) {
    g_button_monitor_active.store(enabled, std::memory_order_relaxed);
}

void SerialINT_Start(AppConfig& config) {
    g_serial_int_active.store(true, std::memory_order_relaxed);
    diag_log("[DIAG] INT called from source=%d\n", (int)getCurrentCommandSource());
    route_output("[Interactive Mode] Type your messages. Type /bye to exit.", true);
    route_output("-> ", false);
}

void SerialINT_HandleLine(const std::string& line, AppConfig& config) {
    AIMaster_RAG_ConfigureRemote(config.ollama_url, config.ollama_model);
    const std::string upper_line = toupper_copy(line);
    if (line == "/bye") {
        g_serial_int_active.store(false, std::memory_order_relaxed);
        g_button_monitor_active.store(false, std::memory_order_relaxed);
        route_output("[Returning to main prompt]", true);
        route_output(modelPrompt(config, "> "), false);
        return;
    }
    if (line == "/n") {
        g_serial_int_active.store(false, std::memory_order_relaxed);
        g_button_monitor_active.store(false, std::memory_order_relaxed);
        route_output(modelPrompt(config, "> "), false);
        return;
    }
    if (upper_line == "BTN" || upper_line == "BTN OFF") {
        processCommand(line, config);
        if (SerialINT_IsActive()) route_output("-> ", false);
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
    {
        std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
        sendMessageToOllama(line, g_chatHistory, config);
    }
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

static bool eq_ci(const std::string& a, const std::string& b){
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i) if (std::tolower((unsigned char)a[i])!=std::tolower((unsigned char)b[i])) return false;
    return true;
}

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
    if (cmd_upper == "/SOUND" || cmd_upper.rfind("/SOUND ", 0) == 0) {
        std::string arg = command.size() > 6 ? trim(command.substr(6)) : "";
        std::string device_error;
        auto devices = listPlaybackDevices(device_error);
        std::string bt_error;
        auto bluetooth_devices = listConnectedBluetoothDevices(bt_error);
        Json::Value device_list(Json::arrayValue);
        for (const auto& device : devices) {
            Json::Value item(Json::objectValue);
            item["id"] = device.id;
            item["description"] = device.description;
            item["default"] = device.is_default;
            item["current"] = (device.id == config.tts_output_device);
            device_list.append(item);
        }
        result["devices"] = device_list;

        if (arg.empty()) {
            route_output("Available sound output devices:", true);
            for (size_t i = 0; i < devices.size(); ++i) {
                std::string label = devices[i].description.empty() ? devices[i].id : devices[i].description;
                std::string line = "  [" + std::to_string(i + 1) + "] " + label;
                if (devices[i].is_default) line += " (default)";
                if (devices[i].id == config.tts_output_device) line += " (current)";
                route_output(line, true);
            }
            if (!device_error.empty()) route_output(std::string("[Warn] ") + device_error, true);
            if (!bt_error.empty()) route_output(std::string("[Warn] ") + bt_error, true);
            if (!bluetooth_devices.empty()) {
                route_output("Only live Bluetooth playback profiles are listed above. If a headset is in mic/SCO mode, A2DP speaker output may be unavailable until it switches back.", true);
            } else {
                route_output("Use /pair to scan, pair, and connect a Bluetooth speaker/headset so it appears in the numbered list above.", true);
            }
            route_output("Use: /sound <#|device> to set the TTS output device.", true);
            result["status"] = "success";
            return result;
        }

        int idx = -1;
        try { idx = std::stoi(arg); } catch (...) { idx = -1; }
        std::string chosen;
        if (idx >= 1 && idx <= (int)devices.size()) {
            chosen = devices[idx - 1].id;
        } else {
            for (const auto& device : devices) {
                if (device.id == arg || eq_ci(device.id, arg) || eq_ci(device.description, arg)) {
                    chosen = device.id;
                    break;
                }
            }
        }

        if (chosen.empty()) {
            route_output(std::string("[Error] Sound device not found: ") + arg, true);
            result["status"] = "error";
            return result;
        }

        config.tts_output_device = chosen;
        result["tts_output_device"] = chosen;
        std::string bt_status;
        configureBluetoothReconnectService(config, bt_status);
        if (saveConfig("config.txt", config)) {
            route_output(std::string("[OK] tts_output_device=") + chosen + " (saved)", true);
        } else {
            route_output(std::string("[OK] tts_output_device=") + chosen + " (save failed)", true);
        }
        if (!bt_status.empty()) route_output(bt_status, true);
        result["status"] = "success";
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
            std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
            sendMessageToOllama(query, g_chatHistory, config);
            result["status"] = "success";
        }
        return result;
    }
    // ===== INT (interactive mode) =====
    else if (cmd_upper == "INT") {
        SerialINT_Start(config);
        result["status"] = "success";
        result["prompt_emitted"] = true;
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

        {
            std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
            sendMessageToOllama(fullMessage, g_chatHistory, config);
        }
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
        result["user_prompt_file"] = config.user_prompt_file;
        result["weather_plugin_enabled"] = config.weather_plugin_enabled;
        result["weather_geocoding_url"] = config.weather_geocoding_url;
        result["weather_forecast_url"] = config.weather_forecast_url;
        result["weather_timeout_seconds"] = Json::Value(static_cast<Json::UInt64>(config.weather_timeout_seconds));
        result["tts_enabled"] = config.tts_enabled;
        result["tts_endpoint_url"] = config.tts_endpoint_url;
        result["tts_timeout_seconds"] = Json::Value(static_cast<Json::UInt64>(config.tts_timeout_seconds));
        result["tts_voice"] = config.tts_voice;
        result["tts_speaker"] = config.tts_speaker;
        result["tts_output_device"] = config.tts_output_device;
        route_output("Current configuration:", true);
        route_output(std::string("\tSerial port: ") + config.serial_port, true);
        route_output(std::string("\tBaudrate: ") + std::to_string(config.baudrate), true);
        route_output(std::string("\tProvider type: ") + effectiveProviderType(config), true);
        route_output(std::string("\tAPI base: ") + effectiveApiBase(config), true);
        route_output(std::string("\tModel: ") + effectiveModel(config), true);
        route_output(std::string("\tTimeout (s): ") + std::to_string(effectiveTimeoutSeconds(config)), true);
        route_output(std::string("\tUser prompt file: ") + config.user_prompt_file, true);
        route_output(std::string("\tWeather plugin: ") + (config.weather_plugin_enabled ? "enabled" : "disabled"), true);
        route_output(std::string("\tWeather geocoding URL: ") + config.weather_geocoding_url, true);
        route_output(std::string("\tWeather forecast URL: ") + config.weather_forecast_url, true);
        route_output(std::string("\tLegacy Ollama URL: ") + config.ollama_url, true);
        route_output(std::string("\tRAG chunks (ASK/INT): ") + std::to_string(config.rag_chunks), true);
        route_output(std::string("\tRAG threshold (ASK/INT): ") + std::to_string(config.rag_threshold), true);
        route_output(std::string("\tChar delay: ") + std::to_string(config.serial_delay_ms), true);
        route_output(std::string("\tNewline: ") + config.serial_newline, true);
        route_output(std::string("\tTTS output device: ") + config.tts_output_device, true);
        route_output(std::string("\tSTT endpoint: ") + (config.stt_endpoint_url.empty() ? "<unset>" : config.stt_endpoint_url), true);
        route_output(std::string("\tSTT model: ") + (config.stt_model.empty() ? "<unset>" : config.stt_model), true);
        route_output(std::string("\tMic record device: ") + (config.mic_record_device.empty() ? "<default>" : config.mic_record_device), true);
        route_output(std::string("\tMic HID binding: ") + micServiceStatus(config), true);
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
            cmds["ASK <question>"] = "Ask the model without an extra prompt.";
            cmds["INT"] = "Enter interactive mode with the model.";
            cmds["READ"] = "Send a file with context to the model.";
            cmds["READPICK"] = "Pick a file from the code directory, then send it with context.";
            cmds["RESET"] = "Clear chat history.";
            cmds["QUIT"] = "Exit AImaster.";
            cmds["DELAY <ms>"] = "Set the serial character send delay.";
            cmds["DIAG [on|off]"] = "Toggle diagnostic logging.";
            cmds["BTN"] = "Enter Dream Cheeky button test mode; in INT, lid/button transitions are shown while the button still controls the mic.";
            cmds["BTN OFF"] = "Stop Dream Cheeky button monitoring.";
            cmds["/RESET"] = "Clear the UART screen and redraw the welcome banner.";
            cmds["/speak on"] = "Enable text-to-speech output for assistant replies.";
            cmds["/speak off"] = "Disable text-to-speech output.";
            cmds["/sound"] = "List sound output devices, including known Bluetooth speaker/headset targets.";
            cmds["/sound <#|device>"] = "Select the TTS output device.";
            cmds["/mic"] = "Show microphone hotkey/recording status.";
            cmds["/mic list"] = "List available microphone capture devices.";
            cmds["/mic device <#|device>"] = "Select the microphone capture device.";
            cmds["/mic on"] = "Start microphone recording immediately.";
            cmds["/mic off"] = "Stop microphone recording and transcribe/send it.";
            cmds["/mic toggle"] = "Toggle microphone recording manually.";
            cmds["/hid"] = "Show the current HID microphone-button binding.";
            cmds["/hid list"] = "List available /dev/input/event* devices.";
            cmds["/hid capture"] = "Wait for a button press and bind it to microphone toggle.";
            cmds["/pair"] = "Scan for named Bluetooth devices.";
            cmds["/pair <#|MAC>"] = "Pair, trust, and connect a Bluetooth device.";
            cmds["CFG"] = "Show current configuration.";
            cmds["HELP"] = "List available commands.";
            cmds["MODEL"] = "List available models.";
            cmds["MODEL <#|name>"] = "Select the active model.";
            cmds["RAG_INGEST"] = "Ingest a folder into the RAG system.";
            cmds["RAG_SHOW"] = "Show the contents of the RAG ingestion.";
            cmds["RAG_ASK"] = "Ask RAG: RAG_ASK [--k N] [--thr T] <question...> (ASK/INT defaults use config: rag_chunks, rag_threshold).";
            cmds["RAG_SESSION SHOW"] = "Display the active RAG session.";
            cmds["RAG_SESSION SET <sid>"] = "Set the active RAG session.";
            cmds["RAG_SESSION CLEAR"] = "Clear the active RAG session.";
        }
        result["commands"] = cmds;
        route_output("Available commands:", true);
        for (auto& key : cmds.getMemberNames()) {
            route_output("  " + key + " - " + cmds[key].asString(), true);
        }
        return result;
    }
    // ===== BTN =====
    else if (cmd_upper == "BTN" || cmd_upper == "BTN OFF") {
        if (cmd_upper == "BTN OFF") {
            ButtonMonitor_SetActive(false);
            route_output("[BTN] Test mode stopped.", true);
            result["status"] = "success";
            return result;
        }
        if (!init_button()) {
            route_output("[Error] Dream Cheeky button not found or not accessible.", true);
            result["status"] = "error";
            return result;
        }
        int state = poll_button();
        if (state == BUTTON_NO_CHANGE) state = button_last_state();
        route_output(std::string("[BTN] Current state: ") + button_state_name(state), true);
        if (SerialINT_IsActive()) {
            ButtonMonitor_SetActive(true);
            route_output("[BTN] Test mode active. Lid/button transitions will be shown while the button still toggles the mic. Use BTN OFF to stop test mode.", true);
            route_output("-> ", false);
        }
        result["status"] = "success";
        return result;
    }
    // ===== /RESET =====
    else if (cmd_upper == "/RESET") {
        if (getCurrentCommandSource() == CommandSource::SERIAL) {
            serialResetTerminal(config);
            result["status"] = "success";
            result["message"] = "UART terminal reset.";
            result["prompt_emitted"] = true;
        } else {
            route_output("[Info] /RESET is only available over the serial terminal.", true);
            result["status"] = "error";
            result["message"] = "/RESET is only available over the serial terminal.";
        }
        return result;
    }
    // ===== /MIC =====
    else if (cmd_upper == "/MIC" || cmd_upper.rfind("/MIC ", 0) == 0) {
        const std::string arg = command.size() > 4 ? trim(command.substr(4)) : "";
        std::string status;
        bool ok = true;
        if (arg.empty() || eq_ci(arg, "status")) {
            status = "[Mic] " + micServiceStatus(config);
        } else if (eq_ci(arg, "list") || eq_ci(arg, "devices")) {
            std::string device_error;
            const auto devices = listCaptureDevices(device_error);
            route_output("Available microphone input devices:", true);
            for (size_t i = 0; i < devices.size(); ++i) {
                std::string label = devices[i].description.empty() ? devices[i].id : devices[i].description;
                std::string line = "  [" + std::to_string(i + 1) + "] " + label;
                if (devices[i].is_default) line += " (default)";
                if ((config.mic_record_device.empty() && devices[i].id == "default") ||
                    (!config.mic_record_device.empty() && devices[i].id == config.mic_record_device)) {
                    line += " (current)";
                }
                route_output(line, true);
            }
            route_output("Use /pair to connect a Bluetooth microphone/headset; once connected it may appear in the ALSA capture list above.", true);
            if (!device_error.empty()) route_output(std::string("[Warn] ") + device_error, true);
            route_output("Use: /mic device <#|device>", true);
            result["status"] = "success";
            return result;
        } else if (arg.rfind("device ", 0) == 0) {
            const std::string choice = trim(arg.substr(7));
            std::string device_error;
            const auto devices = listCaptureDevices(device_error);
            std::string chosen;
            int idx = -1;
            try { idx = std::stoi(choice); } catch (...) { idx = -1; }
            if (idx >= 1 && idx <= static_cast<int>(devices.size())) {
                chosen = devices[idx - 1].id;
            } else {
                for (const auto& device : devices) {
                    if (device.id == choice || eq_ci(device.id, choice) || eq_ci(device.description, choice)) {
                        chosen = device.id;
                        break;
                    }
                }
            }
            if (chosen.empty()) {
                ok = false;
                status = std::string("[Error] Microphone device not found: ") + choice;
            } else {
                config.mic_record_device = (chosen == "default") ? "" : chosen;
                if (saveConfig("config.txt", config)) {
                    status = std::string("[OK] mic_record_device=") + (config.mic_record_device.empty() ? "default" : config.mic_record_device) + " (saved)";
                } else {
                    status = std::string("[OK] mic_record_device=") + (config.mic_record_device.empty() ? "default" : config.mic_record_device) + " (save failed)";
                }
            }
        } else if (eq_ci(arg, "on") || eq_ci(arg, "start")) {
            ok = startMicRecording(config, status);
        } else if (eq_ci(arg, "off") || eq_ci(arg, "stop")) {
            ok = stopMicRecording(config, status);
        } else if (eq_ci(arg, "toggle")) {
            ok = toggleMicRecording(config, status);
        } else {
            ok = false;
            status = "Usage: /mic [status|list|device <#|device>|on|off|toggle]";
        }
        route_output(status, true);
        result["status"] = ok ? "success" : "error";
        result["message"] = status;
        return result;
    }
    // ===== /HID =====
    else if (cmd_upper == "/HID" || cmd_upper.rfind("/HID ", 0) == 0) {
        const std::string arg = command.size() > 4 ? trim(command.substr(4)) : "";
        if (arg.empty() || eq_ci(arg, "status")) {
            route_output("[HID] " + micServiceStatus(config), true);
            result["status"] = "success";
            return result;
        }
        if (eq_ci(arg, "list")) {
            std::string error;
            const auto devices = listInputDevices(error);
            route_output("Input devices:", true);
            for (size_t i = 0; i < devices.size(); ++i) {
                route_output("  [" + std::to_string(i + 1) + "] " + devices[i].path + " - " + devices[i].name, true);
            }
            if (!error.empty()) route_output("[HID] " + error, true);
            result["status"] = devices.empty() ? "error" : "success";
            return result;
        }
        if (eq_ci(arg, "capture")) {
            route_output("[HID] Press the target button now...", true);
            HidCaptureResult capture;
            std::string error;
            if (!captureHidButton(config, capture, error)) {
                route_output("[HID] " + error, true);
                result["status"] = "error";
                return result;
            }
            config.hid_input_device = capture.path;
            config.hid_input_name = capture.name;
            config.hid_button_code = capture.code;
            const bool saved = saveConfig("config.txt", config);
            std::string status;
            configureMicHotkeyService(config, MicTranscriptCallback{}, status);
            route_output("[HID] Bound microphone toggle to " + capture.path + " (" + capture.name +
                         "), code " + std::to_string(capture.code) + (saved ? " (saved)" : " (save failed)"), true);
            result["status"] = saved ? "success" : "error";
            return result;
        }
        route_output("Usage: /hid [status|list|capture]", true);
        result["status"] = "error";
        return result;
    }
    // ===== /PAIR =====
    else if (cmd_upper == "/PAIR" || cmd_upper.rfind("/PAIR ", 0) == 0) {
        const std::string arg = command.size() > 5 ? trim(command.substr(5)) : "";
        if (arg.empty()) {
            route_output("[Bluetooth] Scanning...", true);
            std::string error;
            const auto devices = scanBluetoothDevices(config, error);
            for (size_t i = 0; i < devices.size(); ++i) {
                route_output("  [" + std::to_string(i + 1) + "] " + devices[i].mac + " - " + devices[i].name, true);
            }
            if (!error.empty()) route_output("[Bluetooth] " + error, true);
            else route_output("Use: /pair <#|MAC>", true);
            result["status"] = devices.empty() ? "error" : "success";
            return result;
        }
        std::string status;
        const bool ok = pairBluetoothDevice(config, arg, status);
        route_output(status, true);
        result["status"] = ok ? "success" : "error";
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
        std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
        g_chatHistory.clear();
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

void submitMicTranscript(const std::string& transcript, AppConfig& config) {
    const std::string clean = trim(transcript);
    if (clean.empty()) return;

    route_output("[Mic] " + clean, true);

    std::string rag_answer;
    if (rag_int::TryRAGAnswer(clean, rag_answer, /*k=*/config.rag_chunks, /*threshold=*/config.rag_threshold)) {
        route_output(rag_answer, true);
        return;
    }

    std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
    sendMessageToOllama(clean, g_chatHistory, config);
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
        {
            std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
            sendMessageToOllama(fullMessage, g_chatHistory, config);
        }
        ReadAwait_Reset(); route_output(modelPrompt(config, SerialINT_IsActive() ? "-> " : "> ")); return;
    }
    if (st == ReadStage::WaitingContextPresetFile) {
        g_read_context = line;
        std::string filename = g_read_preset_filename;
        if (std::filesystem::is_directory(filename)) { if (list_dir_and_prompt(filename)) g_read_stage.store(ReadStage::WaitingPickIndex_ContextKnown, std::memory_order_relaxed); return; }
        if (!std::filesystem::exists(filename) || std::filesystem::is_empty(filename)) { route_output("[Error] Invalid file.", true); ReadAwait_Reset(); return; }
        std::ifstream inFile(filename); std::stringstream buffer; buffer << inFile.rdbuf(); std::string fileContents = buffer.str();
        std::string fullMessage = "Context: " + g_read_context + "\n\nFile contents:\n" + fileContents + "\n\nInstruction: Please read and store this content for later reference in our ongoing conversation. " "Acknowledge once you have absorbed it.";
        {
            std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
            sendMessageToOllama(fullMessage, g_chatHistory, config);
        }
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
            {
                std::lock_guard<std::mutex> lock(g_chatHistoryMutex);
                sendMessageToOllama(fullMessage, g_chatHistory, config);
            }
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
