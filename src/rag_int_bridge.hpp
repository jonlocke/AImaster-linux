#pragma once
#include <string>

namespace rag_int {
bool Enabled();
void SetEnabled(bool on);
bool TryRAGAnswer(const std::string& user_input, std::string& out_answer, int k=5, double threshold=0.2);
} // namespace rag_int
