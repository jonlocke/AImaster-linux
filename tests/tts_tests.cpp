#include "tts.hpp"

#include <cassert>
#include <iostream>

static void test_parse_speak_command() {
    auto on = parseSpeakCommand("/speak on");
    assert(on.recognized);
    assert(on.enable);

    auto off = parseSpeakCommand(" /SPEAK off ");
    assert(off.recognized);
    assert(!off.enable);

    auto bad = parseSpeakCommand("/speak maybe");
    assert(bad.recognized);
    assert(bad.message == "Usage: /speak on|off");

    auto other = parseSpeakCommand("hello world");
    assert(!other.recognized);
}

static void test_speak_state_behavior() {
    AppConfig cfg;
    cfg.tts_enabled = false;
    SpeakCommandResult res;

    bool handled = applySpeakCommand("/speak on", cfg, res);
    assert(handled);
    assert(cfg.tts_enabled);
    assert(res.message == "[OK] Speech output enabled.");

    handled = applySpeakCommand("/speak off", cfg, res);
    assert(handled);
    assert(!cfg.tts_enabled);
    assert(res.message == "[OK] Speech output disabled.");

    handled = applySpeakCommand("/speak maybe", cfg, res);
    assert(handled);
    assert(!cfg.tts_enabled);
}

static void test_build_payload_and_decode_success() {
    AppConfig cfg;
    cfg.tts_voice = "en-us";
    cfg.tts_speaker = "narrator";
    Json::Value payload = buildTTSRequestPayload("hello", cfg);
    assert(payload["text"].asString() == "hello");
    assert(payload["prompt"].asString() == "hello");
    assert(payload["voice"].asString() == "en-us");
    assert(payload["speaker"].asString() == "narrator");

    cfg.tts_endpoint_url = "http://127.0.0.1:8092/speak";
    assert(buildTTSRequestUrl(cfg) == "http://127.0.0.1:8092/speak?play=0&return_audio=1");

    TTSResponseAudio audio;
    std::string err;
    bool ok = decodeBase64AudioResponse(R"({"audio":"aGVsbG8=","content_type":"audio/wav"})", audio, err);
    assert(ok);
    assert(err.empty());
    std::string decoded(audio.audio_bytes.begin(), audio.audio_bytes.end());
    assert(decoded == "hello");
    assert(audio.content_type == "audio/wav");
}

static void test_decode_failure_paths() {
    TTSResponseAudio audio;
    std::string err;

    bool ok = decodeBase64AudioResponse("not-json", audio, err);
    assert(!ok);
    assert(err == "invalid JSON");

    err.clear();
    ok = decodeBase64AudioResponse(R"({"message":"missing"})", audio, err);
    assert(!ok);
    assert(err == "missing audio field");

    err.clear();
    ok = decodeBase64AudioResponse(R"({"data":{"audio_data":"aGVsbG8="}})", audio, err);
    assert(ok);
    assert(std::string(audio.audio_bytes.begin(), audio.audio_bytes.end()) == "hello");

    err.clear();
    ok = decodeBase64AudioResponse(R"({"audio":"%%%"})", audio, err);
    assert(!ok);
    assert(!err.empty());
}

void run_tts_tests() {
    test_parse_speak_command();
    test_speak_state_behavior();
    test_build_payload_and_decode_success();
    test_decode_failure_paths();
    std::cout << "tts_tests passed\n";
}
