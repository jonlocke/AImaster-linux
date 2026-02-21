#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <jsoncpp/json/json.h>
#include "rag_adapter.hpp"
#include "rag_state.hpp"

inline void __rag_tokenize(const std::string& line, std::vector<std::string>& toks) {
    std::istringstream iss(line);
    std::string t;
    while (iss >> t) toks.push_back(t);
}

inline std::string __rag_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// Header-only console handler so no separate .cpp is required.
inline bool HandleRAGConsoleCommand(const std::string& line, Json::Value& out) {
    std::vector<std::string> tokens;
    __rag_tokenize(line, tokens);
    if (tokens.empty()) return false;
    const std::string cmd = __rag_upper(tokens[0]);

    // RAG_INGEST <folder>
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

    // RAG_SHOW [N]
    if (cmd == "RAG_SHOW") {
        int max_files = 10;
        if (tokens.size() >= 2) { try { max_files = std::stoi(tokens[1]); } catch (...) {} }
        auto sid = rag_state::GetActiveSession();
        if (sid.empty()) {
            std::cout << "No active RAG session. Run RAG_INGEST <folder> or RAG_SESSION SET <sid>.\n";
            out["ok"] = false; out["error"] = "no-session";
            return true;
        }
        std::string summary = AIMaster_RAG_Summary(sid, max_files);
        std::cout << summary;
        out["ok"] = true; out["summary"] = summary;
        return true;
    }

    // RAG_ASK [--k N] [--thr T] <question...>
    if (cmd == "RAG_ASK") {
        if (!rag_state::HasActiveSession()) {
            std::cout << "No active RAG session. Run RAG_INGEST <folder> or RAG_SESSION SET <sid>.\n";
            out["ok"] = false; out["error"] = "no-session";
            return true;
        }

        int k = 5;
        double threshold = 0.2;
        size_t i = 1;
        for (; i < tokens.size(); ++i) {
            if (tokens[i] == "--k") {
                if (i + 1 >= tokens.size()) {
                    std::cout << "Usage: RAG_ASK [--k N] [--thr T] <question...>\n"
                              << "Note: If no chunk meets threshold, top-k fallback is used when best similarity > 0.0.\n";
                    out["ok"] = false; out["error"] = "usage";
                    return true;
                }
                try { k = std::stoi(tokens[++i]); } catch (...) { k = -1; }
                if (k <= 0) {
                    std::cout << "RAG_ASK error: --k must be > 0\n";
                    out["ok"] = false; out["error"] = "invalid-k";
                    return true;
                }
                continue;
            }
            if (tokens[i] == "--thr") {
                if (i + 1 >= tokens.size()) {
                    std::cout << "Usage: RAG_ASK [--k N] [--thr T] <question...>\n"
                              << "Note: If no chunk meets threshold, top-k fallback is used when best similarity > 0.0.\n";
                    out["ok"] = false; out["error"] = "usage";
                    return true;
                }
                try { threshold = std::stod(tokens[++i]); } catch (...) { threshold = -1.0; }
                if (threshold < 0.0) {
                    std::cout << "RAG_ASK error: --thr must be >= 0\n";
                    out["ok"] = false; out["error"] = "invalid-threshold";
                    return true;
                }
                continue;
            }
            break;
        }

        if (i >= tokens.size()) {
            std::cout << "Usage: RAG_ASK [--k N] [--thr T] <question...>\n"
                      << "Note: If no chunk meets threshold, top-k fallback is used when best similarity > 0.0.\n";
            out["ok"] = false; out["error"] = "usage";
            return true;
        }

        std::ostringstream q;
        for (; i < tokens.size(); ++i) {
            if (q.tellp() > 0) q << ' ';
            q << tokens[i];
        }
        std::string ans = AIMaster_RAG_Ask(rag_state::GetActiveSession(), q.str(), k, threshold);
        if (ans.empty()) {
            std::cout << "RAG ask failed: " << AIMaster_RAG_LastError() << "\n";
            out["ok"] = false; out["error"] = AIMaster_RAG_LastError();
            return true;
        }
        std::cout << ans << "\n";
        out["ok"] = true; out["answer"] = ans;
        out["k"] = k;
        out["threshold"] = threshold;
        return true;
    }

    // RAG_SESSION <SET|SHOW|CLEAR> [sid]
    if (cmd == "RAG_SESSION") {
        const std::string action = tokens.size() >= 2 ? __rag_upper(tokens[1]) : "";
        if (action == "SET") {
            if (tokens.size()<3) {
                std::cout << "Usage: RAG_SESSION SET <sid>\n";
                out["ok"] = false; out["error"] = "usage";
                return true;
            }
            rag_state::SetActiveSession(tokens[2]);
            std::cout << "RAG session set.\n";
            out["ok"] = true; out["session_id"] = tokens[2];
        } else if (action == "SHOW") {
            auto sid = rag_state::GetActiveSession();
            std::cout << (sid.empty() ? "<none>" : sid) << "\n";
            out["ok"] = true; out["session_id"] = sid;
        } else if (action == "CLEAR") {
            rag_state::SetActiveSession("");
            std::cout << "RAG session cleared.\n";
            out["ok"] = true;
        } else {
            std::cout << "Usage: RAG_SESSION <SET|SHOW|CLEAR> [sid]\n";
            out["ok"] = false; out["error"] = "usage";
        }
        return true;
    }

    return false; // not handled
}
