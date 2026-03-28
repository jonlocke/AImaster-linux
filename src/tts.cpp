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
#include <set>
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


std::string appendQueryParam(const std::string& url, const std::string& key, const std::string& value) {
    return url + (url.find('?') == std::string::npos ? "?" : "&") + key + "=" + value;
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

std::vector<std::string> playbackBackendOrder() {
    std::vector<std::string> order;
    const char* env = std::getenv("AIMASTER_TTS_BACKENDS");
    if (env && *env) {
        std::stringstream ss(env);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim_copy(item);
            std::transform(item.begin(), item.end(), item.begin(), [](unsigned char c){ return std::tolower(c); });
            if ((item == "paplay" || item == "aplay" || item == "ffplay") &&
                std::find(order.begin(), order.end(), item) == order.end()) {
                order.push_back(item);
            }
        }
    }
    if (order.empty()) order = {"paplay", "aplay", "ffplay"};
    return order;
}

bool deviceRequiresExplicitAlsaRouting(const AppConfig& config) {
    if (config.tts_output_device.empty()) return false;
    const std::string device = trim_copy(config.tts_output_device);
    if (device.empty()) return false;
    if (device == "default") return false;
    if (device == "plughw:0,0") return false;
    return true;
}

std::string formatCommandForLog(const std::vector<std::string>& argv) {
    std::ostringstream oss;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) oss << ' ';
        const std::string& arg = argv[i];
        const bool needs_quotes = arg.find_first_of(" \t\"'") != std::string::npos;
        if (!needs_quotes) {
            oss << arg;
            continue;
        }
        oss << '\'';
        for (char ch : arg) {
            if (ch == '\'') oss << "'\\''";
            else oss << ch;
        }
        oss << '\'';
    }
    return oss.str();
}

bool playAudioBytes(const std::vector<unsigned char>& audio, const AppConfig& config, std::string& backend_used, std::string& error) {
    std::string path;
    if (!writeAudioTempFile(audio, path, error)) return false;

    struct Backend {
        const char* name;
        std::vector<std::string> args;
    };

    std::vector<Backend> backends;
    std::string exe;
    auto order = playbackBackendOrder();
    if (deviceRequiresExplicitAlsaRouting(config)) {
        std::stable_sort(order.begin(), order.end(), [](const std::string& a, const std::string& b) {
            auto rank = [](const std::string& name) {
                if (name == "aplay") return 0;
                if (name == "paplay") return 1;
                return 2;
            };
            return rank(a) < rank(b);
        });
    }
    for (const auto& name : order) {
        if (!findExecutable(name, exe)) continue;
        if (name == "paplay") {
            if (deviceRequiresExplicitAlsaRouting(config)) continue;
            backends.push_back({"paplay", {exe, path}});
        }
        else if (name == "aplay") {
            std::vector<std::string> args{exe, "-q"};
            const std::string playback_device = trim_copy(config.tts_output_device);
            if (!playback_device.empty()) {
                args.push_back("-D");
                args.push_back(playback_device);
            }
            args.push_back(path);
            backends.push_back({"aplay", std::move(args)});
        }
        else if (name == "ffplay") {
            if (deviceRequiresExplicitAlsaRouting(config)) continue;
            backends.push_back({"ffplay", {exe, "-nodisp", "-autoexit", "-loglevel", "error", path}});
        }
    }

    if (backends.empty()) {
        ::unlink(path.c_str());
        error = deviceRequiresExplicitAlsaRouting(config)
            ? "no playback backend found that supports the selected ALSA/Bluetooth output device"
            : "no playback backend found (tried paplay, aplay, ffplay)";
        return false;
    }

    for (const auto& backend : backends) {
        std::string backend_error;
        const std::string command_for_log = formatCommandForLog(backend.args);
        std::cerr << "[Info] TTS playback command: " << command_for_log << "\n";
        if (runPlayback(backend.args, backend_error)) {
            backend_used = backend.name;
            ::unlink(path.c_str());
            return true;
        }
        error = backend.name + std::string(": ") + backend_error + " | cmd: " + command_for_log;
    }

    ::unlink(path.c_str());
    return false;
}

} // namespace

std::vector<PlaybackDevice> listPlaybackDevices(std::string& error) {
    std::vector<PlaybackDevice> devices;
    devices.push_back({"plughw:0,0", "Linux default card0,0", true});

    FILE* pipe = ::popen("aplay -l 2>/dev/null", "r");
    if (!pipe) {
        error = "unable to execute 'aplay -l'";
        return devices;
    }

    char buffer[512];
    std::set<std::string> seen = {"plughw:0,0"};
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        std::string line = trim_copy(buffer);
        if (line.rfind("card ", 0) != 0) continue;

        int card = -1;
        int device = -1;
        if (std::sscanf(line.c_str(), "card %d: %*[^,], device %d:", &card, &device) != 2) continue;

        std::string id = "plughw:" + std::to_string(card) + "," + std::to_string(device);
        if (!seen.insert(id).second) continue;
        devices.push_back({id, line, card == 0 && device == 0});
    }

    ::pclose(pipe);
    return devices;
}

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
    payload["prompt"] = text;
    if (!config.tts_voice.empty()) {
        payload["voice"] = config.tts_voice;
    }
    if (!config.tts_speaker.empty()) {
        payload["speaker"] = config.tts_speaker;
    }
    return payload;
}


std::string buildTTSRequestUrl(const AppConfig& config) {
    std::string url = config.tts_endpoint_url;
    url = appendQueryParam(url, "play", "0");
    url = appendQueryParam(url, "stream_audio_chunks", "1");
    return url;
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

    auto extractAudio = [](const Json::Value& node) -> std::string {
        const char* fields[] = {"audio", "audio_base64", "wav_base64", "audio_data", "data"};
        for (const char* field : fields) {
            if (!node.isObject() || !node.isMember(field)) continue;
            const Json::Value& value = node[field];
            if (std::string(field) != "data" && value.isString()) return value.asString();
            if (value.isObject()) {
                const char* nested_fields[] = {"audio", "audio_base64", "wav_base64", "audio_data"};
                for (const char* nested : nested_fields) {
                    if (value.isMember(nested) && value[nested].isString()) return value[nested].asString();
                }
            }
        }
        return std::string();
    };

    std::string encoded = extractAudio(root);
    if (encoded.empty()) {
        error = "missing audio field";
        return false;
    }
    out.content_type = root.get("content_type", root.get("mime_type", "audio/wav")).asString();
    return decodeBase64(encoded, out.audio_bytes, error);
}



std::vector<std::string> extractSpeakableChunks(std::string& pending_text, bool flush_all) {
    std::vector<std::string> out;
    size_t sentence_start = 0;
    for (size_t i = 0; i < pending_text.size(); ++i) {
        const char c = pending_text[i];
        const bool boundary = (c == '.' || c == '!' || c == '?' || c == '\n');
        if (!boundary) continue;

        std::string chunk = trim_copy(pending_text.substr(sentence_start, i - sentence_start + 1));
        if (!chunk.empty()) out.push_back(std::move(chunk));
        sentence_start = i + 1;
    }

    if (sentence_start > 0) {
        pending_text.erase(0, sentence_start);
    }

    if (flush_all) {
        std::string tail = trim_copy(pending_text);
        if (!tail.empty()) out.push_back(std::move(tail));
        pending_text.clear();
    }
    return out;
}


static bool parseStreamedAudioChunks(const std::string& response_body,
                                     std::vector<std::vector<unsigned char>>& audio_chunks,
                                     std::string& error) {
    std::istringstream lines(response_body);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim_copy(line);
        if (line.empty()) continue;

        Json::CharReaderBuilder reader;
        Json::Value root;
        std::string errs;
        std::istringstream ss(line);
        if (!Json::parseFromStream(reader, ss, &root, &errs)) {
            error = "invalid streamed JSON";
            return false;
        }

        const std::string type = root.get("type", "").asString();
        if (type == "audio_chunk") {
            std::string encoded = root.get("audio_b64_wav", "").asString();
            if (encoded.empty()) {
                error = "missing audio_b64_wav field";
                return false;
            }
            std::vector<unsigned char> chunk;
            if (!decodeBase64(encoded, chunk, error)) return false;
            audio_chunks.push_back(std::move(chunk));
        } else if (type == "error") {
            error = root.get("detail", "stream error").asString();
            return false;
        }
    }
    if (audio_chunks.empty()) {
        error = "missing audio chunks";
        return false;
    }
    return true;
}

static bool speakTextNow(const std::string& text, const AppConfig& config) {
    if (!config.tts_enabled) return true;
    if (trim_copy(text).empty()) return true;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[Warn] TTS unavailable: curl init failed\n";
        return false;
    }

    std::string response;
    std::string payload_str = Json::writeString(Json::StreamWriterBuilder(), buildTTSRequestPayload(text, config));
    std::string request_url = buildTTSRequestUrl(config);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
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
    char* content_type_cstr = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_cstr);
    std::string content_type = content_type_cstr ? content_type_cstr : "";
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

    std::string error;
    const bool looks_like_wav = response.size() >= 12 && response.compare(0, 4, "RIFF") == 0 && response.compare(8, 4, "WAVE") == 0;
    std::vector<std::vector<unsigned char>> audio_chunks;
    if (content_type == "application/x-ndjson" || response.find("\"audio_b64_wav\"") != std::string::npos) {
        if (!parseStreamedAudioChunks(response, audio_chunks, error)) {
            std::cerr << "[Warn] TTS response invalid: " << error << "\n";
            return false;
        }
    } else if (content_type.rfind("audio/", 0) == 0 || content_type == "application/octet-stream" || looks_like_wav) {
        audio_chunks.push_back(std::vector<unsigned char>(response.begin(), response.end()));
    } else {
        TTSResponseAudio audio;
        if (!decodeBase64AudioResponse(response, audio, error)) {
            std::cerr << "[Warn] TTS response invalid: " << error << "\n";
            return false;
        }
        audio_chunks.push_back(std::move(audio.audio_bytes));
    }

    std::string backend_used;
    for (const auto& chunk : audio_chunks) {
        if (!playAudioBytes(chunk, config, backend_used, error)) {
            std::cerr << "[Warn] TTS playback failed: " << error << "\n";
            return false;
        }
    }

    return true;
}


bool maybeSpeakText(const std::string& text, const AppConfig& config) {
    return speakTextNow(text, config);
}

StreamingTTSPlayer::StreamingTTSPlayer(const AppConfig& config) : config_(config) {
    worker_ = std::thread(&StreamingTTSPlayer::workerLoop, this);
}

StreamingTTSPlayer::~StreamingTTSPlayer() {
    finish();
}

void StreamingTTSPlayer::pushText(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    pending_text_ += text;
    auto chunks = extractSpeakableChunks(pending_text_, false);
    for (auto& chunk : chunks) queue_.push(std::move(chunk));
    cv_.notify_one();
}

void StreamingTTSPlayer::finish() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (finished_) return;
    auto chunks = extractSpeakableChunks(pending_text_, true);
    for (auto& chunk : chunks) queue_.push(std::move(chunk));
    finishing_ = true;
    lock.unlock();
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
    finished_ = true;
}

void StreamingTTSPlayer::workerLoop() {
    for (;;) {
        std::string chunk;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&]{ return finishing_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (finishing_) break;
                continue;
            }
            chunk = std::move(queue_.front());
            queue_.pop();
        }
        (void)speakTextNow(chunk, config_);
    }
}
