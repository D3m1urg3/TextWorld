// aiRender(): events → LLM prose. READ-ONLY BY CONTRACT — SELECTs plus
// network egress only; writes nothing (see prose.hpp).
#include "prose.hpp"

#include "json.hpp"

std::optional<std::string> aiRender(Db& db, int64_t turn) {
    (void)db;
    (void)turn;
    return std::nullopt;
}
