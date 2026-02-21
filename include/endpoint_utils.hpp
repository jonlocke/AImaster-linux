#pragma once
#include <string>
struct EndpointResolver {
    static bool endsWith(const std::string& value, const std::string& suffix) {
        if (suffix.size() > value.size()) return false;
        return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    static std::string normalize(const std::string& base) {
        std::string normalized = base;
        while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();

        // Accept either a host URL (http://host:11434) or a full Ollama API endpoint
        // (http://host:11434/api/chat, /api/generate, etc.) and always return host base.
        static const char* kKnownApiSuffixes[] = {
            "/api/chat",
            "/api/generate",
            "/api/embeddings",
            "/api/tags"
        };
        for (const char* suffix : kKnownApiSuffixes) {
            std::string s(suffix);
            if (endsWith(normalized, s)) {
                normalized.erase(normalized.size() - s.size());
                break;
            }
        }

        while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
        return normalized;
    }
    static std::string deriveTagsEndpoint(const std::string& base_url) {
        return normalize(base_url) + "/api/tags";
    }
    static std::string chatEndpoint(const std::string& base_url) {
        return normalize(base_url) + "/api/generate";
    }
    static std::string embedEndpoint(const std::string& base_url) {
        return normalize(base_url) + "/api/embeddings";
    }
};
