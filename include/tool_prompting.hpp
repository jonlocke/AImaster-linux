#pragma once

#include <json/json.h>

#include <string>

struct ToolPromptDecision {
    bool valid = false;
    bool use_tool = false;
    std::string tool_name;
    Json::Value arguments = Json::Value(Json::objectValue);
};

Json::Value buildToolSelectionSpecs(const Json::Value& tools);
std::string buildToolSelectionPrompt(const Json::Value& tool_specs, const std::string& user_prompt);
ToolPromptDecision parseToolSelectionResponse(const std::string& response_text);
std::string buildToolResultPrompt(const std::string& user_prompt,
                                  const std::string& tool_name,
                                  const Json::Value& tool_arguments,
                                  const Json::Value& tool_result);
