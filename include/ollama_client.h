#pragma once
#include <string>
#include <json/json.h>

struct AppConfig; // defined in config_loader.h

// Prompt helper used by both console and serial paths.
// Outside INT: suffix "> "  |  Inside INT: suffix "-> "
std::string modelPrompt(const AppConfig& cfg, const char* suffix);

// Main command dispatcher (console & serial share this).
Json::Value processCommand(const std::string& command, AppConfig& config);

// Serial interactive mode hooks used by main/serial listener.
bool SerialINT_IsActive();
void SerialINT_Start(AppConfig& config);
void SerialINT_HandleLine(const std::string& line, AppConfig& config);

// Non-blocking READ (interactive)
bool ReadAwait_IsActive();
void ReadAwait_Start(AppConfig& config);
void ReadAwait_StartWithFile(AppConfig& config, const std::string& filename);
void ReadAwait_StartFolderPickWithContext(AppConfig& config, const std::string& ctx, const std::string& dir);
void ReadAwait_StartFolderPickThenAskContext(AppConfig& config, const std::string& dir);
void ReadAwait_HandleLine(const std::string& line, AppConfig& config);
