#include "command_exec.h"
#include "serial_handler.h"

#include <iostream>
#include <string>

// Forward-declared existing dispatcher (implemented in ollama_client.cpp)
Json::Value processCommand(const std::string& line, AppConfig& config);

static thread_local CommandSource tls_source = CommandSource::CONSOLE;

struct ScopedSource {
    CommandSource prev;
    explicit ScopedSource(CommandSource s) : prev(tls_source) { tls_source = s; }
    ~ScopedSource() { tls_source = prev; }
};

// Minimal sanitizer for serial sinks:
// - Replace curly quotes with straight quotes
// - Replace U+00B4 (acute accent) with apostrophe
// - Strip simple ANSI CSI sequences (e.g., \x1B[...m)
// - Map macron (¯) to '-'
static std::string sanitize_for_serial(const std::string& in) {
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ) {
        unsigned char c = static_cast<unsigned char>(in[i]);

        // Strip ANSI CSI sequences: ESC [ ... letter
        if (c == 0x1B && i + 1 < in.size() && in[i+1] == '[') {
            i += 2;
            while (i < in.size()) {
                unsigned char d = static_cast<unsigned char>(in[i]);
                if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')) { i++; break; }
                i++;
            }
            continue;
        }

        // UTF-8 curly singles/doubles: E2 80 98/99/9C/9D
        if (i + 2 < in.size() && c == 0xE2 &&
            static_cast<unsigned char>(in[i+1]) == 0x80) {
            unsigned char t = static_cast<unsigned char>(in[i+2]);
            if (t == 0x98 || t == 0x99) { out.push_back('\''); i += 3; continue; } // ‘ ’ -> '
            if (t == 0x9C || t == 0x9D) { out.push_back('\"'); i += 3; continue; }  // “ ” -> "
        }

        // Latin-1 acute accent: C2 B4 -> '
        if (i + 1 < in.size() && c == 0xC2 &&
            static_cast<unsigned char>(in[i+1]) == 0xB4) {
            out.push_back('\'');
            i += 2;
            continue;
        }

        // Macron: C2 AF -> '-' (overline)
        if (i + 1 < in.size() && c == 0xC2 &&
            static_cast<unsigned char>(in[i+1]) == 0xAF) {
            out.push_back('-');
            i += 2;
            continue;
        }

        // Plain ASCII
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            i++;
            continue;
        }

        // Fallback
        out.push_back('?');
        i++;
    }

    return out;
}

void route_output(const std::string& s, bool newline) {
    if (tls_source == CommandSource::SERIAL && serial_available) {
        std::string cleaned = sanitize_for_serial(s);
        if (!cleaned.empty()) serialSend(cleaned);
        if (newline) serialSend("\r\n");
    } else {
        if (!s.empty()) std::cout << s;
        if (newline) std::cout << std::endl;
    }
}

Json::Value execute_command(const std::string& line, AppConfig& config, CommandSource source) {
    ScopedSource scope(source);
    return processCommand(line, config);
}
