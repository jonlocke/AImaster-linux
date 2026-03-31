void run_tts_tests();

#include "chat_provider.hpp"
#include "config_loader.h"
#include "tool_plugins.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

static void test_config_defaults() {
    AppConfig cfg;
    assert(effectiveProviderType(cfg) == "ollama_native");
    assert(effectiveApiBase(cfg) == "http://localhost:11434");
    assert(effectiveModel(cfg) == "gemma3:4b");
    assert(effectiveTimeoutSeconds(cfg) == 5);
    assert(effectiveChatUrl(cfg) == "http://localhost:11434/api/chat");
}

static void test_openai_payload_mapping() {
    AppConfig cfg;
    cfg.provider_type = "openai_compatible";
    cfg.api_base = "http://litellm:4000";
    cfg.model = "gpt-4o-mini";
    cfg.temperature = 0.25;
    cfg.num_predict = 128;
    cfg.format = "json";
    Json::Value tool(Json::objectValue);
    tool["type"] = "function";
    cfg.tools = Json::arrayValue;
    cfg.tools.append(tool);

    std::vector<Json::Value> history;
    Json::Value sys(Json::objectValue); sys["role"] = "system"; sys["content"] = "You are helpful."; history.push_back(sys);
    Json::Value usr(Json::objectValue); usr["role"] = "user"; usr["content"] = "Hi"; history.push_back(usr);

    Json::Value payload = buildProviderPayload(cfg, history, true);
    assert(payload["model"].asString() == "gpt-4o-mini");
    assert(payload["messages"].size() == 2);
    assert(payload["temperature"].asDouble() == 0.25);
    assert(payload["max_tokens"].asInt() == 128);
    assert(payload["stream"].asBool());
    assert(payload.isMember("response_format"));
    assert(payload["tools"].isArray());
}

static void test_ollama_payload_mapping() {
    AppConfig cfg;
    cfg.temperature = 0.7;
    cfg.num_predict = 42;
    cfg.format = "json";
    std::vector<Json::Value> history;
    Json::Value usr(Json::objectValue); usr["role"] = "user"; usr["content"] = "Hello"; history.push_back(usr);
    Json::Value payload = buildProviderPayload(cfg, history, true);
    assert(payload["model"].asString() == "gemma3:4b");
    assert(payload["options"]["temperature"].asDouble() == 0.7);
    assert(payload["options"]["num_predict"].asInt() == 42);
    assert(payload["format"].asString() == "json");
}

static void test_model_list_parsing() {
    AppConfig openai;
    openai.provider_type = "openai_compatible";
    std::string err;
    auto ids = parseModelListResponse(openai, R"({"data":[{"id":"alpha"},{"id":"beta"}]})", err);
    assert(err.empty());
    assert(ids.size() == 2 && ids[0] == "alpha" && ids[1] == "beta");

    AppConfig ollama;
    err.clear();
    auto names = parseModelListResponse(ollama, R"({"models":[{"name":"qwen3:4b"}]})", err);
    assert(err.empty());
    assert(names.size() == 1 && names[0] == "qwen3:4b");
}

static void test_registered_tool_definitions() {
    AppConfig cfg;
    Json::Value tools = buildRegisteredToolDefinitions(cfg);
    assert(tools.isArray());
    assert(tools.size() >= 1);
    assert(tools[0]["type"].asString() == "function");
    assert(tools[0]["function"]["name"].asString() == "get_weather");

    cfg.weather_plugin_enabled = false;
    Json::Value disabled = buildRegisteredToolDefinitions(cfg);
    assert(disabled.isArray());
    assert(disabled.empty());
}

static void test_tool_call_extraction() {
    Json::Value message(Json::objectValue);
    message["tool_calls"] = Json::arrayValue;
    Json::Value call(Json::objectValue);
    call["id"] = "call_123";
    call["function"] = Json::Value(Json::objectValue);
    call["function"]["name"] = "get_weather";
    call["function"]["arguments"] = R"({"location":"London","days":2})";
    message["tool_calls"].append(call);

    auto invocations = extractPluginInvocations(message);
    assert(invocations.size() == 1);
    assert(invocations[0].id == "call_123");
    assert(invocations[0].name == "get_weather");
    assert(invocations[0].arguments["location"].asString() == "London");
    assert(invocations[0].arguments["days"].asInt() == 2);
}

static void test_config_loader_fallback() {
    AppConfig cfg;
    std::ofstream out("/tmp/aimaster_test_config.txt");
    out << "ollama_url=http://legacy-host:11434/api/chat\n";
    out << "ollama_model=legacy-model\n";
    out << "ollama_timeout_seconds=9\n";
    out.close();
    assert(loadConfig("/tmp/aimaster_test_config.txt", cfg));
    assert(effectiveProviderType(cfg) == "ollama_native");
    assert(effectiveApiBase(cfg) == "http://legacy-host:11434/api/chat");
    assert(effectiveModel(cfg) == "legacy-model");
    assert(effectiveTimeoutSeconds(cfg) == 9);
}

int main() {
    test_config_defaults();
    test_openai_payload_mapping();
    test_ollama_payload_mapping();
    test_model_list_parsing();
    test_config_loader_fallback();
    test_registered_tool_definitions();
    test_tool_call_extraction();
    run_tts_tests();
    std::cout << "chat_provider_tests passed\n";
    return 0;
}
