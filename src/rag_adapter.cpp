#include "rag_adapter.hpp"
#include "rag_session.hpp"
#include <mutex>

static std::mutex g_mutex;
static std::string g_last_error;
static RAGSessionManager g_mgr; // default settings

const std::string& AIMaster_RAG_LastError() { return g_last_error; }
void AIMaster_RAG_SetVerbose(bool v) { std::lock_guard<std::mutex> lock(g_mutex); g_mgr.setVerbose(v); }

std::string AIMaster_RAG_AddFolder(const std::string& folder_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        std::string sid = g_mgr.createSessionFromFolder(folder_path);
        g_last_error.clear();
        return sid;
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return std::string();
    }
}

std::string AIMaster_RAG_Ask(const std::string& session_id,
                             const std::string& question,
                             int k,
                             double score_threshold) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        std::string ans = g_mgr.chat(session_id, question, k, score_threshold);
        g_last_error.clear();
        return ans;
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return std::string();
    }
}
