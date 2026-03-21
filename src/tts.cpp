#include "tts.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string uppercase_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

bool findExecutable(const std::string& name, std::string& out_path) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            out_path = candidate.string();
            return true;
        }
    }
    return false;
}

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<const char*>(contents), total);
    return total;
}

bool decodeBase64(const std::string& input, std::vector<unsigned char>& out, std::string& error) {
    static const int kInvalid = -1;
    static int table[256];
    static bool initialized = false;
    if (!initialized) {
        std::fill(std::begin(table), std::end(table), kInvalid);
        for (int i = 'A'; i <= 'Z'; ++i) table[i] = i - 'A';
        for (int i = 'a'; i <= 'z'; ++i) table[i] = i - 'a' + 26;
        for (int i = '0'; i <= '9'; ++i) table[i] = i - '0' + 52;
        table[static_cast<unsigned char>('+')] = 62;
        table[static_cast<unsigned char>('/')] = 63;
        initialized = true;
    }

    std::string cleaned;
    cleaned.reserve(input.size());
    for (unsigned char c : input) {
        if (!std::isspace(c)) cleaned.push_back(static_cast<char>(c));
    }

    if (cleaned.empty()) {
        error = "empty audio payload";
        return false;
    }
    if (cleaned.size() % 4 != 0) {
        error = "invalid base64 length";
        return false;
    }

    out.clear();
    out.reserve((cleaned.size() / 4) * 3);
    for (size_t i = 0; i < cleaned.size(); i += 4) {
        int vals[4];
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            unsigned char c = static_cast<unsigned char>(cleaned[i + j]);
            if (c == '=') {
                vals[j] = 0;
                ++pad;
                continue;
            }
            vals[j] = table[c];
            if (vals[j] == kInvalid) {
                error = "invalid base64 character";
                return false;
            }
        }
        out.push_back(static_cast<unsigned char>((vals[0] << 2) | (vals[1] >> 4)));
        if (pad < 2) out.push_back(static_cast<unsigned char>(((vals[1] & 0xF) << 4) | (vals[2] >> 2)));
        if (pad < 1) out.push_back(static_cast<unsigned char>(((vals[2] & 0x3) << 6) | vals[3]));
    }
    return true;
}

bool writeAudioTempFile(const std::vector<unsigned char>& audio, std::string& out_path, std::string& error) {
    char path_template[] = "/tmp/aimaster_tts_XXXXXX.wav";
    int fd = ::mkstemps(path_template, 4);
    if (fd < 0) {
        error = std::string("temp file create failed: ") + std::strerror(errno);
        return false;
    }
    ssize_t written = ::write(fd, audio.data(), audio.size());
    ::close(fd);
    if (written < 0 || static_cast<size_t>(written) != audio.size()) {
        ::unlink(path_template);
        error = std::string("temp file write failed: ") + std::strerror(errno);
        return false;
    }
    out_path = path_template;
    return true;
}

bool runPlayback(const std::vector<std::string>& argv, std::string& error) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {
        ::execv(args[0], args.data());
        _exit(127);
    }

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        error = std::string("waitpid failed: ") + std::strerror(errno);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ostringstream oss;
        oss << "player exited with status " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        error = oss.str();
        return false;
    }
    return true;
}

bool playAudioBytes(const std::vector<unsigned char>& audio, std::string& backend_used, std::string& error) {
    std::string path;
    if (!writeAudioTempFile(audio, path, error)) return false;

    struct Backend {
        const char* name;
        std::vector<std::string> args;
    };

    std::vector<Backend> backends;
    std::string exe;
    if (findExecutable("ffplay", exe)) backends.push_back({"ffplay", {exe, "-nodisp", "-autoexit", "-loglevel", "error", path}});
    if (findExecutable("paplay", exe)) backends.push_back({"paplay", {exe, path}});
    if (findExecutable("aplay", exe)) backends.push_back({"aplay", {exe, path}});

    if (backends.empty()) {
        ::unlink(path.c_str());
        error = "no playback backend found (tried ffplay, paplay, aplay)";
        return false;
    }

    for (const auto& backend : backends) {
        std::string backend_error;
        if (runPlayback(backend.args, backend_error)) {
            backend_used = backend.name;
            ::unlink(path.c_str());
            return true;
        }
        error = backend.name + std::string(": ") + backend_error;
    }

    ::unlink(path.c_str());
    return false;
}

} // namespace

SpeakCommandResult parseSpeakCommand(const std::string& command) {
    SpeakCommandResult out;
    std::string trimmed = trim_copy(command);
    std::string upper = uppercase_copy(trimmed);
    if (upper.rfind("/SPEAK", 0) != 0) return out;
    out.recognized = true;

    std::istringstream iss(trimmed);
    std::string verb, arg;
    iss >> verb >> arg;
    arg = uppercase_copy(arg);
    if (arg == "ON") {
        out.enable = true;
        out.message = "[OK] Speech output enabled.";
    } else if (arg == "OFF") {
        out.enable = false;
        out.message = "[OK] Speech output disabled.";
    } else {
        out.message = "Usage: /speak on|off";
    }
    return out;
}


bool applySpeakCommand(const std::string& command, AppConfig& config, SpeakCommandResult& out) {
    out = parseSpeakCommand(command);
    if (!out.recognized) return false;
    std::string upper = uppercase_copy(trim_copy(command));
    if (upper == "/SPEAK ON") config.tts_enabled = true;
    else if (upper == "/SPEAK OFF") config.tts_enabled = false;
    return true;
}

Json::Value buildTTSRequestPayload(const std::string& text, const AppConfig& config) {
    Json::Value payload(Json::objectValue);
    payload["text"] = text;
    if (!config.tts_voice.empty()) payload["voice"] = config.tts_voice;
    if (!config.tts_speaker.empty()) payload["speaker"] = config.tts_speaker;
    return payload;
}

bool decodeBase64AudioResponse(const std::string& response_body,
                               TTSResponseAudio& out,
                               std::string& error) {
    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string errs;
    std::istringstream ss(response_body);
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        error = "invalid JSON";
        return false;
    }
    const char* fields[] = {"audio", "audio_base64", "wav_base64"};
    std::string encoded;
    for (const char* field : fields) {
        if (root.isMember(field) && root[field].isString()) {
            encoded = root[field].asString();
            break;
        }
    }
    if (encoded.empty()) {
        error = "missing audio field";
        return false;
    }
    out.content_type = root.get("content_type", "audio/wav").asString();
    return decodeBase64(encoded, out.audio_bytes, error);
}

bool maybeSpeakText(const std::string& text, const AppConfig& config) {
    if (!config.tts_enabled) return true;
    if (trim_copy(text).empty()) return true;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[Warn] TTS unavailable: curl init failed\n";
        return false;
    }

    std::string response;
    std::string payload_str = Json::writeString(Json::StreamWriterBuilder(), buildTTSRequestPayload(text, config));
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, config.tts_endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload_str.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.tts_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, config.tts_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[Warn] TTS request failed: " << curl_easy_strerror(res) << "\n";
        return false;
    }
    if (http_status < 200 || http_status >= 300) {
        std::cerr << "[Warn] TTS request failed: HTTP " << http_status << "\n";
        return false;
    }

    TTSResponseAudio audio;
    std::string error;
    if (!decodeBase64AudioResponse(response, audio, error)) {
        std::cerr << "[Warn] TTS response invalid: " << error << "\n";
        return false;
    }

    std::string backend_used;
    if (!playAudioBytes(audio.audio_bytes, backend_used, error)) {
        std::cerr << "[Warn] TTS playback failed: " << error << "\n";
        return false;
    }

    std::cerr << "[Info] TTS playback backend: " << backend_used << "\n";
    return true;
}
