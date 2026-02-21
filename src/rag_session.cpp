#include "rag_session.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <regex>
#include <sstream>
#include <unordered_set>

#include <curl/curl.h>
#include <poppler-document.h>
#include <poppler-page.h>
#include <poppler-page-renderer.h>
#include <tesseract/baseapi.h>

#include "endpoint_utils.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

RAGSessionManager::RAGSessionManager(std::string b, std::string u, std::string e, std::string l)
    : base_dir_(std::move(b)), ollama_url_(std::move(u)), embed_model_(std::move(e)), llm_model_(std::move(l)) {
    fs::create_directories(base_dir_);
}

void RAGSessionManager::log(const std::string& s) const {
    if (verbose_) std::cerr << "[RAG] " << s << std::endl;
}

std::string RAGSessionManager::uuid4() {
    static std::mt19937_64 g{std::random_device{}()};
    auto r = []() { return (uint64_t)g(); };
    std::ostringstream o;
    o << std::hex << r() << r();
    auto s = o.str();
    if (s.size() < 32) s.append(32 - s.size(), '0');
    return s.substr(0, 32);
}

std::vector<std::string> RAGSessionManager::findPDFs(const std::string& f) {
    std::vector<std::string> v;
    for (auto& p : fs::recursive_directory_iterator(f)) {
        if (p.is_regular_file() && p.path().extension() == ".pdf") v.push_back(p.path().string());
    }
    return v;
}

std::string RAGSessionManager::extract_text_poppler(const std::string& p) {
    std::unique_ptr<poppler::document> d(poppler::document::load_from_file(p));
    if (!d) return {};
    std::string t;
    for (int i = 0; i < d->pages(); ++i) {
        std::unique_ptr<poppler::page> pg(d->create_page(i));
        if (!pg) continue;
        auto ba = pg->text().to_utf8();
        t.append(ba.begin(), ba.end());
        t += '\n';
    }
    return t;
}

static void img_to_gray(const poppler::image& img, std::vector<unsigned char>& gray) {
    int w = img.width(), h = img.height();
    gray.resize((size_t)w * h);
    auto* src = (const unsigned char*)img.const_data();
    int stride = img.bytes_per_row();

    if (img.format() == poppler::image::format_argb32) {
        for (int y = 0; y < h; ++y) {
            auto* row = src + y * stride;
            for (int x = 0; x < w; ++x) {
                auto* p = row + x * 4;
                unsigned char b = p[0], g = p[1], r = p[2];
                gray[(size_t)y * w + x] = (unsigned char)(0.299 * r + 0.587 * g + 0.114 * b);
            }
        }
    } else if (img.format() == poppler::image::format_rgb24) {
        for (int y = 0; y < h; ++y) {
            auto* row = src + y * stride;
            for (int x = 0; x < w; ++x) {
                auto* p = row + x * 3;
                unsigned char b = p[0], g = p[1], r = p[2];
                gray[(size_t)y * w + x] = (unsigned char)(0.299 * r + 0.587 * g + 0.114 * b);
            }
        }
    } else {
        for (int y = 0; y < h; ++y) {
            auto* row = src + y * stride;
            std::copy(row, row + w, gray.begin() + (size_t)y * w);
        }
    }
}

std::string RAGSessionManager::ocr_pdf_with_poppler_tesseract(const std::string& p, int dpi) {
    std::unique_ptr<poppler::document> d(poppler::document::load_from_file(p));
    if (!d) return {};

    tesseract::TessBaseAPI api;
    if (api.Init(nullptr, "eng")) return {};
    api.SetPageSegMode(tesseract::PSM_AUTO);

    poppler::page_renderer r;
    r.set_render_hint(poppler::page_renderer::antialiasing, true);
    r.set_render_hint(poppler::page_renderer::text_antialiasing, true);

    std::string out;
    for (int i = 0; i < d->pages(); ++i) {
        std::unique_ptr<poppler::page> pg(d->create_page(i));
        if (!pg) continue;

        auto img = r.render_page(pg.get(), dpi, dpi);
        if (!img.is_valid()) continue;

        std::vector<unsigned char> g;
        img_to_gray(img, g);
        api.SetImage(g.data(), img.width(), img.height(), 1, img.width());
        char* txt = api.GetUTF8Text();
        if (txt) {
            out += txt;
            delete[] txt;
        }
        out += '\n';
    }
    api.End();
    return out;
}

std::vector<std::string> RAGSessionManager::split_chunks(const std::string& s, size_t n, size_t o) {
    std::vector<std::string> c;
    if (s.empty()) return c;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = std::min(i + n, s.size());
        c.emplace_back(s.substr(i, e - i));
        if (e == s.size()) break;
        i = e - std::min(o, e);
    }
    return c;
}

static size_t wr(void* ptr, size_t sz, size_t nm, void* ud) {
    ((std::string*)ud)->append((char*)ptr, sz * nm);
    return sz * nm;
}

static std::optional<int> requested_page_number(const std::string& q) {
    std::regex re(R"((?:^|\b)page\s+(\d{1,4})(?:\b|$))", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(q, m, re)) {
        try {
            int p = std::stoi(m[1].str());
            if (p > 0) return p;
        } catch (...) {}
    }
    return std::nullopt;
}

static std::optional<int> chunk_page_number(const std::string& id) {
    auto pos = id.find("#p");
    if (pos == std::string::npos) return std::nullopt;
    pos += 2;
    size_t end = pos;
    while (end < id.size() && std::isdigit(static_cast<unsigned char>(id[end]))) ++end;
    if (end == pos) return std::nullopt;
    try {
        int p = std::stoi(id.substr(pos, end - pos));
        if (p > 0) return p;
    } catch (...) {}
    return std::nullopt;
}


bool RAGSessionManager::ensure_embedding_model_available() {
    if (embed_model_ready_) return true;

    if (ollama_url_.find("localhost") != std::string::npos ||
        ollama_url_.find("127.0.0.1") != std::string::npos) {
        log("ERROR: RAG requires a remote Ollama host. Update 'ollama_url' to a remote server instead of '" + ollama_url_ + "'.");
        return false;
    }

    auto has_model = [&](const json& tags) {
        if (!tags.is_object() || !tags.contains("models") || !tags["models"].is_array()) return false;
        for (const auto& m : tags["models"]) {
            if (!m.is_object()) continue;
            std::string name = m.value("name", "");
            std::string model = m.value("model", "");
            if (name == embed_model_ || model == embed_model_) return true;
            if (!name.empty() && name.rfind(embed_model_ + ":", 0) == 0) return true;
            if (!model.empty() && model.rfind(embed_model_ + ":", 0) == 0) return true;
        }
        return false;
    };

    CURL* c = curl_easy_init();
    if (!c) return false;

    std::string resp;
    std::string url = EndpointResolver::deriveTagsEndpoint(ollama_url_);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        log("ERROR: unable to reach remote Ollama host at '" + ollama_url_ + "' while checking embedding model availability.");
        return false;
    }

    auto tags = json::parse(resp, nullptr, false);
    embed_model_ready_ = has_model(tags);
    if (!embed_model_ready_) {
        log("ERROR: embedding model '" + embed_model_ + "' is not installed on remote Ollama host '" + ollama_url_ +
            "'. Please install it on that remote host (for example: ollama pull " + embed_model_ + ").");
    }
    return embed_model_ready_;
}

std::vector<float> RAGSessionManager::embed(const std::string& t) {
    if (!ensure_embedding_model_available()) {
        log("ERROR: embedding model '" + embed_model_ + "' is unavailable in Ollama.");
        return {};
    }

    CURL* c = curl_easy_init();
    if (!c) return {};

    std::string url = EndpointResolver::embedEndpoint(ollama_url_);
    json payload = {{"model", embed_model_}, {"prompt", t}};
    std::string resp;

    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    auto body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) return {};

    auto j = json::parse(resp, nullptr, false);
    if (!j.is_object() || !j.contains("embedding")) return {};
    return j["embedding"].get<std::vector<float>>();
}

std::string RAGSessionManager::ollama_chat(const std::string& p) {
    CURL* c = curl_easy_init();
    if (!c) return {};

    std::string url = EndpointResolver::normalize(ollama_url_) + "/api/chat";
    json payload = {
        {"model", llm_model_},
        {"messages", json::array({
                         json{{"role", "system"},
                              {"content", "You are a helpful assistant. Answer ONLY with the final answer. Do NOT include chain-of-thought, analysis, or <think> tags."}},
                         json{{"role", "user"}, {"content", p}}})},
        {"stream", false}};

    std::string resp;
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    auto body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) return {};

    auto j = json::parse(resp, nullptr, false);
    if (!j.is_object() || !j.contains("message") || !j["message"].contains("content")) return {};

    std::string out = j["message"]["content"].get<std::string>();
    auto a = out.find("<think>"), b = out.find("</think>");
    if (a != std::string::npos && b != std::string::npos && b > a) out.erase(a, (b + 8) - a);
    while ((a = out.find("<think>")) != std::string::npos) out.erase(a, 7);
    while ((a = out.find("</think>")) != std::string::npos) out.erase(a, 8);
    while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
    size_t i = 0;
    while (i < out.size() && std::isspace((unsigned char)out[i])) ++i;
    return out.substr(i);
}

std::string RAGSessionManager::sessionDir(const std::string& sid) const {
    return (fs::path(base_dir_) / sid).string();
}

void RAGSessionManager::save_index(const SessionIndex& idx) const {
    fs::create_directories(sessionDir(idx.session_id));
    std::ofstream ofs(fs::path(sessionDir(idx.session_id)) / "index.json");
    json j;
    j["session_id"] = idx.session_id;
    j["chunks"] = json::array();
    for (const auto& c : idx.chunks) {
        j["chunks"].push_back({{"id", c.id}, {"text", c.text}, {"embedding", c.embedding}});
    }
    ofs << j.dump(2, ' ', false, json::error_handler_t::replace);
}

std::optional<SessionIndex> RAGSessionManager::load_index(const std::string& sid) const {
    auto p = fs::path(sessionDir(sid)) / "index.json";
    if (!fs::exists(p)) return std::nullopt;

    std::ifstream ifs(p);
    json j;
    ifs >> j;

    SessionIndex idx;
    idx.session_id = j.value("session_id", sid);
    for (auto& cj : j["chunks"]) {
        Chunk c;
        c.id = cj.value("id", "");
        c.text = cj.value("text", "");
        c.embedding = cj.value("embedding", std::vector<float>{});
        idx.chunks.push_back(std::move(c));
    }
    return idx;
}

double RAGSessionManager::cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return -1.0;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na == 0 || nb == 0) return -1.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::string RAGSessionManager::build_prompt(const std::string& ctx, const std::string& q) {
    std::ostringstream o;
    o << "Answer using ONLY the provided context snippets. "
      << "If the question asks for a summary/overview, provide a concise synthesis of the available snippets. "
      << "If the answer is not present, say: I cannot find that in the provided document context.\n\n"
      << "Context:\n"
      << ctx
      << "\nQuestion:\n" << q
      << "\n\nReturn 2-6 sentences and include concrete details from context where possible.";
    return o.str();
}


static std::unordered_set<std::string> token_set(const std::string& s) {
    std::unordered_set<std::string> out;
    std::string cur;
    cur.reserve(32);

    for (char ch : s) {
        unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u)) {
            cur.push_back((char)std::tolower(u));
        } else if (!cur.empty()) {
            if (cur.size() >= 2) out.insert(cur);
            cur.clear();
        }
    }
    if (!cur.empty() && cur.size() >= 2) out.insert(cur);
    return out;
}

std::string RAGSessionManager::createSessionFromFolder(const std::string& folder) {
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        throw std::runtime_error("Folder does not exist: " + folder);
    }

    log("Scanning PDFs in: " + folder);
    auto pdfs = findPDFs(folder);
    if (pdfs.empty()) throw std::runtime_error("No PDFs found in: " + folder);
    log("Found " + std::to_string(pdfs.size()) + " PDF(s).");

    SessionIndex idx;
    idx.session_id = uuid4();

    size_t total_chunks = 0;
    size_t embedded_chunks = 0;
    size_t failed_embeddings = 0;

    size_t n = 0;
    for (const auto& pdf : pdfs) {
        ++n;
        log("[" + std::to_string(n) + "/" + std::to_string(pdfs.size()) + "] Extracting text: " + pdf);

        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::pair<int, std::string>> page_texts;
        {
            std::unique_ptr<poppler::document> d(poppler::document::load_from_file(pdf));
            if (d) {
                for (int page = 0; page < d->pages(); ++page) {
                    std::unique_ptr<poppler::page> pg(d->create_page(page));
                    if (!pg) continue;
                    auto ba = pg->text().to_utf8();
                    std::string page_text(ba.begin(), ba.end());
                    if (!page_text.empty()) page_texts.push_back({page + 1, std::move(page_text)});
                }
            }
        }
        auto t1 = std::chrono::steady_clock::now();

        log("  Text extracted in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()) + " ms.");

        size_t extracted_chars = 0;
        for (const auto& page : page_texts) extracted_chars += page.second.size();

        if (extracted_chars < 40) {
            log("  WARNING: Very little/no text extracted. Falling back to OCR via Poppler+Tesseract...");
            auto o0 = std::chrono::steady_clock::now();
            auto ocr = ocr_pdf_with_poppler_tesseract(pdf, 200);
            auto o1 = std::chrono::steady_clock::now();
            log("  OCR completed in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(o1 - o0).count()) + " ms.");
            if (!ocr.empty()) {
                page_texts.clear();
                page_texts.push_back({1, std::move(ocr)});
            }
        }

        std::vector<std::pair<int, std::vector<std::string>>> page_chunks;
        size_t pdf_total_chunks = 0;
        for (const auto& page : page_texts) {
            auto chunks = split_chunks(page.second, 1024, 100);
            if (!chunks.empty()) {
                pdf_total_chunks += chunks.size();
                page_chunks.push_back({page.first, std::move(chunks)});
            }
        }

        log("  Chunking: " + std::to_string(pdf_total_chunks) + " chunks.");

        total_chunks += pdf_total_chunks;
        size_t cnum = 0;
        for (auto& page_group : page_chunks) {
            int page_num = page_group.first;
            auto& chunks = page_group.second;
            for (size_t i = 0; i < chunks.size(); ++i) {
                ++cnum;
                if (cnum % 25 == 1 || cnum == pdf_total_chunks) {
                    log("    Embedding chunk " + std::to_string(cnum) + "/" + std::to_string(pdf_total_chunks));
                }

                Chunk c;
                c.id = pdf + "#p" + std::to_string(page_num) + "-c" + std::to_string(i);
                c.text = "SOURCE: " + pdf + " page " + std::to_string(page_num) + "\n" + std::move(chunks[i]);
                c.embedding = embed(c.text);

                if (c.embedding.empty()) {
                    ++failed_embeddings;
                    log("    WARNING: embedding failed for " + c.id + " using model '" + embed_model_ + "'");
                    continue;
                }

                idx.chunks.push_back(std::move(c));
                ++embedded_chunks;
            }
        }
    }

    log("Embedding summary: total_chunks=" + std::to_string(total_chunks) +
        ", embedded_chunks=" + std::to_string(embedded_chunks) +
        ", failed_chunks=" + std::to_string(failed_embeddings));

    if (embedded_chunks == 0) {
        throw std::runtime_error("No valid embeddings generated; verify /api/embeddings model availability");
    }

    save_index(idx);
    log("Session ID: " + idx.session_id);
    return idx.session_id;
}

std::string RAGSessionManager::chat(const std::string& sid, const std::string& msg, int k, double thr) {
    auto opt = load_index(sid);
    if (!opt) return "Invalid or unknown session_id";

    auto& idx = *opt;
    if (idx.chunks.empty()) return "No relevant context found in the document to answer your question.";

    auto q = embed(msg);
    std::vector<std::pair<double, size_t>> sc;
    sc.reserve(idx.chunks.size());

    if (!q.empty()) {
        for (size_t i = 0; i < idx.chunks.size(); ++i) {
            const auto& emb = idx.chunks[i].embedding;
            if (emb.empty() || emb.size() != q.size()) continue;
            sc.push_back({cosine(q, emb), i});
        }
    } else {
        log("WARNING: query embedding failed; using lexical fallback retrieval.");
    }

    std::sort(sc.begin(), sc.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    if (verbose_ && !sc.empty()) {
        std::ostringstream o;
        o << "Top similarity scores:";
        size_t topn = std::min<size_t>(3, sc.size());
        for (size_t i = 0; i < topn; ++i) {
            o << " [" << (i + 1) << "] " << idx.chunks[sc[i].second].id << "=" << sc[i].first;
        }
        log(o.str());
    }

    std::string ctx;
    int added = 0;
    std::vector<bool> selected(idx.chunks.size(), false);

    if (auto req_page = requested_page_number(msg)) {
        for (size_t i = 0; i < idx.chunks.size() && added < k; ++i) {
            auto page = chunk_page_number(idx.chunks[i].id);
            if (page && *page == *req_page) {
                ctx += idx.chunks[i].text + "\n\n";
                selected[i] = true;
                ++added;
            }
        }
        if (verbose_ && added > 0) {
            log("Page-directed retrieval selected " + std::to_string(added) + " chunk(s) from page " + std::to_string(*req_page) + ".");
        }
    }

    for (const auto& p : sc) {
        if (added >= k) break;
        if (p.first < thr) break;
        if (selected[p.second]) continue;
        ctx += idx.chunks[p.second].text + "\n\n";
        selected[p.second] = true;
        ++added;
    }

    // If semantic retrieval misses, fall back to token-overlap retrieval.
    if (ctx.empty()) {
        auto qtok = token_set(msg);
        if (!qtok.empty()) {
            std::vector<std::pair<int, size_t>> lexical;
            lexical.reserve(idx.chunks.size());
            for (size_t i = 0; i < idx.chunks.size(); ++i) {
                const auto& t = idx.chunks[i].text;
                int overlap = 0;
                for (const auto& tok : qtok) {
                    if (t.find(tok) != std::string::npos) ++overlap;
                }
                if (overlap > 0) lexical.push_back({overlap, i});
            }
            std::sort(lexical.begin(), lexical.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& p : lexical) {
                if (added >= k) break;
                if (selected[p.second]) continue;
                ctx += idx.chunks[p.second].text + "\n\n";
                selected[p.second] = true;
                ++added;
            }
            if (verbose_ && !lexical.empty()) {
                log("Lexical fallback selected " + std::to_string(std::min<int>(k, (int)lexical.size())) + " chunk(s).");
            }
        }
    }

    if (ctx.empty()) return "No relevant context found in the document to answer your question.";
    auto prompt = build_prompt(ctx, msg);
    return ollama_chat(prompt);
}
