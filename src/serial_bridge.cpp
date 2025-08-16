#include "serial_bridge.h"
#include "serial_handler.h"
#include "route_guard.h"

#include <cctype>
#include <cstdio>
#include <string>

// If you use the default bridge, provide this in your project:
extern bool process_command(const std::string& line);

// Simple helpers
static inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static inline std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static void run_bridge(const std::function<bool(const std::string&)>& dispatcher)
{
    startSerialListener([dispatcher](const std::string& raw){
        std::string line = trim(raw);
        if (line.empty()) { serialSend("-> "); return; }

        std::fprintf(stderr, "[SER->RX] '%s'\n", line.c_str());

        // Route output to SERIAL just for this command.
        RouteGuard guard(CommandSource::SERIAL);

        // Normalize token to uppercase if your dispatcher uses keys.
        // (We still pass the original line to dispatcher in case it expects raw args.)
        std::string token = line;
        auto sp = token.find(' ');
        if (sp != std::string::npos) token.resize(sp);
        std::string token_up = upper(token);

        // Allow dispatcher to decide what to do; many implementations parse internally.
        bool handled = dispatcher(line);

        std::fprintf(stderr, "[DISPATCH] token='%s' handled=%s\n",
                     token_up.c_str(), handled ? "true" : "false");

        if (!handled) {
            serialSend("[ERR] Unknown command\r\n");
        }
        serialSend("-> ");
    });
}

void start_serial_bridge_default()
{
    run_bridge([](const std::string& line){
        return process_command(line);
    });
}

void start_serial_bridge_custom(const std::function<bool(const std::string&)>& dispatcher)
{
    run_bridge(dispatcher);
}
