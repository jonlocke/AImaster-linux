#pragma once
#include <string>
#include <vector>
#include <optional>
#include "json.hpp"

struct Chunk{ std::string id; std::string text; std::vector<float> embedding; };
struct SessionIndex{ std::string session_id; std::vector<Chunk> chunks; };

class RAGSessionManager{
public:
  explicit RAGSessionManager(std::string base_dir="chroma_cpp", std::string ollama_url="http://localhost:11434",
                             std::string embed_model="mxbai-embed-large", std::string llm_model="deepseek-r1:latest");
  void setVerbose(bool v){ verbose_=v; }
  std::string createSessionFromFolder(const std::string& folder_path);
  std::string chat(const std::string& session_id,const std::string& message,int k=5,double score_threshold=0.2);

  // Public methods needed by adapter for code ingestion
  std::vector<float> embed(const std::string& text);
  std::string sessionDir(const std::string& sid) const;
  void save_index(const SessionIndex& idx) const;
  std::optional<SessionIndex> load_index(const std::string& sid) const;

private:
  std::string base_dir_, ollama_url_, embed_model_, llm_model_;
  bool verbose_=true;
  void log(const std::string& msg) const;
  static std::string uuid4();
  static std::vector<std::string> findPDFs(const std::string& folder);
  static std::string extract_text_poppler(const std::string& pdf_path);
  static std::string ocr_pdf_with_poppler_tesseract(const std::string& pdf_path, int dpi=200);
  static std::vector<std::string> split_chunks(const std::string& text, size_t chunk=1024,size_t overlap=100);
  std::string ollama_chat(const std::string& prompt);
  static double cosine(const std::vector<float>& a,const std::vector<float>& b);
  static std::string build_prompt(const std::string& ctx,const std::string& q);
};
