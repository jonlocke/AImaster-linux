# Full rebuild (Poppler + Tesseract)

This is a clean, compiling copy of the RAG C++ module with Poppler text extraction and Tesseract OCR fallback.

## Build
```bash
sudo apt install libpoppler-cpp-dev libtesseract-dev libleptonica-dev tesseract-ocr libcurl4-openssl-dev
g++ -std=c++17 -Iinclude -Isrc \
    src/rag_session.cpp src/rag_adapter.cpp examples/rag_demo.cpp \
    -lcurl $(pkg-config --cflags --libs poppler-cpp) \
    -ltesseract -o rag_demo
```
