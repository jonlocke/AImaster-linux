# Console-mode RAG Update (Slim Patch)

This patch enables RAG commands in **console mode** using a header-only handler.
It also includes the minimal RAG engine, shared session state, and adapter API.

## Files you should copy into your tree
- `src/rag_state.hpp` / `src/rag_state.cpp`
- `src/rag_session.hpp` / `src/rag_adapter.hpp` / `src/rag_adapter.cpp`
- `src/rag_console_commands.hpp` (header-only; no .cpp needed)

## One-line integration in console path
In `src/ollama_client.cpp`, at the **top** of
`Json::Value processCommand(<your-arg-name>, AppConfig& cfg)` add:

```cpp
#include "rag_console_commands.hpp"  // at the top of file

Json::Value ragOut;
if (HandleRAGConsoleCommand(<your-arg-name>, ragOut)) {
    return ragOut;   // handled RAG_INGEST / RAG_ASK / RAG_SESSION
}
```

> **Note**: Replace `<your-arg-name>` with the real parameter name of that function
(e.g. `line`, `cmd`, `input`, etc.).

## Makefile
Ensure your Makefile compiles the new sources (no extra step for the header-only handler):

```
# add to your aimaster / rag_demo source lists
src/rag_state.cpp
src/rag_adapter.cpp
# (rag_session.cpp is compiled elsewhere if you included it too)
```

You’ll also need link libs (already in your Makefile from previous steps):
`-ljsoncpp -lcurl -lpoppler-cpp -ltesseract`.

## Test
```
RAG_INGEST /absolute/path/to/pdfs
RAG_SESSION SHOW
RAG_ASK What are these docs about?
```
