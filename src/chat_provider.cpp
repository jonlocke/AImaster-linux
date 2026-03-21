#include "chat_provider.hpp"

#include "endpoint_utils.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <sstream>

namespace {

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

size_t write_to_string(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    std::string* out = static_cast<std::string*>(userp);
    out->append(static_cast<const char*>(contents), total);
    return total;
}

std::string join_url(std::string base, const std::string& path) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

bool is_openai_provider(const AppConfig& config) {
    return effectiveProviderType(config) == "openai_compatible";
}

std::string map_finish_reason(const std::string& finish_reason) {
    if (finish_reason.empty()) return "stop";
    if (finish_reason == "length") return "length";
    if (finish_reason == "tool_calls" || finish_reason == "function_call") return "tool_calls";
    if (finish_reason == "content_filter") return "content_filter";
    return finish_reason;
}

Json::Value build_openai_message_from_delta(const Json::Value& delta, const Json::Value& final_message) {
    Json::Value message = final_message;
    if (!message.isObject()) message = Json::Value(Json::objectValue);
    if (delta.isMember("role") && delta["role"].isString()) message["role"] = delta["role"].asString();
    if (delta.isMember("content")) {
        if (!message.isMember("content")) message["content"] = "";
        message["content"] = message["content"].asString() + delta["content"].asString();
    }
    if (delta.isMember("tool_calls")) message["tool_calls"] = delta["tool_calls"];
    if (delta.isMember("function_call")) message["function_call"] = delta["function_call"];
    return message;
}

Json::Value map_openai_nonstream_response(const Json::Value& root, ChatProviderResult& out_result) {
    if (!root.isObject() || !root.isMember("choices") || !root["choices"].isArray() || root["choices"].empty()) {
        out_result.error_message = "invalid upstream response shape: missing choices[0]";
        return Json::Value();
    }

    const Json::Value& choice = root["choices"][0];
    Json::Value message = choice["message"];
    if (!message.isObject()) {
        out_result.error_message = "invalid upstream response shape: missing choices[0].message";
        return Json::Value();
    }

    out_result.assistant_message = Json::Value(Json::objectValue);
    out_result.assistant_message["role"] = message.get("role", "assistant");
    out_result.assistant_message["content"] = message.get("content", "");
    if (message.isMember("tool_calls")) out_result.assistant_message["tool_calls"] = message["tool_calls"];
    if (message.isMember("function_call")) out_result.assistant_message["function_call"] = message["function_call"];
    out_result.assistant_content = out_result.assistant_message["content"].asString();
    out_result.assistant_message["finish_reason"] = map_finish_reason(choice.get("finish_reason", "stop").asString());
    if (root.isMember("usage")) out_result.usage = root["usage"];
    return out_result.assistant_message;
}

struct StreamState {
    const AppConfig* config = nullptr;
    ChatProviderResult* result = nullptr;
    ChatStreamEmitter on_text;
    ChatLogEmitter on_log;
    std::string buffer;
    std::string raw_output;
    Json::Value assembled_message = Json::Value(Json::objectValue);
    bool first_chunk_received = false;
    bool saw_done_marker = false;
};

void emit_log(const StreamState& state, const std::string& msg) {
    if (state.on_log) state.on_log(msg);
}

void append_openai_chunk(const Json::Value& parsed, StreamState& state) {
    if (!parsed.isObject()) return;
    if (!parsed.isMember("choices") || !parsed["choices"].isArray() || parsed["choices"].empty()) return;

    const Json::Value& choice = parsed["choices"][0];
    if (choice.isMember("delta") && choice["delta"].isObject()) {
        const Json::Value& delta = choice["delta"];
        state.assembled_message = build_openai_message_from_delta(delta, state.assembled_message);
        if (delta.isMember("content") && delta["content"].isString()) {
            const std::string text = delta["content"].asString();
            if (!text.empty() && state.on_text) state.on_text(text);
            state.result->assistant_content += text;
        }
    }
    if (choice.isMember("message") && choice["message"].isObject()) {
        state.assembled_message = choice["message"];
        state.result->assistant_content = choice["message"].get("content", "").asString();
    }
    if (choice.isMember("finish_reason") && !choice["finish_reason"].isNull()) {
        state.result->assistant_message["finish_reason"] = map_finish_reason(choice["finish_reason"].asString());
    }
    if (parsed.isMember("usage")) state.result->usage = parsed["usage"];
}

void append_ollama_chunk(const Json::Value& parsed, StreamState& state) {
    if (!parsed.isObject()) return;
    if (parsed.isMember("message") && parsed["message"].isObject()) {
        const Json::Value& message = parsed["message"];
        if (message.isMember("content") && message["content"].isString()) {
            const std::string text = message["content"].asString();
            if (!text.empty() && state.on_text) state.on_text(text);
            state.result->assistant_content += text;
        }
        if (message.isMember("tool_calls")) state.result->assistant_message["tool_calls"] = message["tool_calls"];
    }
    if (parsed.isMember("done_reason") && parsed["done_reason"].isString()) {
        state.result->assistant_message["finish_reason"] = map_finish_reason(parsed["done_reason"].asString());
    }
    static const char* usage_fields[] = {"prompt_eval_count", "eval_count", "total_duration", "load_duration"};
    for (const char* field : usage_fields) {
        if (parsed.isMember(field)) state.result->usage[field] = parsed[field];
    }
}

bool process_json_line(const std::string& line, StreamState& state) {
    std::string trimmed = trim_copy(line);
    if (trimmed.empty()) return true;

    if (is_openai_provider(*state.config)) {
        if (trimmed.rfind("data:", 0) == 0) trimmed = trim_copy(trimmed.substr(5));
        if (trimmed == "[DONE]") {
            state.saw_done_marker = true;
            return true;
        }
    }

    Json::CharReaderBuilder reader;
    Json::Value parsed;
    std::string errs;
    std::istringstream ss(trimmed);
    if (!Json::parseFromStream(reader, ss, &parsed, &errs)) {
        emit_log(state, "[Debug] Skipping malformed stream chunk from upstream.");
        return false;
    }

    if (is_openai_provider(*state.config)) append_openai_chunk(parsed, state);
    else append_ollama_chunk(parsed, state);
    return true;
}

void finalize_result(StreamState& state) {
    if (state.result->assistant_message.isNull() || !state.result->assistant_message.isObject()) {
        state.result->assistant_message = Json::Value(Json::objectValue);
    }
    if (is_openai_provider(*state.config)) {
        if (state.assembled_message.isObject()) {
            for (const auto& member : state.assembled_message.getMemberNames()) {
                state.result->assistant_message[member] = state.assembled_message[member];
            }
        }
        if (!state.result->assistant_message.isMember("role")) state.result->assistant_message["role"] = "assistant";
        if (!state.result->assistant_message.isMember("content")) state.result->assistant_message["content"] = state.result->assistant_content;
    } else {
        state.result->assistant_message["role"] = "assistant";
        state.result->assistant_message["content"] = state.result->assistant_content;
    }
}

size_t streaming_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    StreamState* state = static_cast<StreamState*>(userp);
    state->raw_output.append(static_cast<const char*>(contents), total);
    state->buffer.append(static_cast<const char*>(contents), total);

    while (true) {
        size_t newline = state->buffer.find('\n');
        if (newline == std::string::npos) break;
        std::string line = state->buffer.substr(0, newline);
        state->buffer.erase(0, newline + 1);
        process_json_line(line, *state);
    }

    return total;
}

void add_standard_headers(const AppConfig& config, struct curl_slist*& headers) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");
    if (!config.api_key.empty()) {
        headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + config.api_key).c_str());
    }
    if (!config.organization.empty()) {
        headers = curl_slist_append(headers, (std::string("OpenAI-Organization: ") + config.organization).c_str());
    }
    if (!config.project.empty()) {
        headers = curl_slist_append(headers, (std::string("OpenAI-Project: ") + config.project).c_str());
    }
    for (const auto& item : config.extra_headers) {
        headers = curl_slist_append(headers, (item.first + ": " + item.second).c_str());
    }
}

std::string summarize_payload(const Json::Value& payload) {
    std::ostringstream out;
    out << "model=" << payload.get("model", "").asString();
    out << ", messages=" << (payload.isMember("messages") && payload["messages"].isArray() ? static_cast<int>(payload["messages"].size()) : 0);
    out << ", stream=" << (payload.get("stream", false).asBool() ? "true" : "false");
    if (payload.isMember("temperature")) out << ", temperature=" << payload["temperature"].asDouble();
    if (payload.isMember("max_tokens")) out << ", max_tokens=" << payload["max_tokens"].asInt();
    if (payload.isMember("format")) out << ", format=present";
    if (payload.isMember("response_format")) out << ", response_format=present";
    return out.str();
}

} // namespace

std::string effectiveProviderType(const AppConfig& config) {
    const std::string provider = to_lower_copy(trim_copy(config.provider_type));
    return provider.empty() ? "ollama_native" : provider;
}

std::string effectiveApiBase(const AppConfig& config) {
    if (!config.api_base.empty()) return trim_copy(config.api_base);
    return trim_copy(config.ollama_url);
}

std::string effectiveModel(const AppConfig& config) {
    if (!config.model.empty()) return config.model;
    return config.ollama_model;
}

long effectiveTimeoutSeconds(const AppConfig& config) {
    return config.timeout > 0 ? config.timeout : config.ollama_timeout_seconds;
}

std::string effectiveChatUrl(const AppConfig& config) {
    if (is_openai_provider(config)) return join_url(effectiveApiBase(config), "/v1/chat/completions");
    return EndpointResolver::normalize(effectiveApiBase(config)) + "/api/chat";
}

std::string deriveModelsUrl(const AppConfig& config) {
    if (is_openai_provider(config)) return join_url(effectiveApiBase(config), "/v1/models");
    return EndpointResolver::deriveTagsEndpoint(effectiveApiBase(config));
}

Json::Value buildProviderPayload(const AppConfig& config,
                                 const std::vector<Json::Value>& chat_history,
                                 bool stream) {
    Json::Value payload(Json::objectValue);
    payload["model"] = effectiveModel(config);
    payload["messages"] = Json::arrayValue;
    for (const auto& msg : chat_history) payload["messages"].append(msg);
    payload["stream"] = stream;

    if (is_openai_provider(config)) {
        if (config.temperature >= 0.0) payload["temperature"] = config.temperature;
        if (config.num_predict > 0) payload["max_tokens"] = config.num_predict;
        if (!config.format.empty()) {
            Json::Value response_format(Json::objectValue);
            if (config.format == "json" || config.format == "json_object") response_format["type"] = "json_object";
            else response_format["type"] = "text";
            payload["response_format"] = response_format;
            payload["format"] = config.format;
        }
        if (!config.tools.isNull()) payload["tools"] = config.tools;
        if (!config.tool_choice.isNull()) payload["tool_choice"] = config.tool_choice;
    } else {
        if (config.temperature >= 0.0 || config.num_predict > 0) {
            payload["options"] = Json::Value(Json::objectValue);
            if (config.temperature >= 0.0) payload["options"]["temperature"] = config.temperature;
            if (config.num_predict > 0) payload["options"]["num_predict"] = config.num_predict;
        }
        if (!config.format.empty()) payload["format"] = config.format;
        if (!config.tools.isNull()) payload["tools"] = config.tools;
        if (!config.tool_choice.isNull()) payload["tool_choice"] = config.tool_choice;
    }
    return payload;
}

bool executeProviderChat(const AppConfig& config,
                         const std::vector<Json::Value>& chat_history,
                         bool stream,
                         const ChatStreamEmitter& on_text,
                         const ChatLogEmitter& on_log,
                         ChatProviderResult& out_result) {
    const std::string chat_url = effectiveChatUrl(config);
    const Json::Value payload = buildProviderPayload(config, chat_history, stream);
    Json::StreamWriterBuilder builder;
    const std::string body = Json::writeString(builder, payload);

    if (on_log) {
        on_log("[Debug] Provider selected: " + effectiveProviderType(config));
        on_log("[Debug] Upstream URL: " + chat_url);
        on_log("[Debug] Chat mode: " + std::string(stream ? "streaming" : "non-streaming"));
        on_log("[Debug] Request summary: " + summarize_payload(payload));
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        out_result.error_message = "curl init failed";
        return false;
    }

    std::string response;
    StreamState stream_state;
    stream_state.config = &config;
    stream_state.result = &out_result;
    stream_state.on_text = on_text;
    stream_state.on_log = on_log;

    struct curl_slist* headers = nullptr;
    add_standard_headers(config, headers);

    curl_easy_setopt(curl, CURLOPT_URL, chat_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, effectiveTimeoutSeconds(config));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, effectiveTimeoutSeconds(config));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);

    if (stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streaming_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_state);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    }

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_result.http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        out_result.stream_interrupted = stream;
        if (res == CURLE_OPERATION_TIMEDOUT) out_result.error_message = "request timed out after " + std::to_string(effectiveTimeoutSeconds(config)) + " seconds";
        else out_result.error_message = std::string("upstream request failed: ") + curl_easy_strerror(res);
        return false;
    }

    if (out_result.http_status == 401 || out_result.http_status == 403) {
        out_result.error_message = "authentication failed with upstream provider (HTTP " + std::to_string(out_result.http_status) + ")";
        return false;
    }
    if (out_result.http_status == 404) {
        out_result.error_message = "upstream endpoint not found (HTTP 404). Check provider_type and api_base/path.";
        return false;
    }
    if (out_result.http_status >= 400) {
        out_result.error_message = "upstream returned HTTP " + std::to_string(out_result.http_status);
        return false;
    }

    if (stream) {
        if (!stream_state.buffer.empty()) process_json_line(stream_state.buffer, stream_state);
        finalize_result(stream_state);
        out_result.success = !out_result.assistant_content.empty() || !out_result.assistant_message.isNull();
        if (is_openai_provider(config) && !stream_state.saw_done_marker) {
            out_result.stream_interrupted = true;
            if (on_log) on_log("[Debug] OpenAI-compatible stream ended without [DONE] marker.");
        }
    } else {
        Json::CharReaderBuilder reader;
        Json::Value root;
        std::string errs;
        std::istringstream ss(response);
        if (!Json::parseFromStream(reader, ss, &root, &errs)) {
            out_result.error_message = "invalid upstream response shape: unable to parse JSON";
            return false;
        }
        if (is_openai_provider(config)) map_openai_nonstream_response(root, out_result);
        else {
            if (!root.isObject() || !root.isMember("message") || !root["message"].isObject()) {
                out_result.error_message = "invalid upstream response shape: missing message object";
                return false;
            }
            out_result.assistant_message = root["message"];
            out_result.assistant_content = root["message"].get("content", "").asString();
            if (root.isMember("prompt_eval_count")) out_result.usage["prompt_eval_count"] = root["prompt_eval_count"];
            if (root.isMember("eval_count")) out_result.usage["eval_count"] = root["eval_count"];
        }
        out_result.success = out_result.error_message.empty();
    }

    if (out_result.success && on_log) {
        on_log("[Debug] Response summary: content_chars=" + std::to_string(out_result.assistant_content.size()) +
               ", finish_reason=" + out_result.assistant_message.get("finish_reason", "").asString());
    }
    return out_result.success;
}

std::vector<std::string> parseModelListResponse(const AppConfig& config,
                                                const std::string& response,
                                                std::string& error) {
    std::vector<std::string> models;
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    std::istringstream ss(response);
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        error = "JSON parse error: " + errs;
        return models;
    }

    if (is_openai_provider(config)) {
        if (!root.isObject() || !root.isMember("data") || !root["data"].isArray()) {
            error = "unexpected response";
            return models;
        }
        for (const auto& item : root["data"]) {
            if (item.isObject() && item.isMember("id") && item["id"].isString()) models.push_back(item["id"].asString());
        }
        return models;
    }

    if (root.isObject() && root.isMember("models") && root["models"].isArray()) {
        for (const auto& m : root["models"]) {
            if (m.isObject() && m.isMember("name") && m["name"].isString()) models.push_back(m["name"].asString());
            else if (m.isObject() && m.isMember("model") && m["model"].isString()) models.push_back(m["model"].asString());
        }
    } else {
        error = "unexpected response";
    }
    return models;
}
