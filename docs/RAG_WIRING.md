# Wiring RAG_INGEST / RAG_ASK to share session with INT

1) Include headers at the top of your `src/main.cpp` (or wherever your command loop is):
```cpp
#include "rag_adapter.hpp"
#include "rag_state.hpp"
```

2) In your command dispatcher, add:
```cpp
else if (cmd == "RAG_INGEST") {
    if (args.size() < 1) { std::cout<<"Usage: RAG_INGEST <folder>\n"; }
    else {
        auto sid = AIMaster_RAG_AddFolder(args[0]);
        if (sid.empty()) std::cout<<"RAG ingest failed: "<<AIMaster_RAG_LastError()<<"\n";
        else { rag_state::SetActiveSession(sid); std::cout<<"RAG session set: "<<sid<<"\n"; }
    }
}
else if (cmd == "RAG_ASK") {
    if (!rag_state::HasActiveSession()) { std::cout<<"No active RAG session. Run RAG_INGEST <folder> or RAG_SESSION SET <sid>.\n"; }
    else {
        std::string q = join_args(args); // combine tokens to a string
        auto ans = AIMaster_RAG_Ask(rag_state::GetActiveSession(), q, 5, 0.2);
        if (ans.empty()) std::cout<<"RAG ask failed: "<<AIMaster_RAG_LastError()<<"\n";
        else std::cout<<ans<<"\n";
    }
}
else if (cmd == "RAG_SESSION") {
    if (args.size()>=1 && args[0]=="SET") {
        if (args.size()<2) std::cout<<"Usage: RAG_SESSION SET <sid>\n";
        else { rag_state::SetActiveSession(args[1]); std::cout<<"RAG session set.\n"; }
    } else if (args.size()>=1 && args[0]=="SHOW") {
        auto sid = rag_state::GetActiveSession();
        std::cout<<(sid.empty()?"<none>":sid)<<"\n";
    } else if (args.size()>=1 && args[0]=="CLEAR") {
        rag_state::SetActiveSession(""); std::cout<<"RAG session cleared.\n";
    } else {
        std::cout<<"Usage: RAG_SESSION <SET|SHOW|CLEAR> [sid]\n";
    }
}
```

3) If your INT mode keeps a session id, sync it:
```cpp
rag_state::SetActiveSession(current_session_id);
```

4) Rebuild:
```
make clean && make
```
