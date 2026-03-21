#pragma once

#include "config_loader.h"

#include <json/json.h>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
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

std::vector<std::string> extractSpeakableChunks(std::string& pending_text, bool flush_all);

class StreamingTTSPlayer {
public:
    explicit StreamingTTSPlayer(const AppConfig& config);
    ~StreamingTTSPlayer();

    void pushText(const std::string& text);
    void finish();

private:
    void workerLoop();

    AppConfig config_;
    std::string pending_text_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    bool finishing_ = false;
    bool finished_ = false;
    std::thread worker_;
};
