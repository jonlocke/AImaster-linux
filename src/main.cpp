#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <readline/readline.h>
#include <readline/history.h>
#include "config_loader.h"
#include "serial_handler.h"
#include "ollama_client.h"
#include "utils.h"

namespace fs = std::filesystem;
static const char* HISTORY_FILE = "~/.ollama_cli_history";

// Expand tilde in file paths
std::string expandTilde(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

// --- Completion function ---
char* command_generator(const char* text, int state) {
    static size_t list_index;
    static std::vector<std::string> matches;

    if (state == 0) {
        matches.clear();
        list_index = 0;

        std::string buffer(rl_line_buffer);
        size_t spacePos = buffer.find(' ');

        if (spacePos != std::string::npos) {
            std::string cmd = buffer.substr(0, spacePos);
            if (cmd == "READ") {
                std::string partial(text);
                std::string dir = ".";
                std::string prefix = partial;

                size_t slashPos = partial.find_last_of('/');
                if (slashPos != std::string::npos) {
                    dir = partial.substr(0, slashPos);
                    prefix = partial.substr(slashPos + 1);
                }

                if (fs::exists(dir) && fs::is_directory(dir)) {
                    for (const auto& entry : fs::directory_iterator(dir)) {
                        std::string fname = entry.path().filename().string();
                        if (fname.find(prefix) == 0) {
                            matches.push_back(dir + "/" + fname);
                        }
                    }
                }
            }
        }
    }

    if (list_index < matches.size()) {
        return strdup(matches[list_index++].c_str());
    }
    return nullptr;
}

char** custom_completion(const char* text, int start, int end) {
    (void)end;
    if (start > 0) {
        return rl_completion_matches(text, command_generator);
    }
    return nullptr;
}

int main() {
    AppConfig config;
    if (!loadConfig("config.txt", config)) {
        std::cerr << "Error loading config.txt" << std::endl;
        return 1;
    }

    if (!initSerial(config.serial_port, config.baudrate)) {
        std::cerr << "Warning: No serial port available. Using console mode." << std::endl;
    }

    // Set up tab completion
    rl_attempted_completion_function = custom_completion;

    // Load persistent history
    std::string histFile = expandTilde(HISTORY_FILE);
    read_history(histFile.c_str());

    std::cout << "\033[38;2;255;215;0mOllama CLI\033[0m\n\033[38;2;255;239;184mType HELP for a list of commands.\033[0m\n";

    while (true) {
        std::string prompt = "\033[38;2;255;239;184m" + config.ollama_model + "> \033[0m";
        char* input = readline(prompt.c_str());
        if (!input) break;

        if (*input) {
            add_history(input);
            write_history(histFile.c_str());
        }

        std::string command(input);
        free(input);

        std::string cmd_upper = command;
        std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);

        // If READ entered with no args → interactive picker
        if (cmd_upper == "READ") {
            std::string context;
            std::cout << "Enter context: ";
            std::getline(std::cin, context);

            std::string filename = pickFile("code");
            if (filename.empty()) continue;

            command = "READ_CTX:" + context + "|FILE:" + filename;
        }

        Json::Value result = processCommand(command, config);
    }

    // Save history on exit
    write_history(histFile.c_str());

    return 0;
}
