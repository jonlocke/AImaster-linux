# RAG Console Handler Patch

This patch adds a small helper that processes `RAG_*` commands in **console mode** without
having to rewrite your existing command parser.

## Files
- `src/rag_console_commands.hpp`
- `src/rag_console_commands.cpp`

## Integrate in `src/ollama_client.cpp`

1) Include the header near the top:
```cpp
#include "rag_console_commands.hpp"
```

2) In your function that handles a single console line (e.g.)
```cpp
Json::Value processCommand(const std::string& line, AppConfig& cfg)
```
add the following **at the very top** of the function body:
```cpp
Json::Value ragOut;
if (HandleRAGConsoleCommand(line, ragOut)) {
    // Handled RAG_INGEST / RAG_ASK / RAG_SESSION
    return ragOut;
}
```

This early-return pattern ensures RAG commands are printed and a JSON reply is returned,
while leaving your existing commands untouched.

3) Rebuild:
```bash
make clean && make
```

You already have:
- `rag_session`, `rag_adapter`, and `rag_state` in `src/`
- Makefile linking Poppler, Tesseract, jsoncpp, etc.
