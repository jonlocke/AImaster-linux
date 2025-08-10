#include "serial_handler.h"
#include <libserialport.h>
#include <iostream>
#include <cerrno>
#include <cstring>

bool serial_available = false;
static sp_port* serial_port = nullptr;

bool initSerial(const std::string& port, int baudrate) {
    if (port.empty() || baudrate <= 0) {
        serial_available = false;
        return false;
    }

    if (sp_get_port_by_name(port.c_str(), &serial_port) != SP_OK) {
        std::cerr << "[Warning] Could not open serial port: " << port << std::endl;
        serial_available = false;
        return false;
    }

    if (sp_open(serial_port, SP_MODE_READ_WRITE) != SP_OK) {
        std::cerr << "[Warning] Failed to open serial port: " << port << std::endl;
        sp_free_port(serial_port);
        serial_available = false;
        return false;
    }

    sp_set_baudrate(serial_port, baudrate);
    sp_set_bits(serial_port, 8);
    sp_set_parity(serial_port, SP_PARITY_NONE);
    sp_set_stopbits(serial_port, 1);
    sp_set_flowcontrol(serial_port, SP_FLOWCONTROL_NONE);

    serial_available = true;
    return true;
}

void serialSend(const std::string& data) {
    if (!serial_available || !serial_port) return;
    sp_nonblocking_write(serial_port, data.c_str(), data.size());
}

void closeSerial() {
    if (serial_port) {
        sp_close(serial_port);
        sp_free_port(serial_port);
        serial_port = nullptr;
    }
    serial_available = false;
}
