#pragma once

#include "config_loader.h"
#include <curl/curl.h>
#include <functional>
#include <json/json.h>
#include <string>
#include <vector>

struct ChatProviderResult {
    bool success = false;
    std::string assistant_content;
    Json::Value assistant_message;
    Json::Value usage;
    std::string error_message;
    long http_status = 0;
    bool stream_interrupted = false;
};

using ChatStreamEmitter = std::function<void(const std::string&)>;
using ChatLogEmitter = std::function<void(const std::string&)>;

std::string effectiveProviderType(const AppConfig& config);
std::string effectiveApiBase(const AppConfig& config);
std::string effectiveModel(const AppConfig& config);
long effectiveTimeoutSeconds(const AppConfig& config);
std::string effectiveChatUrl(const AppConfig& config);
std::string deriveModelsUrl(const AppConfig& config);

Json::Value buildProviderPayload(const AppConfig& config,
                                 const std::vector<Json::Value>& chat_history,
                                 bool stream);

bool executeProviderChat(const AppConfig& config,
                         const std::vector<Json::Value>& chat_history,
                         bool stream,
                         const ChatStreamEmitter& on_text,
                         const ChatLogEmitter& on_log,
                         ChatProviderResult& out_result);

std::vector<std::string> parseModelListResponse(const AppConfig& config,
                                                const std::string& response,
                                                std::string& error);
