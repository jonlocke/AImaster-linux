#pragma once
#include <string>

std::string AIMaster_RAG_AddFolder(const std::string& folder_path);
std::string AIMaster_RAG_Ask(const std::string& session_id,
                             const std::string& question,
                             int k = 5,
                             double score_threshold = 0.2);
void AIMaster_RAG_SetVerbose(bool v);
const std::string& AIMaster_RAG_LastError();
