#pragma once
#include <string>
#include <mutex>
namespace rag_state {
void SetActiveSession(const std::string& session_id);
std::string GetActiveSession();
bool HasActiveSession();
} // namespace rag_state
