// See aihttp.hpp for the contract: the per-role model rule, the pure body
// parsers, and the one persistent-handle HTTP client the three AI roles share.
#include "aihttp.hpp"

#include <atomic>
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

// libcurl progress callback (REQ-PREGEN-20): returning non-zero makes libcurl
// abandon the transfer with CURLE_ABORTED_BY_CALLBACK, which reads downstream
// as an ordinary transportError — the existing failure path, no new branch.
// `clientp` is the std::atomic<bool>* the caller handed in; it is only ever
// READ here, so the cross-thread access is exactly one atomic load.
int abortIfFlagged(void* clientp, curl_off_t, curl_off_t, curl_off_t,
                   curl_off_t) {
    const auto* flag = static_cast<const std::atomic<bool>*>(clientp);
    return (flag != nullptr && flag->load()) ? 1 : 0;
}

// The whole of one POST, parameterized by the handle that performs it. This is
// anthropicPost's former body verbatim, lifted so BOTH the shared main-thread
// handle and a worker's own handle (REQ-PREGEN-8) run identical code — same
// URL, same three headers, the caller's timeout, same one perform, no retries
// (REQ-PREGEN-9). The handle is a PARAMETER precisely so this function can
// never reach for the file-static shared one.
HttpResponse performPost(CURL* handle, const std::string& requestBody,
                         AiRole role, long timeoutSeconds, bool background,
                         const std::atomic<bool>* abort) {
    HttpResponse resp;
    if (handle == nullptr) {
        resp.transportError = true;
        return resp;
    }
    // Wipe every option from the previous call, then re-apply the full set
    // below (REQ-LAT-8, REQ-LAT-10). What reset does NOT wipe is exactly what
    // this whole change is for: the connection pool, the TLS session, and the
    // DNS cache all belong to the handle and survive.
    curl_easy_reset(handle);

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

    curl_easy_setopt(handle, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(requestBody.size()));
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.list);
    // Total budget, seconds. kAiHttpTimeoutSeconds for every caller but the
    // bard's overture, which passes its own (REQ-BARD-WAKE-5).
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp.body);
    // All of these belong in the PER-CALL block, not a one-time setup path:
    // curl_easy_reset clears them along with everything else.
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);        // REQ-LAT-9
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);   // keep the pooled
                                                           // connection warm
                                                           // across idle gaps
    // The abort pair, likewise per-call and likewise wiped by reset. Absent
    // for the main-thread client, which has nothing to abort for.
    if (abort != nullptr) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, abortIfFlagged);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, abort);
    }

    const CURLcode rc = curl_easy_perform(handle);
    if (rc != CURLE_OK) {
        resp.transportError = true;
    } else {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &resp.status);
    }

    // Everything below is gated: profiling off means none of it runs.
    if (profilingEnabled()) {
        CallRecord record;
        record.role = roleName(role);
        record.model = modelFromRequestBody(requestBody);
        record.turn = profileCurrentTurn();
        record.status = resp.status;
        record.failed = resp.transportError || resp.status != 200;
        record.background = background;  // REQ-PREGEN-24
        record.namelookupUs = timingUs(handle, CURLINFO_NAMELOOKUP_TIME_T);
        record.connectUs = timingUs(handle, CURLINFO_CONNECT_TIME_T);
        record.appconnectUs = timingUs(handle, CURLINFO_APPCONNECT_TIME_T);
        record.starttransferUs = timingUs(handle, CURLINFO_STARTTRANSFER_TIME_T);
        record.totalUs = timingUs(handle, CURLINFO_TOTAL_TIME_T);
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

}  // namespace

const char* roleName(AiRole role) {
    switch (role) {
        case AiRole::Resolve:
            return "resolve";
        case AiRole::Narrate:
            return "narrate";
        case AiRole::Generate:
            return "generate";
        case AiRole::Bard:
            return "bard";
        case AiRole::Speak:
            return "speak";
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
    // Speak is prose too — a character's reply is the one AI output of a talk
    // turn and prints verbatim (REQ-NPCTALK-16, -25), so it takes the prose
    // default rather than the resolver's cheap one.
    switch (role) {
        case AiRole::Resolve:
            return "claude-haiku-4-5";
        case AiRole::Narrate:
        case AiRole::Generate:
        case AiRole::Bard:
        case AiRole::Speak:
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

HttpResponse anthropicPost(const std::string& requestBody, AiRole role,
                           long timeoutSeconds) {
    // The shared, main-thread-only handle, created on first use. Nothing about
    // this call changed when performPost was extracted: same handle, same
    // options, same records, and no abort flag — the main thread has nothing to
    // abort for, it is the thread waiting on the answer.
    if (g_handle == nullptr) g_handle = curl_easy_init();
    return performPost(g_handle, requestBody, role, timeoutSeconds,
                       /*background=*/false, /*abort=*/nullptr);
}

HttpTransport makeAnthropicTransport(AiRole role, long timeoutSeconds) {
    return [role, timeoutSeconds](const std::string& body) {
        return anthropicPost(body, role, timeoutSeconds);
    };
}

AiHttpWorkerClient::AiHttpWorkerClient(const std::atomic<bool>* abort)
    : handle_(curl_easy_init()), abort_(abort) {}

AiHttpWorkerClient::~AiHttpWorkerClient() {
    // Runs on the SAME thread that constructed this, and before
    // aiHttpShutdown() reaches curl_global_cleanup() (REQ-PREGEN-19).
    if (handle_ != nullptr) {
        curl_easy_cleanup(static_cast<CURL*>(handle_));
        handle_ = nullptr;
    }
}

HttpResponse AiHttpWorkerClient::post(const std::string& requestBody,
                                      AiRole role) {
    // The ordinary budget, always: only the overture is the exception, and it
    // runs on the main thread, not here (REQ-BARD-WAKE-5).
    return performPost(static_cast<CURL*>(handle_), requestBody, role,
                       kAiHttpTimeoutSeconds, /*background=*/true, abort_);
}
