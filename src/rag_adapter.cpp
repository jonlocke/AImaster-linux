#include "rag_session.hpp"
#include "rag_adapter.hpp"
#include <mutex>
static std::mutex g_mtx;
static std::string g_last_error;
static RAGSessionManager g_mgr;
const std::string& AIMaster_RAG_LastError(){ return g_last_error; }
void AIMaster_RAG_SetVerbose(bool v){ std::lock_guard<std::mutex> L(g_mtx); g_mgr.setVerbose(v); }
std::string AIMaster_RAG_AddFolder(const std::string& folder){ std::lock_guard<std::mutex> L(g_mtx); try{ auto sid=g_mgr.createSessionFromFolder(folder); g_last_error.clear(); return sid; }catch(const std::exception& e){ g_last_error=e.what(); return {}; } }
std::string AIMaster_RAG_Ask(const std::string& sid,const std::string& q,int k,double thr){ std::lock_guard<std::mutex> L(g_mtx); try{ auto ans=g_mgr.chat(sid,q,k,thr); g_last_error.clear(); return ans; }catch(const std::exception& e){ g_last_error=e.what(); return {}; } }
