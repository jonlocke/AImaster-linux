#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <iostream>
#include <jsoncpp/json/json.h>
#include "rag_adapter.hpp"
#include "rag_state.hpp"
#include "rag_int_bridge.hpp"

inline bool HandleRAGConsoleCommand(const std::string& line, Json::Value& out) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    for (std::string t; iss >> t; ) tokens.push_back(t);
    if (tokens.empty()) return false;
    const std::string& cmd = tokens[0];

    if (cmd == "RAG_INGEST") {
        if (tokens.size() < 2) { std::cout << "Usage: RAG_INGEST <folder>\n"; out["ok"]=false; out["error"]="usage"; return true; }
        const std::string folder = tokens[1];
        std::string sid = AIMaster_RAG_AddFolder(folder);
        if (sid.empty()) { std::cout << "RAG ingest failed: " << AIMaster_RAG_LastError() << "\n"; out["ok"]=false; out["error"]=AIMaster_RAG_LastError(); return true; }
        rag_state::SetActiveSession(sid);
        std::cout << "RAG session set: " << sid << "\n";
        out["ok"]=true; out["session_id"]=sid; return true;
    }
    else if (cmd == "RAG_ASK") {
        if (!rag_state::HasActiveSession()) { std::cout << "No active RAG session. Run RAG_INGEST <folder> or RAG_SESSION SET <sid>.\n"; out["ok"]=false; out["error"]="no_active_session"; return true; }
        std::string q; for (size_t i=1;i<tokens.size();++i) { if (i>1) q += ' '; q += tokens[i]; }
        std::string ans = AIMaster_RAG_Ask(rag_state::GetActiveSession(), q, 5, 0.2);
        if (ans.empty()) { std::cout << "RAG ask failed: " << AIMaster_RAG_LastError() << "\n"; out["ok"]=false; out["error"]=AIMaster_RAG_LastError(); return true; }
        std::cout << ans << "\n"; out["ok"]=true; out["answer"]=ans; return true;
    }
    else if (cmd == "RAG_SESSION") {
        out["ok"]=true;
        if (tokens.size()>=2 && tokens[1]=="SET") {
            if (tokens.size()<3) { std::cout << "Usage: RAG_SESSION SET <sid>\n"; out["ok"]=false; out["error"]="usage"; }
            else { rag_state::SetActiveSession(tokens[2]); std::cout << "RAG session set.\n"; }
        } else if (tokens.size()>=2 && tokens[1]=="SHOW") {
            auto sid = rag_state::GetActiveSession(); std::cout << (sid.empty()? "<none>":sid) << "\n"; out["session_id"]=sid;
        } else if (tokens.size()>=2 && tokens[1]=="CLEAR") {
            rag_state::SetActiveSession(""); std::cout << "RAG session cleared.\n";
        } else {
            std::cout << "Usage: RAG_SESSION <SET|SHOW|CLEAR> [sid]\n"; out["ok"]=false; out["error"]="usage";
        }
        return true;
    }
    else if (cmd == "RAG_INT") {
        out["ok"]=true;
        if (tokens.size()<2 || (tokens[1]!="ON" && tokens[1]!="OFF" && tokens[1]!="STATUS")) {
            std::cout << "Usage: RAG_INT <ON|OFF|STATUS>\n"; out["ok"]=false; out["error"]="usage"; return true;
        }
        if (tokens[1]=="ON")  { rag_int::SetEnabled(true);  std::cout << "RAG_INT: ON\n"; }
        if (tokens[1]=="OFF") { rag_int::SetEnabled(false); std::cout << "RAG_INT: OFF\n"; }
        if (tokens[1]=="STATUS") { std::cout << "RAG_INT: " << (rag_int::Enabled()? "ON":"OFF") << "\n"; out["enabled"]=rag_int::Enabled(); }
        return true;
    }

    return false;
}
