#include "rag_int_bridge.hpp"
#include "rag_state.hpp"
#include "rag_adapter.hpp"
#include <atomic>

namespace rag_int {
static std::atomic<bool> g_enabled{true};
bool Enabled(){ return g_enabled.load(); }
void SetEnabled(bool on){ g_enabled.store(on); }
bool TryRAGAnswer(const std::string& user_input, std::string& out_answer, int k, double threshold) {
    if (!Enabled()) return false;
    if (!rag_state::HasActiveSession()) return false;
    out_answer = AIMaster_RAG_Ask(rag_state::GetActiveSession(), user_input, k, threshold);
    return !out_answer.empty();
}
} // namespace rag_int
