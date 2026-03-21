#pragma once

#include "config_loader.h"

#include <json/json.h>
#include <string>
#include <vector>

struct SpeakCommandResult {
    bool recognized = false;
    bool enable = false;
    std::string message;
};

struct TTSResponseAudio {
    std::vector<unsigned char> audio_bytes;
    std::string content_type;
};

SpeakCommandResult parseSpeakCommand(const std::string& command);
bool applySpeakCommand(const std::string& command, AppConfig& config, SpeakCommandResult& out);
Json::Value buildTTSRequestPayload(const std::string& text, const AppConfig& config);
std::string buildTTSRequestUrl(const AppConfig& config);
bool decodeBase64AudioResponse(const std::string& response_body,
                               TTSResponseAudio& out,
                               std::string& error);

bool maybeSpeakText(const std::string& text, const AppConfig& config);
