# Wire INT mode to RAG session

1) Include in your INT loop file (e.g. `src/main.cpp`):
```cpp
#include "rag_int_bridge.hpp"
```

2) Before routing the user input to the normal LLM path, insert:
```cpp
std::string rag_answer;
if (rag_int::TryRAGAnswer(user_line, rag_answer, /*k=*/5, /*threshold=*/0.2)) {
    std::cout << rag_answer << std::endl;
    continue; // handled via RAG
}
```

3) Console mode toggle:
```
RAG_INT ON
RAG_INT OFF
RAG_INT STATUS
```

4) Makefile: add `src/rag_int_bridge.cpp` to both targets.
