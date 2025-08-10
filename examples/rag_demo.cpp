#include "rag_adapter.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Demo usage:\n"
                  << "  rag_demo ingest <folder>\n"
                  << "  rag_demo ask <session_id> <question>\n"
                  << "  rag_demo verbose <0|1>\n";
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "ingest") {
        if (argc < 3) { std::cerr << "Provide folder path\n"; return 1; }
        std::string sid = AIMaster_RAG_AddFolder(argv[2]);
        if (sid.empty()) {
            std::cerr << "Error: " << AIMaster_RAG_LastError() << "\n";
            return 2;
        }
        std::cout << "Session ID: " << sid << "\n";
    } else if (cmd == "ask") {
        if (argc < 4) { std::cerr << "Provide session_id and question\n"; return 1; }
        std::string q;
        for (int i=3;i<argc;++i) { if (i>3) q += " "; q += argv[i]; }
        std::string ans = AIMaster_RAG_Ask(argv[2], q);
        if (ans.empty()) {
            std::cerr << "Error: " << AIMaster_RAG_LastError() << "\n";
            return 2;
        }
        std::cout << ans << "\n";
    } else if (cmd == "verbose") {
        if (argc < 3) { std::cerr << "Provide 0 or 1\n"; return 1; }
        AIMaster_RAG_SetVerbose(std::string(argv[2])=="1");
        std::cout << "Verbose set\n";
    } else {
        std::cerr << "Unknown command\n"; return 1;
    }
    return 0;
}
