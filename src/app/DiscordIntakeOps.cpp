#include "DiscordIntakeOps.h"

#include "Ingest.h"
#include "Ops.h"

using json = nlohmann::json;

namespace devhub {

ManualCaptureClassification classifyManualCaptureLocked(
    Db::Held held, Db* db, const std::string& content) {
    const DetectionResult detected =
        makeDetectorLocked(held, db).analyze(content);
    ManualCaptureClassification out;
    out.kind = detected.isCandidate && detected.kind == "bug"
        ? "bug" : "suggestion";
    std::vector<std::string> matches = {"manual"};
    if (detected.isCandidate)
        matches.insert(matches.end(), detected.matched.begin(),
                       detected.matched.end());
    out.matched = json(matches).dump();
    return out;
}

std::string resolvedManualCaptureTargetLocked(
    Db::Held held, Db* db, const std::string& targetMessageId) {
    std::string resolved = targetMessageId;
    // A follow-up can quote an earlier admin mention instead of the original
    // user message. Bound traversal against malformed imported chains.
    for (int depth = 0; depth < 8 && !resolved.empty(); ++depth) {
        SQLite::Statement q(db->raw(held), R"sql(
SELECT COALESCE(m.manual_command_state,''),COALESCE(r.message_id,'')
  FROM discord_messages m
  LEFT JOIN discord_messages r ON r.id=m.manual_result_message_row_id
 WHERE m.message_id=?
)sql");
        q.bind(1, resolved);
        if (!q.executeStep() || q.getColumn(0).getString() != "done") break;
        const std::string resultMessageId = q.getColumn(1).getString();
        if (resultMessageId.empty() || resultMessageId == resolved) break;
        resolved = resultMessageId;
    }
    return resolved;
}

MappedManualPromotionOutcome promoteMappedManualCapture(
    Db* db, DiscordBot* bot, long long messageRowId) {
    MappedManualPromotionOutcome out;
    if (!db || messageRowId <= 0) return out;

    long long projectId = 0;
    {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT CASE WHEN p.id IS NOT NULL AND p.archived=0
            THEN COALESCE(c.project_id,0) ELSE 0 END
  FROM discord_messages m
  JOIN discord_channels c ON c.id=m.channel_row_id
  LEFT JOIN projects p ON p.id=c.project_id
 WHERE m.id=?
)sql");
        q.bind(1, messageRowId);
        if (!q.executeStep()) {
            out.error = "captured Discord message no longer exists";
            return out;
        }
        projectId = q.getColumn(0).getInt64();
    }
    if (projectId <= 0) {
        out.ok = true;
        return out;
    }

    out.mapped = true;
    const json promoted = promoteSuggestion(
        db, bot, messageRowId, projectId, "", "", "",
        kDiscordReviewNormalPriority);
    out.ok = promoted.value("ok", false);
    out.duplicate = promoted.value("duplicate", false);
    out.itemId = promoted.value("item_id", 0LL);
    if (!out.ok)
        out.error = promoted.value(
            "error", "mapped Discord capture could not be promoted");
    return out;
}

} // namespace devhub
