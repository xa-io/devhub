#include "Ops.h"
#include "Db.h"
#include "DiscordBot.h" // notification cards (header is dpp/json-free)
#include "PacketData.h"
#include "PacketOps.h"
#include "TicketImageStore.h"
#include "Version.h"
#include "devhub/DiscordRetry.h"
#include "devhub/TicketImage.h"
#include "devhub/Util.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sqlite3.h>

using json = nlohmann::json;

namespace devhub {

static std::string utf8Prefix(std::string_view value, std::size_t maximum) {
    if (value.size() <= maximum) return std::string(value);
    std::size_t cut = maximum;
    while (cut > 0 &&
           (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80)
        --cut;
    return std::string(value.substr(0, cut));
}

// Cross-translation-unit helpers owned by OpsItems.cpp / this file's
// notification-card section. They remain internal to the app target.
long long canonicalSourceIdLocked(Db::Held held, Db* db, long long sourceId);
void updateNotifyCardForItemLocked(Db::Held held, Db* db, DiscordBot* bot,
                                       long long itemId,
                                       const std::string& status);

long long inferProjectLocked(Db::Held held, Db* db, const std::string& content) {
    if (content.size() < 3) return 0;
    std::string text = toLower(content);
    long long bestId = 0;
    size_t bestLen = 0;
    SQLite::Statement q(db->raw(held),
        "SELECT id, name, aliases FROM projects WHERE archived=0");
    while (q.executeStep()) {
        long long id = q.getColumn(0).getInt64();
        std::vector<std::string> needles;
        needles.push_back(q.getColumn(1).getString());
        std::string aliases = q.getColumn(2).getString();
        size_t start = 0;
        while (start <= aliases.size()) {
            size_t comma = aliases.find(',', start);
            needles.push_back(aliases.substr(
                start, comma == std::string::npos ? std::string::npos
                                                  : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        for (const auto& raw : needles) {
            std::string needle = toLower(trim(raw));
            if (needle.size() < 3 || needle.size() <= bestLen) continue;
            if (text.find(needle) != std::string::npos) {
                bestLen = needle.size();
                bestId = id;
            }
        }
    }
    return bestId;
}

long long inferProjectFromChannelLocked(Db::Held held, Db* db,
                                        const std::string& channelName) {
    if (channelName.empty()) return 0;
    std::string t = channelName;
    for (char& ch : t)
        if (ch == '-' || ch == '_' || ch == '.') ch = ' ';
    return inferProjectLocked(held, db, t);
}

SuggestionDetector makeDetectorLocked(Db::Held held, Db* db) {
    std::string raw = db->getSetting(held, "detection_patterns");
    if (!raw.empty()) {
        json j = json::parse(raw, nullptr, false);
        if (!j.is_discarded()) {
            auto pats = SuggestionDetector::patternsFromJson(j);
            if (!pats.empty()) return SuggestionDetector(std::move(pats));
        }
    }
    return SuggestionDetector();
}

// Channel row find/create shared by normal ingest and manual capture.
// Returns the row id; *enabledOut gets the channel's enabled flag.
static long long ensureChannelRowLocked(Db::Held held, Db* db, const std::string& channelId,
                                        const std::string& channelName,
                                        const std::string& guildId,
                                        const std::string& guildName,
                                        int* enabledOut) {
    long long rowId = 0;
    int enabled = 1;
    {
        SQLite::Statement q(db->raw(held),
            "SELECT id, enabled FROM discord_channels WHERE channel_id=?");
        q.bind(1, channelId);
        if (q.executeStep()) {
            rowId = q.getColumn(0).getInt64();
            enabled = q.getColumn(1).getInt();
        }
    }
    if (rowId == 0) {
        SQLite::Statement ins(db->raw(held),
            "INSERT INTO discord_channels(channel_id,guild_id,guild_name,"
            "channel_name,created_at) VALUES(?,?,?,?,?)");
        ins.bind(1, channelId);
        ins.bind(2, guildId);
        ins.bind(3, guildName);
        ins.bind(4, channelName);
        ins.bind(5, nowIsoUtc());
        ins.exec();
        rowId = db->raw(held).getLastInsertRowid();
    } else if (!channelName.empty() || !guildId.empty()) {
        // Keep names fresh (channels get renamed) and backfill guild_id on
        // rows created before v4 / by hand.
        SQLite::Statement up(db->raw(held),
            "UPDATE discord_channels SET "
            "channel_name=CASE WHEN ?='' THEN channel_name ELSE ? END, "
            "guild_name=CASE WHEN ?='' THEN guild_name ELSE ? END, "
            "guild_id=CASE WHEN ?='' THEN guild_id ELSE ? END WHERE id=?");
        up.bind(1, channelName);
        up.bind(2, channelName);
        up.bind(3, guildName);
        up.bind(4, guildName);
        up.bind(5, guildId);
        up.bind(6, guildId);
        up.bind(7, rowId);
        up.exec();
    }
    if (enabledOut) *enabledOut = enabled;
    return rowId;
}

struct TicketAttachmentStageResult {
    int total = 0;
    int changed = 0;
};

static TicketAttachmentStageResult stageTicketAttachmentsLocked(
    Db::Held held, Db* db, long long messageRowId,
    const std::vector<DiscordAttachmentMeta>& attachments,
    long long itemId = 0) {
    TicketAttachmentStageResult result;
    if (messageRowId <= 0 || attachments.empty()) return result;
    const bool promoted = itemId > 0;
    const std::string now = nowIsoUtc();
    for (const DiscordAttachmentMeta& meta : attachments) {
        if (!isDecimalDiscordSnowflake(meta.sourceChannelId) ||
            !isDecimalDiscordSnowflake(meta.sourceMessageId) ||
            !isDecimalDiscordSnowflake(meta.attachmentId) ||
            !isSupportedTicketAttachmentMetadata(meta.contentType,
                                                 meta.filename,
                                                 meta.ephemeral) ||
            meta.sizeBytes > static_cast<std::uint64_t>(
                                 std::numeric_limits<long long>::max()))
            continue;
        std::string role = meta.sourceRole;
        if (role != "suggestion" && role != "admin_reply" &&
            role != "direct_ping")
            role = "suggestion";
        const std::string filename = utf8Prefix(meta.filename, 255);
        const std::string contentType = meta.contentType.substr(0, 127);
        SQLite::Statement ins(db->raw(held), R"sql(
INSERT INTO ticket_attachments(
 discord_message_row_id,item_id,source_channel_id,source_message_id,attachment_id,
 source_role,original_filename,content_type,declared_size,width,height,state,
 created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(source_message_id,attachment_id) DO UPDATE SET
 discord_message_row_id=excluded.discord_message_row_id,
 item_id=excluded.item_id,
 source_channel_id=excluded.source_channel_id,
 source_role=excluded.source_role,
 original_filename=excluded.original_filename,
 content_type=excluded.content_type,
 declared_size=excluded.declared_size,
 width=excluded.width,
 height=excluded.height,
 state=excluded.state,
 updated_at=excluded.updated_at
WHERE ticket_attachments.state='captured'
)sql");
        ins.bind(1, messageRowId);
        if (promoted) ins.bind(2, itemId); else ins.bind(2);
        ins.bind(3, meta.sourceChannelId);
        ins.bind(4, meta.sourceMessageId);
        ins.bind(5, meta.attachmentId);
        ins.bind(6, role);
        ins.bind(7, filename);
        ins.bind(8, contentType);
        ins.bind(9, static_cast<long long>(meta.sizeBytes));
        ins.bind(10, static_cast<long long>(meta.width));
        ins.bind(11, static_cast<long long>(meta.height));
        ins.bind(12, promoted ? "queued" : "captured");
        ins.bind(13, now);
        ins.bind(14, now);
        result.changed += ins.exec();
    }
    SQLite::Statement count(db->raw(held), promoted
        ? "SELECT COUNT(*) FROM ticket_attachments WHERE item_id=? AND state='queued'"
        : "SELECT COUNT(*) FROM ticket_attachments WHERE discord_message_row_id=? AND state='captured'");
    count.bind(1, promoted ? itemId : messageRowId);
    count.executeStep();
    result.total = count.getColumn(0).getInt();
    return result;
}

IngestOutcome ingestDiscordMessage(Db* db, const std::string& channelId,
                                   const std::string& channelName,
                                   const std::string& guildId,
                                   const std::string& guildName,
                                   const std::string& messageId,
                                   const std::string& author,
                                   const std::string& authorId,
                                   const std::string& content,
                                   const std::string& postedAt,
                                   bool asCommand,
                                   const std::vector<DiscordAttachmentMeta>& attachments) {
    IngestOutcome out;
    if (channelId.empty() || messageId.empty()) return out;
    auto lk = db->guard();
    try {
        // One atomic unit: channel metadata, the message row, staged attachment
        // metadata, and the read cursor. A failed delivery rolls back so a
        // later gateway/history retry can capture the complete message.
        SQLite::Transaction tx(db->raw(lk.token()));

        int enabled = 1;
        long long rowId = ensureChannelRowLocked(lk.token(),
            db, channelId, channelName, guildId, guildName, &enabled);
        if (!enabled) {
            out.skippedDisabled = true;
            tx.commit();
            return out;
        }

        DetectionResult det;
        if (!asCommand) {
            SuggestionDetector detector = makeDetectorLocked(lk.token(), db);
            det = detector.analyze(content);
        }
        // Project attribution: an explicit mention in the message wins; when
        // the text names nothing, the channel name itself is the signal
        // ("#xa-dashboard" -> XA Dashboard).
        long long inferred = det.isCandidate
                                 ? inferProjectLocked(lk.token(), db, content) : 0;
        if (det.isCandidate && inferred == 0)
            inferred = inferProjectFromChannelLocked(lk.token(), db, channelName);
        out.score = det.score;

        try {
            SQLite::Statement ins(db->raw(lk.token()),
                "INSERT INTO discord_messages(channel_row_id,message_id,author,"
                "author_id,content,posted_at,ingested_at,kind,score,matched,"
                "inferred_project_id) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            ins.bind(1, rowId);
            ins.bind(2, messageId);
            ins.bind(3, author);
            ins.bind(4, authorId);
            ins.bind(5, content);
            ins.bind(6, postedAt);
            ins.bind(7, nowIsoUtc());
            ins.bind(8, det.isCandidate ? det.kind : "none");
            ins.bind(9, det.score);
            ins.bind(10, nlohmann::json(det.matched).dump());
            if (inferred > 0) ins.bind(11, inferred); else ins.bind(11);
            ins.exec();
            out.messageRowId = db->raw(lk.token()).getLastInsertRowid();
            if (det.isCandidate)
                out.imageCount = stageTicketAttachmentsLocked(lk.token(),
                    db, out.messageRowId, attachments).total;
        } catch (const SQLite::Exception& e) {
            // A constraint is a duplicate only when the exact message_id row
            // exists. CHECK/FK/disk/lock failures must remain failures.
            if (e.getErrorCode() != SQLITE_CONSTRAINT) throw;
            SQLite::Statement existing(db->raw(lk.token()),
                "SELECT id,state FROM discord_messages WHERE message_id=?");
            existing.bind(1, messageId);
            if (!existing.executeStep()) throw;

            out.messageRowId = existing.getColumn(0).getInt64();
            SQLite::Statement up(db->raw(lk.token()),
                "UPDATE discord_channels SET last_scan_at=? WHERE id=?");
            up.bind(1, nowIsoUtc());
            up.bind(2, rowId);
            up.exec();
            // A retained deletion tombstone is terminal. Late gateway or
            // history callbacks must not recreate attachment metadata.
            if (det.isCandidate &&
                existing.getColumn(1).getString() != "deleted")
                out.imageCount = stageTicketAttachmentsLocked(lk.token(),
                    db, out.messageRowId, attachments).total;
            tx.commit();
            // Do not classify the delivery as a clean duplicate until every
            // reconciliation write is durable. A failed commit is an ingest
            // failure and must remain retryable instead of returning both
            // duplicate=true and failed=true.
            out.duplicate = true;
            return out;
        }

        out.ingested = true;
        if (det.isCandidate) {
            out.kind = det.kind;
            appLog("[detect] " + det.kind + " " +
                   std::to_string((int)(det.score * 100)) + "% from " +
                   author + " in #" +
                   (channelName.empty() ? channelId : channelName));
        }

        if (!postedAt.empty()) {
            SQLite::Statement up(db->raw(lk.token()),
                "UPDATE discord_channels SET last_read_ts=MAX(last_read_ts,?), "
                "last_scan_at=? WHERE id=?");
            up.bind(1, postedAt);
            up.bind(2, nowIsoUtc());
            up.bind(3, rowId);
            up.exec();
        }
        tx.commit();
        return out;
    } catch (const std::exception& e) {
        if (!out.duplicate) {
            out.ingested = false;
            out.messageRowId = 0;
            out.imageCount = 0;
        }
        out.failed = true;
        out.error = e.what();
        appLog("[ingest] FAILED for message " + messageId + ": " +
               redactHttpUrls(out.error));
        return out;
    } catch (...) {
        if (!out.duplicate) {
            out.ingested = false;
            out.messageRowId = 0;
            out.imageCount = 0;
        }
        out.failed = true;
        out.error = "unknown ingest failure";
        appLog("[ingest] FAILED for message " + messageId +
               ": unknown exception");
        return out;
    }
}

IngestOutcome reconcileDiscordMessageEdit(
    Db* db, const std::string& channelId, const std::string& channelName,
    const std::string& guildId, const std::string& guildName,
    const std::string& messageId, const std::string& author,
    const std::string& authorId, const std::string& content,
    const std::string& postedAt,
    const std::vector<DiscordAttachmentMeta>& attachments) {
    IngestOutcome out;
    if (!db || channelId.empty() || messageId.empty()) return out;

    // An update can be the first event observed after a reconnect. Let normal
    // ingest establish the row, then fall through only if another delivery won
    // the race and reported the same message id as a duplicate.
    bool exists = false;
    {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT 1 FROM discord_messages WHERE message_id=?");
        q.bind(1, messageId);
        exists = q.executeStep();
    }
    if (!exists) {
        out = ingestDiscordMessage(
            db, channelId, channelName, guildId, guildName, messageId,
            author, authorId, content, postedAt, /*asCommand=*/false,
            attachments);
        if (!out.duplicate) return out;
    }

    auto lk = db->guard();
    try {
        SQLite::Transaction tx(db->raw(lk.token()));
        int enabled = 1;
        const long long channelRow = ensureChannelRowLocked(
            lk.token(), db, channelId, channelName, guildId, guildName,
            &enabled);
        if (!enabled) {
            out.skippedDisabled = true;
            tx.commit();
            return out;
        }

        long long rowId = 0;
        long long itemId = 0;
        long long priorInferred = 0;
        std::string state, priorKind, priorContent, priorAuthor;
        std::string priorAuthorId, priorMatched, manualState;
        double priorScore = 0;
        {
            SQLite::Statement existing(db->raw(lk.token()), R"sql(
SELECT m.id,m.state,m.kind,m.content,m.author,m.author_id,m.score,m.matched,
       COALESCE(m.inferred_project_id,0),COALESCE(m.item_id,0),
       COALESCE(m.manual_command_state,''),
       EXISTS(SELECT 1 FROM discord_notify_cards n
               WHERE n.discord_message_row_id=m.id)
  FROM discord_messages m WHERE m.message_id=?
)sql");
            existing.bind(1, messageId);
            if (!existing.executeStep()) {
                tx.commit();
                return out;
            }
            int c = 0;
            rowId = existing.getColumn(c++).getInt64();
            state = existing.getColumn(c++).getString();
            priorKind = existing.getColumn(c++).getString();
            priorContent = existing.getColumn(c++).getString();
            priorAuthor = existing.getColumn(c++).getString();
            priorAuthorId = existing.getColumn(c++).getString();
            priorScore = existing.getColumn(c++).getDouble();
            priorMatched = existing.getColumn(c++).getString();
            priorInferred = existing.getColumn(c++).getInt64();
            itemId = existing.getColumn(c++).getInt64();
            manualState = existing.getColumn(c++).getString();
            out.hasCard = existing.getColumn(c++).getInt() != 0;
        }
        out.messageRowId = rowId;
        out.itemId = itemId;

        // A deletion marker is terminal. Manual-command edits are owned by the
        // command lifecycle helper, which runs before this reconciliation.
        if (state == "deleted" || !manualState.empty()) {
            out.duplicate = true;
            tx.commit();
            return out;
        }

        const SuggestionDetector detector = makeDetectorLocked(lk.token(), db);
        const DetectionResult detected = detector.analyze(content);
        std::string nextState = state;
        std::string nextKind = priorKind;
        double nextScore = priorScore;
        std::string nextMatched = priorMatched;
        long long nextInferred = priorInferred;
        if (detected.isCandidate) {
            nextKind = detected.kind;
            nextScore = detected.score;
            nextMatched = json(detected.matched).dump();
            nextInferred = inferProjectLocked(lk.token(), db, content);
            if (nextInferred == 0)
                nextInferred = inferProjectFromChannelLocked(
                    lk.token(), db, channelName);
        } else if (state == "new" &&
                   (priorKind == "suggestion" || priorKind == "bug")) {
            // Editing a pending candidate so it is no longer a candidate
            // closes that inbox entry and dirties its existing card. A later
            // replay cannot reopen it; promoted sources are handled below.
            nextState = "dismissed";
        } else if (state == "new") {
            nextKind = "none";
            nextScore = 0;
            nextMatched = "[]";
            nextInferred = 0;
        }

        // Promotion is durable operator intent. An edit may refresh retained
        // source text, author, classification and the card, but never detaches
        // or reopens the promoted item.
        if (itemId > 0 || state == "promoted") nextState = state;

        const std::string nextAuthor = author.empty() ? priorAuthor : author;
        const std::string nextAuthorId =
            authorId.empty() || authorId == "0" ? priorAuthorId : authorId;
        const bool changed = channelRow > 0 &&
            (priorContent != content || priorAuthor != nextAuthor ||
             priorAuthorId != nextAuthorId || priorKind != nextKind ||
             priorScore != nextScore || priorMatched != nextMatched ||
             priorInferred != nextInferred || state != nextState);
        if (changed) {
            SQLite::Statement update(db->raw(lk.token()), R"sql(
UPDATE discord_messages
   SET channel_row_id=?,author=?,author_id=?,content=?,kind=?,score=?,matched=?,
       inferred_project_id=?,state=?
 WHERE id=? AND state!='deleted'
)sql");
            update.bind(1, channelRow);
            update.bind(2, nextAuthor);
            update.bind(3, nextAuthorId);
            update.bind(4, content);
            update.bind(5, nextKind);
            update.bind(6, nextScore);
            update.bind(7, nextMatched);
            if (nextInferred > 0) update.bind(8, nextInferred);
            else update.bind(8);
            update.bind(9, nextState);
            update.bind(10, rowId);
            update.exec();
        }

        TicketAttachmentStageResult staged;
        if (detected.isCandidate && nextState != "dismissed")
            staged = stageTicketAttachmentsLocked(
                lk.token(), db, rowId, attachments, itemId);
        out.imageCount = staged.total;
        out.imagesQueued = staged.changed;
        out.ingested = changed || staged.changed > 0;
        out.duplicate = !out.ingested;
        out.kind = nextKind;
        out.score = nextScore;
        if (!postedAt.empty()) {
            SQLite::Statement cursor(db->raw(lk.token()),
                "UPDATE discord_channels SET last_read_ts=MAX(last_read_ts,?),"
                "last_scan_at=? WHERE id=?");
            cursor.bind(1, postedAt);
            cursor.bind(2, nowIsoUtc());
            cursor.bind(3, channelRow);
            cursor.exec();
        }
        tx.commit();
        if (out.ingested)
            appLog("[discord] reconciled edited message " + messageId +
                   (itemId > 0 ? " without reopening its promoted item" : ""));
        return out;
    } catch (const std::exception& e) {
        out.ingested = false;
        out.failed = true;
        out.error = e.what();
        appLog("[discord] edit reconciliation failed for message " +
               messageId + ": " + redactHttpUrls(out.error));
        return out;
    }
}

bool parseLeaderboardExcludedSourceIds(
    const std::string& value, std::vector<long long>& sourceIds,
    std::string* error) {
    sourceIds.clear();
    if (error) error->clear();
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return true;
        unsigned long long parsed = 0;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end || parsed == 0 ||
            parsed > static_cast<unsigned long long>(
                         std::numeric_limits<long long>::max()))
            return false;
        sourceIds.push_back(static_cast<long long>(parsed));
        token.clear();
        return true;
    };
    for (unsigned char ch : value) {
        if (ch == ',' || ch == ';' || ch <= ' ') {
            if (!flush()) {
                sourceIds.clear();
                if (error) *error = "leaderboard exclusions must be positive source IDs";
                return false;
            }
        } else {
            token.push_back(static_cast<char>(ch));
        }
    }
    if (!flush() || (!trim(value).empty() && sourceIds.empty())) {
        sourceIds.clear();
        if (error) *error = "leaderboard exclusions must be positive source IDs";
        return false;
    }
    std::sort(sourceIds.begin(), sourceIds.end());
    sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()),
                    sourceIds.end());
    return true;
}

static bool canonicalLeaderboardExclusionsLocked(
    Db::Held held, Db* db, std::vector<long long>& sourceIds, std::string* error) {
    if (!parseLeaderboardExcludedSourceIds(
            db->getSetting(held, "leaderboard_excluded_source_ids"), sourceIds,
            error))
        return false;
    for (long long& sourceId : sourceIds) {
        const long long canonical = canonicalSourceIdLocked(held, db, sourceId);
        // Retain unknown historical IDs fail-closed. They cannot match a
        // visible contributor today, and an incomplete import should not
        // silently weaken an operator's privacy preference.
        if (canonical > 0) sourceId = canonical;
    }
    std::sort(sourceIds.begin(), sourceIds.end());
    sourceIds.erase(std::unique(sourceIds.begin(), sourceIds.end()),
                    sourceIds.end());
    return true;
}

bool loadLeaderboardExcludedSourceIds(
    Db* db, std::vector<long long>& sourceIds, std::string* error) {
    sourceIds.clear();
    if (!db) {
        if (error) *error = "database unavailable";
        return false;
    }
    auto lk = db->guard();
    return canonicalLeaderboardExclusionsLocked(lk.token(), db, sourceIds, error);
}

bool setLeaderboardSourceExcluded(Db* db, long long sourceId, bool excluded,
                                  std::string* error) {
    if (error) error->clear();
    if (!db || sourceId <= 0) {
        if (error) *error = "invalid contributor source";
        return false;
    }
    auto lk = db->guard();
    sourceId = canonicalSourceIdLocked(lk.token(), db, sourceId);
    if (sourceId <= 0) {
        if (error) *error = "contributor source no longer exists";
        return false;
    }
    std::vector<long long> sourceIds;
    std::string parseError;
    if (!canonicalLeaderboardExclusionsLocked(lk.token(), db, sourceIds, &parseError)) {
        if (error) *error = parseError;
        return false;
    }
    SQLite::Statement exists(db->raw(lk.token()), "SELECT 1 FROM sources WHERE id=?");
    exists.bind(1, sourceId);
    if (!exists.executeStep()) {
        if (error) *error = "contributor source no longer exists";
        return false;
    }
    auto at = std::lower_bound(sourceIds.begin(), sourceIds.end(), sourceId);
    if (excluded) {
        if (at == sourceIds.end() || *at != sourceId)
            sourceIds.insert(at, sourceId);
    } else if (at != sourceIds.end() && *at == sourceId) {
        sourceIds.erase(at);
    }
    std::ostringstream canonical;
    for (std::size_t i = 0; i < sourceIds.size(); ++i) {
        if (i) canonical << ',';
        canonical << sourceIds[i];
    }
    db->setSetting(lk.token(), "leaderboard_excluded_source_ids", canonical.str());
    return true;
}

std::vector<LeaderboardEntry> contributorLeaderboard(
    Db* db, std::size_t limit, std::string* error) {
    if (error) error->clear();
    std::vector<LeaderboardEntry> rows;
    if (!db || limit == 0) return rows;
    limit = std::min<std::size_t>(limit, 100);
    auto lk = db->guard();
    std::vector<long long> excluded;
    if (!canonicalLeaderboardExclusionsLocked(lk.token(), db, excluded, error))
        return rows; // fail closed: malformed policy never exposes a name
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT s.id,s.name,COUNT(*),
       SUM(CASE WHEN i.status='completed' THEN 1 ELSE 0 END)
 FROM sources s
  JOIN item_sources x ON x.source_id=s.id
  JOIN items i ON i.id=x.item_id
 WHERE i.type IN ('fix','implementation') AND i.status!='merged'
   AND NOT EXISTS(SELECT 1 FROM source_merges m WHERE m.source_id=s.id)
 GROUP BY s.id,s.name
 ORDER BY 4 DESC,3 DESC,s.name COLLATE NOCASE,s.id
)sql");
    while (q.executeStep() && rows.size() < limit) {
        const long long sourceId = q.getColumn(0).getInt64();
        if (std::binary_search(excluded.begin(), excluded.end(), sourceId))
            continue;
        LeaderboardEntry entry;
        entry.sourceId = sourceId;
        entry.name = q.getColumn(1).getString();
        entry.submitted = q.getColumn(2).getInt64();
        entry.shipped = q.getColumn(3).getInt64();
        rows.push_back(std::move(entry));
    }
    return rows;
}

std::string compactLeaderboardDescription(
    const std::vector<LeaderboardEntry>& entries) {
    std::string description;
    const std::size_t count = std::min<std::size_t>(entries.size(), 10);
    for (std::size_t i = 0; i < count; ++i) {
        std::string name = entries[i].name;
        for (char& ch : name) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte < 0x80 &&
                (ch == '\r' || ch == '\n' || ch == '\t' || ch == '`' ||
                 ch == '*' || ch == '_' || ch == '~' || ch == '|' ||
                 ch == '[' || ch == ']' || ch == '<' || ch == '>' ||
                 ch == '\\' || ch == '@'))
                ch = ' ';
        }
        name = trim(name);
        if (name.empty()) name = "unknown contributor";
        name = utf8Prefix(name, 80);
        description += "**" + name + "** \xE2\x80\x94 " +
            std::to_string(entries[i].shipped) + "/" +
            std::to_string(entries[i].submitted) + " Implemented\n";
    }
    return description;
}

static IngestOutcome manualCaptureSuggestionLocked(
                                      Db::Held held, Db* db, const std::string& channelId,
                                      const std::string& channelName,
                                      const std::string& guildId,
                                      const std::string& guildName,
                                      const std::string& messageId,
                                      const std::string& author,
                                      const std::string& authorId,
                                      const std::string& content,
                                      const std::string& postedAt,
                                      const std::string& adminNote,
                                      const std::vector<DiscordAttachmentMeta>& attachments) {
    IngestOutcome out;
    if (channelId.empty() || messageId.empty()) return out;

    // Manual captures ignore the channel's enabled flag - the admin asked
    // for this specific message, that beats the automatic-detection switch.
    int enabled = 1;
    long long rowId = ensureChannelRowLocked(held, db, channelId, channelName,
                                             guildId, guildName, &enabled);

    long long msgRow = 0;
    long long existingItemId = 0;
    std::string kind, state, oldNote;
    {
        SQLite::Statement q(db->raw(held),
            "SELECT m.id,m.kind,m.state,m.admin_note,m.item_id,"
            "EXISTS(SELECT 1 FROM discord_notify_cards n "
            "WHERE n.discord_message_row_id=m.id) "
            "FROM discord_messages m WHERE m.message_id=?");
        q.bind(1, messageId);
        if (q.executeStep()) {
            msgRow = q.getColumn(0).getInt64();
            kind = q.getColumn(1).getString();
            state = q.getColumn(2).getString();
            oldNote = q.getColumn(3).getString();
            if (!q.getColumn(4).isNull())
                existingItemId = q.getColumn(4).getInt64();
            out.hasCard = q.getColumn(5).getInt() != 0;
        }
    }

    std::string note = trim(adminNote);
    long long inferred = inferProjectLocked(held, db, content);
    if (inferred == 0) inferred = inferProjectFromChannelLocked(held, db, channelName);

    if (msgRow != 0) {
        out.messageRowId = msgRow;
        // A Discord delete may race an authorized reply-target fetch. The
        // scrubbed tombstone is terminal and must never regain text, notes, or
        // attachment metadata from that delayed callback.
        if (state == "deleted") {
            out.itemId = existingItemId;
            out.duplicate = true;
            return out;
        }
        std::string newNote = oldNote;
        if (!note.empty()) newNote += (newNote.empty() ? "" : "\n") + note;

        // An exact reply to an already-promoted source enriches that same
        // item. New attachment identities are queued directly because there
        // will be no second Promote transition to move captured rows.
        if (state == "promoted") {
            out.itemId = existingItemId;
            if (existingItemId <= 0) {
                out.duplicate = true;
                return out;
            }
            long long itemProjectId = 0;
            {
                SQLite::Statement item(db->raw(held),
                    "SELECT project_id FROM items WHERE id=?");
                item.bind(1, existingItemId);
                if (!item.executeStep()) {
                    out.duplicate = true;
                    return out;
                }
                itemProjectId = item.getColumn(0).getInt64();
            }

            TicketAttachmentStageResult staged = stageTicketAttachmentsLocked(held,
                db, msgRow, attachments, existingItemId);
            out.imageCount = staged.total;
            out.imagesQueued = staged.changed;
            out.appendedToItem = !note.empty() || staged.changed > 0;
            if (!out.appendedToItem) {
                out.duplicate = true;
                return out;
            }

            if (!note.empty()) {
                SQLite::Statement updateMessage(db->raw(held),
                    "UPDATE discord_messages SET admin_note=? WHERE id=?");
                updateMessage.bind(1, newNote);
                updateMessage.bind(2, msgRow);
                updateMessage.exec();
            }
            const std::string bodyAppend = note.empty()
                ? std::string()
                : "\n\n-- Discord follow-up:\n" + note;
            SQLite::Statement updateItem(db->raw(held),
                "UPDATE items SET body=body||?,updated_at=? WHERE id=?");
            updateItem.bind(1, bodyAppend);
            updateItem.bind(2, nowIsoUtc());
            updateItem.bind(3, existingItemId);
            updateItem.exec();
            db->logActivity(held,
                "discord_evidence_appended", itemProjectId,
                "I" + std::to_string(existingItemId) + ": " +
                std::to_string(staged.changed) + " new attachment(s)" +
                (note.empty() ? "" : " and a follow-up note"));
            out.kind = "suggestion";
            out.score = 1.0;
            appLog("[manual] appended Discord evidence to I" +
                   std::to_string(existingItemId) + " (" +
                   std::to_string(staged.changed) + " attachment(s)" +
                   (note.empty() ? "" : ", note") + ")");
            return out;
        }
        out.imageCount = stageTicketAttachmentsLocked(held,
            db, msgRow, attachments).total;
        if (kind == "suggestion" && state == "new") {
            // Already waiting in the inbox - just attach the note.
            SQLite::Statement up(db->raw(held),
                "UPDATE discord_messages SET content=?, admin_note=? WHERE id=?");
            up.bind(1, content);
            up.bind(2, newNote);
            up.bind(3, msgRow);
            up.exec();
            out.duplicate = true;
            return out;
        }
        // Captured as none/bug or previously dismissed - upgrade to a
        // manual suggestion back in the inbox.
        SQLite::Statement up(db->raw(held),
            "UPDATE discord_messages SET content=?, kind='suggestion', score=1.0, "
            "matched=?, state='new', admin_note=?, "
            "inferred_project_id=CASE WHEN inferred_project_id IS NULL "
            "AND ?>0 THEN ? ELSE inferred_project_id END WHERE id=?");
        up.bind(1, content);
        up.bind(2, json::array({"manual"}).dump());
        up.bind(3, newNote);
        up.bind(4, inferred);
        up.bind(5, inferred);
        up.bind(6, msgRow);
        up.exec();
        out.ingested = true;
    } else {
        SQLite::Statement ins(db->raw(held),
            "INSERT INTO discord_messages(channel_row_id,message_id,author,"
            "author_id,content,posted_at,ingested_at,kind,score,matched,"
            "inferred_project_id,admin_note) VALUES(?,?,?,?,?,?,?,"
            "'suggestion',1.0,?,?,?)");
        ins.bind(1, rowId);
        ins.bind(2, messageId);
        ins.bind(3, author);
        ins.bind(4, authorId);
        ins.bind(5, content);
        ins.bind(6, postedAt);
        ins.bind(7, nowIsoUtc());
        ins.bind(8, json::array({"manual"}).dump());
        if (inferred > 0) ins.bind(9, inferred); else ins.bind(9);
        ins.bind(10, note);
        ins.exec();
        out.messageRowId = db->raw(held).getLastInsertRowid();
        out.imageCount = stageTicketAttachmentsLocked(held,
            db, out.messageRowId, attachments).total;
        out.ingested = true;
        if (!postedAt.empty()) {
            SQLite::Statement up(db->raw(held),
                "UPDATE discord_channels SET last_read_ts=MAX(last_read_ts,?), "
                "last_scan_at=? WHERE id=?");
            up.bind(1, postedAt);
            up.bind(2, nowIsoUtc());
            up.bind(3, rowId);
            up.exec();
        }
    }
    out.kind = "suggestion";
    out.score = 1.0;
    appLog("[manual] suggestion captured from " + author + " in #" +
           (channelName.empty() ? channelId : channelName) +
           (note.empty() ? "" : " (with note)"));
    // The pending discord_messages row is already projected into Recent
    // activity and disappears on promote/dismiss/delete. Do not duplicate
    // author/content into the permanent, unlinked activity history.
    return out;
}

IngestOutcome manualCaptureSuggestion(Db* db, const std::string& channelId,
                                      const std::string& channelName,
                                      const std::string& guildId,
                                      const std::string& guildName,
                                      const std::string& messageId,
                                      const std::string& author,
                                      const std::string& authorId,
                                      const std::string& content,
                                      const std::string& postedAt,
                                      const std::string& adminNote,
                                      const std::vector<DiscordAttachmentMeta>& attachments) {
    IngestOutcome out;
    if (!db || channelId.empty() || messageId.empty()) return out;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    out = manualCaptureSuggestionLocked(lk.token(),
        db, channelId, channelName, guildId, guildName, messageId, author,
        authorId, content, postedAt, adminNote, attachments);
    tx.commit();
    return out;
}

static std::string boundedManualCommandError(std::string value) {
    value = trim(redactHttpUrls(value));
    for (char& ch : value)
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    if (value.empty()) value = "Discord target fetch failed";
    if (value.size() > 500) value = utf8Prefix(value, 497) + "...";
    return value;
}

ManualCommandStageOutcome stageManualCaptureCommand(
    Db* db, const std::string& channelId, const std::string& channelName,
    const std::string& guildId, const std::string& guildName,
    const std::string& commandMessageId, const std::string& adminName,
    const std::string& adminId, const std::string& commandContent,
    const std::string& postedAt, const std::string& targetMessageId,
    const std::string& note,
    const std::vector<DiscordAttachmentMeta>& attachments) {
    ManualCommandStageOutcome out;
    if (!db || channelId.empty() || commandMessageId.empty() ||
        targetMessageId.empty()) return out;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    int enabled = 1;
    const long long channelRow = ensureChannelRowLocked(lk.token(),
        db, channelId, channelName, guildId, guildName, &enabled);

    long long commandRow = 0;
    std::string lifecycle;
    std::string sourceState;
    {
        SQLite::Statement existing(db->raw(lk.token()),
            "SELECT id,manual_command_state,state FROM discord_messages "
            "WHERE message_id=?");
        existing.bind(1, commandMessageId);
        if (existing.executeStep()) {
            commandRow = existing.getColumn(0).getInt64();
            lifecycle = existing.getColumn(1).getString();
            sourceState = existing.getColumn(2).getString();
            out.duplicate = true;
        }
    }
    if (commandRow == 0) {
        SQLite::Statement ins(db->raw(lk.token()),
            "INSERT INTO discord_messages(channel_row_id,message_id,author,"
            "author_id,content,posted_at,ingested_at,kind,score,matched,"
            "admin_note,manual_target_message_id,manual_command_state) "
            "VALUES(?,?,?,?,?,?,?,'none',0,'[]',?,?,'pending')");
        ins.bind(1, channelRow);
        ins.bind(2, commandMessageId);
        ins.bind(3, adminName);
        ins.bind(4, adminId);
        ins.bind(5, commandContent);
        ins.bind(6, postedAt);
        ins.bind(7, nowIsoUtc());
        ins.bind(8, trim(note));
        ins.bind(9, targetMessageId);
        ins.exec();
        commandRow = db->raw(lk.token()).getLastInsertRowid();
        lifecycle = "pending";
    } else if (sourceState == "deleted") {
        // The command message itself was deleted while a gateway/backfill
        // callback was still in flight. Its scrubbed marker is terminal.
        lifecycle = "deleted";
    } else if (lifecycle.empty()) {
        // A v10-era command can be recovered when Discord re-delivers it: the
        // current event supplies the target/note that the old row lacked.
        SQLite::Statement recover(db->raw(lk.token()),
            "UPDATE discord_messages SET author=?,author_id=?,content=?,"
            "posted_at=?,admin_note=?,manual_target_message_id=?,"
            "manual_command_state='pending',manual_attempts=0,"
            "manual_next_retry_at='',manual_last_error='' WHERE id=?");
        recover.bind(1, adminName);
        recover.bind(2, adminId);
        recover.bind(3, commandContent);
        recover.bind(4, postedAt);
        recover.bind(5, trim(note));
        recover.bind(6, targetMessageId);
        recover.bind(7, commandRow);
        recover.exec();
        lifecycle = "pending";
    }

    if (lifecycle == "pending")
        stageTicketAttachmentsLocked(lk.token(), db, commandRow, attachments);
    if (!postedAt.empty()) {
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE discord_channels SET last_read_ts=MAX(last_read_ts,?),"
            "last_scan_at=? WHERE id=?");
        up.bind(1, postedAt);
        up.bind(2, nowIsoUtc());
        up.bind(3, channelRow);
        up.exec();
    }
    tx.commit();

    out.accepted = lifecycle == "pending" || lifecycle == "done" ||
                   lifecycle == "failed";
    out.state = lifecycle;
    out.command.commandRowId = commandRow;
    out.command.commandMessageId = commandMessageId;
    out.command.targetMessageId = targetMessageId;
    out.command.channelId = channelId;
    out.command.channelName = channelName;
    out.command.guildId = guildId;
    out.command.guildName = guildName;
    out.command.adminName = adminName;
    out.command.adminId = adminId;
    out.command.note = trim(note);
    out.command.postedAt = postedAt;
    out.command.commandAttachments = attachments;
    return out;
}

bool nextPendingManualCaptureCommand(Db* db, ManualCaptureCommand& command) {
    command = {};
    if (!db) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT m.id,m.message_id,m.manual_target_message_id,c.channel_id,c.channel_name,
       c.guild_id,c.guild_name,m.author,m.author_id,m.admin_note,m.posted_at,
       m.manual_attempts
  FROM discord_messages m
  JOIN discord_channels c ON c.id=m.channel_row_id
 WHERE m.manual_command_state='pending'
   AND (m.manual_next_retry_at='' OR
        datetime(m.manual_next_retry_at)<=datetime('now'))
 ORDER BY m.ingested_at,m.id LIMIT 1
)sql");
    if (!q.executeStep()) return false;
    int c = 0;
    command.commandRowId = q.getColumn(c++).getInt64();
    command.commandMessageId = q.getColumn(c++).getString();
    command.targetMessageId = q.getColumn(c++).getString();
    command.channelId = q.getColumn(c++).getString();
    command.channelName = q.getColumn(c++).getString();
    command.guildId = q.getColumn(c++).getString();
    command.guildName = q.getColumn(c++).getString();
    command.adminName = q.getColumn(c++).getString();
    command.adminId = q.getColumn(c++).getString();
    command.note = q.getColumn(c++).getString();
    command.postedAt = q.getColumn(c++).getString();
    command.attempts = q.getColumn(c++).getInt();

    SQLite::Statement images(db->raw(lk.token()),
        "SELECT attachment_id,source_channel_id,source_message_id,source_role,"
        "original_filename,content_type,declared_size,width,height "
        "FROM ticket_attachments WHERE discord_message_row_id=? "
        "AND state='captured' ORDER BY id");
    images.bind(1, command.commandRowId);
    while (images.executeStep()) {
        DiscordAttachmentMeta meta;
        int i = 0;
        meta.attachmentId = images.getColumn(i++).getString();
        meta.sourceChannelId = images.getColumn(i++).getString();
        meta.sourceMessageId = images.getColumn(i++).getString();
        meta.sourceRole = images.getColumn(i++).getString();
        meta.filename = images.getColumn(i++).getString();
        meta.contentType = images.getColumn(i++).getString();
        meta.sizeBytes = static_cast<std::uint64_t>(
            std::max<long long>(0, images.getColumn(i++).getInt64()));
        meta.width = static_cast<std::uint32_t>(
            std::max(0, images.getColumn(i++).getInt()));
        meta.height = static_cast<std::uint32_t>(
            std::max(0, images.getColumn(i++).getInt()));
        command.commandAttachments.push_back(std::move(meta));
    }
    return command.commandRowId > 0 && !command.targetMessageId.empty();
}

ManualCommandFailureOutcome recordManualCaptureCommandFailure(
    Db* db, long long commandRowId, const std::string& error, bool retryable) {
    ManualCommandFailureOutcome out;
    if (!db || commandRowId <= 0) return out;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    int attempts = 0;
    std::string state;
    {
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT manual_command_state,manual_attempts FROM discord_messages "
            "WHERE id=?");
        q.bind(1, commandRowId);
        if (!q.executeStep()) return out;
        state = q.getColumn(0).getString();
        attempts = q.getColumn(1).getInt();
    }
    if (state != "pending") {
        out.terminal = state == "failed";
        out.attempts = attempts;
        tx.commit();
        return out;
    }
    attempts = std::max(0, attempts) + 1;
    constexpr int kMaxManualCaptureAttempts = 5;
    out.terminal = !retryable || attempts >= kMaxManualCaptureAttempts;
    out.retryScheduled = !out.terminal;
    out.attempts = attempts;
    out.error = boundedManualCommandError(error);

    std::string nextRetry;
    if (out.retryScheduled) {
        const int delay = attempts == 1 ? 5 : attempts == 2 ? 15
                         : attempts == 3 ? 60 : 300;
        SQLite::Statement due(db->raw(lk.token()),
            "SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now','+' || ? || ' seconds')");
        due.bind(1, delay);
        due.executeStep();
        nextRetry = due.getColumn(0).getString();
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_messages SET manual_command_state=?,manual_attempts=?,"
        "manual_next_retry_at=?,manual_last_error=? "
        "WHERE id=? AND manual_command_state='pending'");
    up.bind(1, out.terminal ? "failed" : "pending");
    up.bind(2, attempts);
    up.bind(3, nextRetry);
    up.bind(4, out.error);
    up.bind(5, commandRowId);
    up.exec();
    tx.commit();
    return out;
}

IngestOutcome completeManualCaptureCommand(
    Db* db, const ManualCaptureCommand& command,
    const std::string& targetAuthor, const std::string& targetAuthorId,
    const std::string& targetContent, const std::string& targetPostedAt,
    const std::vector<DiscordAttachmentMeta>& targetAttachments) {
    IngestOutcome out;
    if (!db || command.commandRowId <= 0 || command.targetMessageId.empty())
        return out;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    std::string state, target;
    {
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT manual_command_state,manual_target_message_id "
            "FROM discord_messages WHERE id=?");
        q.bind(1, command.commandRowId);
        if (!q.executeStep()) return out;
        state = q.getColumn(0).getString();
        target = q.getColumn(1).getString();
    }
    if (state != "pending" || target != command.targetMessageId) {
        out.duplicate = state == "done";
        return out;
    }
    out = manualCaptureSuggestionLocked(lk.token(),
        db, command.channelId, command.channelName, command.guildId,
        command.guildName, command.targetMessageId, targetAuthor,
        targetAuthorId, targetContent, targetPostedAt, command.note,
        targetAttachments);
    const std::string resultKind = out.appendedToItem
        ? "appended" : (out.ingested ? "captured" : "repeat");
    SQLite::Statement done(db->raw(lk.token()),
        "UPDATE discord_messages SET manual_command_state='done',"
        "manual_next_retry_at='',manual_last_error='',"
        "manual_result_kind=?,manual_result_message_row_id=?,"
        "manual_result_item_id=?,manual_result_images_queued=?,"
        "manual_effect_state='pending',manual_effect_attempts=0,"
        "manual_effect_next_retry_at='',manual_effect_error='',"
        "manual_effect_updated_at=? "
        "WHERE id=? AND manual_command_state='pending' "
        "AND manual_target_message_id=?");
    done.bind(1, resultKind);
    done.bind(2, out.messageRowId);
    done.bind(3, out.itemId);
    done.bind(4, std::max(0, out.imagesQueued));
    done.bind(5, nowIsoUtc());
    done.bind(6, command.commandRowId);
    done.bind(7, command.targetMessageId);
    if (done.exec() != 1)
        throw std::runtime_error("manual capture command lost its pending state");
    tx.commit();
    return out;
}

bool nextPendingManualCaptureEffect(Db* db, ManualCaptureEffect& effect) {
    effect = {};
    if (!db) return false;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    // A missing REST callback must not hold an effect forever. The fifth
    // expired lease is terminal and remains inspectable on the command row.
    db->raw(lk.token()).exec(R"sql(
UPDATE discord_messages
   SET manual_effect_state='failed',
       manual_effect_error='Discord acknowledgement callback timed out after five attempts',
       manual_effect_next_retry_at='',
       manual_effect_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE manual_command_state='done'
   AND manual_effect_state='posting'
   AND manual_effect_attempts>=5
   AND datetime(CASE WHEN manual_effect_updated_at=''
                     THEN ingested_at ELSE manual_effect_updated_at END)
       <=datetime('now','-45 seconds');
)sql");

    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT m.id,m.message_id,c.channel_id,c.channel_name,c.guild_id,c.guild_name,
       m.author,m.manual_result_kind,m.manual_result_message_row_id,
       m.manual_result_item_id,m.manual_result_images_queued,m.manual_effect_attempts,
       m.manual_target_message_id,COALESCE(r.author,''),COALESCE(r.content,''),
       COALESCE(r.kind,'suggestion'),COALESCE(r.score,1.0)
  FROM discord_messages m
  JOIN discord_channels c ON c.id=m.channel_row_id
  LEFT JOIN discord_messages r ON r.id=m.manual_result_message_row_id
 WHERE m.manual_command_state='done'
   AND m.manual_effect_attempts<5
   AND ((m.manual_effect_state='pending' AND
         (m.manual_effect_next_retry_at='' OR
          datetime(m.manual_effect_next_retry_at)<=datetime('now')))
        OR
        (m.manual_effect_state='posting' AND
         datetime(CASE WHEN m.manual_effect_updated_at=''
                       THEN m.ingested_at ELSE m.manual_effect_updated_at END)
         <=datetime('now','-45 seconds')))
 ORDER BY m.ingested_at,m.id LIMIT 1
)sql");
    if (!q.executeStep()) {
        tx.commit();
        return false;
    }
    int c = 0;
    effect.commandRowId = q.getColumn(c++).getInt64();
    effect.commandMessageId = q.getColumn(c++).getString();
    effect.channelId = q.getColumn(c++).getString();
    effect.channelName = q.getColumn(c++).getString();
    effect.guildId = q.getColumn(c++).getString();
    effect.guildName = q.getColumn(c++).getString();
    effect.adminName = q.getColumn(c++).getString();
    effect.resultKind = q.getColumn(c++).getString();
    effect.messageRowId = q.getColumn(c++).getInt64();
    effect.itemId = q.getColumn(c++).getInt64();
    effect.imagesQueued = q.getColumn(c++).getInt();
    effect.attempt = std::max(0, q.getColumn(c++).getInt()) + 1;
    effect.targetMessageId = q.getColumn(c++).getString();
    effect.targetAuthor = q.getColumn(c++).getString();
    effect.targetContent = q.getColumn(c++).getString();
    effect.targetKind = q.getColumn(c++).getString();
    effect.targetScore = q.getColumn(c++).getDouble();

    SQLite::Statement claim(db->raw(lk.token()),
        "UPDATE discord_messages SET manual_effect_state='posting',"
        "manual_effect_attempts=?,manual_effect_next_retry_at='',"
        "manual_effect_updated_at=? WHERE id=? AND manual_command_state='done' "
        "AND manual_effect_attempts=? AND manual_effect_state IN ('pending','posting')");
    claim.bind(1, effect.attempt);
    claim.bind(2, nowIsoUtc());
    claim.bind(3, effect.commandRowId);
    claim.bind(4, effect.attempt - 1);
    if (claim.exec() != 1) {
        tx.commit();
        effect = {};
        return false;
    }
    tx.commit();
    return true;
}

ManualCommandFailureOutcome recordManualCaptureEffectFailure(
    Db* db, long long commandRowId, int attempt, const std::string& error,
    bool retryable) {
    ManualCommandFailureOutcome out;
    if (!db || commandRowId <= 0 || attempt <= 0) return out;
    out.attempts = attempt;
    out.error = boundedManualCommandError(error);
    constexpr int kMaxManualEffectAttempts = 5;
    out.terminal = !retryable || attempt >= kMaxManualEffectAttempts;
    out.retryScheduled = !out.terminal;
    std::string nextRetry;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    if (out.retryScheduled) {
        const int delay = attempt == 1 ? 5 : attempt == 2 ? 15
                         : attempt == 3 ? 60 : 300;
        SQLite::Statement due(db->raw(lk.token()),
            "SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now','+' || ? || ' seconds')");
        due.bind(1, delay);
        due.executeStep();
        nextRetry = due.getColumn(0).getString();
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_messages SET manual_effect_state=?,"
        "manual_effect_next_retry_at=?,manual_effect_error=?,"
        "manual_effect_updated_at=? WHERE id=? "
        "AND manual_effect_state='posting' AND manual_effect_attempts=?");
    up.bind(1, out.terminal ? "failed" : "pending");
    up.bind(2, nextRetry);
    up.bind(3, out.error);
    up.bind(4, nowIsoUtc());
    up.bind(5, commandRowId);
    up.bind(6, attempt);
    if (up.exec() != 1) {
        out.retryScheduled = false;
        out.terminal = false;
    }
    tx.commit();
    return out;
}

bool completeManualCaptureEffect(Db* db, long long commandRowId, int attempt) {
    if (!db || commandRowId <= 0 || attempt <= 0) return false;
    auto lk = db->guard();
    SQLite::Statement done(db->raw(lk.token()),
        "UPDATE discord_messages SET manual_effect_state='done',"
        "manual_effect_next_retry_at='',manual_effect_error='',"
        "manual_effect_updated_at=? WHERE id=? "
        "AND manual_effect_state='posting' AND manual_effect_attempts=?");
    done.bind(1, nowIsoUtc());
    done.bind(2, commandRowId);
    done.bind(3, attempt);
    return done.exec() == 1;
}

std::vector<TicketMenuEntry> ticketMenuProjects(Db* db) {
    std::vector<TicketMenuEntry> out;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT p.id, p.name, COUNT(i.id) "
        "FROM projects p "
        "JOIN items i ON i.project_id=p.id "
        " AND i.status IN ('open','in_progress','blocked') "
        " AND i.type IN ('fix','implementation') "
        "WHERE p.archived=0 AND p.discord_tickets=1 "
        "GROUP BY p.id, p.name HAVING COUNT(i.id)>0 "
        "ORDER BY p.name COLLATE NOCASE");
    while (q.executeStep()) {
        TicketMenuEntry e;
        e.projectId = q.getColumn(0).getInt64();
        e.name = q.getColumn(1).getString();
        e.openCount = q.getColumn(2).getInt();
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<TicketLine> ticketTitlesForProject(Db* db, long long projectId) {
    std::vector<TicketLine> out;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT type, title FROM items WHERE project_id=? "
        "AND status IN ('open','in_progress','blocked') "
        "AND type IN ('fix','implementation') "
        "ORDER BY CASE type WHEN 'fix' THEN 0 ELSE 1 END, id");
    q.bind(1, projectId);
    while (q.executeStep()) {
        TicketLine t;
        t.type = q.getColumn(0).getString();
        t.title = q.getColumn(1).getString();
        out.push_back(std::move(t));
    }
    return out;
}

void discordMessageDeleted(Db* db, DiscordBot* bot,
                           const std::string& messageId) {
    if (messageId.empty()) return;
    long long rowId = 0;
    long long itemId = 0;
    long long cardRowId = 0;
    std::string author, state, kind, notifyChan, notifyMsg;
    bool tombstoneQueued = false;
    {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        {
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT m.id,m.author,m.state,m.kind,"
                "COALESCE(NULLIF(n.notify_channel_id,''),m.notify_channel_id),"
                "COALESCE(NULLIF(n.notify_message_id,''),m.notify_message_id),"
                "m.item_id,n.id "
                "FROM discord_messages m LEFT JOIN discord_notify_cards n "
                "ON n.discord_message_row_id=m.id WHERE m.message_id=?");
            q.bind(1, messageId);
            if (!q.executeStep()) return;
            rowId = q.getColumn(0).getInt64();
            author = q.getColumn(1).getString();
            state = q.getColumn(2).getString();
            kind = q.getColumn(3).getString();
            notifyChan = q.getColumn(4).getString();
            notifyMsg = q.getColumn(5).getString();
            if (!q.getColumn(6).isNull()) itemId = q.getColumn(6).getInt64();
            if (!q.getColumn(7).isNull())
                cardRowId = q.getColumn(7).getInt64();
        }
        SQLite::Statement delAttachments(db->raw(lk.token()),
            "DELETE FROM ticket_attachments "
            "WHERE discord_message_row_id=? AND item_id IS NULL");
        delAttachments.bind(1, rowId);
        delAttachments.exec();

        // Repair a legacy card into canonical lineage before scrubbing the
        // source. The card stores only bot-owned delivery identity; no deleted
        // user content is copied into the durable outbox.
        if (cardRowId == 0 && !notifyChan.empty() && !notifyMsg.empty()) {
            const std::string now = nowIsoUtc();
            SQLite::Statement repair(db->raw(lk.token()),
                "INSERT OR IGNORE INTO discord_notify_cards("
                "discord_message_row_id,item_id,notify_channel_id,"
                "notify_message_id,post_state,created_at,updated_at) "
                "VALUES(?,?,?,?,'posted',?,?)");
            repair.bind(1, rowId);
            if (itemId > 0) repair.bind(2, itemId); else repair.bind(2);
            repair.bind(3, notifyChan);
            repair.bind(4, notifyMsg);
            repair.bind(5, now);
            repair.bind(6, now);
            repair.exec();
            SQLite::Statement repaired(db->raw(lk.token()),
                "SELECT id FROM discord_notify_cards "
                "WHERE discord_message_row_id=?");
            repaired.bind(1, rowId);
            if (repaired.executeStep())
                cardRowId = repaired.getColumn(0).getInt64();
        }

        // Retain only message/channel identity plus item/card lineage. The
        // terminal marker prevents a delayed gateway/history/reply callback
        // from re-inserting the deleted Discord source under the same ID.
        SQLite::Statement scrub(db->raw(lk.token()),
            "UPDATE discord_messages SET state='deleted',author='',"
            "author_id='',content='',posted_at='',ingested_at='',kind=?,"
            "score=0,matched='[]',admin_note='',inferred_project_id=NULL,"
            "manual_target_message_id='',manual_command_state='',"
            "manual_attempts=0,manual_next_retry_at='',manual_last_error='',"
            "manual_result_kind='',manual_result_message_row_id=0,"
            "manual_result_item_id=0,manual_result_images_queued=0,"
            "manual_effect_state='',manual_effect_attempts=0,"
            "manual_effect_next_retry_at='',manual_effect_error='',"
            "manual_effect_updated_at='' "
            "WHERE id=?");
        scrub.bind(1, kind.empty() ? "message" : kind);
        scrub.bind(2, rowId);
        tombstoneQueued = scrub.exec() == 1;
        tx.commit();
    }
    if (itemId > 0) {
        // The promoted item and bot-owned card survive; immediately remove the
        // dead Discord jump/source text from the composed card.
        refreshTicketNotifyCard(db, bot, itemId);
        // A source scrub can also invalidate an in-flight create generation;
        // wake the replacement create even though no posted edit was dirtied.
        if (tombstoneQueued && bot) bot->requestNotifyCardPost();
    } else if (tombstoneQueued && bot) {
        // The schema trigger queued the revision in the scrub transaction.
        // Wake the worker; reconnect/startup polling owns offline recovery.
        bot->requestNotifyCardPost();
    }
    appLog("[discord] message by " + author +
           " was deleted on Discord - captured copy " +
           (tombstoneQueued ? "scrubbed; card tombstone queued" : "removed") +
           " (was '" + state + "')" +
           (itemId > 0 ? "; promoted item/card retained" : ""));
}

// ---------------------------------------------------------------------------
// notification cards: the bot posts one embed per detection into the
// configured notify channel and edits that same card as the capture moves
// through its life (promoted -> completed / dismissed / deleted).
// ---------------------------------------------------------------------------
static std::string jumpUrl(const std::string& guildId,
                           const std::string& channelId,
                           const std::string& messageId) {
    if (guildId.empty() || channelId.empty() || messageId.empty()) return {};
    return "https://discord.com/channels/" + guildId + "/" + channelId + "/" +
           messageId;
}

static std::string snippet(const std::string& s, size_t n = 1200) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && cut < s.size() &&
           (static_cast<unsigned char>(s[cut]) & 0xc0) == 0x80)
        --cut;
    return s.substr(0, cut) + "...[omitted " +
           std::to_string(s.size() - cut) + " UTF-8 bytes]";
}

void notifyDetection(Db* db, DiscordBot* bot, long long messageRowId,
                     const std::string& guildId, const std::string& channelId,
                     const std::string& channelName,
                     const std::string& messageId, const std::string& author,
                     const std::string& content, const std::string& kind,
                     double score) {
    if (!bot || messageRowId == 0) return;
    (void)guildId; (void)channelId; (void)channelName; (void)messageId;
    (void)author; (void)content; (void)kind; (void)score;
    {
        auto lk = db->guard();
        const std::string notifyChan = trim(db->getSetting(lk.token(), "notify_channel_id"));
        if (!notifyChan.empty()) {
            const std::string now = nowIsoUtc();
            SQLite::Statement ins(db->raw(lk.token()),
                "INSERT OR IGNORE INTO discord_notify_cards("
                "discord_message_row_id,item_id,notify_channel_id,"
                "notify_message_id,post_state,created_at,updated_at) "
                "SELECT id,item_id,?,'','pending',?,? "
                "FROM discord_messages WHERE id=?");
            ins.bind(1, notifyChan);
            ins.bind(2, now);
            ins.bind(3, now);
            ins.bind(4, messageRowId);
            ins.exec();
        }
    }
    bot->requestNotifyCardPost();
}

static bool ensureTicketNotifyCardLocked(Db::Held held, Db* db, long long messageRowId,
                                         long long itemId) {
    if (!db || itemId <= 0) return false;
    const std::string notifyChan = trim(db->getSetting(held, "notify_channel_id"));
    if (notifyChan.empty()) return false;

    if (messageRowId > 0) {
        SQLite::Statement link(db->raw(held),
            "UPDATE discord_notify_cards SET item_id=?,updated_at=? "
            "WHERE discord_message_row_id=? AND item_id IS NULL");
        link.bind(1, itemId);
        link.bind(2, nowIsoUtc());
        link.bind(3, messageRowId);
        link.exec();
    }

    SQLite::Statement exists(db->raw(held),
        "SELECT EXISTS(SELECT 1 FROM discord_notify_cards "
        "WHERE item_id=? OR (? > 0 AND discord_message_row_id=?))");
    exists.bind(1, itemId);
    exists.bind(2, messageRowId);
    exists.bind(3, messageRowId);
    exists.executeStep();
    bool ready = exists.getColumn(0).getInt() != 0;
    if (!ready) {
        const std::string now = nowIsoUtc();
        SQLite::Statement source(db->raw(held),
            "INSERT OR IGNORE INTO discord_notify_cards("
            "discord_message_row_id,item_id,notify_channel_id,"
            "notify_message_id,post_state,created_at,updated_at) "
            "SELECT id,?,?,'','pending',?,? FROM discord_messages "
            "WHERE id=?");
        source.bind(1, itemId);
        source.bind(2, notifyChan);
        source.bind(3, now);
        source.bind(4, now);
        source.bind(5, messageRowId);
        ready = source.exec() == 1;
        if (!ready) {
            // The approved source may already have been deleted. An item-owned
            // lineage intentionally carries no author/text URL.
            SQLite::Statement itemOnly(db->raw(held),
                "INSERT INTO discord_notify_cards(discord_message_row_id,"
                "item_id,notify_channel_id,notify_message_id,post_state,"
                "created_at,updated_at) VALUES(NULL,?,?,'','pending',?,?)");
            itemOnly.bind(1, itemId);
            itemOnly.bind(2, notifyChan);
            itemOnly.bind(3, now);
            itemOnly.bind(4, now);
            itemOnly.exec();
            ready = true;
        }
    }
    return ready;
}

void ensureTicketNotifyCard(Db* db, DiscordBot* bot, long long messageRowId,
                            long long itemId) {
    if (!db || itemId <= 0) return;
    bool ready = false;
    {
        auto lk = db->guard();
        ready = ensureTicketNotifyCardLocked(lk.token(), db, messageRowId, itemId);
    }
    if (ready && bot) bot->requestNotifyCardPost();
}

void sendNotifyTest(Db* db, DiscordBot* bot) {
    std::string notifyChan;
    {
        auto lk = db->guard();
        notifyChan = trim(db->getSetting(lk.token(), "notify_channel_id"));
    }
    if (!bot || notifyChan.empty()) {
        appLog("[notify] test skipped - no notify channel configured");
        return;
    }
    NotifyCard card;
    card.title = "\xE2\x9C\x85 XA DevHub bot notifications test";
    card.description =
        "Bot notifications are wired up. Every detected suggestion/bug posts "
        "a card like this one - and the SAME card is edited as you promote, "
        "complete, or dismiss it in DevHub.";
    card.color = 0x3fb950;
    card.footer = DEVHUB_APP_NAME;
    bot->postCard(notifyChan, card, nullptr);
    appLog("[notify] test card sent");
}

// Item refreshes are durable desired-state records. The worker composes the
// current card only after claiming the latest revision, so image/status races
// cannot leave an older fire-and-forget edit as the final Discord state.
void updateNotifyCardForItemLocked(Db::Held held, Db* db, DiscordBot* bot,
                                   long long itemId,
                                   const std::string& status) {
    (void)status;
    if (itemId == 0) return;
    SQLite::Statement dirty(db->raw(held),
        "UPDATE discord_notify_cards SET edit_state='pending',"
        "edit_attempts=0,edit_revision=edit_revision+1,"
        "edit_next_retry_at='',edit_last_error='',edit_updated_at=?,updated_at=? "
        "WHERE item_id=? AND notify_message_id!=''");
    const std::string now = nowIsoUtc();
    dirty.bind(1, now);
    dirty.bind(2, now);
    dirty.bind(3, itemId);
    if (dirty.exec() > 0 && bot) bot->requestNotifyCardPost();
}

void refreshTicketNotifyCard(Db* db, DiscordBot* bot, long long itemId) {
    if (!db || itemId <= 0) return;
    auto lk = db->guard();
    updateNotifyCardForItemLocked(lk.token(), db, bot, itemId, "");
}

bool retryFailedNotifyCard(Db* db, DiscordBot* bot, long long cardRowId) {
    if (!db || cardRowId <= 0) return false;
    int changed = 0;
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        const std::string now = nowIsoUtc();
        SQLite::Statement retryPost(db->raw(lk.token()),
            "UPDATE discord_notify_cards SET post_state='pending',"
            "post_attempts=0,post_revision=post_revision+1,"
            "post_next_retry_at='',post_last_error='',updated_at=? "
            "WHERE id=? AND post_state='failed' AND notify_message_id=''");
        retryPost.bind(1, now);
        retryPost.bind(2, cardRowId);
        changed += retryPost.exec();
        SQLite::Statement retryEdit(db->raw(lk.token()),
            "UPDATE discord_notify_cards SET edit_state='pending',"
            "edit_attempts=0,edit_revision=edit_revision+1,"
            "edit_next_retry_at='',edit_last_error='',edit_updated_at=?,updated_at=? "
            "WHERE id=? AND edit_state='failed' AND notify_message_id!=''");
        retryEdit.bind(1, now);
        retryEdit.bind(2, now);
        retryEdit.bind(3, cardRowId);
        changed += retryEdit.exec();
        tx.commit();
    } catch (const std::exception& e) {
        appLog("[notify] failed-card retry could not be queued: " +
               redactHttpUrls(e.what()));
        return false;
    }
    if (changed > 0 && bot) bot->requestNotifyCardPost();
    return changed > 0;
}

bool recreateFailedNotifyCard(Db* db, DiscordBot* bot, long long cardRowId,
                              const std::string& expectedOperation,
                              int expectedRevision) {
    if (!db || cardRowId <= 0) return false;
    if (!expectedOperation.empty() &&
        (expectedRevision < 0 ||
         (expectedOperation != "create" && expectedOperation != "edit")))
        return false;
    int changed = 0;
    try {
        auto lk = db->guard();
        const std::string newChannelId = trim(db->getSetting(lk.token(), "notify_channel_id"));
        if (!isDecimalDiscordSnowflake(newChannelId)) return false;
        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement current(db->raw(lk.token()),
            "SELECT discord_message_row_id,notify_channel_id,notify_message_id,"
            "post_revision,edit_revision FROM discord_notify_cards WHERE id=? AND "
            "((post_state='failed' AND notify_message_id='') OR "
            " (post_state='posted' AND edit_state='failed' "
            "  AND notify_message_id!=''))");
        current.bind(1, cardRowId);
        if (!current.executeStep()) return false;
        const long long sourceRowId = current.getColumn(0).isNull()
            ? 0 : current.getColumn(0).getInt64();
        const std::string oldChannelId = current.getColumn(1).getString();
        const std::string oldMessageId = current.getColumn(2).getString();
        const int oldPostRevision = current.getColumn(3).getInt();
        const int oldEditRevision = current.getColumn(4).getInt();
        if (!expectedOperation.empty()) {
            const std::string currentOperation =
                oldMessageId.empty() ? "create" : "edit";
            const int currentRevision = currentOperation == "create"
                ? oldPostRevision : oldEditRevision;
            if (expectedOperation != currentOperation ||
                expectedRevision != currentRevision)
                return false;
        }

        const std::string now = nowIsoUtc();
        SQLite::Statement replace(db->raw(lk.token()),
            "UPDATE discord_notify_cards SET notify_channel_id=?,"
            "notify_message_id='',post_state='pending',post_attempts=0,"
            "post_revision=post_revision+1,post_next_retry_at='',"
            "post_last_error='',edit_state='idle',edit_attempts=0,"
            "edit_revision=edit_revision+1,edit_next_retry_at='',"
            "edit_last_error='',edit_updated_at=?,updated_at=? WHERE id=? AND "
            "notify_channel_id=? AND notify_message_id=? AND post_revision=? "
            "AND edit_revision=? AND "
            "((post_state='failed' AND notify_message_id='') OR "
            " (post_state='posted' AND edit_state='failed' "
            "  AND notify_message_id!=''))");
        replace.bind(1, newChannelId);
        replace.bind(2, now);
        replace.bind(3, now);
        replace.bind(4, cardRowId);
        replace.bind(5, oldChannelId);
        replace.bind(6, oldMessageId);
        replace.bind(7, oldPostRevision);
        replace.bind(8, oldEditRevision);
        changed = replace.exec();
        if (changed == 1 && sourceRowId > 0) {
            // Synchronize the compatibility receipt. Canonical ownership
            // remains the discord_notify_cards row above.
            SQLite::Statement legacy(db->raw(lk.token()),
                "UPDATE discord_messages SET notify_channel_id=?,"
                "notify_message_id='' WHERE id=?");
            legacy.bind(1, newChannelId);
            legacy.bind(2, sourceRowId);
            legacy.exec();
        }
        tx.commit();
    } catch (const std::exception& e) {
        appLog("[notify] replacement card could not be queued: " +
               redactHttpUrls(e.what()));
        return false;
    }
    if (changed != 1) return false;
    if (bot) bot->requestNotifyCardPost();
    return true;
}

bool dismissFailedNotifyCardAlert(Db* db, long long cardRowId,
                                  const std::string& operation,
                                  int expectedRevision) {
    if (!db || cardRowId <= 0 || expectedRevision < 0 ||
        (operation != "create" && operation != "edit"))
        return false;
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement dismiss(db->raw(lk.token()), R"sql(
INSERT OR IGNORE INTO discord_notify_failure_dismissals(
    card_row_id,operation,revision,dismissed_at,reason)
SELECT id,?,?,?,'operator'
  FROM discord_notify_cards
 WHERE id=?
   AND ((?='create' AND post_state='failed' AND post_revision=?)
     OR (?='edit' AND edit_state='failed' AND edit_revision=?)))sql");
        dismiss.bind(1, operation);
        dismiss.bind(2, expectedRevision);
        dismiss.bind(3, nowIsoUtc());
        dismiss.bind(4, cardRowId);
        dismiss.bind(5, operation);
        dismiss.bind(6, expectedRevision);
        dismiss.bind(7, operation);
        dismiss.bind(8, expectedRevision);
        dismiss.exec();

        // INSERT OR IGNORE is intentionally idempotent, but a dismissal is
        // successful only if this exact generation is still the live failure.
        SQLite::Statement current(db->raw(lk.token()), R"sql(
SELECT 1
  FROM discord_notify_failure_dismissals d
  JOIN discord_notify_cards n ON n.id=d.card_row_id
 WHERE d.card_row_id=? AND d.operation=? AND d.revision=?
   AND ((d.operation='create' AND n.post_state='failed'
         AND n.post_revision=d.revision)
     OR (d.operation='edit' AND n.edit_state='failed'
         AND n.edit_revision=d.revision)))sql");
        current.bind(1, cardRowId);
        current.bind(2, operation);
        current.bind(3, expectedRevision);
        const bool acknowledged = current.executeStep();
        tx.commit();
        return acknowledged;
    } catch (const std::exception& e) {
        appLog("[notify] failure alert could not be dismissed: " +
               redactHttpUrls(e.what()));
        return false;
    }
}

bool restoreFailedNotifyCardAlert(Db* db, long long cardRowId,
                                  const std::string& operation,
                                  int expectedRevision) {
    if (!db || cardRowId <= 0 || expectedRevision < 0 ||
        (operation != "create" && operation != "edit"))
        return false;
    try {
        auto lk = db->guard();
        SQLite::Statement restore(db->raw(lk.token()), R"sql(
DELETE FROM discord_notify_failure_dismissals
 WHERE card_row_id=? AND operation=? AND revision=?
   AND EXISTS(
       SELECT 1 FROM discord_notify_cards n
        WHERE n.id=?
          AND ((?='create' AND n.post_state='failed'
                AND n.post_revision=?)
            OR (?='edit' AND n.edit_state='failed'
                AND n.edit_revision=?))))sql");
        restore.bind(1, cardRowId);
        restore.bind(2, operation);
        restore.bind(3, expectedRevision);
        restore.bind(4, cardRowId);
        restore.bind(5, operation);
        restore.bind(6, expectedRevision);
        restore.bind(7, operation);
        restore.bind(8, expectedRevision);
        return restore.exec() == 1;
    } catch (const std::exception& e) {
        appLog("[notify] dismissed failure alert could not be restored: " +
               redactHttpUrls(e.what()));
        return false;
    }
}

NotifyFailureDismissResult dismissOldMessageNotifyCardAlerts(
    Db* db, const std::vector<NotifyFailureAlertTarget>& targets) {
    if (!db || targets.empty() ||
        targets.size() > kMaxNotifyFailureDismissBatch)
        return {};
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        int changed = 0;
        for (const NotifyFailureAlertTarget& target : targets) {
            if (target.cardRowId <= 0 || target.revision < 0)
                return {};
            SQLite::Statement dismiss(db->raw(lk.token()), R"sql(
INSERT OR IGNORE INTO discord_notify_failure_dismissals(
    card_row_id,operation,revision,dismissed_at,reason)
SELECT id,'edit',edit_revision,?,'operator-old-message-limit'
  FROM discord_notify_cards
 WHERE id=? AND edit_state='failed' AND edit_revision=?
   AND (instr(edit_last_error,
       'Maximum number of edits to messages older than 1 hour reached')>0
     OR instr(edit_last_error,'30046')>0))sql");
            dismiss.bind(1, nowIsoUtc());
            dismiss.bind(2, target.cardRowId);
            dismiss.bind(3, target.revision);
            changed += dismiss.exec();
        }
        tx.commit();
        return {true, changed};
    } catch (const std::exception& e) {
        appLog("[notify] old-message failure alerts could not be dismissed: " +
               redactHttpUrls(e.what()));
        return {};
    }
}

bool retryFailedNotifyOrphan(Db* db, DiscordBot* bot, long long orphanRowId) {
    if (!db || orphanRowId <= 0) return false;
    int changed = 0;
    try {
        auto lk = db->guard();
        SQLite::Statement retry(db->raw(lk.token()),
            "UPDATE discord_notify_orphans SET cleanup_state='pending',"
            "cleanup_attempts=0,cleanup_revision=cleanup_revision+1,"
            "cleanup_next_retry_at='',cleanup_last_error='',updated_at=? "
            "WHERE id=? AND cleanup_state='failed'");
        retry.bind(1, nowIsoUtc());
        retry.bind(2, orphanRowId);
        changed = retry.exec();
    } catch (const std::exception& e) {
        appLog("[notify] orphan cleanup retry could not be queued: " +
               redactHttpUrls(e.what()));
        return false;
    }
    if (changed == 1 && bot) bot->requestNotifyCardPost();
    return changed == 1;
}

bool retryFailedReviewControlCleanup(Db* db, DiscordBot* bot,
                                     long long cleanupRowId) {
    if (!db || cleanupRowId <= 0) return false;
    int changed = 0;
    try {
        auto lk = db->guard();
        SQLite::Statement retry(db->raw(lk.token()),
            "UPDATE discord_review_control_cleanups "
            "SET cleanup_state='pending',cleanup_attempts=0,"
            "cleanup_revision=cleanup_revision+1,cleanup_next_retry_at='',"
            "cleanup_last_error='',updated_at=? "
            "WHERE id=? AND cleanup_state='failed'");
        retry.bind(1, nowIsoUtc());
        retry.bind(2, cleanupRowId);
        changed = retry.exec();
    } catch (const std::exception& e) {
        appLog("[review] reaction cleanup retry could not be queued: " +
               redactHttpUrls(e.what()));
        return false;
    }
    if (changed == 1 && bot) bot->requestNotifyCardPost();
    return changed == 1;
}

namespace {

struct NotifyPostWork {
    long long rowId = 0;
    int attempt = 0;
    int revision = 0;
    std::string channelId;
    std::string messageId;
    long long itemId = 0;
    bool pendingReviewControls = false;
    bool removeReviewControls = false;
    NotifyCard card;
};

struct NotifyEditWork {
    NotifyPostWork current;
    int revision = 0;
    int attempt = 0;
};

struct NotifyOrphanWork {
    long long rowId = 0;
    long long cardRowId = 0;
    int attempt = 0;
    int revision = 0;
    std::string channelId;
    std::string messageId;
};

struct ReviewControlCleanupWork {
    long long rowId = 0;
    long long cardRowId = 0;
    int attempt = 0;
    int revision = 0;
    std::string channelId;
    std::string messageId;
};

static std::string ticketAttachmentSummaryLocked(Db::Held held, Db* db, long long itemId) {
    SQLite::Statement q(db->raw(held),
        "SELECT COUNT(*),COALESCE(SUM(state='saved'),0),"
        "COALESCE(SUM(state='queued'),0),COALESCE(SUM(state='failed'),0) "
        "FROM ticket_attachments WHERE item_id=?");
    q.bind(1, itemId);
    if (!q.executeStep() || q.getColumn(0).getInt() == 0) return {};
    return std::to_string(q.getColumn(1).getInt()) + " saved, " +
           std::to_string(q.getColumn(2).getInt()) + " queued, " +
           std::to_string(q.getColumn(3).getInt()) + " failed";
}

static bool buildNotifyPostCardLocked(Db::Held held, Db* db, long long cardRowId,
                                      NotifyPostWork& work) {
    SQLite::Statement lineage(db->raw(held), R"sql(
SELECT n.notify_channel_id,n.notify_message_id,n.item_id,m.id,COALESCE(m.state,''),
       COALESCE(m.author,''),COALESCE(m.content,''),COALESCE(m.admin_note,''),
       COALESCE(m.kind,''),
       COALESCE(m.score,0),COALESCE(m.message_id,''),
       COALESCE(c.guild_id,''),COALESCE(c.channel_id,''),
       COALESCE(c.channel_name,''),
       COALESCE(c.project_id,m.inferred_project_id,0),COALESCE(p.name,'')
  FROM discord_notify_cards n
  LEFT JOIN discord_messages m ON m.id=n.discord_message_row_id
  LEFT JOIN discord_channels c ON c.id=m.channel_row_id
  LEFT JOIN projects p ON p.id=COALESCE(c.project_id,m.inferred_project_id)
 WHERE n.id=?
)sql");
    lineage.bind(1, cardRowId);
    if (!lineage.executeStep()) return false;
    int c = 0;
    work.rowId = cardRowId;
    work.channelId = lineage.getColumn(c++).getString();
    work.messageId = lineage.getColumn(c++).getString();
    if (!lineage.getColumn(c).isNull())
        work.itemId = lineage.getColumn(c).getInt64();
    ++c;
    const bool sourcePresent = !lineage.getColumn(c).isNull();
    ++c;
    const std::string sourceState = lineage.getColumn(c++).getString();
    const std::string author = lineage.getColumn(c++).getString();
    const std::string content = lineage.getColumn(c++).getString();
    const std::string adminNote = lineage.getColumn(c++).getString();
    const std::string kind = lineage.getColumn(c++).getString();
    const double score = lineage.getColumn(c++).getDouble();
    const std::string messageId = lineage.getColumn(c++).getString();
    const std::string guildId = lineage.getColumn(c++).getString();
    const std::string channelId = lineage.getColumn(c++).getString();
    const std::string channelName = lineage.getColumn(c++).getString();
    const long long pendingProjectId = lineage.getColumn(c++).getInt64();
    const std::string pendingProject = lineage.getColumn(c++).getString();
    const bool sourceAvailable = sourcePresent && sourceState != "deleted";
    const bool reviewSource = kind == "suggestion" || kind == "bug";
    work.pendingReviewControls = sourcePresent && work.itemId <= 0 &&
                                 reviewSource && sourceState == "new";
    work.removeReviewControls = work.itemId > 0 ||
                                (sourcePresent && reviewSource &&
                                 sourceState != "new");

    if (work.itemId > 0) {
        SQLite::Statement item(db->raw(held),
            "SELECT p.name,i.type,i.title,i.status FROM items i "
            "JOIN projects p ON p.id=i.project_id WHERE i.id=?");
        item.bind(1, work.itemId);
        if (!item.executeStep()) return false;
        const std::string project = item.getColumn(0).getString();
        const std::string type = item.getColumn(1).getString();
        const std::string title = item.getColumn(2).getString();
        const std::string status = item.getColumn(3).getString();
        work.card.description = sourceAvailable
            ? snippet(content, 500)
            : "Original Discord source was deleted after promotion. DevHub "
              "retained the approved item: " + snippet(title, 350);
        if (sourceAvailable)
            work.card.url = jumpUrl(guildId, channelId, messageId);
        work.card.footer = DEVHUB_APP_NAME;
        work.card.fields = {{"project", project}, {"type", type},
                            {"by", sourceAvailable ? author
                                                  : "deleted Discord source"}};
        const std::string attachments =
            ticketAttachmentSummaryLocked(held, db, work.itemId);
        if (!attachments.empty())
            work.card.fields.push_back({"attachments", attachments});
        if (status == "completed") {
            work.card.title = "\xF0\x9F\x8E\x89 completed - " + project;
            work.card.color = 0x3fb950;
            work.card.fields.push_back({"status", "completed - shipping soon"});
        } else if (status == "wont_do") {
            work.card.title = "\xF0\x9F\x9A\xAB won't do - " + project;
            work.card.color = 0x8b949e;
            work.card.fields.push_back({"status", "closed as won't do"});
        } else {
            work.card.title = "\xE2\x9C\x85 promoted - " + project +
                              " (" + type + ")";
            work.card.color = 0x39c5cf;
            work.card.fields.push_back({"status", status + " - on the board"});
        }
        return !work.channelId.empty();
    }

    if (!sourcePresent) return false;
    work.card.footer = DEVHUB_APP_NAME;
    if (sourceState == "deleted") {
        work.card.title = "\xF0\x9F\x97\x91 deleted on Discord - " +
                          (kind.empty() ? "message" : kind);
        work.card.description =
            "Original Discord message was deleted before promotion.";
        work.card.color = 0x8b949e;
        work.card.fields = {{"status", "message was deleted"}};
    } else if (sourceState == "dismissed") {
        work.card.title = "\xF0\x9F\x97\x91 dismissed - " + kind;
        work.card.description = snippet(content, 500);
        work.card.color = 0x8b949e;
        work.card.fields = {{"author", author.empty() ? "?" : author},
                            {"status", "dismissed in DevHub"}};
    } else {
        const bool bug = kind == "bug";
        work.card.title = std::string(
            bug ? "\xF0\x9F\x90\x9E bug " : "\xF0\x9F\x92\xA1 suggestion ") +
            std::to_string(static_cast<int>(score * 100 + 0.5)) + "% - #" +
            (channelName.empty() ? channelId : channelName);
        work.card.description = snippet(content);
        work.card.color = bug ? 0xf85149 : 0x4f8ff7;
        work.card.fields = {{"author", author.empty() ? "?" : author},
                            {"status", "new - waiting in the inbox"}};
        if (!adminNote.empty())
            work.card.fields.push_back({"admin note", snippet(adminNote, 300)});
        if (pendingProjectId > 0 && !pendingProject.empty())
            work.card.fields.push_back({"project", pendingProject});
        work.card.fields.push_back({
            "admin review",
            pendingProjectId > 0
                ? "\xE2\x9C\x85 approve  \xE2\x80\xA2  \xE2\x9D\x8C reject  \xE2\x80\xA2  reply with a note to approve"
                : "assign or map a project in DevHub before approval; \xE2\x9D\x8C rejects"});
        work.card.url = jumpUrl(guildId, channelId, messageId);
    }
    return !work.channelId.empty();
}

static bool claimNotifyPost(Db* db, NotifyPostWork& work) {
    work = {};
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    db->raw(lk.token()).exec(R"sql(
UPDATE discord_notify_cards
   SET post_state='failed',post_next_retry_at='',
       post_last_error='Discord notification callback timed out after five attempts',
       updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE notify_message_id='' AND post_state='posting' AND post_attempts>=5
   AND datetime(updated_at)<=datetime('now','-45 seconds');
)sql");
    SQLite::Statement due(db->raw(lk.token()), R"sql(
SELECT id,post_attempts,post_revision FROM discord_notify_cards
 WHERE notify_message_id='' AND post_attempts<5
   AND ((post_state='pending' AND
         (post_next_retry_at='' OR datetime(post_next_retry_at)<=datetime('now')))
        OR (post_state='posting' AND
            datetime(updated_at)<=datetime('now','-45 seconds')))
 ORDER BY created_at,id LIMIT 1
)sql");
    if (!due.executeStep()) {
        tx.commit();
        return false;
    }
    const long long rowId = due.getColumn(0).getInt64();
    const int priorAttempts = std::max(0, due.getColumn(1).getInt());
    const int postRevision = std::max(0, due.getColumn(2).getInt());
    if (!buildNotifyPostCardLocked(lk.token(), db, rowId, work)) {
        SQLite::Statement stale(db->raw(lk.token()),
            "DELETE FROM discord_notify_cards WHERE id=? AND notify_message_id=''");
        stale.bind(1, rowId);
        stale.exec();
        tx.commit();
        work = {};
        return false;
    }
    work.attempt = priorAttempts + 1;
    work.revision = postRevision;
    SQLite::Statement claim(db->raw(lk.token()),
        "UPDATE discord_notify_cards SET post_state='posting',post_attempts=?,"
        "post_next_retry_at='',post_last_error='',updated_at=? "
        "WHERE id=? AND notify_message_id='' AND post_attempts=? "
        "AND post_revision=? "
        "AND post_state IN ('pending','posting')");
    claim.bind(1, work.attempt);
    claim.bind(2, nowIsoUtc());
    claim.bind(3, rowId);
    claim.bind(4, priorAttempts);
    claim.bind(5, postRevision);
    if (claim.exec() != 1) {
        tx.commit();
        work = {};
        return false;
    }
    tx.commit();
    return true;
}

static std::string boundedNotifyError(std::string value) {
    value = trim(redactHttpUrls(value));
    for (char& ch : value)
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    if (value.empty()) value = "Discord notification delivery failed";
    if (value.size() > 500) value = utf8Prefix(value, 497) + "...";
    return value;
}

static void queueReviewControlCleanupLocked(
    Db::Held held, Db* db, long long cardRowId, const std::string& channelId,
    const std::string& messageId) {
    if (!isDecimalDiscordSnowflake(channelId) ||
        !isDecimalDiscordSnowflake(messageId))
        throw std::runtime_error(
            "review-reaction cleanup has invalid Discord identity");
    const std::string now = nowIsoUtc();
    SQLite::Statement add(db->raw(held), R"sql(
INSERT INTO discord_review_control_cleanups(
 card_row_id,notify_channel_id,notify_message_id,cleanup_state,
 created_at,updated_at)
VALUES(?,?,?,'pending',?,?)
ON CONFLICT(notify_channel_id,notify_message_id) DO UPDATE SET
 card_row_id=excluded.card_row_id,
 cleanup_state=CASE
   WHEN cleanup_state IN ('deleting','failed') THEN 'pending'
   ELSE cleanup_state END,
 cleanup_attempts=CASE
   WHEN cleanup_state IN ('deleting','failed') THEN 0
   ELSE cleanup_attempts END,
 cleanup_revision=CASE
   WHEN cleanup_state IN ('deleting','failed') THEN cleanup_revision+1
   ELSE cleanup_revision END,
 cleanup_next_retry_at=CASE
   WHEN cleanup_state IN ('deleting','failed') THEN ''
   ELSE cleanup_next_retry_at END,
 cleanup_last_error=CASE
   WHEN cleanup_state IN ('deleting','failed') THEN ''
   ELSE cleanup_last_error END,
 updated_at=CASE
   WHEN cleanup_state IN ('deleting','failed') THEN excluded.updated_at
   ELSE updated_at END
)sql");
    add.bind(1, std::max<long long>(0, cardRowId));
    add.bind(2, channelId);
    add.bind(3, messageId);
    add.bind(4, now);
    add.bind(5, now);
    add.exec();
}

static bool claimReviewControlCleanup(
    Db* db, ReviewControlCleanupWork& work) {
    work = {};
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    db->raw(lk.token()).exec(R"sql(
UPDATE discord_review_control_cleanups
   SET cleanup_state='failed',cleanup_next_retry_at='',
       cleanup_last_error=
         'Discord all-reaction cleanup callback timed out after five attempts',
       updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE cleanup_state='deleting' AND cleanup_attempts>=5
   AND datetime(updated_at)<=datetime('now','-135 seconds');
)sql");
    SQLite::Statement due(db->raw(lk.token()), R"sql(
SELECT id,card_row_id,notify_channel_id,notify_message_id,
       cleanup_attempts,cleanup_revision
  FROM discord_review_control_cleanups
 WHERE cleanup_attempts<5 AND
       ((cleanup_state='pending' AND
         (cleanup_next_retry_at='' OR
          datetime(cleanup_next_retry_at)<=datetime('now')))
        OR (cleanup_state='deleting' AND
            datetime(updated_at)<=datetime('now','-135 seconds')))
 ORDER BY created_at,id LIMIT 1
)sql");
    if (!due.executeStep()) {
        tx.commit();
        return false;
    }
    work.rowId = due.getColumn(0).getInt64();
    work.cardRowId = due.getColumn(1).getInt64();
    work.channelId = due.getColumn(2).getString();
    work.messageId = due.getColumn(3).getString();
    const int priorAttempts = std::max(0, due.getColumn(4).getInt());
    work.revision = std::max(0, due.getColumn(5).getInt());
    work.attempt = priorAttempts + 1;
    SQLite::Statement claim(db->raw(lk.token()),
        "UPDATE discord_review_control_cleanups "
        "SET cleanup_state='deleting',cleanup_attempts=?,"
        "cleanup_next_retry_at='',cleanup_last_error='',updated_at=? "
        "WHERE id=? AND cleanup_attempts=? AND cleanup_revision=? "
        "AND cleanup_state IN ('pending','deleting')");
    claim.bind(1, work.attempt);
    claim.bind(2, nowIsoUtc());
    claim.bind(3, work.rowId);
    claim.bind(4, priorAttempts);
    claim.bind(5, work.revision);
    if (claim.exec() != 1) {
        tx.commit();
        work = {};
        return false;
    }
    tx.commit();
    return true;
}

static void finishReviewControlCleanup(
    Db* db, const ReviewControlCleanupWork& work) {
    auto lk = db->guard();
    SQLite::Statement remove(db->raw(lk.token()),
        "DELETE FROM discord_review_control_cleanups WHERE id=? "
        "AND notify_channel_id=? AND notify_message_id=? "
        "AND cleanup_state='deleting' AND cleanup_attempts=? "
        "AND cleanup_revision=?");
    remove.bind(1, work.rowId);
    remove.bind(2, work.channelId);
    remove.bind(3, work.messageId);
    remove.bind(4, work.attempt);
    remove.bind(5, work.revision);
    remove.exec();
}

static void failReviewControlCleanup(
    Db* db, const ReviewControlCleanupWork& work,
    const std::string& error, bool retryable) {
    constexpr int kMaxCleanupAttempts = 5;
    const bool terminal = !retryable || work.attempt >= kMaxCleanupAttempts;
    std::string nextRetry;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    if (!terminal) {
        const int delay = work.attempt == 1 ? 5 : work.attempt == 2 ? 15
                         : work.attempt == 3 ? 60 : 300;
        SQLite::Statement due(db->raw(lk.token()),
            "SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now','+' || ? || ' seconds')");
        due.bind(1, delay);
        due.executeStep();
        nextRetry = due.getColumn(0).getString();
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_review_control_cleanups SET cleanup_state=?,"
        "cleanup_next_retry_at=?,cleanup_last_error=?,updated_at=? "
        "WHERE id=? AND cleanup_state='deleting' AND cleanup_attempts=? "
        "AND cleanup_revision=?");
    up.bind(1, terminal ? "failed" : "pending");
    up.bind(2, nextRetry);
    up.bind(3, boundedNotifyError(error));
    up.bind(4, nowIsoUtc());
    up.bind(5, work.rowId);
    up.bind(6, work.attempt);
    up.bind(7, work.revision);
    up.exec();
    tx.commit();
}

static void heartbeatReviewControlCleanup(
    Db* db, const ReviewControlCleanupWork& work) {
    auto lk = db->guard();
    SQLite::Statement touch(db->raw(lk.token()),
        "UPDATE discord_review_control_cleanups SET updated_at=? "
        "WHERE id=? AND cleanup_state='deleting' AND cleanup_attempts=? "
        "AND cleanup_revision=?");
    touch.bind(1, nowIsoUtc());
    touch.bind(2, work.rowId);
    touch.bind(3, work.attempt);
    touch.bind(4, work.revision);
    touch.exec();
}

static void failNotifyPost(Db* db, const NotifyPostWork& claimed,
                           const std::string& error, bool retryable) {
    constexpr int kMaxNotifyAttempts = 5;
    const bool terminal = !retryable || claimed.attempt >= kMaxNotifyAttempts;
    std::string nextRetry;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    if (!terminal) {
        const int delay = claimed.attempt == 1 ? 5 : claimed.attempt == 2 ? 15
                         : claimed.attempt == 3 ? 60 : 300;
        SQLite::Statement due(db->raw(lk.token()),
            "SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now','+' || ? || ' seconds')");
        due.bind(1, delay);
        due.executeStep();
        nextRetry = due.getColumn(0).getString();
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_notify_cards SET post_state=?,post_next_retry_at=?,"
        "post_last_error=?,updated_at=? WHERE id=? AND notify_message_id='' "
        "AND post_state='posting' AND post_attempts=? AND post_revision=?");
    up.bind(1, terminal ? "failed" : "pending");
    up.bind(2, nextRetry);
    up.bind(3, boundedNotifyError(error));
    up.bind(4, nowIsoUtc());
    up.bind(5, claimed.rowId);
    up.bind(6, claimed.attempt);
    up.bind(7, claimed.revision);
    up.exec();
    tx.commit();
}

static void queueNotifyOrphanLocked(Db::Held held, Db* db, long long cardRowId,
                                    const std::string& channelId,
                                    const std::string& messageId) {
    if (!isDecimalDiscordSnowflake(channelId) ||
        !isDecimalDiscordSnowflake(messageId))
        throw std::runtime_error(
            "callback-generated orphan has invalid Discord identity");
    const std::string now = nowIsoUtc();
    SQLite::Statement add(db->raw(held),
        "INSERT OR IGNORE INTO discord_notify_orphans(card_row_id,"
        "notify_channel_id,notify_message_id,cleanup_state,created_at,updated_at) "
        "VALUES(?,?,?,'pending',?,?)");
    add.bind(1, cardRowId);
    add.bind(2, channelId);
    add.bind(3, messageId);
    add.bind(4, now);
    add.bind(5, now);
    add.exec();
}

static bool queueNotifyOrphan(Db* db, long long cardRowId,
                              const std::string& channelId,
                              const std::string& messageId) {
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        queueNotifyOrphanLocked(lk.token(), db, cardRowId, channelId, messageId);
        tx.commit();
        return true;
    } catch (const std::exception& e) {
        appLog("[notify] exact orphan cleanup could not be persisted: " +
               boundedNotifyError(e.what()));
        return false;
    }
}

static bool claimNotifyOrphan(Db* db, NotifyOrphanWork& work) {
    work = {};
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    db->raw(lk.token()).exec(R"sql(
UPDATE discord_notify_orphans
   SET cleanup_state='failed',cleanup_next_retry_at='',
       cleanup_last_error='Discord orphan cleanup callback timed out after five attempts',
       updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE cleanup_state='deleting' AND cleanup_attempts>=5
   AND datetime(updated_at)<=datetime('now','-45 seconds');
)sql");
    SQLite::Statement due(db->raw(lk.token()), R"sql(
SELECT id,card_row_id,notify_channel_id,notify_message_id,
       cleanup_attempts,cleanup_revision
  FROM discord_notify_orphans
 WHERE cleanup_attempts<5 AND
       ((cleanup_state='pending' AND
         (cleanup_next_retry_at='' OR
          datetime(cleanup_next_retry_at)<=datetime('now')))
        OR (cleanup_state='deleting' AND
            datetime(updated_at)<=datetime('now','-45 seconds')))
 ORDER BY created_at,id LIMIT 1
)sql");
    if (!due.executeStep()) {
        tx.commit();
        return false;
    }
    work.rowId = due.getColumn(0).getInt64();
    work.cardRowId = due.getColumn(1).getInt64();
    work.channelId = due.getColumn(2).getString();
    work.messageId = due.getColumn(3).getString();
    const int priorAttempts = std::max(0, due.getColumn(4).getInt());
    work.revision = std::max(0, due.getColumn(5).getInt());
    work.attempt = priorAttempts + 1;
    SQLite::Statement claim(db->raw(lk.token()),
        "UPDATE discord_notify_orphans SET cleanup_state='deleting',"
        "cleanup_attempts=?,cleanup_next_retry_at='',cleanup_last_error='',"
        "updated_at=? WHERE id=? AND cleanup_attempts=? AND cleanup_revision=? "
        "AND cleanup_state IN ('pending','deleting')");
    claim.bind(1, work.attempt);
    claim.bind(2, nowIsoUtc());
    claim.bind(3, work.rowId);
    claim.bind(4, priorAttempts);
    claim.bind(5, work.revision);
    if (claim.exec() != 1) {
        tx.commit();
        work = {};
        return false;
    }
    tx.commit();
    return true;
}

static void finishNotifyOrphan(Db* db, const NotifyOrphanWork& work) {
    auto lk = db->guard();
    SQLite::Statement remove(db->raw(lk.token()),
        "DELETE FROM discord_notify_orphans WHERE id=? "
        "AND notify_channel_id=? AND notify_message_id=?");
    remove.bind(1, work.rowId);
    remove.bind(2, work.channelId);
    remove.bind(3, work.messageId);
    remove.exec();
}

static void failNotifyOrphan(Db* db, const NotifyOrphanWork& work,
                             const std::string& error, bool retryable) {
    constexpr int kMaxNotifyAttempts = 5;
    const bool terminal = !retryable || work.attempt >= kMaxNotifyAttempts;
    std::string nextRetry;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    if (!terminal) {
        const int delay = work.attempt == 1 ? 5 : work.attempt == 2 ? 15
                         : work.attempt == 3 ? 60 : 300;
        SQLite::Statement due(db->raw(lk.token()),
            "SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now','+' || ? || ' seconds')");
        due.bind(1, delay);
        due.executeStep();
        nextRetry = due.getColumn(0).getString();
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_notify_orphans SET cleanup_state=?,"
        "cleanup_next_retry_at=?,cleanup_last_error=?,updated_at=? "
        "WHERE id=? AND cleanup_state='deleting' AND cleanup_attempts=? "
        "AND cleanup_revision=?");
    up.bind(1, terminal ? "failed" : "pending");
    up.bind(2, nextRetry);
    up.bind(3, boundedNotifyError(error));
    up.bind(4, nowIsoUtc());
    up.bind(5, work.rowId);
    up.bind(6, work.attempt);
    up.bind(7, work.revision);
    up.exec();
    tx.commit();
}

static bool finishNotifyPost(Db* db, const NotifyPostWork& claimed,
                             const std::string& messageId) {
    bool orphan = false;
    NotifyPostWork current;
    {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE discord_notify_cards SET notify_message_id=?,"
            "post_state='posted',post_next_retry_at='',post_last_error='',"
            "updated_at=? WHERE id=? AND notify_message_id='' "
            "AND post_state='posting' AND post_attempts=? AND post_revision=?");
        up.bind(1, messageId);
        up.bind(2, nowIsoUtc());
        up.bind(3, claimed.rowId);
        up.bind(4, claimed.attempt);
        up.bind(5, claimed.revision);
        if (up.exec() != 1) {
            queueNotifyOrphanLocked(lk.token(),
                db, claimed.rowId, claimed.channelId, messageId);
            orphan = true;
        } else {
            if (!buildNotifyPostCardLocked(lk.token(), db, claimed.rowId, current)) {
                queueNotifyOrphanLocked(lk.token(),
                    db, claimed.rowId, claimed.channelId, messageId);
                orphan = true;
                SQLite::Statement remove(db->raw(lk.token()),
                    "DELETE FROM discord_notify_cards WHERE id=?");
                remove.bind(1, claimed.rowId);
                remove.exec();
            } else {
                SQLite::Statement legacy(db->raw(lk.token()),
                    "UPDATE discord_messages SET notify_channel_id=?,"
                    "notify_message_id=? WHERE id=(SELECT discord_message_row_id "
                    "FROM discord_notify_cards WHERE id=?)");
                legacy.bind(1, claimed.channelId);
                legacy.bind(2, messageId);
                legacy.bind(3, claimed.rowId);
                legacy.exec();
                // The notify-message receipt fires the schema's lineage
                // trigger in this same transaction. That queues a revisioned
                // latest-state edit for any change made while create was in
                // flight, including across a restart.
            }
        }
        tx.commit();
    }
    return orphan;
}

static bool claimNotifyEdit(Db* db, NotifyEditWork& work) {
    work = {};
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    db->raw(lk.token()).exec(R"sql(
UPDATE discord_notify_cards
   SET edit_state='failed',edit_next_retry_at='',
       edit_last_error='Discord notification edit callback timed out after five attempts',
       edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
       updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE notify_message_id!='' AND edit_state='posting' AND edit_attempts>=5
   AND datetime(CASE WHEN edit_updated_at='' THEN updated_at ELSE edit_updated_at END)
       <=datetime('now','-45 seconds');
)sql");
    SQLite::Statement due(db->raw(lk.token()), R"sql(
SELECT id,edit_revision,edit_attempts FROM discord_notify_cards
 WHERE notify_message_id!='' AND edit_attempts<5
   AND ((edit_state='pending' AND
         (edit_next_retry_at='' OR datetime(edit_next_retry_at)<=datetime('now')))
        OR (edit_state='posting' AND
            datetime(CASE WHEN edit_updated_at='' THEN updated_at ELSE edit_updated_at END)
            <=datetime('now','-45 seconds')))
 ORDER BY CASE WHEN edit_updated_at='' THEN updated_at ELSE edit_updated_at END,id
 LIMIT 1
)sql");
    if (!due.executeStep()) {
        tx.commit();
        return false;
    }
    const long long rowId = due.getColumn(0).getInt64();
    work.revision = due.getColumn(1).getInt();
    const int priorAttempts = std::max(0, due.getColumn(2).getInt());
    if (!buildNotifyPostCardLocked(lk.token(), db, rowId, work.current) ||
        work.current.messageId.empty()) {
        SQLite::Statement failed(db->raw(lk.token()),
            "UPDATE discord_notify_cards SET edit_state='failed',"
            "edit_next_retry_at='',edit_last_error=?,edit_updated_at=?,updated_at=? "
            "WHERE id=? AND edit_revision=?");
        const std::string now = nowIsoUtc();
        failed.bind(1, "current notification card state could not be composed");
        failed.bind(2, now);
        failed.bind(3, now);
        failed.bind(4, rowId);
        failed.bind(5, work.revision);
        failed.exec();
        tx.commit();
        work = {};
        return false;
    }
    work.attempt = priorAttempts + 1;
    SQLite::Statement claim(db->raw(lk.token()),
        "UPDATE discord_notify_cards SET edit_state='posting',edit_attempts=?,"
        "edit_next_retry_at='',edit_last_error='',edit_updated_at=?,updated_at=? "
        "WHERE id=? AND edit_revision=? AND edit_attempts=? "
        "AND edit_state IN ('pending','posting')");
    const std::string now = nowIsoUtc();
    claim.bind(1, work.attempt);
    claim.bind(2, now);
    claim.bind(3, now);
    claim.bind(4, rowId);
    claim.bind(5, work.revision);
    claim.bind(6, priorAttempts);
    if (claim.exec() != 1) {
        tx.commit();
        work = {};
        return false;
    }
    tx.commit();
    return true;
}

static void failNotifyEdit(Db* db, const NotifyEditWork& work,
                           const std::string& error, bool retryable) {
    constexpr int kMaxNotifyEditAttempts = 5;
    const bool terminal = !retryable ||
                          work.attempt >= kMaxNotifyEditAttempts;
    std::string nextRetry;
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    if (!terminal) {
        const int delay = work.attempt == 1 ? 5 : work.attempt == 2 ? 15
                         : work.attempt == 3 ? 60 : 300;
        SQLite::Statement due(db->raw(lk.token()),
            "SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now','+' || ? || ' seconds')");
        due.bind(1, delay);
        due.executeStep();
        nextRetry = due.getColumn(0).getString();
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_notify_cards SET edit_state=?,edit_next_retry_at=?,"
        "edit_last_error=?,edit_updated_at=?,updated_at=? WHERE id=? "
        "AND edit_revision=? AND edit_state='posting' AND edit_attempts=?");
    const std::string now = nowIsoUtc();
    up.bind(1, terminal ? "failed" : "pending");
    up.bind(2, nextRetry);
    up.bind(3, boundedNotifyError(error));
    up.bind(4, now);
    up.bind(5, now);
    up.bind(6, work.current.rowId);
    up.bind(7, work.revision);
    up.bind(8, work.attempt);
    up.exec();
    if (terminal && work.current.removeReviewControls)
        queueReviewControlCleanupLocked(lk.token(),
            db, work.current.rowId, work.current.channelId,
            work.current.messageId);
    tx.commit();
}

static bool finishNotifyEdit(Db* db, const NotifyEditWork& work) {
    auto lk = db->guard();
    SQLite::Transaction tx(db->raw(lk.token()));
    SQLite::Statement done(db->raw(lk.token()),
        "UPDATE discord_notify_cards SET edit_state='idle',edit_attempts=0,"
        "edit_next_retry_at='',edit_last_error='',edit_updated_at=?,updated_at=? "
        "WHERE id=? AND edit_revision=? AND edit_state='posting' "
        "AND edit_attempts=?");
    const std::string now = nowIsoUtc();
    done.bind(1, now);
    done.bind(2, now);
    done.bind(3, work.current.rowId);
    done.bind(4, work.revision);
    done.bind(5, work.attempt);
    if (done.exec() == 1) {
        if (work.current.removeReviewControls)
            queueReviewControlCleanupLocked(lk.token(),
                db, work.current.rowId, work.current.channelId,
                work.current.messageId);
        // Keep scrubbed deletion tombstones as durable lineage. An older REST
        // edit can still arrive after this acknowledgement; its callback then
        // needs this row to enqueue the compensating current-state revision.
        tx.commit();
        return true;
    }

    // The REST edit already succeeded externally, but a newer revision won
    // the database race. That older payload may have landed last on Discord;
    // atomically enqueue another current-state revision to compensate.
    SQLite::Statement compensate(db->raw(lk.token()),
        "UPDATE discord_notify_cards SET edit_state='pending',edit_attempts=0,"
        "edit_revision=edit_revision+1,edit_next_retry_at='',edit_last_error='',"
        "edit_updated_at=?,updated_at=? WHERE id=? AND notify_message_id!=''");
    const std::string compensateAt = nowIsoUtc();
    compensate.bind(1, compensateAt);
    compensate.bind(2, compensateAt);
    compensate.bind(3, work.current.rowId);
    compensate.exec();
    tx.commit();
    return false;
}

static void completeNotifyEdit(Db* db, DiscordBot* bot,
                               const NotifyEditWork& work) {
    bool currentRevision = false;
    try {
        currentRevision = finishNotifyEdit(db, work);
    } catch (const std::exception& e) {
        // Leave the revision posting; its 45-second lease can reclaim a
        // callback whose durable acknowledgement could not commit.
        appLog("[notify] couldn't persist card edit success: " +
               boundedNotifyError(e.what()));
    }
    if (currentRevision && work.current.pendingReviewControls) {
        // A previously dismissed source can be manually captured back into
        // review. Restore controls only after its pending card is visible.
        bot->queuePendingReviewReactions(work.current.channelId,
                                         work.current.messageId);
    }
    bot->requestNotifyCardPost();
}

static void rejectNotifyEdit(Db* db, DiscordBot* bot,
                             const NotifyEditWork& work,
                             const std::string& error, bool retryable) {
    try {
        failNotifyEdit(db, work, error, retryable);
    } catch (const std::exception& e) {
        appLog("[notify] couldn't persist card edit failure: " +
               boundedNotifyError(e.what()));
    }
    bot->requestNotifyCardPost();
}

} // namespace

bool stageReviewControlCleanup(Db* db, long long cardRowId,
                               const std::string& notifyChannelId,
                               const std::string& notifyMessageId) {
    if (!db) return false;
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        queueReviewControlCleanupLocked(lk.token(),
            db, cardRowId, notifyChannelId, notifyMessageId);
        tx.commit();
        return true;
    } catch (const std::exception& e) {
        appLog("[review] reaction cleanup could not be staged: " +
               boundedNotifyError(e.what()));
        return false;
    }
}

void resumePendingNotifyCards(Db* db, DiscordBot* bot) {
    if (!db || !bot || !bot->running() ||
        bot->status().state != "connected") return;
    NotifyOrphanWork orphanWork;
    if (claimNotifyOrphan(db, orphanWork)) {
        bot->deleteCard(
            orphanWork.channelId, orphanWork.messageId,
            [db, bot, orphanWork]() {
                try {
                    finishNotifyOrphan(db, orphanWork);
                } catch (const std::exception& e) {
                    // Leave the deleting lease reclaimable when even the
                    // acknowledgement cannot be persisted.
                    appLog("[notify] couldn't persist orphan cleanup success: " +
                           boundedNotifyError(e.what()));
                }
                bot->requestNotifyCardPost();
            },
            [db, bot, orphanWork](const std::string& error, bool retryable) {
                try {
                    failNotifyOrphan(db, orphanWork, error, retryable);
                } catch (const std::exception& e) {
                    appLog("[notify] couldn't persist orphan cleanup failure: " +
                           boundedNotifyError(e.what()));
                }
                bot->requestNotifyCardPost();
            });
    }

    ReviewControlCleanupWork cleanupWork;
    if (claimReviewControlCleanup(db, cleanupWork)) {
        bot->queueTerminalReviewReactionCleanup(
            cleanupWork.channelId, cleanupWork.messageId,
            [db, bot, cleanupWork]() {
                try {
                    finishReviewControlCleanup(db, cleanupWork);
                } catch (const std::exception& e) {
                    // Keep the deleting lease reclaimable when its durable
                    // acknowledgement cannot be persisted.
                    appLog("[review] couldn't persist reaction cleanup success: " +
                           boundedNotifyError(e.what()));
                }
                bot->requestNotifyCardPost();
            },
            [db, bot, cleanupWork](const std::string& error,
                                    bool retryable) {
                try {
                    failReviewControlCleanup(
                        db, cleanupWork, error, retryable);
                } catch (const std::exception& e) {
                    appLog("[review] couldn't persist reaction cleanup failure: " +
                           boundedNotifyError(e.what()));
                }
                bot->requestNotifyCardPost();
            },
            [db, cleanupWork]() {
                heartbeatReviewControlCleanup(db, cleanupWork);
            },
            /*logSuccess=*/false);
    }

    NotifyPostWork postWork;
    if (claimNotifyPost(db, postWork)) {
        bot->postCard(
        postWork.channelId, postWork.card,
        [db, bot, postWork](const std::string& messageId) {
            bool orphan = false;
            try {
                orphan = finishNotifyPost(db, postWork, messageId);
            } catch (const std::exception& e) {
                // The create succeeded but its durable callback write did not.
                // Persist the exact returned identity separately; the posting
                // lease remains reclaimable and can create a replacement.
                appLog("[notify] couldn't persist posted card: " +
                       boundedNotifyError(e.what()));
                if (!queueNotifyOrphan(
                        db, postWork.rowId, postWork.channelId, messageId))
                    bot->deleteCard(postWork.channelId, messageId);
                bot->requestNotifyCardPost();
                return;
            }
            if (orphan) {
                // The lineage disappeared or changed while this create was in
                // flight. Its exact callback-generated ID is already in the
                // durable bounded cleanup queue.
                bot->requestNotifyCardPost();
                return;
            }
            // The exact callback ID is durable now. Queue controls only after
            // that acceptance; the bot re-checks current pending state before
            // touching Discord, so promoted/dismissed races are skipped.
            bot->queuePendingReviewReactions(postWork.channelId, messageId);
            bot->requestNotifyCardPost();
        },
        [db, bot, postWork](const std::string& error, bool retryable) {
            try {
                failNotifyPost(db, postWork, error, retryable);
            } catch (const std::exception& e) {
                // Leave the row posting. Its 45-second lease is the durable
                // recovery path if even failure accounting temporarily fails.
                appLog("[notify] couldn't persist post failure: " +
                       boundedNotifyError(e.what()));
            }
            bot->requestNotifyCardPost();
        });
    }

    NotifyEditWork editWork;
    if (!claimNotifyEdit(db, editWork)) return;
    bot->editCard(
        editWork.current.channelId, editWork.current.messageId,
        editWork.current.card,
        [db, bot, editWork]() {
            // Persist exact-card all-reaction cleanup in its own outbox in the
            // same transaction that acknowledges this edit. Reaction REST
            // failure can no longer replay a successful Discord message PATCH.
            completeNotifyEdit(db, bot, editWork);
        },
        [db, bot, editWork](const std::string& error, bool retryable) {
            rejectNotifyEdit(db, bot, editWork, error, retryable);
        });
}

namespace {

struct QueuedTicketImage {
    long long id = 0;
    std::string channelId, messageId, attachmentId;
    std::string originalFilename, contentType;
    std::uint64_t declaredSize = 0;
    std::uint32_t width = 0, height = 0;
};

struct TicketImagePersistSummary {
    int saved = 0;
    int failed = 0;
    int pending = 0;
    bool deferred = false;
};

// Promotion and reconnect recovery can arrive concurrently. Serialize the
// bounded claim/download/save pass inside the application process; each DB
// transition still checks state='queued' for cross-process defense in depth.
std::mutex ticketImagePersistMutex;
constexpr std::string_view kLegacyDiscordMimeMismatch =
    "downloaded image signature does not match Discord metadata";

std::string boundedImageError(std::string error) {
    error = trim(redactHttpUrls(std::move(error)));
    if (error.empty()) error = "ticket attachment save failed";
    if (error.size() > 500) error = utf8Prefix(error, 497) + "...";
    return error;
}

std::string boundedMimeForLog(std::string value) {
    value = trim(std::move(value));
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) ch = ' ';
    }
    if (value.size() > 127) value.resize(127);
    return value.empty() ? "(none)" : value;
}

int requeueLegacyDiscordMimeMismatches(Db* db) {
    try {
        auto lk = db->guard();
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE ticket_attachments SET state='queued',relative_path='',"
            "actual_size=0,sha256='',saved_at='',error=?,updated_at=? "
            "WHERE state='failed' AND item_id IS NOT NULL AND item_id>0 "
            "AND error=?");
        // A corrected legacy row is new work, not a transient retry. Leaving
        // the error blank lets the bounded virgin-work lane pick it up now.
        up.bind(1, "");
        up.bind(2, nowIsoUtc());
        up.bind(3, std::string(kLegacyDiscordMimeMismatch));
        return up.exec();
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not requeue legacy MIME failures: ") +
               e.what());
        return 0;
    }
}

bool markTicketImageWaiting(Db* db, long long rowId,
                            const std::string& error) {
    try {
        auto lk = db->guard();
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE ticket_attachments SET error=?,updated_at=? "
            "WHERE id=? AND state='queued'");
        up.bind(1, boundedImageError(error));
        up.bind(2, nowIsoUtc());
        up.bind(3, rowId);
        up.exec();
        return true;
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not record queued state: ") + e.what());
        return false;
    }
}

bool markTicketImageFailed(Db* db, long long rowId,
                           const std::string& error,
                           const DiscordAttachmentMeta* fresh = nullptr) {
    try {
        auto lk = db->guard();
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE ticket_attachments SET state='failed',"
            "original_filename=CASE WHEN ?='' THEN original_filename ELSE ? END,"
            "content_type=CASE WHEN ?='' THEN content_type ELSE ? END,"
            "declared_size=CASE WHEN ?>=0 THEN ? ELSE declared_size END,"
            "width=CASE WHEN ?>=0 THEN ? ELSE width END,"
            "height=CASE WHEN ?>=0 THEN ? ELSE height END,"
            "relative_path='',actual_size=0,sha256='',saved_at='',error=?,updated_at=? "
            "WHERE id=? AND state='queued'");
        const std::string filename = fresh ? utf8Prefix(fresh->filename, 255) : "";
        const std::string contentType = fresh ? fresh->contentType.substr(0, 127) : "";
        const long long declared = fresh
            ? static_cast<long long>(fresh->sizeBytes) : -1;
        const long long width = fresh ? static_cast<long long>(fresh->width) : -1;
        const long long height = fresh ? static_cast<long long>(fresh->height) : -1;
        up.bind(1, filename); up.bind(2, filename);
        up.bind(3, contentType); up.bind(4, contentType);
        up.bind(5, declared); up.bind(6, declared);
        up.bind(7, width); up.bind(8, width);
        up.bind(9, height); up.bind(10, height);
        up.bind(11, boundedImageError(error));
        up.bind(12, nowIsoUtc());
        up.bind(13, rowId);
        up.exec();
        return true;
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not record failure: ") + e.what());
        return false;
    }
}

bool markTicketImageSaved(Db* db, long long rowId,
                          const DiscordAttachmentMeta& fresh,
                          const TicketImageSaveResult& saved) {
    try {
        auto lk = db->guard();
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE ticket_attachments SET state='saved',original_filename=?,"
            "content_type=?,declared_size=?,width=?,height=?,relative_path=?,"
            "actual_size=?,sha256=?,error='',updated_at=?,saved_at=? "
            "WHERE id=? AND state='queued'");
        up.bind(1, utf8Prefix(fresh.filename, 255));
        up.bind(2, saved.contentType);
        up.bind(3, static_cast<long long>(fresh.sizeBytes));
        up.bind(4, static_cast<long long>(fresh.width));
        up.bind(5, static_cast<long long>(fresh.height));
        up.bind(6, saved.relativePath);
        up.bind(7, static_cast<long long>(saved.actualSize));
        up.bind(8, saved.sha256);
        const std::string now = nowIsoUtc();
        up.bind(9, now);
        up.bind(10, now);
        up.bind(11, rowId);
        return up.exec() == 1;
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not record saved file: ") + e.what());
        return false;
    }
}

bool ticketImageRowMatchesSaved(Db* db, long long rowId,
                                const TicketImageSaveResult& saved) {
    try {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT state,relative_path,sha256,actual_size "
            "FROM ticket_attachments WHERE id=?");
        q.bind(1, rowId);
        return q.executeStep() && q.getColumn(0).getString() == "saved" &&
            q.getColumn(1).getString() == saved.relativePath &&
            q.getColumn(2).getString() == saved.sha256 &&
            q.getColumn(3).getInt64() ==
                static_cast<long long>(saved.actualSize);
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not reconcile saved row: ") +
               e.what());
        return false;
    }
}

std::vector<QueuedTicketImage> loadQueuedTicketImages(Db* db,
                                                       long long itemId,
                                                       bool retryPass,
                                                       bool& deferred) {
    std::vector<QueuedTicketImage> rows;
    deferred = false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT id,source_channel_id,source_message_id,attachment_id,"
        "original_filename,content_type,declared_size,width,height "
        "FROM ticket_attachments WHERE item_id=? AND state='queued' "
        "AND ((?=1 AND error!='') OR (?=0 AND error='')) "
        "ORDER BY updated_at,id LIMIT ?");
    q.bind(1, itemId);
    q.bind(2, retryPass ? 1 : 0);
    q.bind(3, retryPass ? 1 : 0);
    q.bind(4, static_cast<int>(kMaxTicketImageCandidatesPerPass + 1));
    while (q.executeStep()) {
        QueuedTicketImage row;
        int c = 0;
        row.id = q.getColumn(c++).getInt64();
        row.channelId = q.getColumn(c++).getString();
        row.messageId = q.getColumn(c++).getString();
        row.attachmentId = q.getColumn(c++).getString();
        row.originalFilename = q.getColumn(c++).getString();
        row.contentType = q.getColumn(c++).getString();
        row.declaredSize = static_cast<std::uint64_t>(
            std::max<long long>(0, q.getColumn(c++).getInt64()));
        row.width = static_cast<std::uint32_t>(
            std::max<long long>(0, q.getColumn(c++).getInt64()));
        row.height = static_cast<std::uint32_t>(
            std::max<long long>(0, q.getColumn(c++).getInt64()));
        rows.push_back(std::move(row));
    }
    if (rows.size() > kMaxTicketImageCandidatesPerPass) {
        rows.pop_back();
        // Drain virgin work promptly. Error-bearing transient rows get one
        // bounded pass only and wait for the normal five-minute recovery sweep.
        deferred = !retryPass;
    }
    return rows;
}

bool yieldQueuedTicketImageClass(Db* db, long long itemId, bool retryPass) {
    try {
        auto lk = db->guard();
        const std::string now = nowIsoUtc();
        // Treat a remaining row's timestamp as its last scheduling turn. This
        // rotates large items behind their peers without changing error/state.
        SQLite::Statement rotate(db->raw(lk.token()),
            "UPDATE ticket_attachments SET updated_at=? WHERE item_id=? "
            "AND state='queued' AND ((?=1 AND error!='') OR (?=0 AND error=''))");
        rotate.bind(1, now);
        rotate.bind(2, itemId);
        rotate.bind(3, retryPass ? 1 : 0);
        rotate.bind(4, retryPass ? 1 : 0);
        rotate.exec();
        SQLite::Statement remaining(db->raw(lk.token()),
            "SELECT EXISTS(SELECT 1 FROM ticket_attachments WHERE item_id=? "
            "AND state='queued' AND ((?=1 AND error!='') OR (?=0 AND error='')))");
        remaining.bind(1, itemId);
        remaining.bind(2, retryPass ? 1 : 0);
        remaining.bind(3, retryPass ? 1 : 0);
        remaining.executeStep();
        return remaining.getColumn(0).getInt() != 0;
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not rotate worker turn: ") +
               e.what());
        return false;
    }
}

int markTicketImagesWaitingForBot(Db* db, long long itemId) {
    try {
        auto lk = db->guard();
        SQLite::Statement waiting(db->raw(lk.token()),
            "UPDATE ticket_attachments SET error=?,updated_at=? "
            "WHERE item_id=? AND state='queued'");
        waiting.bind(
            1, boundedImageError(
                   "waiting for the embedded Discord bot to reconnect"));
        waiting.bind(2, nowIsoUtc());
        waiting.bind(3, itemId);
        return waiting.exec();
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not record reconnect wait: ") +
               e.what());
        return 0;
    }
}

TicketImagePersistSummary persistQueuedTicketImages(Db* db, DiscordBot* bot,
                                                     long long itemId,
                                                     bool retryPass) {
    std::lock_guard<std::mutex> persistLock(ticketImagePersistMutex);
    TicketImagePersistSummary summary;
    if (!bot || !bot->running() || bot->status().state != "connected") {
        summary.pending = markTicketImagesWaitingForBot(db, itemId);
        return summary;
    }
    bool deferred = false;
    std::vector<QueuedTicketImage> rows =
        loadQueuedTicketImages(db, itemId, retryPass, deferred);
    summary.deferred = deferred;
    if (rows.empty()) return summary;

    std::size_t persistedCount = 0;
    std::size_t persistedBytes = 0;
    {
        auto lk = db->guard();
        SQLite::Statement saved(db->raw(lk.token()),
            "SELECT COUNT(*),COALESCE(SUM(actual_size),0) "
            "FROM ticket_attachments WHERE item_id=? AND state='saved'");
        saved.bind(1, itemId);
        if (saved.executeStep()) {
            persistedCount = static_cast<std::size_t>(
                std::max(0, saved.getColumn(0).getInt()));
            persistedBytes = static_cast<std::size_t>(
                std::max<long long>(0, saved.getColumn(1).getInt64()));
        }
    }
    TicketImageCapacity capacity(persistedCount, persistedBytes);
    std::size_t cursor = 0;
    while (cursor < rows.size()) {
        if (bot->stopRequested()) return summary;
        if (capacity.persistedSavedCount() >= kMaxTicketAttachments ||
            capacity.persistedSavedBytes() >= kMaxTicketAttachmentTotalBytes) {
            const char* reason =
                capacity.persistedSavedCount() >= kMaxTicketAttachments
                ? "ticket exceeds the 10-attachment limit"
                : "ticket exceeds the 50 MiB attachment limit";
            for (; cursor < rows.size(); ++cursor) {
                markTicketImageFailed(db, rows[cursor].id, reason);
                ++summary.failed;
            }
            break;
        }

        const QueuedTicketImage& first = rows[cursor];
        if (first.declaredSize > kMaxTicketAttachmentBytes) {
            markTicketImageFailed(db, first.id,
                                  "attachment exceeds the 10 MiB per-file limit");
            ++summary.failed;
            ++cursor;
            continue;
        }
        if (!isDecimalDiscordSnowflake(first.channelId) ||
            !isDecimalDiscordSnowflake(first.messageId) ||
            !isDecimalDiscordSnowflake(first.attachmentId)) {
            markTicketImageFailed(db, first.id,
                                  "stored Discord attachment identity is invalid");
            ++summary.failed;
            ++cursor;
            continue;
        }

        const std::string sourceChannel = first.channelId;
        const std::string sourceMessage = first.messageId;
        std::vector<std::size_t> indices;
        std::vector<std::string> wanted;
        std::size_t chunkEnd = cursor;
        while (chunkEnd < rows.size() &&
               indices.size() < kMaxTicketImageBodiesPerRequest) {
            const QueuedTicketImage& candidate = rows[chunkEnd];
            if (candidate.channelId != sourceChannel ||
                candidate.messageId != sourceMessage ||
                candidate.declaredSize > kMaxTicketAttachmentBytes ||
                !isDecimalDiscordSnowflake(candidate.channelId) ||
                !isDecimalDiscordSnowflake(candidate.messageId) ||
                !isDecimalDiscordSnowflake(candidate.attachmentId))
                break;
            indices.push_back(chunkEnd);
            wanted.push_back(candidate.attachmentId);
            ++chunkEnd;
        }
        std::vector<DiscordAttachmentDownload> downloads =
            bot->downloadAttachments(sourceChannel, sourceMessage, wanted);
        std::map<std::string, DiscordAttachmentDownload> byId;
        for (auto& download : downloads)
            byId[download.meta.attachmentId] = std::move(download);

        for (std::size_t index : indices) {
            if (bot->stopRequested()) return summary;
            const QueuedTicketImage& row = rows[index];
            auto found = byId.find(row.attachmentId);
            if (found == byId.end()) {
                markTicketImageFailed(db, row.id,
                                      "Discord returned no attachment result");
                ++summary.failed;
                continue;
            }
            DiscordAttachmentDownload& download = found->second;
            if (!download.error.empty()) {
                if (download.retryable) {
                    markTicketImageWaiting(db, row.id, download.error);
                    ++summary.pending;
                } else {
                    markTicketImageFailed(db, row.id, download.error,
                                          &download.meta);
                    ++summary.failed;
                }
                continue;
            }
            if (download.bytes.empty()) {
                markTicketImageFailed(db, row.id,
                                      "Discord returned an empty attachment body",
                                      &download.meta);
                ++summary.failed;
                continue;
            }
            if (download.bytes.size() > kMaxTicketAttachmentBytes) {
                markTicketImageFailed(db, row.id,
                                      "attachment exceeds the 10 MiB per-file limit",
                                      &download.meta);
                ++summary.failed;
                continue;
            }
            if (capacity.persistedSavedCount() >= kMaxTicketAttachments) {
                markTicketImageFailed(db, row.id,
                                      "ticket exceeds the 10-attachment limit",
                                      &download.meta);
                ++summary.failed;
                continue;
            }
            if (!capacity.canPersist(download.bytes.size())) {
                markTicketImageFailed(db, row.id,
                                      "ticket exceeds the 50 MiB attachment limit",
                                      &download.meta);
                ++summary.failed;
                continue;
            }
            TicketImageSaveResult saved = saveTicketAttachmentBytes(
                db->path(), itemId, row.messageId, row.attachmentId,
                download.meta.filename, download.meta.contentType,
                download.bytes);
            if (!saved.ok) {
                markTicketImageFailed(db, row.id, saved.error, &download.meta);
                ++summary.failed;
                continue;
            }
            const TicketImageKind downloadedKind =
                detectTicketImageKind(download.bytes);
            const bool discordMimeMatches = saved.isImage &&
                ticketImageMimeMatchesKind(download.meta.contentType,
                                           downloadedKind);
            const bool responseMimeMatches = saved.isImage &&
                ticketImageMimeMatchesKind(download.responseContentType,
                                           downloadedKind);
            if (saved.isImage &&
                (!discordMimeMatches || !responseMimeMatches)) {
                appLog("[ticket image] advisory MIME mismatch for A" +
                    std::to_string(row.id) + " (message " + row.messageId +
                    ", attachment " + row.attachmentId + "): Discord=" +
                    boundedMimeForLog(download.meta.contentType) + ", HTTP=" +
                    boundedMimeForLog(download.responseContentType) +
                    ", detected=" + saved.contentType + ", bytes=" +
                    std::to_string(download.bytes.size()) +
                    "; saving validated bytes");
            }
            if (!markTicketImageSaved(db, row.id, download.meta, saved)) {
                if (ticketImageRowMatchesSaved(db, row.id, saved)) {
                    if (!capacity.recordPersisted(saved.actualSize))
                        appLog("[ticket image] saved-row capacity reconciliation failed");
                    ++summary.saved;
                    continue;
                }
                if (saved.created && !saved.absolutePath.empty()) {
                    std::error_code cleanup;
                    std::filesystem::remove(saved.absolutePath, cleanup);
                }
                markTicketImageFailed(
                    db, row.id,
                    "attachment was written but its database record failed",
                    &download.meta);
                ++summary.failed;
                continue;
            }
            if (!capacity.recordPersisted(saved.actualSize))
                appLog("[ticket image] persisted image exceeded in-memory capacity ledger");
            ++summary.saved;
        }
        cursor = chunkEnd;
        // The shared worker must return to card/manual work after one bounded
        // source-message request (whose download helper has a 30-second cap).
        break;
    }
    const bool remaining =
        yieldQueuedTicketImageClass(db, itemId, retryPass);
    if (!retryPass && remaining) summary.deferred = true;
    return summary;
}

} // namespace

int requeueLegacyTicketImageFailures(Db* db) {
    return db ? requeueLegacyDiscordMimeMismatches(db) : 0;
}

bool retryFailedTicketImage(Db* db, DiscordBot* bot,
                            long long attachmentRowId) {
    if (!db || attachmentRowId <= 0) return false;
    long long itemId = 0;
    try {
        auto lk = db->guard();
        SQLite::Statement find(db->raw(lk.token()),
            "SELECT item_id FROM ticket_attachments "
            "WHERE id=? AND state='failed' AND item_id IS NOT NULL AND item_id>0");
        find.bind(1, attachmentRowId);
        if (!find.executeStep()) return false;
        itemId = find.getColumn(0).getInt64();

        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE ticket_attachments SET state='queued',relative_path='',"
            "actual_size=0,sha256='',saved_at='',error=?,updated_at=? "
            "WHERE id=? AND state='failed' AND item_id=?");
        // An operator explicitly requested this attempt. Classify it as new
        // work so it is not held for the periodic transient-retry sweep.
        up.bind(1, "");
        up.bind(2, nowIsoUtc());
        up.bind(3, attachmentRowId);
        up.bind(4, itemId);
        if (up.exec() != 1) return false;
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] retry request failed: ") + e.what());
        return false;
    }

    appLog("[ticket image] retry queued for A" +
           std::to_string(attachmentRowId) + " on item I" +
           std::to_string(itemId));
    if (bot) {
        bot->requestQueuedTicketImageSave();
        refreshTicketNotifyCard(db, bot, itemId);
    }
    return true;
}

void resumeQueuedTicketImages(Db* db, DiscordBot* bot, bool recoverySweep) {
    if (!db || !bot || !bot->running() ||
        bot->status().state != "connected") return;
    const int legacyRequeued = requeueLegacyTicketImageFailures(db);
    if (legacyRequeued > 0) {
        appLog("[ticket image] requeued " + std::to_string(legacyRequeued) +
               " legacy Discord MIME mismatch failure(s)");
    }
    try {
        // Promotion can queue evidence while the embedded bot is offline.
        // Once the connection is back, only those exact connection-hold
        // reasons become virgin work; arbitrary CDN/REST failures retain
        // their normal recovery delay and cannot create a hot loop.
        auto lk = db->guard();
        SQLite::Statement activate(db->raw(lk.token()),
            "UPDATE ticket_attachments SET error='',updated_at=? "
            "WHERE state='queued' AND item_id IS NOT NULL "
            "AND error IN (?,?)");
        activate.bind(1, nowIsoUtc());
        activate.bind(2, "waiting for the embedded Discord bot to reconnect");
        activate.bind(3, "Discord bot is not connected");
        activate.exec();
    } catch (const std::exception& e) {
        appLog(std::string("[ticket image] could not activate reconnect waits: ") +
               e.what());
    }
    std::vector<long long> itemIds;
    bool hasVirgin = false;
    bool retryPass = false;
    bool moreImmediateItems = false;
    {
        auto lk = db->guard();
        SQLite::Statement classes(db->raw(lk.token()),
            "SELECT EXISTS(SELECT 1 FROM ticket_attachments WHERE state='queued' "
            "AND item_id IS NOT NULL AND error=''),"
            "EXISTS(SELECT 1 FROM ticket_attachments WHERE state='queued' "
            "AND item_id IS NOT NULL AND error!='')");
        classes.executeStep();
        hasVirgin = classes.getColumn(0).getInt() != 0;
        const bool hasRetry = classes.getColumn(1).getInt() != 0;
        // External/reconnect wakes drain only immediate work. The periodic
        // sweep reserves its turn for a retry class when one exists.
        retryPass = ticketImageRetryClassForTurn(recoverySweep, hasRetry);
        if (!retryPass && !hasVirgin) return;
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT item_id FROM ticket_attachments "
            "WHERE state='queued' AND item_id IS NOT NULL "
            "AND ((?=1 AND error!='') OR (?=0 AND error='')) GROUP BY item_id "
            "ORDER BY MIN(updated_at),item_id LIMIT ?");
        q.bind(1, retryPass ? 1 : 0);
        q.bind(2, retryPass ? 1 : 0);
        q.bind(3, static_cast<int>(kMaxTicketImageItemsPerTurn + 1));
        while (q.executeStep()) itemIds.push_back(q.getColumn(0).getInt64());
        if (itemIds.size() > kMaxTicketImageItemsPerTurn) {
            itemIds.pop_back();
            moreImmediateItems = !retryPass;
        }
    }
    for (long long itemId : itemIds) {
        if (bot->stopRequested()) break;
        TicketImagePersistSummary summary =
            persistQueuedTicketImages(db, bot, itemId, retryPass);
        refreshTicketNotifyCard(db, bot, itemId);
        if (summary.deferred) bot->requestQueuedTicketImageSave();
        if (summary.saved || summary.failed)
            appLog("[ticket image] resumed item I" + std::to_string(itemId) +
                   ": " + std::to_string(summary.saved) + " saved, " +
                   std::to_string(summary.failed) + " failed");
    }
    if (moreImmediateItems || (retryPass && hasVirgin))
        bot->requestQueuedTicketImageSave();
}

static json promoteSuggestionImpl(
    Db* db, DiscordBot* bot, long long msgId, long long projectIdOverride,
    const std::string& typeOverride, const std::string& titleOverride,
    const std::string& adminNoteAppend, int priorityOverride,
    const PendingSuggestionEdit* pendingEdit) {
    if (priorityOverride < 1 || priorityOverride > 4)
        return json{{"ok", false}, {"error", "priority must be P1 through P4"}};
    long long itemId = 0;
    long long projectId = projectIdOverride;
    std::string title, author;
    bool alreadyPromoted = false;
    {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT m.*, c.channel_name, c.project_id AS channel_project_id "
            "FROM discord_messages m JOIN discord_channels c ON c.id=m.channel_row_id "
            "WHERE m.id=?");
        q.bind(1, msgId);
        if (!q.executeStep())
            return json{{"ok", false}, {"error", "message not found"}};
        json msg = Db::rowToJson(q);
        const std::string sourceState = msg.value("state", "new");
        if ((sourceState == "promoted" || sourceState == "deleted") &&
            msg.contains("item_id") && msg["item_id"].is_number()) {
            itemId = msg["item_id"].get<long long>();
            alreadyPromoted = itemId > 0;
        }
        // Ordinary promotion reactions remain replay-safe, but an editor save
        // must never claim that stale edits were applied after another
        // terminal action won the source race.
        if (alreadyPromoted && pendingEdit)
            return json{{"ok", false},
                        {"error", "message is no longer pending"}};
        if (!alreadyPromoted) {
            if (sourceState != "new")
                return json{{"ok", false},
                            {"error", "message is no longer pending"}};
            if (projectId <= 0 && !msg["channel_project_id"].is_null())
                projectId = msg["channel_project_id"].get<long long>();
            if (projectId <= 0 && msg.contains("inferred_project_id") &&
                msg["inferred_project_id"].is_number())
                projectId = msg["inferred_project_id"].get<long long>();
            if (projectId <= 0)
                return json{{"ok", false},
                            {"error", "no project - pick one (general message)"}};

            if (pendingEdit) {
                SQLite::Statement project(db->raw(lk.token()),
                    "SELECT archived FROM projects WHERE id=?");
                project.bind(1, projectId);
                if (!project.executeStep() || project.getColumn(0).getInt() != 0)
                    return json{{"ok", false},
                                {"error", "destination project is missing or archived"}};
            }

            const std::string kind = msg.value("kind", "suggestion");
            const std::string type = !typeOverride.empty()
                ? typeOverride
                : (kind == "bug" ? std::string("fix")
                                  : std::string("implementation"));
            author = msg.value("author", "");
            const long long sourceId = ensureSourceLocked(lk.token(),
                db, author, "discord", msg.value("author_id", ""));
            const std::string content = pendingEdit
                ? utf8Prefix(pendingEdit->content, 16000)
                : msg.value("content", "");
            title = !titleOverride.empty() ? titleOverride : makeTitle(content);
            if (title.empty()) title = "Discord attachment";
            std::string itemBody = content;
            std::string adminNote = pendingEdit
                ? utf8Prefix(trim(pendingEdit->adminNote), 4000)
                : msg.value("admin_note", "");
            const std::string appendedNote =
                utf8Prefix(trim(adminNoteAppend), 4000);
            if (!appendedNote.empty())
                adminNote += (adminNote.empty() ? "" : "\n") + appendedNote;
            if (!adminNote.empty())
                itemBody += "\n\n-- Admin notes:\n" + adminNote;
            itemBody += "\n\n-- Discord: " + author + " in #" +
                        msg.value("channel_name", "") + " at " +
                        msg.value("posted_at", "");

            SQLite::Transaction tx(db->raw(lk.token()));
            itemId = insertItemLocked(lk.token(), db, projectId, type, title, itemBody,
                                      priorityOverride, sourceId, "discord",
                                      "", "");
            // Promotion is the operator's attribution decision. Record its
            // contributor credit in the same transaction instead of requiring
            // a second Credits-page acknowledgement later.
            if (sourceId > 0)
                setItemSourceCreditedLocked(lk.token(), db, itemId, sourceId, true);
            SQLite::Statement up(db->raw(lk.token()),
                "UPDATE discord_messages SET state='promoted', item_id=?, "
                "content=?,admin_note=?,inferred_project_id=? "
                "WHERE id=? AND state='new'");
            up.bind(1, itemId);
            up.bind(2, content);
            up.bind(3, adminNote);
            up.bind(4, projectId);
            up.bind(5, msgId);
            up.exec();
            SQLite::Statement card(db->raw(lk.token()),
                "UPDATE discord_notify_cards SET item_id=?,updated_at=? "
                "WHERE discord_message_row_id=?");
            card.bind(1, itemId);
            card.bind(2, nowIsoUtc());
            card.bind(3, msgId);
            card.exec();
            // The source-to-item transition and its canonical outbox lineage
            // commit together. A crash can no longer leave a promoted item
            // invisible to startup notification recovery.
            ensureTicketNotifyCardLocked(lk.token(), db, msgId, itemId);
            SQLite::Statement queue(db->raw(lk.token()),
                "UPDATE ticket_attachments SET item_id=?,state='queued',error='',"
                "updated_at=? WHERE discord_message_row_id=? AND state='captured'");
            queue.bind(1, itemId);
            queue.bind(2, nowIsoUtc());
            queue.bind(3, msgId);
            queue.exec();
            db->logActivity(lk.token(), "discord_promoted", projectId,
                            title + " (from " + author + ")");
            tx.commit();
        } else {
            // A replay can come from a pre-fix promotion. Prefer the captured
            // stable Discord identity so an item merge or later contributor
            // edit cannot redirect credit to an unrelated primary source.
            long long sourceId = 0;
            const std::string authorHandle = msg.value("author_id", "");
            if (!authorHandle.empty()) {
                SQLite::Statement source(db->raw(lk.token()),
                    "SELECT id FROM sources WHERE platform='discord' AND handle=? "
                    "ORDER BY id LIMIT 1");
                source.bind(1, authorHandle);
                if (source.executeStep())
                    sourceId = canonicalSourceIdLocked(lk.token(),
                        db, source.getColumn(0).getInt64());
            }
            // Deleted promoted messages intentionally have their author
            // metadata scrubbed. The Discord-origin primary link is the only
            // durable fallback available for those pre-fix rows.
            if (sourceId <= 0) {
                SQLite::Statement primary(db->raw(lk.token()),
                    "SELECT source_id FROM items WHERE id=? AND origin='discord'");
                primary.bind(1, itemId);
                if (primary.executeStep() && !primary.getColumn(0).isNull())
                    sourceId = primary.getColumn(0).getInt64();
            }
            if (sourceId > 0) {
                SQLite::Transaction tx(db->raw(lk.token()));
                setItemSourceCreditedLocked(lk.token(), db, itemId, sourceId, true);
                tx.commit();
            }
        }
    }

    // An already-promoted result from an older build may predate canonical
    // lineage. New promotions created it inside the transaction above.
    if (alreadyPromoted)
        ensureTicketNotifyCard(db, bot, msgId, itemId);
    else if (bot)
        bot->requestNotifyCardPost();

    TicketImagePersistSummary images;
    if (bot && bot->status().state == "connected") {
        {
            auto lk = db->guard();
            SQLite::Statement pending(db->raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments "
                "WHERE item_id=? AND state='queued'");
            pending.bind(1, itemId);
            pending.executeStep();
            images.pending = pending.getColumn(0).getInt();
        }
        if (images.pending > 0) bot->requestQueuedTicketImageSave();
    } else {
        images = persistQueuedTicketImages(
            db, bot, itemId, /*retryPass=*/false);
    }
    {
        auto lk = db->guard();
        // Flip the notification card only after the promotion-owned save pass.
        updateNotifyCardForItemLocked(lk.token(), db, bot, itemId, "open");
    }
    return json{{"ok", true}, {"item_id", itemId},
                {"duplicate", alreadyPromoted},
                {"attachments_saved", images.saved},
                {"attachments_failed", images.failed},
                {"attachments_pending", images.pending},
                // Compatibility aliases for older internal callers.
                {"images_saved", images.saved},
                {"images_failed", images.failed},
                {"images_pending", images.pending}};
}

json promoteSuggestion(Db* db, DiscordBot* bot, long long msgId,
                       long long projectIdOverride,
                       const std::string& typeOverride,
                       const std::string& titleOverride,
                       const std::string& adminNoteAppend,
                       int priorityOverride) {
    return promoteSuggestionImpl(
        db, bot, msgId, projectIdOverride, typeOverride, titleOverride,
        adminNoteAppend, priorityOverride, nullptr);
}

json savePendingSuggestionEdit(Db* db, DiscordBot* bot, long long msgId,
                               const PendingSuggestionEdit& edit) {
    if (!db || msgId <= 0)
        return json{{"ok", false}, {"error", "pending message is invalid"}};
    if (edit.projectId > 0) {
        if (edit.priority < 1 || edit.priority > 4)
            return json{{"ok", false},
                        {"error", "priority must be P1 through P4"}};
        if (edit.type != "implementation" && edit.type != "fix" &&
            edit.type != "note")
            return json{{"ok", false},
                        {"error", "pending ticket type is invalid"}};
    }

    const std::string content = utf8Prefix(edit.content, 16000);
    const std::string adminNote = utf8Prefix(trim(edit.adminNote), 4000);
    if (trim(content).empty() && adminNote.empty()) {
        auto lk = db->guard();
        SQLite::Statement attachment(db->raw(lk.token()),
            "SELECT EXISTS(SELECT 1 FROM ticket_attachments "
            "WHERE discord_message_row_id=? AND state='captured')");
        attachment.bind(1, msgId);
        attachment.executeStep();
        if (attachment.getColumn(0).getInt() == 0)
            return json{{"ok", false},
                        {"error", "pending ticket needs notes or an attachment"}};
    }

    if (edit.projectId > 0) {
        PendingSuggestionEdit normalized = edit;
        normalized.content = content;
        normalized.adminNote = adminNote;
        normalized.title = utf8Prefix(trim(edit.title), 500);
        return promoteSuggestionImpl(
            db, bot, msgId, normalized.projectId, normalized.type,
            normalized.title, {}, normalized.priority, &normalized);
    }

    try {
        {
            auto lk = db->guard();
            SQLite::Transaction tx(db->raw(lk.token()));
            SQLite::Statement update(db->raw(lk.token()),
                "UPDATE discord_messages SET content=?,admin_note=?,"
                "inferred_project_id=NULL WHERE id=? AND state='new' "
                "AND kind IN ('suggestion','bug')");
            update.bind(1, content);
            update.bind(2, adminNote);
            update.bind(3, msgId);
            if (update.exec() != 1)
                return json{{"ok", false},
                            {"error", "message is no longer pending"}};
            tx.commit();
        }
        if (bot) bot->requestNotifyCardPost();
        return json{{"ok", true}, {"pending", true}};
    } catch (const std::exception& e) {
        return json{{"ok", false}, {"error", e.what()}};
    }
}

json dismissSuggestion(Db* db, DiscordBot* bot, long long msgId) {
    bool found = false;
    bool editQueued = false;
    {
        auto lk = db->guard();
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE discord_messages SET state='dismissed' "
            "WHERE id=? AND state='new'");
        up.bind(1, msgId);
        found = up.exec() == 1;
        if (!found)
            return json{{"ok", false},
                        {"error", "message is no longer pending"}};
        // The state-change trigger queues the card revision atomically with
        // this update; this flag only wakes the delivery worker promptly.
        editQueued = true;
    }
    // The bot only ever touches ITS OWN messages: dismissing grays out the
    // notification card; the user's original message is never deleted.
    if (bot && editQueued) bot->requestNotifyCardPost();
    return json{{"ok", true}};
}

bool pendingDiscordReviewCard(Db* db, const std::string& notifyChannelId,
                              const std::string& notifyMessageId) {
    if (!db || notifyChannelId.empty() || notifyMessageId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT EXISTS(
 SELECT 1 FROM discord_notify_cards n
 JOIN discord_messages m ON m.id=n.discord_message_row_id
 WHERE n.notify_channel_id=? AND n.notify_message_id=?
   AND m.state='new' AND m.kind IN ('suggestion','bug')
)
)sql");
    q.bind(1, notifyChannelId);
    q.bind(2, notifyMessageId);
    q.executeStep();
    return q.getColumn(0).getInt() != 0;
}

bool terminalDiscordReviewCard(Db* db, const std::string& notifyChannelId,
                               const std::string& notifyMessageId) {
    if (!db || notifyChannelId.empty() || notifyMessageId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT EXISTS(
 SELECT 1 FROM discord_notify_cards n
 LEFT JOIN discord_messages m ON m.id=n.discord_message_row_id
 WHERE n.notify_channel_id=? AND n.notify_message_id=?
   AND n.post_state='posted'
   AND (n.item_id IS NOT NULL OR
        (m.kind IN ('suggestion','bug') AND m.state!='new'))
)
)sql");
    q.bind(1, notifyChannelId);
    q.bind(2, notifyMessageId);
    q.executeStep();
    return q.getColumn(0).getInt() != 0;
}

int reconcileLegacyTerminalReviewControls(Db* db) {
    if (!db) return 0;
    auto lk = db->guard();
    constexpr const char* kMarker =
        "discord_terminal_review_control_cleanup_v2";
    if (db->getSetting(lk.token(), kMarker) == "1") return 0;

    SQLite::Transaction tx(db->raw(lk.token()));
    const std::string now = nowIsoUtc();
    SQLite::Statement queue(db->raw(lk.token()), R"sql(
INSERT OR IGNORE INTO discord_review_control_cleanups(
 card_row_id,notify_channel_id,notify_message_id,cleanup_state,
 created_at,updated_at)
SELECT id,notify_channel_id,notify_message_id,'pending',?,?
  FROM discord_notify_cards
 WHERE post_state='posted' AND notify_channel_id!=''
   AND notify_message_id!=''
   AND (item_id IS NOT NULL OR EXISTS(
        SELECT 1 FROM discord_messages m
         WHERE m.id=discord_notify_cards.discord_message_row_id
           AND m.kind IN ('suggestion','bug') AND m.state!='new'))
)sql");
    queue.bind(1, now);
    queue.bind(2, now);
    const int changed = queue.exec();
    db->setSetting(lk.token(), kMarker, "1");
    tx.commit();
    return changed;
}

std::vector<DiscordReviewCard> pendingDiscordReviewCards(
    Db* db, std::size_t limit) {
    std::vector<DiscordReviewCard> rows;
    if (!db || limit == 0) return rows;
    // One bounded snapshot avoids reconnect pagination races while still
    // allowing every realistic backlog, not just the oldest 50, to receive
    // controls after an upgrade/restart.
    limit = std::min<std::size_t>(limit, 10000);
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT n.notify_channel_id,n.notify_message_id
  FROM discord_notify_cards n
  JOIN discord_messages m ON m.id=n.discord_message_row_id
 WHERE n.notify_channel_id!='' AND n.notify_message_id!=''
   AND n.post_state='posted' AND m.state='new'
   AND m.kind IN ('suggestion','bug')
 ORDER BY n.created_at DESC,n.id DESC LIMIT ?
)sql");
    q.bind(1, static_cast<int>(limit));
    while (q.executeStep())
        rows.push_back({q.getColumn(0).getString(),
                        q.getColumn(1).getString()});
    return rows;
}

DiscordReviewOutcome reviewNotificationCard(
    Db* db, DiscordBot* bot, const std::string& notifyChannelId,
    const std::string& notifyMessageId, const std::string& actorUserId,
    bool approve, const std::string& note, int approvalPriority) {
    DiscordReviewOutcome out;
    if (!db || notifyChannelId.empty() || notifyMessageId.empty()) return out;
    try {
    {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT m.id,m.state,COALESCE(m.item_id,0)
  FROM discord_notify_cards n
  JOIN discord_messages m ON m.id=n.discord_message_row_id
 WHERE n.notify_channel_id=? AND n.notify_message_id=?
   AND m.kind IN ('suggestion','bug')
)sql");
        q.bind(1, notifyChannelId);
        q.bind(2, notifyMessageId);
        if (!q.executeStep()) return out;
        out.matched = true;
        out.messageRowId = q.getColumn(0).getInt64();
        out.state = q.getColumn(1).getString();
        out.itemId = q.getColumn(2).getInt64();
    }
    out.authorized = discordAdminUserAllowed(db, actorUserId);
    if (!out.authorized) {
        out.error = "Discord user is not in the admin/developer whitelist";
        return out;
    }
    if (out.state != "new") {
        out.ok = true;
        out.duplicate = true;
        return out;
    }
    if (approve &&
        (approvalPriority < kDiscordReviewLowPriority ||
         approvalPriority > kDiscordReviewHighPriority)) {
        out.error = "Discord approval priority must be low, normal, or high";
        return out;
    }

    json result = approve
        ? promoteSuggestion(db, bot, out.messageRowId, 0, "", "", note,
                            approvalPriority)
        : dismissSuggestion(db, bot, out.messageRowId);
    if (result.value("ok", false)) {
        out.ok = true;
        out.duplicate = result.value("duplicate", false);
        out.itemId = result.value("item_id", out.itemId);
        out.state = approve ? "promoted" : "dismissed";
        return out;
    }

    out.error = result.value("error", "Discord review action failed");
    // Another authorized event may have won between exact lookup and the
    // canonical transition. Re-read and treat every terminal state as an
    // idempotent no-op rather than attempting an opposite transition.
    {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT state,COALESCE(item_id,0) FROM discord_messages WHERE id=?");
        q.bind(1, out.messageRowId);
        if (q.executeStep()) {
            out.state = q.getColumn(0).getString();
            out.itemId = q.getColumn(1).getInt64();
            if (out.state != "new") {
                out.ok = true;
                out.duplicate = true;
                out.error.clear();
            }
        }
    }
    return out;
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = e.what();
        return out;
    }
}

json rescanNewMessages(Db* db) {
    auto lk = db->guard();
    SuggestionDetector detector = makeDetectorLocked(lk.token(), db);
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT id, content FROM discord_messages WHERE state='new'");
    int rescanned = 0, candidates = 0;
    struct Upd { long long id; std::string kind; double score;
                 std::string matched; long long inferred; };
    std::vector<Upd> updates;
    while (q.executeStep()) {
        long long mid = q.getColumn(0).getInt64();
        std::string content = q.getColumn(1).getString();
        DetectionResult det = detector.analyze(content);
        updates.push_back({mid, det.isCandidate ? det.kind : "none", det.score,
                           json(det.matched).dump(), 0});
        ++rescanned;
        if (det.isCandidate) ++candidates;
    }
    // Inference in a second pass (the SELECT statement must be finished
    // before inferProjectLocked opens its own). Same chain as live ingest:
    // content mention first, channel name as the fallback.
    {
        SQLite::Statement q2(db->raw(lk.token()),
            "SELECT m.id, m.content, IFNULL(c.channel_name,'') "
            "FROM discord_messages m "
            "JOIN discord_channels c ON c.id=m.channel_row_id "
            "WHERE m.state='new'");
        std::map<long long, std::pair<std::string, std::string>> rows;
        while (q2.executeStep())
            rows[q2.getColumn(0).getInt64()] = {q2.getColumn(1).getString(),
                                                q2.getColumn(2).getString()};
        for (auto& u : updates) {
            if (u.kind == "none") continue;
            u.inferred = inferProjectLocked(lk.token(), db, rows[u.id].first);
            if (u.inferred == 0)
                u.inferred = inferProjectFromChannelLocked(lk.token(), db, rows[u.id].second);
        }
    }
    for (auto& u : updates) {
        SQLite::Statement up(db->raw(lk.token()),
            "UPDATE discord_messages SET kind=?, score=?, matched=?, "
            "inferred_project_id=? WHERE id=?");
        up.bind(1, u.kind);
        up.bind(2, u.score);
        up.bind(3, u.matched);
        if (u.inferred > 0) up.bind(4, u.inferred); else up.bind(4);
        up.bind(5, u.id);
        up.exec();
    }
    return json{{"ok", true}, {"rescanned", rescanned}, {"candidates", candidates}};
}

} // namespace devhub
