#include <iostream>
#include <string>
#include <jsoncpp/json/json.h>
#include <libserialport.h>
#include "config.h"

void sendCommand(const std::string& command, sp_port* port);
std::string receiveCommand();
Json::Value processCommand(const std::string& command);

#define handle_error(msg, ret) \
    if ((ret) != SP_OK) { \
        std::cerr << "Error: " << msg << " (" << ret << ")" << std::endl; \
        return 1; \
    }

int main() {
    struct sp_port* port;

    // Get port by name
    handle_error("Getting serial port", sp_get_port_by_name(CONFIG_SERIAL_PORT, &port));

    // Open port
    handle_error("Opening serial port", sp_open(port, SP_MODE_READ_WRITE));

    // Set baudrate
    handle_error("Setting baudrate", sp_set_baudrate(port, CONFIG_SERIAL_BAUDRATE));

    std::cout << "Ollama CLI (Linux Version)" << std::endl;

    while (true) {
        std::string command = receiveCommand();
        if (command == "exit") {
            break;
        }

        sendCommand(command, port);
        Json::Value result = processCommand(command);

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::cout << Json::writeString(writer, result) << std::endl;
    }

    sp_close(port);
    sp_free_port(port);

    return 0;
}

void sendCommand(const std::string& command, sp_port* port) {
    sp_nonblocking_write(port, command.c_str(), command.size());
}

std::string receiveCommand() {
    std::string command;
    std::getline(std::cin, command);
    return command;
}

Json::Value processCommand(const std::string& command) {
    Json::Value result;
    result["status"] = "success";
    result["message"] = "Command: " + command;
    return result;
}

