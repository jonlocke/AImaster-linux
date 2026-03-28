#include "hid_button.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

bool IsDiagnosticModeEnabled();

namespace {
constexpr unsigned kVid = 0x1d34;
constexpr unsigned kPid = 0x000d;

int g_fd = -1;
std::string g_path;
int g_last_state = BUTTON_NO_CHANGE;
bool g_pressed_edge = false;

void diag(const char* fmt, ...) {
    if (!IsDiagnosticModeEnabled()) return;
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[DIAG] hid_button: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

void reset_button() {
    if (g_fd >= 0) ::close(g_fd);
    g_fd = -1;
    g_path.clear();
}

bool matches_button(const std::string& hidraw_name) {
    const std::string uevent = "/sys/class/hidraw/" + hidraw_name + "/device/uevent";
    std::ifstream in(uevent);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("HID_ID=", 0) != 0) continue;
        unsigned bus = 0, vid = 0, pid = 0;
        if (std::sscanf(line.c_str(), "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) {
            return vid == kVid && pid == kPid;
        }
    }
    return false;
}

bool auto_detect_button(std::string& out_path) {
    DIR* dir = ::opendir("/sys/class/hidraw");
    if (!dir) return false;
    dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr) {
        const std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (name.rfind("hidraw", 0) != 0) continue;
        if (!matches_button(name)) continue;
        out_path = "/dev/" + name;
        ::closedir(dir);
        return true;
    }
    ::closedir(dir);
    return false;
}

bool exchange_state(int& out_state) {
    unsigned char buf[8] = {};
    buf[0] = 0x08;
    buf[7] = 0x02;
    const ssize_t w = ::write(g_fd, buf, sizeof(buf));
    if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        diag("write failed on %s: %s", g_path.c_str(), std::strerror(errno));
        reset_button();
        return false;
    }

    unsigned char resp[8] = {};
    const ssize_t r = ::read(g_fd, resp, sizeof(resp));
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        diag("read failed on %s: %s", g_path.c_str(), std::strerror(errno));
        reset_button();
        return false;
    }
    if (r < 1) return false;
    out_state = static_cast<int>(resp[0]);
    return out_state == BUTTON_LID_CLOSED || out_state == BUTTON_BUTTON_PRESSED || out_state == BUTTON_LID_OPEN;
}
} // namespace

bool init_button() {
    if (g_fd >= 0) return true;
    if (!auto_detect_button(g_path)) {
        diag("device %04x:%04x not found", kVid, kPid);
        return false;
    }
    g_fd = ::open(g_path.c_str(), O_RDWR | O_NONBLOCK);
    if (g_fd < 0) {
        diag("open failed on %s: %s", g_path.c_str(), std::strerror(errno));
        g_path.clear();
        return false;
    }
    diag("opened %s", g_path.c_str());
    return true;
}

int poll_button() {
    g_pressed_edge = false;
    if (!init_button()) return BUTTON_NO_CHANGE;
    int state = BUTTON_NO_CHANGE;
    if (!exchange_state(state)) return BUTTON_NO_CHANGE;
    if (state == g_last_state) return BUTTON_NO_CHANGE;
    g_pressed_edge = (state == BUTTON_BUTTON_PRESSED && g_last_state != BUTTON_BUTTON_PRESSED);
    g_last_state = state;
    return state;
}

bool is_button_pressed_edge() {
    return g_pressed_edge;
}

const char* button_state_name(int state) {
    switch (state) {
        case BUTTON_LID_CLOSED: return "LID_CLOSED";
        case BUTTON_BUTTON_PRESSED: return "BUTTON_PRESSED";
        case BUTTON_LID_OPEN: return "LID_OPEN";
        default: return "UNKNOWN";
    }
}

int button_last_state() {
    return g_last_state;
}
