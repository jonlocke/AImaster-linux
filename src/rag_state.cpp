#include "rag_state.hpp"
namespace rag_state {
static std::mutex g_mtx;
static std::string g_active_sid;
void SetActiveSession(const std::string& session_id){ std::lock_guard<std::mutex> L(g_mtx); g_active_sid = session_id; }
std::string GetActiveSession(){ std::lock_guard<std::mutex> L(g_mtx); return g_active_sid; }
bool HasActiveSession(){ std::lock_guard<std::mutex> L(g_mtx); return !g_active_sid.empty(); }
} // namespace rag_state
