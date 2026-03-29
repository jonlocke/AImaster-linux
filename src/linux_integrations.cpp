#include "linux_integrations.hpp"

#include "config_loader.h"
#include "command_exec.h"
#include "ollama_client.h"
#include "route_context.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace {

struct MicServiceState {
    std::mutex mutex;
    std::thread watcher_thread;
    bool watcher_running = false;
    bool stop_requested = false;
    AppConfig* config = nullptr;
    MicTranscriptCallback callback;

    bool recording = false;
    bool transcribing = false;
    pid_t recorder_pid = -1;
    std::string audio_path;
    std::chrono::steady_clock::time_point last_press = std::chrono::steady_clock::time_point::min();
};

MicServiceState g_mic_service;
struct BluetoothReconnectState {
    std::mutex mutex;
    std::thread worker_thread;
    bool worker_running = false;
    bool stop_requested = false;
    AppConfig* config = nullptr;
    std::string last_attempt_mac;
    std::chrono::steady_clock::time_point last_attempt_at = std::chrono::steady_clock::time_point::min();
};

BluetoothReconnectState g_bt_reconnect;
std::mutex g_bt_mutex;
std::vector<BluetoothDeviceInfo> g_last_bt_scan;

struct BluealsaPcmInfo {
    std::string id;
    std::string description;
    bool playback = false;
    bool capture = false;
};

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string shell_escape(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

std::vector<BluealsaPcmInfo> list_bluealsa_pcms() {
    std::vector<BluealsaPcmInfo> devices;
    FILE* pipe = ::popen("bluealsa-aplay -L 2>/dev/null", "r");
    if (!pipe) return devices;

    char buffer[512];
    BluealsaPcmInfo current;
    bool have_current = false;
    auto flush_current = [&]() {
        if (have_current && (!current.id.empty()) && (current.playback || current.capture)) {
            devices.push_back(current);
        }
        current = BluealsaPcmInfo{};
        have_current = false;
    };

    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        std::string line = trim_copy(buffer);
        if (line.empty()) continue;
        if (line.rfind("bluealsa:DEV=", 0) == 0) {
            flush_current();
            current.id = line;
            have_current = true;
            continue;
        }
        if (!have_current) continue;
        if (line.find(", playback") != std::string::npos) {
            current.playback = true;
            current.description = line;
        } else if (line.find(", capture") != std::string::npos) {
            current.capture = true;
            current.description = line;
        }
    }

    flush_current();
    ::pclose(pipe);
    return devices;
}

bool has_bluealsa_pcm(const std::string& device_id, bool want_capture) {
    for (const auto& pcm : list_bluealsa_pcms()) {
        if (pcm.id != device_id) continue;
        return want_capture ? pcm.capture : pcm.playback;
    }
    return false;
}

bool wait_for_bluealsa_pcm(const std::string& device_id, bool want_capture,
                           std::chrono::milliseconds timeout = std::chrono::seconds(8),
                           std::chrono::milliseconds poll_interval = std::chrono::milliseconds(200)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (has_bluealsa_pcm(device_id, want_capture)) return true;
        std::this_thread::sleep_for(poll_interval);
    }
    return has_bluealsa_pcm(device_id, want_capture);
}

void diag_log_bluealsa_capture_state(const std::string& selected_device) {
    if (!IsDiagnosticModeEnabled()) return;
    std::fprintf(stderr, "[DIAG] mic selected device: %s\n", selected_device.c_str());
    const auto pcms = list_bluealsa_pcms();
    if (pcms.empty()) {
        std::fprintf(stderr, "[DIAG] mic live bluealsa capture devices: <none>\n");
        std::fflush(stderr);
        return;
    }
    bool any_capture = false;
    for (const auto& pcm : pcms) {
        if (!pcm.capture) continue;
        any_capture = true;
        std::fprintf(stderr, "[DIAG] mic live bluealsa capture: %s | %s\n",
                     pcm.id.c_str(), pcm.description.c_str());
    }
    if (!any_capture) std::fprintf(stderr, "[DIAG] mic live bluealsa capture devices: <none>\n");
    std::fflush(stderr);
}

static size_t curl_write_string(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<const char*>(contents), total);
    return total;
}

bool is_mac_address(const std::string& value) {
    if (value.size() != 17) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

std::string mac_from_bluealsa_device(const std::string& device) {
    const std::string prefix = "bluealsa:DEV=";
    if (device.rfind(prefix, 0) != 0) return {};
    const auto start = prefix.size();
    const auto end = device.find(',', start);
    const std::string mac = device.substr(start, end == std::string::npos ? std::string::npos : end - start);
    return is_mac_address(mac) ? mac : std::string();
}

std::string run_command_capture(const std::string& command, int* exit_code = nullptr) {
    std::array<char, 512> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        if (exit_code) *exit_code = -1;
        return {};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    int status = pclose(pipe);
    if (exit_code) {
        if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
        else *exit_code = -1;
    }
    return output;
}

std::string resolve_transcript_text(const std::string& response) {
    std::string trimmed = trim_copy(response);
    if (trimmed.empty()) return {};

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    std::istringstream iss(trimmed);
    if (Json::parseFromStream(builder, iss, &root, &errs) && root.isObject()) {
        if (root.isMember("error")) return {};
        if (root.isMember("text") && root["text"].isString()) return trim_copy(root["text"].asString());
        if (root.isMember("transcript") && root["transcript"].isString()) return trim_copy(root["transcript"].asString());
    }
    return trimmed;
}

std::string transcribe_via_command(const AppConfig& config, const std::string& audio_path, std::string& error) {
    std::string command = config.stt_command;
    const std::string escaped_path = shell_escape(audio_path);
    const std::string placeholder = "{audio}";
    const auto pos = command.find(placeholder);
    if (pos == std::string::npos) command += " " + escaped_path;
    else command.replace(pos, placeholder.size(), escaped_path);

    int exit_code = 0;
    std::string output = run_command_capture(command + " 2>&1", &exit_code);
    if (exit_code != 0) {
        error = output.empty() ? ("stt_command failed with exit code " + std::to_string(exit_code)) : trim_copy(output);
        return {};
    }

    std::string transcript = resolve_transcript_text(output);
    if (transcript.empty()) error = "stt_command returned an empty transcript";
    return transcript;
}

std::string transcribe_via_http(const AppConfig& config, const std::string& audio_path, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl init failed";
        return {};
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, config.stt_endpoint_url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.stt_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* headers = nullptr;
    const std::string api_key = config.stt_api_key.empty() ? config.api_key : config.stt_api_key;
    if (!api_key.empty()) headers = curl_slist_append(headers, (std::string("Authorization: Bearer ") + api_key).c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, audio_path.c_str());

    const bool quick_stt_endpoint = config.stt_endpoint_url.find("/inference") != std::string::npos;

    if (!quick_stt_endpoint && !config.stt_model.empty()) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "model");
        curl_mime_data(part, config.stt_model.c_str(), CURL_ZERO_TERMINATED);
    }

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, quick_stt_endpoint ? "json" : "text", CURL_ZERO_TERMINATED);

    if (quick_stt_endpoint) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "temperature");
        curl_mime_data(part, "0.0", CURL_ZERO_TERMINATED);

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "temperature_inc");
        curl_mime_data(part, "0.2", CURL_ZERO_TERMINATED);
    }

    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_mime_free(mime);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
        return {};
    }
    if (http_code < 200 || http_code >= 300) {
        error = "STT HTTP " + std::to_string(http_code) + ": " + trim_copy(response);
        return {};
    }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    std::istringstream iss(response);
    if (Json::parseFromStream(builder, iss, &root, &errs) && root.isObject() && root.isMember("error")) {
        if (root["error"].isString()) error = trim_copy(root["error"].asString());
        else error = trim_copy(response);
        return {};
    }

    std::string transcript = resolve_transcript_text(response);
    if (transcript.empty()) error = "STT response was empty";
    return transcript;
}

std::string transcribe_audio(const AppConfig& config, const std::string& audio_path, std::string& error) {
    if (!config.stt_command.empty()) return transcribe_via_command(config, audio_path, error);
    if (!config.stt_endpoint_url.empty()) return transcribe_via_http(config, audio_path, error);
    error = "No STT backend configured. Set stt_command or stt_endpoint_url/stt_model.";
    return {};
}

bool start_recording_locked(AppConfig& config, std::string& status) {
    const bool bluetooth_mic = !config.mic_record_device.empty() &&
                               config.mic_record_device.rfind("bluealsa:", 0) == 0;
    if (g_mic_service.recording) {
        status = "[Info] Microphone is already recording.";
        return true;
    }
    if (g_mic_service.transcribing) {
        status = "[Info] Microphone is still transcribing the previous clip.";
        return false;
    }
    if (bluetooth_mic && !wait_for_bluealsa_pcm(config.mic_record_device, true)) {
        diag_log_bluealsa_capture_state(config.mic_record_device);
        status = "[Mic] Bluetooth headset transport is not available. Reconnect or power-cycle the headset/dongle, then make sure headset/SCO mode is active.";
        return false;
    }

    char tmpl[] = "/tmp/aimaster-mic-XXXXXX.wav";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) {
        status = "[Error] Could not create temporary WAV file.";
        return false;
    }
    close(fd);

    std::vector<std::string> args = {
        "arecord", "-q", "-f", "S16_LE",
        "-r", std::to_string(config.mic_sample_rate),
        "-c", std::to_string(config.mic_channels),
        "-t", "wav"
    };
    if (!config.mic_record_device.empty()) {
        args.push_back("-D");
        args.push_back(config.mic_record_device);
    }
    args.push_back(tmpl);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        std::filesystem::remove(tmpl);
        status = "[Error] Failed to fork arecord.";
        return false;
    }

    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    int child_status = 0;
    const pid_t ready = waitpid(pid, &child_status, WNOHANG);
    if (ready == pid) {
        std::filesystem::remove(tmpl);
        if (bluetooth_mic) {
            status = "[Mic] Bluetooth headset transport dropped while opening the microphone. Reconnect or power-cycle the headset/dongle and try again.";
        } else {
            status = "[Error] Unable to start arecord. Confirm ALSA and the capture device.";
        }
        return false;
    }

    g_mic_service.recording = true;
    g_mic_service.recorder_pid = pid;
    g_mic_service.audio_path = tmpl;
    status = "[Mic] Recording started.";
    return true;
}

bool stop_recording_locked(AppConfig& config, std::string& status) {
    if (!g_mic_service.recording) {
        status = "[Info] Microphone is not recording.";
        return true;
    }

    const pid_t pid = g_mic_service.recorder_pid;
    const std::string audio_path = g_mic_service.audio_path;
    g_mic_service.recording = false;
    g_mic_service.recorder_pid = -1;
    g_mic_service.audio_path.clear();
    g_mic_service.transcribing = true;

    kill(pid, SIGINT);
    for (int i = 0; i < 20; ++i) {
        int child_status = 0;
        const pid_t done = waitpid(pid, &child_status, WNOHANG);
        if (done == pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    int child_status = 0;
    if (waitpid(pid, &child_status, WNOHANG) == 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &child_status, 0);
    }

    status = "[Mic] Recording stopped. Transcribing...";
    auto callback = g_mic_service.callback;
    AppConfig* cfg_ptr = g_mic_service.config;
    const CommandSource output_source = getCurrentCommandSource();

    std::thread([audio_path, callback, cfg_ptr, output_source]() {
        setCurrentCommandSource(output_source);
        std::string transcript;
        std::string error;
        if (cfg_ptr) transcript = transcribe_audio(*cfg_ptr, audio_path, error);
        std::filesystem::remove(audio_path);

        if (!transcript.empty() && callback) callback(transcript);
        else route_output(std::string("[Mic] ") + (error.empty() ? "Transcription failed." : error), true);

        std::lock_guard<std::mutex> lock(g_mic_service.mutex);
        g_mic_service.transcribing = false;
    }).detach();

    return true;
}

void mic_watcher_loop() {
    std::string active_path;
    int active_code = -1;
    int fd = -1;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_mic_service.mutex);
            if (g_mic_service.stop_requested) break;
            if (!g_mic_service.config || g_mic_service.config->hid_input_device.empty() || g_mic_service.config->hid_button_code < 0) {
                active_path.clear();
                active_code = -1;
            } else {
                active_path = g_mic_service.config->hid_input_device;
                active_code = g_mic_service.config->hid_button_code;
            }
        }

        if (active_path.empty() || active_code < 0) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        if (fd < 0) {
            fd = open(active_path.c_str(), O_RDONLY);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int poll_result = poll(&pfd, 1, 500);
        if (poll_result <= 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(fd);
            fd = -1;
            continue;
        }

        input_event ev{};
        const ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;
        if (ev.type != EV_KEY || ev.code != active_code || ev.value != 1) continue;

        std::string status;
        bool should_toggle = false;
        {
            std::lock_guard<std::mutex> lock(g_mic_service.mutex);
            const auto now = std::chrono::steady_clock::now();
            const auto debounce = std::chrono::milliseconds(
                g_mic_service.config ? g_mic_service.config->hid_debounce_ms : 500);
            if (now - g_mic_service.last_press >= debounce) {
                g_mic_service.last_press = now;
                should_toggle = true;
            }
        }
        if (!should_toggle) continue;

        toggleMicRecording(*g_mic_service.config, status);
        route_output(status, true);
    }

    if (fd >= 0) close(fd);
}

void bluetooth_reconnect_loop() {
    using namespace std::chrono_literals;
    for (;;) {
        AppConfig* cfg = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_bt_reconnect.mutex);
            if (g_bt_reconnect.stop_requested) break;
            cfg = g_bt_reconnect.config;
        }

        std::string target_mac;
        if (cfg) target_mac = mac_from_bluealsa_device(cfg->tts_output_device);
        if (target_mac.empty()) {
            std::this_thread::sleep_for(2s);
            continue;
        }

        std::string connected_error;
        const auto connected = listConnectedBluetoothDevices(connected_error);
        bool already_connected = false;
        for (const auto& device : connected) {
            if (device.mac == target_mac) {
                already_connected = true;
                break;
            }
        }
        if (already_connected) {
            std::this_thread::sleep_for(3s);
            continue;
        }

        std::string known_error;
        const auto known = listKnownBluetoothDevices(known_error);
        bool known_device = false;
        for (const auto& device : known) {
            if (device.mac == target_mac) {
                known_device = true;
                break;
            }
        }
        if (!known_device) {
            std::this_thread::sleep_for(3s);
            continue;
        }

        bool should_attempt = false;
        {
            std::lock_guard<std::mutex> lock(g_bt_reconnect.mutex);
            const auto now = std::chrono::steady_clock::now();
            const auto cooldown = 8s;
            if (g_bt_reconnect.last_attempt_mac != target_mac || now - g_bt_reconnect.last_attempt_at >= cooldown) {
                g_bt_reconnect.last_attempt_mac = target_mac;
                g_bt_reconnect.last_attempt_at = now;
                should_attempt = true;
            }
        }
        if (!should_attempt) {
            std::this_thread::sleep_for(1s);
            continue;
        }

        int exit_code = 0;
        const std::string command = "printf 'connect " + target_mac + "\\nquit\\n' | bluetoothctl";
        const std::string output = run_command_capture("sh -lc " + shell_escape(command), &exit_code);
        if (output.find("Connection successful") != std::string::npos ||
            output.find("Connected: yes") != std::string::npos) {
            route_output("[Bluetooth] Reconnected " + target_mac, true);
        }

        std::this_thread::sleep_for(2s);
    }
}

} // namespace

std::vector<InputDeviceInfo> listInputDevices(std::string& error) {
    std::vector<InputDeviceInfo> devices;
    error.clear();
    namespace fs = std::filesystem;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/dev/input", ec)) {
        if (ec) break;
        const auto filename = entry.path().filename().string();
        if (filename.rfind("event", 0) != 0) continue;

        InputDeviceInfo info;
        info.path = entry.path().string();
        int fd = open(info.path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            char name[256] = {};
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) info.name = name;
            close(fd);
        }
        if (info.name.empty()) info.name = filename;
        devices.push_back(info);
    }

    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
        return a.path < b.path;
    });

    if (devices.empty()) error = "No /dev/input/event* devices found or accessible.";
    return devices;
}

bool captureHidButton(const AppConfig& config, HidCaptureResult& result, std::string& error) {
    result = HidCaptureResult{};
    error.clear();

    auto devices = listInputDevices(error);
    if (devices.empty()) return false;

    struct PollDevice {
        int fd = -1;
        InputDeviceInfo info;
    };
    std::vector<PollDevice> opened;
    std::vector<pollfd> pfds;
    opened.reserve(devices.size());
    pfds.reserve(devices.size());

    for (const auto& device : devices) {
        int fd = open(device.path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        opened.push_back({fd, device});
        pfds.push_back({fd, POLLIN, 0});
    }
    if (opened.empty()) {
        error = "Input devices exist, but none could be opened. Add the user to the input group or run with permission.";
        return false;
    }

    const int timeout_ms = 15000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        const int poll_result = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 250);
        if (poll_result <= 0) continue;

        for (size_t i = 0; i < pfds.size(); ++i) {
            if (!(pfds[i].revents & POLLIN)) continue;
            input_event ev{};
            while (read(pfds[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.value == 1) {
                    result.path = opened[i].info.path;
                    result.name = opened[i].info.name;
                    result.code = ev.code;
                    for (const auto& dev : opened) close(dev.fd);
                    return true;
                }
            }
        }
    }

    for (const auto& dev : opened) close(dev.fd);
    error = "Timed out waiting for a HID button press.";
    return false;
}

bool configureMicHotkeyService(AppConfig& config, MicTranscriptCallback callback, std::string& status) {
    std::lock_guard<std::mutex> lock(g_mic_service.mutex);
    g_mic_service.config = &config;
    if (callback) g_mic_service.callback = std::move(callback);

    if (!g_mic_service.watcher_running) {
        g_mic_service.stop_requested = false;
        g_mic_service.watcher_running = true;
        g_mic_service.watcher_thread = std::thread(mic_watcher_loop);
    }

    if (config.hid_input_device.empty() || config.hid_button_code < 0) {
        status = "[Mic] HID hotkey watcher is idle. Use /hid capture to bind a button.";
    } else {
        status = "[Mic] Watching " + config.hid_input_device + " for button code " + std::to_string(config.hid_button_code) + ".";
    }
    return true;
}

void stopMicHotkeyService() {
    std::unique_lock<std::mutex> lock(g_mic_service.mutex);
    if (!g_mic_service.watcher_running) return;
    g_mic_service.stop_requested = true;
    lock.unlock();

    if (g_mic_service.watcher_thread.joinable()) g_mic_service.watcher_thread.join();

    lock.lock();
    g_mic_service.watcher_running = false;
}

bool toggleMicRecording(AppConfig& config, std::string& status) {
    std::lock_guard<std::mutex> lock(g_mic_service.mutex);
    g_mic_service.config = &config;
    return g_mic_service.recording ? stop_recording_locked(config, status)
                                   : start_recording_locked(config, status);
}

bool startMicRecording(AppConfig& config, std::string& status) {
    std::lock_guard<std::mutex> lock(g_mic_service.mutex);
    g_mic_service.config = &config;
    return start_recording_locked(config, status);
}

bool stopMicRecording(AppConfig& config, std::string& status) {
    std::lock_guard<std::mutex> lock(g_mic_service.mutex);
    g_mic_service.config = &config;
    return stop_recording_locked(config, status);
}

std::string micServiceStatus(const AppConfig& config) {
    std::lock_guard<std::mutex> lock(g_mic_service.mutex);
    std::ostringstream oss;
    oss << "HID device: " << (config.hid_input_device.empty() ? "<unset>" : config.hid_input_device);
    if (!config.hid_input_name.empty()) oss << " (" << config.hid_input_name << ")";
    oss << ", button code: ";
    if (config.hid_button_code >= 0) oss << config.hid_button_code;
    else oss << "<unset>";
    oss << ", state: ";
    if (g_mic_service.recording) oss << "recording";
    else if (g_mic_service.transcribing) oss << "transcribing";
    else oss << "idle";
    return oss.str();
}

std::vector<BluetoothDeviceInfo> scanBluetoothDevices(const AppConfig& config, std::string& error) {
    error.clear();
    const int seconds = std::max(1, config.bluetooth_scan_seconds);
    const std::string command =
        "sh -lc " +
        shell_escape("bluetoothctl --timeout " + std::to_string(seconds) + " scan on >/dev/null 2>&1; bluetoothctl devices");

    int exit_code = 0;
    const std::string output = run_command_capture(command, &exit_code);
    if (exit_code != 0 && output.empty()) {
        error = "Failed to run bluetoothctl. Confirm BlueZ is installed and bluetoothd is running.";
        return {};
    }

    std::vector<BluetoothDeviceInfo> devices;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("Device ", 0) != 0) continue;
        std::istringstream ls(line);
        std::string tag;
        BluetoothDeviceInfo info;
        ls >> tag >> info.mac;
        std::getline(ls, info.name);
        info.name = trim_copy(info.name);
        if (!is_mac_address(info.mac)) continue;
        if (info.name.empty()) continue;
        if (info.name == info.mac) continue;
        devices.push_back(info);
    }

    {
        std::lock_guard<std::mutex> lock(g_bt_mutex);
        g_last_bt_scan = devices;
    }

    if (devices.empty()) error = "No Bluetooth devices discovered.";
    return devices;
}

const std::vector<BluetoothDeviceInfo>& lastScannedBluetoothDevices() {
    return g_last_bt_scan;
}

bool pairBluetoothDevice(const AppConfig& config, const std::string& target, std::string& status) {
    std::string mac = trim_copy(target);
    if (!is_mac_address(mac)) {
        int idx = -1;
        try { idx = std::stoi(mac); } catch (...) { idx = -1; }
        if (idx >= 1) {
            std::lock_guard<std::mutex> lock(g_bt_mutex);
            if (static_cast<size_t>(idx) <= g_last_bt_scan.size()) mac = g_last_bt_scan[idx - 1].mac;
        }
    }

    if (!is_mac_address(mac)) {
        status = "[Error] Provide a Bluetooth device number from /pair or a MAC address.";
        return false;
    }

    const std::string script =
        "printf 'pair " + mac + "\\ntrust " + mac + "\\nconnect " + mac + "\\nquit\\n' | bluetoothctl";
    int exit_code = 0;
    const std::string output = run_command_capture("sh -lc " + shell_escape(script), &exit_code);
    status = trim_copy(output);
    if (status.empty()) status = exit_code == 0 ? "[OK] Pair/connect command sent." : "[Error] bluetoothctl returned no output.";

    const bool ok =
        output.find("Pairing successful") != std::string::npos ||
        output.find("Connection successful") != std::string::npos ||
        output.find("trust succeeded") != std::string::npos ||
        output.find("Connected: yes") != std::string::npos;

    if (!ok && exit_code != 0) {
        status = "[Error] " + status;
        return false;
    }
    return ok || exit_code == 0;
}

std::vector<BluetoothDeviceInfo> listKnownBluetoothDevices(std::string& error) {
    error.clear();
    int exit_code = 0;
    const std::string output = run_command_capture("bluetoothctl devices 2>/dev/null", &exit_code);
    if (exit_code != 0 && output.empty()) {
        error = "Failed to query Bluetooth devices.";
        return {};
    }

    std::vector<BluetoothDeviceInfo> devices;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("Device ", 0) != 0) continue;
        std::istringstream ls(line);
        std::string tag;
        BluetoothDeviceInfo info;
        ls >> tag >> info.mac;
        std::getline(ls, info.name);
        info.name = trim_copy(info.name);
        if (!is_mac_address(info.mac)) continue;
        if (info.name.empty() || info.name == info.mac) continue;
        devices.push_back(info);
    }
    return devices;
}

std::vector<BluetoothDeviceInfo> listConnectedBluetoothDevices(std::string& error) {
    error.clear();
    int exit_code = 0;
    const std::string output = run_command_capture("bluetoothctl devices Connected 2>/dev/null", &exit_code);
    if (exit_code != 0 && output.empty()) {
        error = "Failed to query connected Bluetooth devices.";
        return {};
    }

    std::vector<BluetoothDeviceInfo> devices;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_copy(line);
        if (line.rfind("Device ", 0) != 0) continue;
        std::istringstream ls(line);
        std::string tag;
        BluetoothDeviceInfo info;
        ls >> tag >> info.mac;
        std::getline(ls, info.name);
        info.name = trim_copy(info.name);
        if (!is_mac_address(info.mac)) continue;
        if (info.name.empty() || info.name == info.mac) continue;
        devices.push_back(info);
    }
    return devices;
}

std::vector<CaptureDeviceInfo> listCaptureDevices(std::string& error) {
    std::vector<CaptureDeviceInfo> devices;
    devices.push_back({"default", "ALSA default capture device", true});
    std::set<std::string> seen = {"default"};

    FILE* pipe = ::popen("arecord -l 2>/dev/null", "r");
    if (!pipe) {
        error = "unable to execute 'arecord -l'";
    } else {
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), pipe)) {
            std::string line = trim_copy(buffer);
            if (line.rfind("card ", 0) != 0) continue;

            int card = -1;
            int device = -1;
            if (std::sscanf(line.c_str(), "card %d: %*[^,], device %d:", &card, &device) != 2) continue;

            std::string id = "plughw:" + std::to_string(card) + "," + std::to_string(device);
            if (!seen.insert(id).second) continue;
            devices.push_back({id, line, false});
        }
        ::pclose(pipe);
    }

    for (const auto& pcm : list_bluealsa_pcms()) {
        if (!pcm.capture) continue;
        if (!seen.insert(pcm.id).second) continue;
        devices.push_back({pcm.id, pcm.description, false});
    }

    return devices;
}

bool configureBluetoothReconnectService(AppConfig& config, std::string& status) {
    std::lock_guard<std::mutex> lock(g_bt_reconnect.mutex);
    g_bt_reconnect.config = &config;
    if (!g_bt_reconnect.worker_running) {
        g_bt_reconnect.stop_requested = false;
        g_bt_reconnect.worker_running = true;
        g_bt_reconnect.worker_thread = std::thread(bluetooth_reconnect_loop);
    }
    const std::string mac = mac_from_bluealsa_device(config.tts_output_device);
    if (mac.empty()) {
        status = "[Bluetooth] Auto-reconnect idle. Select a Bluetooth /sound device to enable it.";
    } else {
        status = "[Bluetooth] Auto-reconnect watching " + mac + ".";
    }
    return true;
}

void stopBluetoothReconnectService() {
    std::unique_lock<std::mutex> lock(g_bt_reconnect.mutex);
    if (!g_bt_reconnect.worker_running) return;
    g_bt_reconnect.stop_requested = true;
    lock.unlock();
    if (g_bt_reconnect.worker_thread.joinable()) g_bt_reconnect.worker_thread.join();
    lock.lock();
    g_bt_reconnect.worker_running = false;
}
