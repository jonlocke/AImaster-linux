#ifndef CHAT_SESSION_H
#define CHAT_SESSION_H

#include <string>
#include <vector>

struct ChatMessage {
    std::string role;
    std::string content;
};

extern std::vector<ChatMessage> conversation_history;
extern std::string current_model;

#endif
