#pragma once
#include <string>
#include <jsoncpp/json/json.h>
#include "config_loader.h"

Json::Value processCommand(const std::string& command, const AppConfig& config);
