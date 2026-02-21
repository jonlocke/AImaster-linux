#include "rag_int_bridge.hpp"
#include "rag_state.hpp"
#include "rag_adapter.hpp"
#include <atomic>
#include <algorithm>
#include <iostream>
#include <cctype>

namespace {
bool IsRAGSentinelResponse(const std::string& response) {
    return response.find("No relevant context found in the document to answer your question.") != std::string::npos ||
           response.find("Invalid or unknown session_id") != std::string::npos;
}

bool IsSummaryLikeQuery(const std::string& user_input) {
    std::string s = user_input;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s.find("summar") != std::string::npos ||
           s.find("overview") != std::string::npos ||
           s.find("entire") != std::string::npos ||
           s.find("whole") != std::string::npos;
}

}

namespace rag_int {
static std::atomic<bool> g_enabled{true};
bool Enabled(){ return g_enabled.load(); }
void SetEnabled(bool on){ g_enabled.store(on); }
bool TryRAGAnswer(const std::string& user_input, std::string& out_answer, int k, double threshold) {
    if (!Enabled()) return false;
    if (!rag_state::HasActiveSession()) return false;

    int effective_k = k;
    double effective_threshold = threshold;
    if (IsSummaryLikeQuery(user_input) && effective_k < 24) {
        effective_k = 24;
        effective_threshold = std::min(effective_threshold, 0.10);
    }

    out_answer = AIMaster_RAG_Ask(rag_state::GetActiveSession(), user_input, effective_k, effective_threshold);
    if (IsRAGSentinelResponse(out_answer)) {
        std::cerr << "[RAG miss; using base model]" << std::endl;
        return false;
    }
    return !out_answer.empty();
}
} // namespace rag_int
