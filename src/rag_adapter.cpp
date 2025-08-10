
#include "rag_session.hpp"
#include "rag_adapter.hpp"
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

static std::mutex g_mtx;
static std::string g_last_error;
static RAGSessionManager g_mgr;

const std::string& AIMaster_RAG_LastError(){ return g_last_error; }
void AIMaster_RAG_SetVerbose(bool v){ std::lock_guard<std::mutex> L(g_mtx); g_mgr.setVerbose(v); }

// -------- Minimal, safe code ingestion appended after PDF session creation --------

static bool is_text_ext(const std::string& ext){
    static const char* exts[] = {
        ".c",".h",".cpp",".hpp",".cc",".hh",
        ".py",".js",".ts",".tsx",".jsx",
        ".java",".cs",".go",".rs",".rb",".php",".swift",
        ".kt",".m",".mm",".sh",".ps1",".bat",".cmd",
        ".cmake",".make",".mk",".json",".toml",
        ".yaml",".yml",".ini",".cfg",".conf",
        ".md",".rst",".tex"
    };
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    for (auto* x : exts) if (e == x) return true;
    return false;
}

static bool should_skip_path(const std::string& s){
    return s.find("/.git/")!=std::string::npos
        || s.find("/node_modules/")!=std::string::npos
        || s.find("/dist/")!=std::string::npos
        || s.find("/build/")!=std::string::npos
        || s.find("/.venv/")!=std::string::npos;
}

static std::string read_text_file(const fs::path& p, size_t max_bytes=512*1024){
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    // binary sniff
    std::string head(8192, '\0');
    ifs.read(head.data(), head.size());
    head.resize((size_t)ifs.gcount());
    if (head.find('\0') != std::string::npos) return {}; // looks binary
    // read up to max
    ifs.clear(); ifs.seekg(0);
    std::string data; data.reserve(std::min(max_bytes, (size_t)fs::file_size(p)));
    char buf[8192]; size_t total=0;
    while (ifs){
        ifs.read(buf, sizeof(buf));
        std::streamsize n = ifs.gcount();
        if (n<=0) break;
        size_t add = (total + (size_t)n > max_bytes) ? (max_bytes - total) : (size_t)n;
        data.append(buf, buf+add);
        total += add;
        if (total >= max_bytes) break;
    }
    return data;
}

static std::vector<std::string> code_chunks(const std::string& text, size_t max_chars=1200, size_t overlap=120){
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = std::min(text.size(), i + max_chars);
        size_t j = end;
        while (j > i + 200 && j < text.size() && text[j] != '\n') --j;
        if (j <= i + 200) j = end;
        out.emplace_back(text.substr(i, j - i));
        if (j >= text.size()) break;
        i = j > overlap ? j - overlap : j;
    }
    return out;
}

static void append_code_to_session(const std::string& folder, const std::string& sid){
    auto opt = g_mgr.load_index(sid);
    if (!opt) return;
    auto idx = *opt;

    size_t added_chunks = 0;
    for (auto it = fs::recursive_directory_iterator(folder); it != fs::recursive_directory_iterator(); ++it){
        if (!it->is_regular_file()) continue;
        auto p = it->path();
        std::string pstr = p.string();
        if (should_skip_path(pstr)) continue;
        if (!is_text_ext(p.extension().string())) continue;

        auto text = read_text_file(p);
        if (text.empty()) continue;

        auto chunks = code_chunks(text);
        for (size_t i = 0; i < chunks.size(); ++i){
            Chunk c;
            c.id = pstr + "#" + std::to_string(i);
            // include a short header so answers can surface file context
            c.text = "FILE: " + pstr + "\n" + chunks[i];
            c.embedding = g_mgr.embed(c.text);
            if (!c.embedding.empty()){
                idx.chunks.push_back(std::move(c));
                ++added_chunks;
            }
        }
    }

    if (added_chunks > 0) {
        g_mgr.save_index(idx);
        g_mgr.setVerbose(true);
        g_mgr.setVerbose(false);
        std::cerr << "[RAG] Appended " << added_chunks << " code chunk(s) to session.\n";
    }
}

// Public API

std::string AIMaster_RAG_AddFolder(const std::string& folder){
    try{
        std::lock_guard<std::mutex> L(g_mtx);
        g_last_error.clear();
        // Step 1: create session from PDFs (existing behavior)
        auto sid = g_mgr.createSessionFromFolder(folder);
        // Step 2: append code files automatically
        append_code_to_session(folder, sid);
        return sid;
    }catch(const std::exception& e){
        g_last_error = e.what();
        return {};
    }
}

std::string AIMaster_RAG_Ask(const std::string& sid, const std::string& question, int k, double score_threshold){
    try{
        std::lock_guard<std::mutex> L(g_mtx);
        g_last_error.clear();
        return g_mgr.chat(sid, question, k, score_threshold);
    }catch(const std::exception& e){
        g_last_error = e.what();
        return {};
    }
}


#include <map>

static std::string file_ext(const std::string& path){
    auto dot = path.find_last_of('.');
    if (dot==std::string::npos) return "";
    std::string e = path.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

std::string AIMaster_RAG_Summary(const std::string& session_id, int max_files){
    try{
        std::lock_guard<std::mutex> L(g_mtx);
        g_last_error.clear();
        if (session_id.empty()) return "No active RAG session.";
        auto opt = g_mgr.load_index(session_id);
        if (!opt) return "No index found for session: " + session_id;
        auto idx = *opt;

        size_t chunk_count = idx.chunks.size();
        std::map<std::string,int> by_ext;
        std::map<std::string,int> by_file;
        for (const auto& c : idx.chunks){
            auto hash = c.id.find('#');
            std::string path = hash==std::string::npos ? c.id : c.id.substr(0, hash);
            by_file[path]++;
            by_ext[file_ext(path)]++;
        }

        // Build a human-readable summary
        std::ostringstream o;
        o << "Session: " << session_id << "\n";
        o << "Chunks: " << chunk_count << "\n";
        // Ext summary (pdf vs code)
        int pdfs=0, codes=0;
        for (auto& kv : by_ext){
            if (kv.first == ".pdf") pdfs += kv.second;
            else codes += kv.second;
        }
        o << "Files (by extension entries): pdf=" << pdfs << ", code=" << codes << "\n";

        // List top files
        std::vector<std::pair<int,std::string>> top;
        top.reserve(by_file.size());
        for (auto& kv : by_file) top.emplace_back(kv.second, kv.first);
        std::sort(top.begin(), top.end(), [](auto&a, auto&b){ return a.first>b.first; });
        int limit = std::max(1, max_files);
        o << "Top files:\n";
        for (int i=0; i<(int)top.size() && i<limit; ++i){
            o << "  " << top[i].second << " (" << top[i].first << " chunks)\n";
        }
        return o.str();
    }catch(const std::exception& e){
        g_last_error = e.what();
        return "Error: " + g_last_error;
    }
}
