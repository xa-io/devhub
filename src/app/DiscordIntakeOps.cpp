#include "DiscordIntakeOps.h"

#include "Ingest.h"
#include "Ops.h"
#include "DiscordBot.h"
#include "devhub/Util.h"
#include <stdexcept>

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

DiscordReviewOutcome replyToNotificationCard(
    Db* db, DiscordBot* bot, const std::string& notifyChannelId,
    const std::string& notifyMessageId, const std::string& actorUserId,
    const std::string& replyMessageId, const std::string& note,
    const std::vector<DiscordAttachmentMeta>& attachments) {
    // Probe ownership/authorization without changing the card on invalid input.
    DiscordReviewOutcome out = reviewNotificationCard(
        db, nullptr, notifyChannelId, notifyMessageId, "", true);
    if (!out.matched) return out;
    out.authorized = discordAdminUserAllowed(db, actorUserId);
    if (!out.authorized) return out;
    out.error.clear();
    const std::string text = trim(note);
    if (replyMessageId.empty() || (text.empty() && attachments.empty())) {
        out.error = "A card reply requires text or a supported attachment";
        return out;
    }
    // Promotion and note delivery are separate retryable steps. A failed note
    // transaction never acknowledges success; replay can finish the append.
    out = reviewNotificationCard(
        db, bot, notifyChannelId, notifyMessageId, actorUserId, true);
    if (!out.matched || !out.authorized) return out;
    out.ok = false;
    out.duplicate = false;
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement target(db->raw(lk.token()),
            "SELECT m.id,COALESCE(m.item_id,0),COALESCE(i.project_id,0),m.state "
            "FROM discord_notify_cards n "
            "JOIN discord_messages m ON m.id=n.discord_message_row_id "
            "LEFT JOIN items i ON i.id=m.item_id "
            "WHERE n.notify_channel_id=? AND n.notify_message_id=? "
            "AND (m.state='new' OR (m.state='promoted' AND i.status<>'merged'))");
        target.bind(1, notifyChannelId);
        target.bind(2, notifyMessageId);
        if (!target.executeStep()) {
            out.error = "This card is no longer pending or linked to a promoted ticket";
            return out;
        }
        out.messageRowId = target.getColumn(0).getInt64();
        out.itemId = target.getColumn(1).getInt64();
        const long long projectId = target.getColumn(2).getInt64();
        out.state = target.getColumn(3).getString();
        out.error.clear();
        SQLite::Statement prior(db->raw(lk.token()),
            "SELECT state,matched,manual_target_message_id FROM discord_messages "
            "WHERE message_id=?");
        prior.bind(1, replyMessageId);
        if (prior.executeStep()) {
            out.duplicate = prior.getColumn(0).getString() == "ignored" &&
                prior.getColumn(1).getString() == "[\"notification_reply\"]" &&
                prior.getColumn(2).getString() == notifyMessageId;
            out.ok = out.duplicate;
            if (!out.ok) out.error = "Reply already recorded by another operation";
            return out;
        }
        const auto staged = stageTicketAttachmentsLocked(
            lk.token(), db, out.messageRowId, attachments, out.itemId);
        if (text.empty() && staged.changed == 0) {
            out.error = "No supported attachment could be saved";
            return out;
        }
        const std::string now = nowIsoUtc();
        SQLite::Statement channel(db->raw(lk.token()),
            "INSERT INTO discord_channels(channel_id,created_at) VALUES(?,?) "
            "ON CONFLICT(channel_id) DO NOTHING");
        channel.bind(1, notifyChannelId); channel.bind(2, now); channel.exec();
        SQLite::Statement channelId(db->raw(lk.token()),
            "SELECT id FROM discord_channels WHERE channel_id=?");
        channelId.bind(1, notifyChannelId);
        if (!channelId.executeStep())
            throw std::runtime_error("Reply channel could not be recorded");
        const long long channelRow = channelId.getColumn(0).getInt64();
        SQLite::Statement receipt(db->raw(lk.token()),
            "INSERT INTO discord_messages(channel_row_id,message_id,author_id,"
            "content,ingested_at,state,matched,manual_target_message_id,item_id) "
            "VALUES(?,?,?,?,?,'ignored','[\"notification_reply\"]',?,?)");
        receipt.bind(1, channelRow); receipt.bind(2, replyMessageId);
        receipt.bind(3, actorUserId); receipt.bind(4, text);
        receipt.bind(5, now); receipt.bind(6, notifyMessageId);
        if (out.itemId > 0) receipt.bind(7, out.itemId);
        else receipt.bind(7);
        receipt.exec();
        if (out.state == "promoted" && !text.empty()) {
            SQLite::Statement item(db->raw(lk.token()),
                "UPDATE items SET body=body||?,updated_at=? WHERE id=?");
            item.bind(1, "\n\n-- Discord follow-up:\n" + text);
            item.bind(2, now); item.bind(3, out.itemId); item.exec();
        }
        if (!text.empty()) {
            SQLite::Statement source(db->raw(lk.token()),
                "UPDATE discord_messages SET admin_note=admin_note||"
                "CASE WHEN admin_note='' THEN '' ELSE char(10) END||? WHERE id=?");
            source.bind(1, text); source.bind(2, out.messageRowId); source.exec();
        }
        // Captured pending attachments do not have an item to fire the item
        // refresh trigger. Refresh their existing card in the same transaction.
        if (staged.changed > 0 && out.state == "new") {
            SQLite::Statement dirty(db->raw(lk.token()),
                "UPDATE discord_notify_cards SET edit_state='pending',"
                "edit_attempts=0,edit_revision=edit_revision+1,"
                "edit_next_retry_at='',edit_last_error='',edit_updated_at=?,updated_at=? "
                "WHERE discord_message_row_id=? AND notify_message_id<>''");
            dirty.bind(1, now); dirty.bind(2, now);
            dirty.bind(3, out.messageRowId); dirty.exec();
        }
        db->logActivity(lk.token(), "discord_evidence_appended", projectId,
            "I" + std::to_string(out.itemId) + ": card reply from " + actorUserId);
        tx.commit();
        out.ok = true;
        lk.unlock();
        if (bot) {
            bot->requestNotifyCardPost();
            if (out.itemId > 0 && staged.changed > 0)
                bot->requestQueuedTicketImageSave();
        }
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace devhub
