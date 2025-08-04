#pragma once
#include <string>
#include <libserialport.h>
#include "config.h"
#include "serial_handler.h"
#include <iostream>

#if !defined(_WIN32)
#include <filesystem>
#include <vector>  // ✅ Added for std::vector
#endif

extern struct sp_port* port;
extern bool serial_available;
extern std::string detected_port;

bool initSerial(const AppConfig& config);
void closeSerial();
void sendCommand(const std::string& command);
std::string receiveCommand();
#ifndef _WIN32
std::string autoDetectSerialPort();
#endif
