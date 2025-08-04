#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <iomanip> // For std::hex
#include <cstdio>    // For std::printf

#include <jsoncpp/json/json.h> // For JSON parsing

// Include SerialPort header (assuming it's installed)
#include <libserialport.h>

// Configuration File
#include "config.h"

// Function Prototypes
void sendCommand(const std::string& command);
std::string receiveCommand();
Json::Value processCommand(const std::string& command);

// Error Handling Macro
#define handle_error(msg, ret) \
    if ((ret) < 0) { \
        std::cerr << "Error: " << msg << " (" << msg << ")" << std::endl; \
        return 1; \
    }

int main() {
    // Initialize SerialPort
    if (sp_dev_init() < 0) {
        std::cerr << "Error: Failed to initialize libserialport." << std::endl;
        return 1;
    }

    sp_dev_t dev = sp_dev_open(CONFIG_SERIAL_PORT, CONFIG_SERIAL_BAUDRATE);
    handle_error("Opening serial port", dev);

    std::cout << "Ollama CLI (Linux Version)" << std::endl;

    while (true) {
        std::string command = receiveCommand();
        if (command == "exit") {
            break;
        }

        Json::Value result = processCommand(command);

        std::cout << result.toPrettyPrint() << std::endl; // Print the JSON response
    }

    // Clean up SerialPort
    if (dev != NULL) {
        sp_dev_close(dev);
    }

    sp_dev_term(); // Terminate the libserialport library

    return 0;
}

void sendCommand(const std::string& command) {
    // In a real implementation, this would send the command over the serial port.
    // For simplicity, we'll just print it.
    std::cout << "Sending: " << command << std::endl;
}

std::string receiveCommand() {
    // In a real implementation, this would read a command from the serial port.
    // For simplicity, we'll just read from stdin.
    std::string command;
    std::getline(std::cin, command);
    return command;
}

Json::Value processCommand(const std::string& command) {
    // A placeholder response - replace with your Ollama API call
    Json::Value result;
    result["status"] = "success";
    result["message"] = "Command: " << command;
    return result;
}
