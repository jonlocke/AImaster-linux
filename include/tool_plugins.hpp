#pragma once

#include "config_loader.h"

#include <json/json.h>
#include <string>
#include <vector>

struct PluginInvocation {
    std::string id;
    std::string name;
    Json::Value arguments;
};

struct PluginResult {
    bool success = false;
    std::string content;
    std::string error_message;
};

Json::Value buildRegisteredToolDefinitions(const AppConfig& config);
std::vector<PluginInvocation> extractPluginInvocations(const Json::Value& assistant_message);
PluginResult executePluginInvocation(const PluginInvocation& invocation, const AppConfig& config);
Json::Value buildPluginResultMessage(const PluginInvocation& invocation, const PluginResult& result);
