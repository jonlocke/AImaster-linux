#include "serial_handler.h"
#include "ollama_client.h"
#include "config.h"
#include <jsoncpp/json/json.h>
#include <iostream>

int main() {
    AppConfig config;
    if (!loadConfig("config.txt", config)) return 1;
    loadCommands("cmds.txt", config);

    initSerial(config);

    std::cout << "Ollama CLI\nType HELP for a list of commands.\n";

    while (true) {
        std::string command = receiveCommand();
        if (command == "exit") break;

        sendCommand(command);
        Json::Value result = processCommand(command, config);

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::cout << Json::writeString(writer, result) << std::endl;
    }

    closeSerial();
    return 0;
}
