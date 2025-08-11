#ifndef SERIAL_HANDLER_H
#define SERIAL_HANDLER_H

#include <string>
#include <functional>

extern bool serial_available;

bool initSerial(const std::string& port, int baudrate);
void serialSend(const std::string& data);
void setSerialSendDelay(int delay_ms);
bool serialReadLine(std::string& out, int timeout_ms = 50);
void startSerialListener(const std::function<void(const std::string&)>& on_line,
                         int timeout_ms = 50);
void stopSerialListener();
void closeSerial();

#endif
