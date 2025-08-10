# AImaster RAG Integration (Poppler + Tesseract OCR fallback)

This build keeps **native Poppler** text extraction and adds **automatic OCR** using **Tesseract** when little/no text is found.

## Dependencies
```bash
sudo apt update
sudo apt install libpoppler-cpp-dev libtesseract-dev libleptonica-dev tesseract-ocr
sudo apt install libcurl4-openssl-dev  # or your curl dev package
```
Also install/pull Ollama models as before.

## Build (demo)
```bash
g++ -std=c++17 -Iinclude -Isrc \
    src/rag_session.cpp src/rag_adapter.cpp examples/rag_demo.cpp \
    -lcurl $(pkg-config --cflags --libs poppler-cpp) \
    -ltesseract -o rag_demo
```

## Behavior
- Poppler extracts text page-by-page.
- If total extracted text `< 40` chars for a PDF, the module logs a warning and **falls back to OCR**:
  - Renders pages with Poppler `page_renderer` at 200 DPI.
  - Converts to grayscale buffer.
  - Sends to Tesseract C++ API (`SetImage` + `GetUTF8Text`).
- Status messages show OCR kicks-in and timings.

## Integrate into AImaster
- Same adapter methods:
  - `AIMaster_RAG_AddFolder(<folder>) -> session_id`
  - `AIMaster_RAG_Ask(<sid>, <question>, [k], [threshold]) -> answer`
  - `AIMaster_RAG_SetVerbose(0|1)`

## Windows/macOS
- Windows: use prebuilt Poppler and Tesseract binaries (add include/lib paths and DLLs on PATH).
- macOS: `brew install poppler tesseract`

