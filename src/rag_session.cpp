#include "rag_session.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <iostream>

#include <curl/curl.h>

#include <poppler-document.h>
#include <poppler-page.h>
#include <poppler-page-renderer.h>

#include <tesseract/baseapi.h>

namespace fs = std::filesystem;

// -------------------------- Utility: grayscale conversion --------------------------
static void img_to_grayscale(const poppler::image& img, std::vector<unsigned char>& gray) {
    const int w = img.width();
    const int h = img.height();
    gray.resize(static_cast<size_t>(w) * h);

    const unsigned char* src = reinterpret_cast<const unsigned char*>(img.const_data());
    const int stride = img.bytes_per_row();

    switch (img.format()) {
        case poppler::image::format_argb32: {
            for (int y=0; y<h; ++y) {
                const unsigned char* row = src + y * stride;
                for (int x=0; x<w; ++x) {
                    const unsigned char* p = row + x*4; // BGRA
                    unsigned char b = p[0], g = p[1], r = p[2];
                    gray[static_cast<size_t>(y)*w + x] =
                        static_cast<unsigned char>(0.299*r + 0.587*g + 0.114*b);
                }
            }
            break;
        }
        case poppler::image::format_rgb24: {
            for (int y=0; y<h; ++y) {
                const unsigned char* row = src + y * stride;
                for (int x=0; x<w; ++x) {
                    const unsigned char* p = row + x*3; // BGR
                    unsigned char b = p[0], g = p[1], r = p[2];
                    gray[static_cast<size_t>(y)*w + x] =
                        static_cast<unsigned char>(0.299*r + 0.587*g + 0.114*b);
                }
            }
            break;
        }
        case poppler::image::format_mono:
        case poppler::image::format_gray8: {
            for (int y=0; y<h; ++y) {
                const unsigned char* row = src + y * stride;
                std::copy(row, row + w, gray.begin() + static_cast<size_t>(y)*w);
            }
            break;
        }
        default: {
            for (int y=0; y<h; ++y) {
                const unsigned char* row = src + y * stride;
                for (int x=0; x<w; ++x) gray[static_cast<size_t>(y)*w + x] = row[x];
            }
            break;
        }
    }
}

// -------------------------- Class impl --------------------------

RAGSessionManager::RAGSessionManager(std::string base_dir,
                                     std::string ollama_url,
                                     std::string embed_model,
                                     std::string llm_model)
: base_dir_(std::move(base_dir)),
  ollama_url_(std::move(ollama_url)),
  embed_model_(std::move(embed_model)),
  llm_model_(std::move(llm_model)) {
    fs::create_directories(base_dir_);
}

void RAGSessionManager::log(const std::string& msg) const {
    if (!verbose_) return;
    if (logger_) logger_(msg);
    else std::cerr << "[RAG] " << msg << std::endl;
}

std::string RAGSessionManager::uuid4() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    auto to_hex = [](uint64_t x){
        std::ostringstream oss; oss<<std::hex;
        oss<<((x>>56)&0xFFu)<<((x>>48)&0xFFu)<<((x>>40)&0xFFu)<<((x>>32)&0xFFu)
           <<((x>>24)&0xFFu)<<((x>>16)&0xFFu)<<((x>>8)&0xFFu)<<(x&0xFFu);
        return oss.str();
    };
    uint64_t a=dis(gen), b=dis(gen);
    std::string s = to_hex(a)+to_hex(b);
    if (s.size()<32) s.append(32-s.size(),'0');
    return s.substr(0,32);
}

std::string RAGSessionManager::sanitizePath(const std::string& s) {
    std::string out = s;
    for(char& c: out){ if(c=='\\' || c=='/') c='_'; }
    return out;
}

std::vector<std::string> RAGSessionManager::findPDFs(const std::string& folder_path) {
    std::vector<std::string> pdfs;
    for (auto& p : fs::recursive_directory_iterator(folder_path)) {
        if (!p.is_regular_file()) continue;
        auto ext = p.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext==".pdf") pdfs.push_back(p.path().string());
    }
    return pdfs;
}

std::string RAGSessionManager::extract_text_poppler(const std::string& pdf_path) {
    std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(pdf_path));
    if (!doc) throw std::runtime_error("Poppler failed to open PDF: " + pdf_path);
    std::string text;
    for (int i = 0; i < doc->pages(); ++i) {
        std::unique_ptr<poppler::page> page(doc->create_page(i));
        if (!page) continue;
        auto ba = page->text().to_utf8();
        text.append(ba.begin(), ba.end());
        text += "\n";
    }
    return text;
}

std::string RAGSessionManager::ocr_pdf_with_poppler_tesseract(const std::string& pdf_path, int dpi) {
    std::unique_ptr<poppler::document> doc(poppler::document::load_from_file(pdf_path));
    if (!doc) throw std::runtime_error("Poppler failed to open for OCR: " + pdf_path);

    tesseract::TessBaseAPI api;
    if (api.Init(nullptr, "eng")) {
        throw std::runtime_error("Tesseract Init failed (lang=eng). Ensure tesseract and traineddata are installed.");
    }
    api.SetPageSegMode(tesseract::PSM_AUTO);

    poppler::page_renderer renderer;
    renderer.set_render_hint(poppler::page_renderer::antialiasing, true);
    renderer.set_render_hint(poppler::page_renderer::text_antialiasing, true);

    std::string ocr_text;
    for (int i=0; i<doc->pages(); ++i) {
        std::unique_ptr<poppler::page> page(doc->create_page(i));
        if (!page) continue;
        auto img = renderer.render_page(page.get(), dpi, dpi);
        if (!img.is_valid()) continue;

        std::vector<unsigned char> gray;
        img_to_grayscale(img, gray);
        api.SetImage(gray.data(), img.width(), img.height(), 1, img.width());
        char* out = api.GetUTF8Text();
        if (out) { ocr_text += out; delete [] out; }
        ocr_text += "\n";
    }
    api.End();
    return ocr_text;
}

std::vector<std::string> RAGSessionManager::split_chunks(const std::string& text, size_t chunk_size, size_t overlap) {
    std::vector<std::string> chunks;
    if (text.empty()) return chunks;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = std::min(start + chunk_size, text.size());
        chunks.emplace_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end - std::min(overlap, end);
    }
    return chunks;
}

static size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size*nmemb);
    return size*nmemb;
}

std::vector<float> RAGSessionManager::embed(const std::string& text) {
    CURL* curl = curl_easy_init();
    if(!curl) throw std::runtime_error("curl_easy_init failed");

    std::string url = ollama_url_ + "/api/embeddings";
    json payload = { {"model", embed_model_}, {"prompt", text} };
    std::string payload_str = payload.dump();

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK) throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));

    auto j = json::parse(response);
    if (!j.contains("embedding")) throw std::runtime_error("Ollama embeddings response missing 'embedding'");
    std::vector<float> vec = j["embedding"].get<std::vector<float>>();
    return vec;
}

std::string RAGSessionManager::ollama_chat(const std::string& prompt) {
    CURL* curl = curl_easy_init();
    if(!curl) throw std::runtime_error("curl_easy_init failed");

    std::string url = ollama_url_ + "/api/chat";
    json payload = {
        {"model", llm_model_},
        {"messages", json::array({
            json{{"role","system"},{"content","You are a helpful assistant. Answer ONLY with the final answer. Do NOT include chain-of-thought, analysis, or <think> tags."}},
            json{{"role","user"},{"content", prompt}}
        })},
        {"stream", false}
    };
    std::string payload_str = payload.dump();

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK) throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));

    auto j = json::parse(response);
    if (!j.contains("message") || !j["message"].contains("content")) {
        throw std::runtime_error("Ollama chat response missing 'message.content'");
    }
    std::string out = j["message"]["content"].get<std::string>();
    size_t start = out.find("<think>");
    size_t end = out.find("</think>");
    if (start != std::string::npos && end != std::string::npos && end > start) {
        out.erase(start, (end + 8) - start);
    }
    while ((start = out.find("<think>")) != std::string::npos) out.erase(start, 7);
    while ((start = out.find("</think>")) != std::string::npos) out.erase(start, 8);
    while (!out.empty() && (out.back()=='\n' || out.back()=='\r' || out.back()==' ' || out.back()=='\t')) out.pop_back();
    size_t i=0; while (i<out.size() && (out[i]=='\n' || out[i]=='\r' || out[i]==' ' || out[i]=='\t')) ++i; out = out.substr(i);
    return out;
}

std::string RAGSessionManager::sessionDir(const std::string& session_id) const {
    return (fs::path(base_dir_) / session_id).string();
}

void RAGSessionManager::save_index(const SessionIndex& idx) const {
    fs::create_directories(sessionDir(idx.session_id));
    std::ofstream ofs(fs::path(sessionDir(idx.session_id)) / "index.json");
    json j;
    j["session_id"] = idx.session_id;
    j["chunks"] = json::array();
    for (const auto& c : idx.chunks) {
        j["chunks"].push_back({ {"id", c.id}, {"text", c.text}, {"embedding", c.embedding} });
    }
    ofs << j.dump(2);
}

std::optional<SessionIndex> RAGSessionManager::load_index(const std::string& session_id) const {
    auto path = fs::path(sessionDir(session_id)) / "index.json";
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream ifs(path);
    json j; ifs >> j;
    SessionIndex idx;
    idx.session_id = j.value("session_id", session_id);
    for (auto& cj : j["chunks"]) {
        Chunk c;
        c.id = cj.value("id","");
        c.text = cj.value("text","");
        c.embedding = cj.value("embedding", std::vector<float>{});
        idx.chunks.push_back(std::move(c));
    }
    return idx;
}

double RAGSessionManager::cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size()!=b.size()) return -1.0;
    double dot=0.0, na=0.0, nb=0.0;
    for (size_t i=0;i<a.size();++i) { dot += a[i]*b[i]; na += a[i]*a[i]; nb += b[i]*b[i]; }
    if (na==0 || nb==0) return -1.0;
    return dot / (std::sqrt(na)*std::sqrt(nb));
}

std::vector<size_t> RAGSessionManager::topk(const std::vector<double>& scores, int k) {
    std::vector<size_t> idx(scores.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::partial_sort(idx.begin(), idx.begin()+std::min(k,(int)idx.size()), idx.end(),
        [&](size_t i, size_t j){ return scores[i] > scores[j]; });
    if ((int)idx.size() > k) idx.resize(k);
    return idx;
}

std::string RAGSessionManager::build_prompt(const std::string& context, const std::string& question) {
    std::ostringstream oss;
    oss << "Answer the question based only on the context.\n\nContext:\n"
        << context << "\n\nQuestion:\n" << question
        << "\n\nAnswer concisely and accurately in three sentences or less.";
    return oss.str();
}

std::string RAGSessionManager::createSessionFromFolder(const std::string& folder_path) {
    if (!fs::exists(folder_path) || !fs::is_directory(folder_path)) {
        throw std::runtime_error("Folder does not exist: " + folder_path);
    }
    auto t0 = std::chrono::steady_clock::now();
    log(std::string("Scanning PDFs in: ") + folder_path);
    std::vector<std::string> pdfs = findPDFs(folder_path);
    if (pdfs.empty()) throw std::runtime_error("No PDFs found in: " + folder_path);
    log("Found " + std::to_string(pdfs.size()) + " PDF(s).");

    SessionIndex idx;
    idx.session_id = uuid4();

    size_t total_chunks = 0;
    size_t file_num = 0;
    for (const auto& pdf : pdfs) {
        ++file_num;
        log("[" + std::to_string(file_num) + "/" + std::to_string(pdfs.size()) + "] Extracting text: " + pdf);
        auto f_t0 = std::chrono::steady_clock::now();
        std::string text = extract_text_poppler(pdf);
        auto f_t1 = std::chrono::steady_clock::now();
        log("  Text extracted in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(f_t1 - f_t0).count()) + " ms.");
        if (text.size() < 40) {
            log("  WARNING: Very little/no text extracted. Falling back to OCR via Poppler+Tesseract...");
            auto o_t0 = std::chrono::steady_clock::now();
            std::string ocr = ocr_pdf_with_poppler_tesseract(pdf, 200);
            auto o_t1 = std::chrono::steady_clock::now();
            log("  OCR completed in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(o_t1 - o_t0).count()) + " ms.");
            if (!ocr.empty()) text = std::move(ocr);
        }

        auto chunks = split_chunks(text, 1024, 100);
        log("  Chunking: " + std::to_string(chunks.size()) + " chunks.");
        total_chunks += chunks.size();

        size_t cnum = 0;
        for (size_t i=0; i<chunks.size(); ++i) {
            ++cnum;
            if (cnum % 25 == 1 || cnum == chunks.size()) {
                log("    Embedding chunk " + std::to_string(cnum) + "/" + std::to_string(chunks.size()));
            }
            Chunk c;
            c.id = sanitizePath(pdf) + "#" + std::to_string(i);
            c.text = std::move(chunks[i]);
            c.embedding = embed(c.text);
            idx.chunks.push_back(std::move(c));
        }
    }

    save_index(idx);
    auto t1 = std::chrono::steady_clock::now();
    log("Ingestion complete. Files: " + std::to_string(pdfs.size()) + ", Chunks: " + std::to_string(total_chunks) +
        ", Elapsed: " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(t1 - t0).count()) + " s.");
    log("Session ID: " + idx.session_id);
    return idx.session_id;
}

std::string RAGSessionManager::chat(const std::string& session_id,
                                    const std::string& message,
                                    int k,
                                    double score_threshold) {
    auto opt = load_index(session_id);
    if (!opt) throw std::runtime_error("Invalid or unknown session_id");

    log("Retrieving context for query (k=" + std::to_string(k) + ", threshold=" + std::to_string(score_threshold) + ")");
    auto& idx = *opt;
    auto q_t0 = std::chrono::steady_clock::now();
    auto qvec = embed(message);
    auto q_t1 = std::chrono::steady_clock::now();
    log("  Query embedded in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(q_t1 - q_t0).count()) + " ms.");
    std::vector<double> scores(idx.chunks.size());
    for (size_t i=0;i<idx.chunks.size();++i) scores[i] = cosine_similarity(qvec, idx.chunks[i].embedding);
    std::vector<size_t> order(idx.chunks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return scores[a] > scores[b]; });

    std::string context;
    int added = 0;
    for (size_t i : order) {
        if (scores[i] < score_threshold) break;
        context += idx.chunks[i].text + "\n\n";
        if (++added >= k) break;
    }
    if (context.empty()) {
        log("  No relevant context found.");
        return "No relevant context found in the document to answer your question.";
    }

    auto p = build_prompt(context, message);
    log("  Sending prompt to LLM (" + std::to_string(p.size()) + " chars).");
    auto a_t0 = std::chrono::steady_clock::now();
    auto ans = ollama_chat(p);
    auto a_t1 = std::chrono::steady_clock::now();
    log("Answer generated in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(a_t1 - a_t0).count()) + " ms.");
    return ans;
}
