#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include "config_loader.h"

void start_console_mode(AppConfig &config);
void send_llm_request(AppConfig &cfg, const std::string &prompt, std::string &full_response);

#endif
