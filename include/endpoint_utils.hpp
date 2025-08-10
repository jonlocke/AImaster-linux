#pragma once
#include <string>
struct EndpointResolver {
    static std::string normalize(const std::string& base) {
        if (!base.empty() && base.back()=='/') return base.substr(0, base.size()-1);
        return base;
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