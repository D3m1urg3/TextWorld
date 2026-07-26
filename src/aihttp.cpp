// See aihttp.hpp for the contract. Step 4 ships the pure pieces only — the
// per-role model rule and the two body parsers; the persistent-handle curl
// client follows in Step 6.
#include "aihttp.hpp"

#include <cstdlib>
#include <optional>

#include "json.hpp"

namespace {

using nlohmann::json;

// A JSON integer field, or nullopt if absent / not an integer. nlohmann's
// is_number_integer() is false for floats and strings, which is what we want:
// a malformed count must read as "unknown", never as a coerced number.
std::optional<long long> intField(const json& obj, const char* key) {
    if (!obj.contains(key) || !obj[key].is_number_integer()) return std::nullopt;
    return obj[key].get<long long>();
}

}  // namespace

const char* roleName(AiRole role) {
    switch (role) {
        case AiRole::Resolve:
            return "resolve";
        case AiRole::Narrate:
            return "narrate";
        case AiRole::Generate:
            return "generate";
    }
    return "unknown";
}

std::string modelForRole(AiRole role) {
    // Level 1: the global override, unchanged from before this file existed —
    // set AND non-empty, applies to every role (REQ-LAT-13).
    const char* env = std::getenv("TEXTWORLD_MODEL");
    if (env != nullptr && env[0] != '\0') return env;

    // Level 2: the per-role default (REQ-LAT-12). Resolve is a constrained,
    // schema-gated classification — the cheap model does it, and a wrong answer
    // fails the same validation gate and falls back to the fixed-verb parser as
    // before. Narration and generation are prose quality; they stay on Opus.
    switch (role) {
        case AiRole::Resolve:
            return "claude-haiku-4-5";
        case AiRole::Narrate:
        case AiRole::Generate:
            return "claude-opus-4-8";
    }
    return "claude-opus-4-8";
}

AiUsage parseUsage(const std::string& responseBody) {
    AiUsage usage;
    const json j = json::parse(responseBody, /*cb=*/nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return usage;
    if (!j.contains("usage") || !j["usage"].is_object()) return usage;

    const std::optional<long long> in = intField(j["usage"], "input_tokens");
    const std::optional<long long> out = intField(j["usage"], "output_tokens");
    if (!in || !out) return usage;  // partial usage reads as unknown, not zero

    usage.known = true;
    usage.inputTokens = *in;
    usage.outputTokens = *out;
    return usage;
}

std::string modelFromRequestBody(const std::string& requestBody) {
    const json j = json::parse(requestBody, /*cb=*/nullptr,
                               /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return "";
    if (!j.contains("model") || !j["model"].is_string()) return "";
    return j["model"].get<std::string>();
}
