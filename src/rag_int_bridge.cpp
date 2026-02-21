#include "rag_int_bridge.hpp"
#include "rag_state.hpp"
#include "rag_adapter.hpp"
#include <atomic>
#include <iostream>

namespace {
bool IsRAGSentinelResponse(const std::string& response) {
    return response.find("No relevant context found in the document to answer your question.") != std::string::npos ||
           response.find("Invalid or unknown session_id") != std::string::npos;
}
}

namespace rag_int {
static std::atomic<bool> g_enabled{true};
bool Enabled(){ return g_enabled.load(); }
void SetEnabled(bool on){ g_enabled.store(on); }
bool TryRAGAnswer(const std::string& user_input, std::string& out_answer, int k, double threshold) {
    if (!Enabled()) return false;
    if (!rag_state::HasActiveSession()) return false;
    out_answer = AIMaster_RAG_Ask(rag_state::GetActiveSession(), user_input, k, threshold);
    if (IsRAGSentinelResponse(out_answer)) {
        std::cerr << "[RAG miss; using base model]" << std::endl;
        return false;
    }
    return !out_answer.empty();
}
} // namespace rag_int
