#include "command_exec.h"
#include "serial_handler.h"

#include <iostream>
#include <string>
#include <atomic>
#include <cctype>

Json::Value processCommand(const std::string& line, AppConfig& config);

//static thread_local CommandSource tls_source = CommandSource::CONSOLE;
thread_local CommandSource tls_source = CommandSource::CONSOLE;


struct ScopedSource {
    CommandSource prev;
    explicit ScopedSource(CommandSource s) : prev(tls_source) { tls_source = s; }
    ~ScopedSource() { tls_source = prev; }
};

CommandSource getCurrentCommandSource() { return tls_source; }

static std::atomic<bool> g_serial_override{false};
void setSerialOutputOverride(bool enabled) {
    g_serial_override.store(enabled, std::memory_order_relaxed);
}

static std::atomic<int> g_wrap_cols{80};
static thread_local int  tl_cur_col = 0;
static thread_local std::string tl_pending_word;
static thread_local bool tl_line_started = false;

void setSerialWrapColumns(int cols) {
    if (cols < 10) cols = 10;
    if (cols > 240) cols = 240;
    g_wrap_cols.store(cols, std::memory_order_relaxed);
}

static std::string sanitize_for_serial(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0x1B && i + 1 < in.size() && in[i+1] == '[') {
            i += 2;
            while (i < in.size()) {
                unsigned char d = (unsigned char)in[i];
                if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')) { i++; break; }
                i++;
            }
            continue;
        }
        if (i + 2 < in.size() && c == 0xE2 && (unsigned char)in[i+1] == 0x80) {
            unsigned char t = (unsigned char)in[i+2];
            if (t == 0x98 || t == 0x99) { out.push_back('\''); i += 3; continue; }
            if (t == 0x9C || t == 0x9D) { out.push_back('\"');  i += 3; continue; }
        }
        if (c < 0x80) { out.push_back((char)c); i++; }
        else { out.push_back('?'); i++; }
    }
    return out;
}

static void place_word(const std::string& word, int wrap_cols, std::string& out) {
    if (word.empty()) return;
    size_t idx = 0;
    while (idx < word.size()) {
        int remaining = wrap_cols - tl_cur_col;
        if (tl_cur_col == 0) remaining = wrap_cols;

        if (tl_cur_col == 0 && (int)(word.size() - idx) > wrap_cols) {
            out.append(word.substr(idx, wrap_cols));
            out.push_back('\n');
            tl_cur_col = 0;
            tl_line_started = false;
            idx += wrap_cols;
            continue;
        }
        if (tl_line_started) {
            size_t leftover = word.size() - idx;
            if ((int)leftover + 1 <= remaining) {
                out.push_back(' ');
                out.append(word.substr(idx));
                tl_cur_col += 1 + (int)leftover;
                return;
            } else {
                out.push_back('\n');
                tl_cur_col = 0;
                tl_line_started = false;
                continue;
            }
        } else {
            size_t wlen = word.size() - idx;
            if ((int)wlen <= wrap_cols) {
                out.append(word.substr(idx));
                tl_cur_col = (int)wlen;
                tl_line_started = true;
                return;
            } else {
                out.append(word.substr(idx, wrap_cols));
                out.push_back('\n');
                tl_cur_col = 0;
                tl_line_started = false;
                idx += wrap_cols;
            }
        }
    }
}

static void append_wrapped(const std::string& s, bool force_newline, std::string& out) {
    const int wrap_cols = g_wrap_cols.load(std::memory_order_relaxed);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r' || c == '\n') {
            if (!tl_pending_word.empty()) {
                place_word(tl_pending_word, wrap_cols, out);
                tl_pending_word.clear();
            }
            out.push_back('\n');
            tl_cur_col = 0;
            tl_line_started = false;
            continue;
        }
        if (c == '\t') c = ' ';
        if (std::isspace((unsigned char)c)) {
            if (!tl_pending_word.empty()) {
                place_word(tl_pending_word, wrap_cols, out);
                tl_pending_word.clear();
            }
        } else {
            tl_pending_word.push_back(c);
        }
    }
    if (force_newline) {
        if (!tl_pending_word.empty()) {
            place_word(tl_pending_word, wrap_cols, out);
            tl_pending_word.clear();
        }
        out.push_back('\n');
        tl_cur_col = 0;
        tl_line_started = false;
    }
}
// Setter used by sendMessageToOllama to keep routing aligned with caller
void setCurrentCommandSource(CommandSource s) {
    // tls_source is already defined in this translation unit
    tls_source = s;
}

void route_output(const std::string& s, bool newline) {
    const bool use_serial = (tls_source == CommandSource::SERIAL) ||
                            g_serial_override.load(std::memory_order_relaxed);
::fprintf(stderr, "[DIAG] route_output to=%c src=%d msg=%.40s\n",
          (use_serial && serial_available) ? 's' : 'c',
          (int)getCurrentCommandSource(),
          s.c_str());

    if (use_serial && serial_available) {
        std::string cleaned = sanitize_for_serial(s);
        std::string wrapped;
        append_wrapped(cleaned, newline, wrapped);
        if (!wrapped.empty()) serialSend(wrapped);
    } else {
        if (!s.empty()) std::cout << s;
        if (newline) std::cout << std::endl;
    }
}

Json::Value execute_command(const std::string& line, AppConfig& config, CommandSource source) {
    ScopedSource scope(source);
    return processCommand(line, config);
}
