#include "rag_console_commands.hpp"
#include "rag_adapter.hpp"
#include "rag_state.hpp"
#include <sstream>
#include <vector>
#include <iostream>

static void tokenize(const std::string& line, std::vector<std::string>& toks) {
    std::istringstream iss(line);
    std::string t;
    while (iss >> t) toks.push_back(t);
}

bool HandleRAGConsoleCommand(const std::string& line, Json::Value& out) {
    std::vector<std::string> tokens;
    tokenize(line, tokens);
    if (tokens.empty()) return false;
    const std::string& cmd = tokens[0];

    if (cmd == "RAG_INGEST") {
        if (tokens.size() < 2) {
            std::cout << "Usage: RAG_INGEST <folder>\n";
            out["ok"] = false; out["error"] = "usage";
            return true;
        }
        const std::string folder = tokens[1];
        std::string sid = AIMaster_RAG_AddFolder(folder);
        if (sid.empty()) {
            std::cout << "RAG ingest failed: " << AIMaster_RAG_LastError() << "\n";
            out["ok"] = false; out["error"] = AIMaster_RAG_LastError();
            return true;
        }
        rag_state::SetActiveSession(sid);
        std::cout << "RAG session set: " << sid << "\n";
        out["ok"] = true; out["session_id"] = sid;
        return true;
    }
    else if (cmd == "RAG_ASK") {
        if (!rag_state::HasActiveSession()) {
            std::cout << "No active RAG session. Run RAG_INGEST <folder> or RAG_SESSION SET <sid>.\n";
            out["ok"] = false; out["error"] = "no_active_session";
            return true;
        }
        std::string q;
        for (size_t i=1;i<tokens.size();++i) { if (i>1) q += ' '; q += tokens[i]; }
        std::string ans = AIMaster_RAG_Ask(rag_state::GetActiveSession(), q, 5, 0.2);
        if (ans.empty()) {
            std::cout << "RAG ask failed: " << AIMaster_RAG_LastError() << "\n";
            out["ok"] = false; out["error"] = AIMaster_RAG_LastError();
            return true;
        }
        std::cout << ans << "\n";
        out["ok"] = true; out["answer"] = ans;
        return true;
    }
    else if (cmd == "RAG_SESSION") {
        out["ok"] = true;
        if (tokens.size()>=2 && tokens[1]=="SET") {
            if (tokens.size()<3) {
                std::cout << "Usage: RAG_SESSION SET <sid>\n";
                out["ok"] = false; out["error"] = "usage";
            } else {
                rag_state::SetActiveSession(tokens[2]);
                std::cout << "RAG session set.\n";
            }
        } else if (tokens.size()>=2 && tokens[1]=="SHOW") {
            auto sid = rag_state::GetActiveSession();
            std::cout << (sid.empty() ? "<none>" : sid) << "\n";
            out["session_id"] = sid;
        } else if (tokens.size()>=2 && tokens[1]=="CLEAR") {
            rag_state::SetActiveSession("");
            std::cout << "RAG session cleared.\n";
        } else {
            std::cout << "Usage: RAG_SESSION <SET|SHOW|CLEAR> [sid]\n";
            out["ok"] = false; out["error"] = "usage";
        }
        return true;
    }

    return false; // not handled
}
