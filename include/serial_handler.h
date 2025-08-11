#ifndef SERIAL_HANDLER_H
#define SERIAL_HANDLER_H

#include <string>

// Extern defined elsewhere in your project
extern bool serial_available;

// Initialize serial connection with port and baudrate
// (Signature unchanged for drop-in compatibility.)
bool initSerial(const std::string& port, int baudrate);

// Send a string over serial (if available). This now sends characters
// at a configurable rate (see setSerialSendDelay).
void serialSend(const std::string& data);

// Adjust the inter-character delay (in milliseconds).
// Use 0 for "as fast as possible". Default is 50 ms.
void setSerialSendDelay(int delay_ms);

// Close serial connection
void closeSerial();

#endif
