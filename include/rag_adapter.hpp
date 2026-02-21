#pragma once
#include <string>
std::string AIMaster_RAG_AddFolder(const std::string& folder_path);
std::string AIMaster_RAG_Ask(const std::string& session_id, const std::string& question, int k=5, double score_threshold=0.2);
const std::string& AIMaster_RAG_LastError();
void AIMaster_RAG_SetVerbose(bool v);
void AIMaster_RAG_ConfigureRemote(const std::string& ollama_url, const std::string& llm_model);

std::string AIMaster_RAG_Summary(const std::string& session_id, int max_files=10);
