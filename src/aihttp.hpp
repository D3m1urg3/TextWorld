// Shared AI HTTP layer: the one place the three AI roles agree on which model
// they call and (from Step 6) how they call it.
//
// This file deliberately REVERSES the earlier "each unit reimplements its own
// curlTransport" stance. That decision shared only the HttpTransport *type* so
// tests could inject fakes — a goal this file leaves fully intact, since the
// seam type and signature do not change. What triplication cannot survive is a
// PERSISTENT handle: handle ownership, curl_easy_reset + full option
// re-application, getinfo timing capture, and profile emission would be
// written three times, i.e. three chances to leak an option.
#pragma once

#include <string>

// The three AI call sites. The names are exactly the strings the profile
// records carry in their `role=` field.
enum class AiRole { Resolve, Narrate, Generate };

const char* roleName(AiRole role);

// The model id for one role. THE single place the precedence rule lives —
// exactly two levels (REQ-LAT-13):
//
//   1. TEXTWORLD_MODEL, when set AND non-empty, applies to ALL roles;
//   2. otherwise the per-role default (REQ-LAT-12):
//        Resolve  -> claude-haiku-4-5   (fast/cheap; the gate still governs)
//        Narrate  -> claude-opus-4-8    (prose quality)
//        Generate -> claude-opus-4-8    (prose quality)
//
// Per-role environment overrides (TEXTWORLD_MODEL_RESOLVE and friends) are
// OUT OF SCOPE by decision, not by oversight: their absence is deliberate. If
// they are ever added they layer on top of level 2, below TEXTWORLD_MODEL.
std::string modelForRole(AiRole role);

// Token counts read back from an Anthropic response body's `usage` object.
// `known` false means the counts could not be read — NEVER that they were
// zero (REQ-LAT-4: note the failure, do not fabricate).
struct AiUsage {
    bool known = false;
    long long inputTokens = 0;
    long long outputTokens = 0;
};

// Pure. Reads usage.input_tokens / usage.output_tokens out of a response body.
// NEVER throws: a non-JSON body, a missing `usage`, or non-integer fields all
// yield known = false.
AiUsage parseUsage(const std::string& responseBody);

// Pure. Reads "model" back out of a REQUEST body, so a profile record can name
// the model actually sent without widening the HttpTransport seam to carry it.
// Returns "" on any parse failure. Only ever called when profiling is on.
std::string modelFromRequestBody(const std::string& requestBody);
