#ifndef SERIAL_HANDLER_H
#define SERIAL_HANDLER_H

#include <string>
#include "config_loader.h"

extern bool serial_available;

// Initialize serial connection with port and baudrate
bool initSerial(const std::string& port, int baudrate);

// Send a string over serial (if available)
void serialSend(const std::string& data);

// Close serial connection
void closeSerial();

#endif
