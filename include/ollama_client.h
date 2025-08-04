#pragma once
#include <string>
#include <jsoncpp/json/json.h>
#include "config.h"

Json::Value processCommand(const std::string& command, const AppConfig& config);
