// See aihttp.hpp for the contract: the per-role model rule, the pure body
// parsers, and the one persistent-handle HTTP client the three AI roles share.
#include "aihttp.hpp"

#include <cstdlib>
#include <optional>

#include <curl/curl.h>

#include "json.hpp"
#include "profile.hpp"

namespace {

using nlohmann::json;

// A JSON integer field, or nullopt if absent / not an integer. nlohmann's
// is_number_integer() is false for floats and strings, which is what we want:
// a malformed count must read as "unknown", never as a coerced number.
std::optional<long long> intField(const json& obj, const char* key) {
    if (!obj.contains(key) || !obj[key].is_number_integer()) return std::nullopt;
    return obj[key].get<long long>();
}

// --- the shared persistent handle -------------------------------------------

// Created on first use, reused for every call of every role, and destroyed
// ONLY by aiHttpShutdown(). All three roles POST the same host with an
// identical option set, sequentially, on one thread — so one handle means the
// narrate call reuses the connection the resolve call just opened.
CURL* g_handle = nullptr;
bool g_globalInitialized = false;

// libcurl write callback: append the response bytes to a std::string. The one
// copy in the codebase now — the three per-unit duplicates are gone.
size_t appendToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Frees the per-call header list on EVERY exit path. The list must outlive
// curl_easy_perform and the getinfo reads — the handle holds the pointer until
// the next curl_easy_reset — so this guard sits in the enclosing scope and
// runs last.
struct SlistGuard {
    curl_slist* list = nullptr;
    ~SlistGuard() {
        if (list != nullptr) curl_slist_free_all(list);
    }
};

// One CURLINFO_*_TIME_T microsecond field, or 0 if libcurl declines it.
int64_t timingUs(CURL* handle, CURLINFO info) {
    curl_off_t value = 0;
    if (curl_easy_getinfo(handle, info, &value) != CURLE_OK) return 0;
    return static_cast<int64_t>(value);
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

void aiHttpInit() {
    if (g_globalInitialized) return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_globalInitialized = true;
}

void aiHttpShutdown() {
    // ORDER IS LOAD-BEARING: the easy handle must die before global cleanup.
    if (g_handle != nullptr) {
        curl_easy_cleanup(g_handle);  // the ONLY cleanup in the codebase
        g_handle = nullptr;
    }
    if (g_globalInitialized) {
        curl_global_cleanup();
        g_globalInitialized = false;
    }
}

HttpResponse anthropicPost(const std::string& requestBody, AiRole role) {
    HttpResponse resp;

    if (g_handle == nullptr) g_handle = curl_easy_init();
    if (g_handle == nullptr) {
        resp.transportError = true;
        return resp;
    }
    // Wipe every option from the previous call, then re-apply the full set
    // below (REQ-LAT-8, REQ-LAT-10). What reset does NOT wipe is exactly what
    // this whole change is for: the connection pool, the TLS session, and the
    // DNS cache all belong to the handle and survive.
    curl_easy_reset(g_handle);

    // The key is read at CALL time, so the header list is rebuilt per call. It
    // goes into x-api-key ONLY — never into the payload, a log line, or a
    // profile record.
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    SlistGuard headers;
    headers.list = curl_slist_append(
        headers.list,
        ("x-api-key: " + std::string(key != nullptr ? key : "")).c_str());
    headers.list =
        curl_slist_append(headers.list, "anthropic-version: 2023-06-01");
    headers.list = curl_slist_append(headers.list, "content-type: application/json");

    curl_easy_setopt(g_handle, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(g_handle, CURLOPT_POST, 1L);
    curl_easy_setopt(g_handle, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(g_handle, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(requestBody.size()));
    curl_easy_setopt(g_handle, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(g_handle, CURLOPT_TIMEOUT, 8L);  // total budget, seconds
    curl_easy_setopt(g_handle, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(g_handle, CURLOPT_WRITEDATA, &resp.body);
    // Both of these belong in the PER-CALL block, not a one-time setup path:
    // curl_easy_reset clears them along with everything else.
    curl_easy_setopt(g_handle, CURLOPT_NOSIGNAL, 1L);        // REQ-LAT-9
    curl_easy_setopt(g_handle, CURLOPT_TCP_KEEPALIVE, 1L);   // keep the pooled
                                                             // connection warm
                                                             // across idle gaps

    const CURLcode rc = curl_easy_perform(g_handle);
    if (rc != CURLE_OK) {
        resp.transportError = true;
    } else {
        curl_easy_getinfo(g_handle, CURLINFO_RESPONSE_CODE, &resp.status);
    }

    // Everything below is gated: profiling off means none of it runs.
    if (profilingEnabled()) {
        CallRecord record;
        record.role = roleName(role);
        record.model = modelFromRequestBody(requestBody);
        record.turn = profileCurrentTurn();
        record.status = resp.status;
        record.failed = resp.transportError || resp.status != 200;
        record.namelookupUs = timingUs(g_handle, CURLINFO_NAMELOOKUP_TIME_T);
        record.connectUs = timingUs(g_handle, CURLINFO_CONNECT_TIME_T);
        record.appconnectUs = timingUs(g_handle, CURLINFO_APPCONNECT_TIME_T);
        record.starttransferUs = timingUs(g_handle, CURLINFO_STARTTRANSFER_TIME_T);
        record.totalUs = timingUs(g_handle, CURLINFO_TOTAL_TIME_T);
        if (!record.failed) {
            const AiUsage usage = parseUsage(resp.body);
            record.tokensKnown = usage.known;
            record.inputTokens = usage.inputTokens;
            record.outputTokens = usage.outputTokens;
        }
        profileEmit(record);
    }

    return resp;  // headers.list freed here, AFTER perform and every getinfo
}

HttpTransport makeAnthropicTransport(AiRole role) {
    return [role](const std::string& body) { return anthropicPost(body, role); };
}
