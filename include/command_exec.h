#ifndef COMMAND_EXEC_H
#define COMMAND_EXEC_H

#include <string>
#include <json/json.h>
#include "config_loader.h"

enum class CommandSource { CONSOLE = 0, SERIAL = 1 };

Json::Value execute_command(const std::string& line, AppConfig& config, CommandSource source);
void route_output(const std::string& s, bool newline=false);
void setSerialWrapColumns(int cols);
CommandSource getCurrentCommandSource();
void setSerialOutputOverride(bool enabled);
inline void route_newline() { route_output(std::string(), true); }

#endif // COMMAND_EXEC_H
