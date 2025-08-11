#ifndef SERIAL_HANDLER_H
#define SERIAL_HANDLER_H

#include <string>
#include <functional>

extern bool serial_available;

bool initSerial(const std::string& port, int baudrate);
void serialSend(const std::string& data);
void setSerialSendDelay(int delay_ms);

// New: configure how '\n' is written to the wire.
// Accepts: "CRLF" (default), "LFCR", "LF", "CR" (case-insensitive, others -> default)
void setSerialNewlinePolicy(const std::string& policy);

// Read a line from serial (CR, LF, or CRLF ends a line)
bool serialReadLine(std::string& out, int timeout_ms = 50);

// Background listener
void startSerialListener(const std::function<void(const std::string&)>& on_line, int timeout_ms = 50);
void stopSerialListener();

void closeSerial();

#endif
