#include "tool_prompting.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <vector>

namespace {

std::string json_stringify(const Json::Value& value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

std::string replace_all(std::string text, const std::string& needle, const std::string& replacement) {
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return text;
}

bool parse_json(const std::string& text, Json::Value& out) {
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream ss(text);
    return Json::parseFromStream(reader, ss, &out, &errors);
}

std::vector<std::string> extract_json_object_candidates(const std::string& text) {
    std::vector<std::string> candidates;

    const std::string fence = "```";
    std::size_t fence_pos = 0;
    while ((fence_pos = text.find(fence, fence_pos)) != std::string::npos) {
        std::size_t block_start = text.find('\n', fence_pos);
        if (block_start == std::string::npos) break;
        std::size_t fence_end = text.find(fence, block_start + 1);
        if (fence_end == std::string::npos) break;
        std::string block = text.substr(block_start + 1, fence_end - block_start - 1);
        if (!block.empty()) candidates.push_back(block);
        fence_pos = fence_end + fence.size();
    }

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') continue;

        bool in_string = false;
        bool escape = false;
        int depth = 0;
        for (std::size_t j = i; j < text.size(); ++j) {
            const char ch = text[j];
            if (escape) {
                escape = false;
                continue;
            }
            if (ch == '\\') {
                if (in_string) escape = true;
                continue;
            }
            if (ch == '"') {
                in_string = !in_string;
                continue;
            }
            if (in_string) continue;
            if (ch == '{') ++depth;
            else if (ch == '}') {
                --depth;
                if (depth == 0) {
                    candidates.push_back(text.substr(i, j - i + 1));
                    break;
                }
            }
        }
    }

    return candidates;
}

std::string normalize_alias(const std::string& tool_name) {
    std::string key = tool_name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    static const std::map<std::string, std::string> aliases = {
        {"weather", "get_weather"},
        {"get_current_weather", "get_weather"},
        {"weather.get_weather", "get_weather"},
        {"weather.get_current_weather", "get_weather"}
    };

    const auto it = aliases.find(key);
    return it == aliases.end() ? tool_name : it->second;
}

} // namespace

Json::Value buildToolSelectionSpecs(const Json::Value& tools) {
    Json::Value specs(Json::arrayValue);
    if (!tools.isArray()) return specs;

    for (const auto& tool : tools) {
        Json::Value spec(Json::objectValue);
        const Json::Value& function = tool["function"];
        if (!function.isObject()) continue;
        spec["name"] = function.get("name", "");
        spec["description"] = function.get("description", "");
        spec["input_schema"] = function.get("parameters", Json::Value(Json::objectValue));
        specs.append(spec);
    }
    return specs;
}

std::string buildToolSelectionPrompt(const Json::Value& tool_specs, const std::string& user_prompt) {
    static const char* kTemplate =
        "You are a tool selector, not the final assistant answerer.\n"
        "Respond with only one valid JSON object and nothing else.\n"
        "If a tool is needed, respond in this exact shape: {\"tool\":\"tool.name\",\"arguments\":{\"key\":\"value\"}}\n"
        "If no tool is needed, respond in this exact shape: {\"tool\":null,\"arguments\":{}}\n"
        "Do not answer the user directly in this step.\n"
        "Do not use markdown, code fences, prose, or explanatory text.\n"
        "Only select a tool when the user explicitly needs external data or an external action that one of the listed tools provides.\n"
        "For ordinary conversation, general questions, writing help, explanations, and chat, return {\"tool\":null,\"arguments\":{}}.\n"
        "When a tool argument is a location, path, URL, command, hostname, or other literal user-provided value, copy that value verbatim from the user request. Do not rewrite, expand, explain, or invent arguments.\n"
        "\n"
        "Available tools:\n"
        "{tool_specs}\n"
        "\n"
        "User request:\n"
        "{user_prompt}\n";

    std::string prompt = kTemplate;
    prompt = replace_all(prompt, "{tool_specs}", json_stringify(tool_specs));
    prompt = replace_all(prompt, "{user_prompt}", user_prompt);
    return prompt;
}

ToolPromptDecision parseToolSelectionResponse(const std::string& response_text) {
    ToolPromptDecision decision;
    const std::vector<std::string> candidates = extract_json_object_candidates(response_text);

    for (const auto& candidate : candidates) {
        Json::Value payload;
        if (!parse_json(candidate, payload) || !payload.isObject()) continue;

        const Json::Value raw_tool = payload["tool"];
        Json::Value arguments = payload["arguments"];
        if (raw_tool.isNull()) {
            if (!arguments.isObject()) arguments = Json::Value(Json::objectValue);
            decision.valid = true;
            decision.use_tool = false;
            decision.arguments = arguments;
            return decision;
        }

        const std::string raw_tool_name = raw_tool.asString();
        if (!arguments.isObject() && !raw_tool_name.empty()) {
            arguments = Json::Value(Json::objectValue);
            for (const auto& member : payload.getMemberNames()) {
                if (member != "tool") arguments[member] = payload[member];
            }
        }
        if (raw_tool_name.empty() || !arguments.isObject()) continue;

        decision.valid = true;
        decision.use_tool = true;
        decision.tool_name = normalize_alias(raw_tool_name);
        decision.arguments = arguments;
        return decision;
    }

    return decision;
}

std::string buildToolResultPrompt(const std::string& user_prompt,
                                  const std::string& tool_name,
                                  const Json::Value& tool_arguments,
                                  const Json::Value& tool_result) {
    static const char* kTemplate =
        "Answer the user using the tool result below.\n"
        "If the tool result reports an error or missing input, respond with exactly two sentences: first briefly explain the problem, then ask one short clarification question for only the missing information needed to retry.\n"
        "Do not answer your own question. Do not invent values. Do not include markdown, bullets, labels, examples, code fences, or multiple questions.\n"
        "Do not claim to have used any other tools.\n"
        "\n"
        "Original user request:\n"
        "{user_prompt}\n"
        "\n"
        "Tool result:\n"
        "{tool_payload}\n";

    Json::Value payload(Json::objectValue);
    payload["tool"] = tool_name;
    payload["arguments"] = tool_arguments;
    payload["result"] = tool_result;

    std::string prompt = kTemplate;
    prompt = replace_all(prompt, "{user_prompt}", user_prompt);
    prompt = replace_all(prompt, "{tool_payload}", json_stringify(payload));
    return prompt;
}
