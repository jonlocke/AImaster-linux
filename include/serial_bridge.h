#ifndef SERIAL_BRIDGE_H
#define SERIAL_BRIDGE_H

#include <functional>
#include <string>

// Starts the serial listener and forwards each line to your dispatcher with
// routing safely set to SERIAL for the duration of the command.
//
// Option A (zero changes if you already expose process_command):
//   extern bool process_command(const std::string&);
//   void start_serial_bridge_default();
//
// Option B (supply your own dispatcher explicitly):
//   void start_serial_bridge_custom(const std::function<bool(const std::string&)>& dispatcher);
//
// The bridge normalizes tokens to uppercase (for typical command tables),
// trims whitespace, sends prompt "-> " after each command, and prints clear DIAG logs.

void start_serial_bridge_default();
void start_serial_bridge_custom(const std::function<bool(const std::string&)>& dispatcher);

#endif // SERIAL_BRIDGE_H
