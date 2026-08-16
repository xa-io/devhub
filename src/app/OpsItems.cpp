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

// Item, contributor, merge, and status operations extracted from Ops.cpp.
// Keeps a message's Discord notification card in sync with its item's
// status (defined with the other card helpers below).
void updateNotifyCardForItemLocked(Db::Held held, Db* db, DiscordBot* bot,
                                          long long itemId,
                                          const std::string& status);

long long canonicalSourceIdLocked(Db::Held held, Db* db, long long sourceId) {
    if (!db || sourceId <= 0) return 0;
    std::vector<long long> seen;
    long long current = sourceId;
    for (int depth = 0; depth < 64; ++depth) {
        if (std::find(seen.begin(), seen.end(), current) != seen.end())
            throw std::runtime_error("contributor merge lineage contains a cycle");
        seen.push_back(current);
        SQLite::Statement exists(db->raw(held),
            "SELECT 1 FROM sources WHERE id=?");
        exists.bind(1, current);
        if (!exists.executeStep()) return 0;
        SQLite::Statement q(db->raw(held),
            "SELECT target_source_id FROM source_merges WHERE source_id=?");
        q.bind(1, current);
        if (!q.executeStep()) return current;
        current = q.getColumn(0).getInt64();
        if (current <= 0)
            throw std::runtime_error("contributor merge lineage has an invalid target");
    }
    throw std::runtime_error("contributor merge lineage is too deep");
}

long long ensureSourceLocked(Db::Held held, Db* db, const std::string& name,
                             const std::string& platform,
                             const std::string& handle) {
    std::string n = trim(name);
    if (n.empty()) return 0;
    const std::string p = trim(platform);
    const std::string h = trim(handle);
    // Discord display names are mutable and non-unique. Resolve a known
    // contributor by the stable platform/user ID first so a rename cannot
    // evade a leaderboard exclusion or split credit history.
    if (!p.empty() && !h.empty()) {
        SQLite::Statement stable(db->raw(held),
            "SELECT s.id,s.name FROM sources s "
            "LEFT JOIN source_merges m ON m.source_id=s.id "
            "WHERE s.platform=? COLLATE NOCASE AND s.handle=? "
            "ORDER BY CASE WHEN m.source_id IS NULL THEN 0 ELSE 1 END,s.id "
            "LIMIT 1");
        stable.bind(1, p);
        stable.bind(2, h);
        if (stable.executeStep()) {
            const long long id = stable.getColumn(0).getInt64();
            const long long canonicalId = canonicalSourceIdLocked(held, db, id);
            // Preserve the alias row's historical name and stable identity.
            // Renaming the canonical target from an alias event would turn a
            // later message from John into a visible rename of Susan.
            if (canonicalId != id) return canonicalId;
            const std::string oldName = stable.getColumn(1).getString();
            if (oldName != n) {
                SQLite::Statement available(db->raw(held),
                    "SELECT 1 FROM sources WHERE name=? COLLATE NOCASE AND id<>?");
                available.bind(1, n);
                available.bind(2, id);
                if (!available.executeStep()) {
                    SQLite::Statement rename(db->raw(held),
                        "UPDATE sources SET name=? WHERE id=?");
                    rename.bind(1, n);
                    rename.bind(2, id);
                    rename.exec();
                }
            }
            return id;
        }
    }
    {
        SQLite::Statement q(db->raw(held),
            "SELECT id,platform,handle FROM sources WHERE name=? COLLATE NOCASE "
            "ORDER BY id LIMIT 1");
        q.bind(1, n);
        if (q.executeStep()) {
            const long long id = q.getColumn(0).getInt64();
            const long long canonicalId = canonicalSourceIdLocked(held, db, id);
            const std::string existingPlatform = q.getColumn(1).getString();
            const std::string existingHandle = q.getColumn(2).getString();
            const bool exactIdentity = !h.empty() && !existingHandle.empty() &&
                toLower(existingPlatform) == toLower(p) &&
                existingHandle == h;
            const bool canonicalCanAdoptIdentity = canonicalId == id &&
                !h.empty() && existingHandle.empty();
            if (h.empty() || exactIdentity || canonicalCanAdoptIdentity) {
                if (canonicalId == id && !h.empty() && existingHandle.empty()) {
                    SQLite::Statement identify(db->raw(held),
                        "UPDATE sources SET platform=?,handle=? WHERE id=?");
                    identify.bind(1, p);
                    identify.bind(2, h);
                    identify.bind(3, id);
                    identify.exec();
                }
                return canonicalId;
            }

            // Two Discord users can share one display name. Preserve separate
            // credit identities without exposing their numeric user IDs.
            const std::string base = n + " (" +
                (p.empty() ? std::string("source") : p) + ")";
            n = base;
            for (int suffix = 2;; ++suffix) {
                SQLite::Statement collision(db->raw(held),
                    "SELECT 1 FROM sources WHERE name=? COLLATE NOCASE");
                collision.bind(1, n);
                if (!collision.executeStep()) break;
                n = base + " #" + std::to_string(suffix);
            }
        }
    }
    SQLite::Statement ins(db->raw(held),
        "INSERT INTO sources(name,platform,handle,created_at) VALUES(?,?,?,?)");
    ins.bind(1, n);
    ins.bind(2, p);
    ins.bind(3, h);
    ins.bind(4, nowIsoUtc());
    ins.exec();
    return db->raw(held).getLastInsertRowid();
}

long long insertItemLocked(Db::Held held, Db* db, long long projectId, const std::string& type,
                           const std::string& title, const std::string& body,
                           int priority, long long sourceId,
                           const std::string& origin, const std::string& dueDate,
                           const std::string& tags) {
    if (sourceId > 0) sourceId = canonicalSourceIdLocked(held, db, sourceId);
    std::string now = nowIsoUtc();
    SQLite::Statement ins(db->raw(held),
        "INSERT INTO items(project_id,type,title,body,status,priority,source_id,"
        "origin,due_date,tags,created_at,updated_at) "
        "VALUES(?,?,?,?, 'open', ?,?,?,?,?,?,?)");
    ins.bind(1, projectId);
    ins.bind(2, type);
    ins.bind(3, title);
    ins.bind(4, body);
    ins.bind(5, priority);
    if (sourceId > 0) ins.bind(6, sourceId); else ins.bind(6);
    ins.bind(7, origin);
    ins.bind(8, dueDate);
    ins.bind(9, tags);
    ins.bind(10, now);
    ins.bind(11, now);
    ins.exec();
    long long itemId = db->raw(held).getLastInsertRowid();
    if (sourceId > 0) addItemSourceLocked(held, db, itemId, sourceId);
    return itemId;
}

static void syncLegacyCreditLocked(Db::Held held, Db* db, long long itemId) {
    SQLite::Statement up(db->raw(held), R"sql(
UPDATE items SET
 source_id=COALESCE(source_id,(SELECT MIN(source_id) FROM item_sources WHERE item_id=?)),
 credited=CASE
   WHEN EXISTS(SELECT 1 FROM item_sources WHERE item_id=?)
    AND NOT EXISTS(SELECT 1 FROM item_sources WHERE item_id=? AND credited=0) THEN 1
   ELSE 0 END
WHERE id=?)sql");
    up.bind(1, itemId); up.bind(2, itemId); up.bind(3, itemId); up.bind(4, itemId);
    up.exec();
}

bool addItemSourceLocked(Db::Held held, Db* db, long long itemId, long long sourceId) {
    if (itemId <= 0 || sourceId <= 0) return false;
    sourceId = canonicalSourceIdLocked(held, db, sourceId);
    if (sourceId <= 0) return false;
    SQLite::Statement ins(db->raw(held),
        "INSERT OR IGNORE INTO item_sources(item_id,source_id,credited,added_at) "
        "VALUES(?,?,0,?)");
    ins.bind(1, itemId); ins.bind(2, sourceId); ins.bind(3, nowIsoUtc());
    ins.exec();
    syncLegacyCreditLocked(held, db, itemId);
    return true;
}

bool removeItemSourceLocked(Db::Held held, Db* db, long long itemId, long long sourceId) {
    sourceId = canonicalSourceIdLocked(held, db, sourceId);
    if (sourceId <= 0) return false;
    SQLite::Statement del(db->raw(held),
        "DELETE FROM item_sources WHERE item_id=? AND source_id=?");
    del.bind(1, itemId); del.bind(2, sourceId); del.exec();
    SQLite::Statement primary(db->raw(held),
        "UPDATE items SET source_id=(SELECT MIN(source_id) FROM item_sources WHERE item_id=?) "
        "WHERE id=?");
    primary.bind(1, itemId); primary.bind(2, itemId); primary.exec();
    syncLegacyCreditLocked(held, db, itemId);
    return true;
}

bool setItemSourceCreditedLocked(Db::Held held, Db* db, long long itemId, long long sourceId,
                                 bool credited) {
    sourceId = canonicalSourceIdLocked(held, db, sourceId);
    if (sourceId <= 0) return false;
    SQLite::Statement up(db->raw(held),
        "UPDATE item_sources SET credited=? WHERE item_id=? AND source_id=?");
    up.bind(1, credited ? 1 : 0); up.bind(2, itemId); up.bind(3, sourceId);
    up.exec();
    syncLegacyCreditLocked(held, db, itemId);
    return true;
}

bool mergeSourcesLocked(Db::Held held, Db* db, long long sourceId, long long targetSourceId,
                        std::string& error) {
    error.clear();
    if (!db || sourceId <= 0 || targetSourceId <= 0 ||
        sourceId == targetSourceId) {
        error = "choose a different contributor";
        return false;
    }

    const long long sourceRoot = canonicalSourceIdLocked(held, db, sourceId);
    const long long targetRoot = canonicalSourceIdLocked(held, db, targetSourceId);
    if (sourceRoot <= 0 || targetRoot <= 0) {
        error = "contributor no longer exists";
        return false;
    }
    // A retry of the same already-applied merge is a successful no-op.
    if (sourceRoot == targetRoot) return true;
    if (sourceRoot != sourceId) {
        error = "the source contributor has already been merged";
        return false;
    }

    std::string sourceName, targetName;
    auto readName = [&](long long id, std::string& value) {
        SQLite::Statement q(db->raw(held), "SELECT name FROM sources WHERE id=?");
        q.bind(1, id);
        if (!q.executeStep()) return false;
        value = q.getColumn(0).getString();
        return true;
    };
    if (!readName(sourceRoot, sourceName) || !readName(targetRoot, targetName)) {
        error = "contributor no longer exists";
        return false;
    }

    std::vector<long long> excluded;
    std::string exclusionError;
    if (!parseLeaderboardExcludedSourceIds(
            db->getSetting(held, "leaderboard_excluded_source_ids"), excluded,
            &exclusionError)) {
        error = exclusionError;
        return false;
    }
    bool mergedIdentityHidden = false;
    std::vector<long long> canonicalExclusions;
    for (long long excludedId : excluded) {
        SQLite::Statement exists(db->raw(held),
            "SELECT 1 FROM sources WHERE id=?");
        exists.bind(1, excludedId);
        const long long root = exists.executeStep()
            ? canonicalSourceIdLocked(held, db, excludedId) : excludedId;
        if (root == sourceRoot || root == targetRoot)
            mergedIdentityHidden = true;
        else
            canonicalExclusions.push_back(root);
    }
    if (mergedIdentityHidden) canonicalExclusions.push_back(targetRoot);
    std::sort(canonicalExclusions.begin(), canonicalExclusions.end());
    canonicalExclusions.erase(
        std::unique(canonicalExclusions.begin(), canonicalExclusions.end()),
        canonicalExclusions.end());

    SQLite::Transaction tx(db->raw(held));
    std::vector<long long> affectedItems;
    {
        SQLite::Statement q(db->raw(held),
            "SELECT item_id FROM item_sources WHERE source_id=? ORDER BY item_id");
        q.bind(1, sourceRoot);
        while (q.executeStep())
            affectedItems.push_back(q.getColumn(0).getInt64());
    }

    SQLite::Statement transfer(db->raw(held), R"sql(
INSERT INTO item_sources(item_id,source_id,credited,added_at)
SELECT item_id,?,credited,added_at FROM item_sources WHERE source_id=?
ON CONFLICT(item_id,source_id) DO UPDATE SET
 credited=MAX(item_sources.credited,excluded.credited),
 added_at=MIN(item_sources.added_at,excluded.added_at))sql");
    transfer.bind(1, targetRoot);
    transfer.bind(2, sourceRoot);
    transfer.exec();

    SQLite::Statement removeOld(db->raw(held),
        "DELETE FROM item_sources WHERE source_id=?");
    removeOld.bind(1, sourceRoot);
    removeOld.exec();

    SQLite::Statement legacySource(db->raw(held),
        "UPDATE items SET source_id=?,updated_at=? WHERE source_id=?");
    legacySource.bind(1, targetRoot);
    legacySource.bind(2, nowIsoUtc());
    legacySource.bind(3, sourceRoot);
    legacySource.exec();
    for (long long itemId : affectedItems)
        syncLegacyCreditLocked(held, db, itemId);

    SQLite::Statement alias(db->raw(held),
        "INSERT INTO source_merges(source_id,target_source_id,merged_at) "
        "VALUES(?,?,?)");
    alias.bind(1, sourceRoot);
    alias.bind(2, targetRoot);
    alias.bind(3, nowIsoUtc());
    alias.exec();

    // Exclusion is privacy/presentation policy.  If either identity was
    // hidden, keep the combined identity hidden until the operator explicitly
    // re-enables it.  All aliases are collapsed to canonical IDs.
    std::ostringstream canonical;
    for (std::size_t i = 0; i < canonicalExclusions.size(); ++i) {
        if (i) canonical << ',';
        canonical << canonicalExclusions[i];
    }
    db->setSetting(held, "leaderboard_excluded_source_ids", canonical.str());

    db->logActivity(held,
        "source_merged", 0,
        sourceName + " -> " + targetName + " (" +
            std::to_string(affectedItems.size()) + " ticket attribution" +
            (affectedItems.size() == 1 ? ")" : "s)"));
    tx.commit();
    return true;
}

bool mergeItemsLocked(Db::Held held, Db* db, long long sourceItemId, long long targetItemId,
                      std::string& error) {
    if (sourceItemId <= 0 || targetItemId <= 0 || sourceItemId == targetItemId) {
        error = "choose a different target item";
        return false;
    }
    long long sourceProject = 0, targetProject = 0;
    std::string sourceTitle, sourceBody, sourceStatus, targetStatus;
    auto readItem = [&](long long id, long long& project, std::string& title,
                        std::string& body, std::string& status) {
        SQLite::Statement q(db->raw(held),
            "SELECT project_id,title,body,status FROM items WHERE id=?");
        q.bind(1, id);
        if (!q.executeStep()) return false;
        project = q.getColumn(0).getInt64(); title = q.getColumn(1).getString();
        body = q.getColumn(2).getString(); status = q.getColumn(3).getString();
        return true;
    };
    std::string targetTitle, targetBody;
    if (!readItem(sourceItemId, sourceProject, sourceTitle, sourceBody, sourceStatus) ||
        !readItem(targetItemId, targetProject, targetTitle, targetBody, targetStatus)) {
        error = "item not found";
        return false;
    }
    if (sourceProject != targetProject) {
        error = "feedback can only be merged within the same project";
        return false;
    }
    if (sourceStatus == "merged" || targetStatus == "merged") {
        error = "a merged audit record cannot be used as a merge target";
        return false;
    }
    {
        SQLite::Statement queuedImages(db->raw(held),
            "SELECT COUNT(*) FROM ticket_attachments WHERE item_id IN (?,?) "
            "AND state='queued'");
        queuedImages.bind(1, sourceItemId);
        queuedImages.bind(2, targetItemId);
        queuedImages.executeStep();
        if (queuedImages.getColumn(0).getInt64() > 0) {
            error = "wait for queued ticket attachments to finish before merging";
            return false;
        }
        SQLite::Statement imageTotals(db->raw(held),
            "SELECT COUNT(*),COALESCE(SUM(actual_size),0) "
            "FROM ticket_attachments WHERE item_id IN (?,?) "
            "AND state='saved'");
        imageTotals.bind(1, sourceItemId);
        imageTotals.bind(2, targetItemId);
        imageTotals.executeStep();
        const long long imageCount = imageTotals.getColumn(0).getInt64();
        const long long imageBytes = imageTotals.getColumn(1).getInt64();
        if (imageCount > static_cast<long long>(kMaxTicketImages) ||
            imageBytes > static_cast<long long>(kMaxTicketImageTotalBytes)) {
            error =
                "merge would exceed the 10-attachment or 50 MiB ticket limit";
            return false;
        }
    }

    SQLite::Transaction tx(db->raw(held));
    SQLite::Statement transfer(db->raw(held),
        "INSERT INTO item_sources(item_id,source_id,credited,added_at) "
        "SELECT ?,source_id,credited,added_at FROM item_sources WHERE item_id=? "
        "ON CONFLICT(item_id,source_id) DO UPDATE SET "
        "credited=MAX(item_sources.credited,excluded.credited)");
    transfer.bind(1, targetItemId); transfer.bind(2, sourceItemId); transfer.exec();

    std::string mergedText = targetBody;
    if (!sourceTitle.empty() || !sourceBody.empty()) {
        if (!mergedText.empty()) mergedText += "\n\n---\n";
        mergedText += "Merged feedback: " + sourceTitle;
        if (!sourceBody.empty()) mergedText += "\n\n" + sourceBody;
    }
    SQLite::Statement target(db->raw(held),
        "UPDATE items SET body=?,updated_at=? WHERE id=?");
    target.bind(1, mergedText); target.bind(2, nowIsoUtc()); target.bind(3, targetItemId);
    target.exec();
    SQLite::Statement msg(db->raw(held),
        "UPDATE discord_messages SET item_id=? WHERE item_id=?");
    msg.bind(1, targetItemId); msg.bind(2, sourceItemId); msg.exec();
    SQLite::Statement manualResults(db->raw(held),
        "UPDATE discord_messages SET manual_result_item_id=? "
        "WHERE manual_result_item_id=?");
    manualResults.bind(1, targetItemId);
    manualResults.bind(2, sourceItemId);
    manualResults.exec();
    SQLite::Statement cards(db->raw(held),
        "UPDATE discord_notify_cards SET item_id=?,updated_at=? WHERE item_id=?");
    cards.bind(1, targetItemId); cards.bind(2, nowIsoUtc());
    cards.bind(3, sourceItemId); cards.exec();
    SQLite::Statement images(db->raw(held),
        "UPDATE ticket_attachments SET item_id=?,updated_at=? WHERE item_id=?");
    images.bind(1, targetItemId); images.bind(2, nowIsoUtc());
    images.bind(3, sourceItemId); images.exec();
    SQLite::Statement audit(db->raw(held),
        "INSERT INTO item_merges(source_item_id,target_item_id,merged_at) VALUES(?,?,?) "
        "ON CONFLICT(source_item_id) DO UPDATE SET target_item_id=excluded.target_item_id,"
        "merged_at=excluded.merged_at");
    audit.bind(1, sourceItemId); audit.bind(2, targetItemId); audit.bind(3, nowIsoUtc());
    audit.exec();
    SQLite::Statement delLinks(db->raw(held), "DELETE FROM item_sources WHERE item_id=?");
    delLinks.bind(1, sourceItemId); delLinks.exec();
    SQLite::Statement source(db->raw(held),
        "UPDATE items SET status='merged',credited=1,completed_at='',updated_at=? WHERE id=?");
    source.bind(1, nowIsoUtc()); source.bind(2, sourceItemId); source.exec();
    SQLite::Statement delEvents(db->raw(held),
        "DELETE FROM events WHERE item_id=? AND kind='completion'");
    delEvents.bind(1, sourceItemId); delEvents.exec();
    syncLegacyCreditLocked(held, db, targetItemId);
    db->logActivity(held, "item_merged", sourceProject,
                    sourceTitle + " -> " + targetTitle);
    tx.commit();
    return true;
}

bool saveItemEditLocked(Db::Held held, Db* db, long long itemId, const ItemEdit& edit,
                        bool* projectChanged, std::string& error,
                        DiscordBot* bot) {
    error.clear();
    if (projectChanged) *projectChanged = false;
    if (!db || itemId <= 0 || edit.projectId <= 0) {
        error = "item and destination project are required";
        return false;
    }

    try {
        long long currentProjectId = 0;
        std::string currentProjectName;
        std::string currentStatus;
        {
            SQLite::Statement item(db->raw(held),
                "SELECT i.project_id,p.name,i.status FROM items i "
                "JOIN projects p ON p.id=i.project_id WHERE i.id=?");
            item.bind(1, itemId);
            if (!item.executeStep()) {
                error = "item not found";
                return false;
            }
            currentProjectId = item.getColumn(0).getInt64();
            currentProjectName = item.getColumn(1).getString();
            currentStatus = item.getColumn(2).getString();
        }

        const bool moved = currentProjectId != edit.projectId;
        std::string destinationName = currentProjectName;
        if (moved) {
            SQLite::Statement destination(db->raw(held),
                "SELECT name,archived FROM projects WHERE id=?");
            destination.bind(1, edit.projectId);
            if (!destination.executeStep()) {
                error = "destination project not found";
                return false;
            }
            destinationName = destination.getColumn(0).getString();
            if (destination.getColumn(1).getInt() != 0) {
                error = "destination project is archived";
                return false;
            }

            SQLite::Statement merged(db->raw(held),
                "SELECT EXISTS(SELECT 1 FROM item_merges "
                "WHERE source_item_id=? OR target_item_id=?)");
            merged.bind(1, itemId);
            merged.bind(2, itemId);
            merged.executeStep();
            if (currentStatus == "merged" ||
                merged.getColumn(0).getInt() != 0) {
                error =
                    "merge-linked items cannot move projects without moving "
                    "their full audit lineage";
                return false;
            }
        }

        SQLite::Transaction tx(db->raw(held));
        SQLite::Statement update(db->raw(held),
            "UPDATE items SET project_id=?,title=?,type=?,priority=?,"
            "due_date=?,review_date=?,blocked_reason=?,body=?,updated_at=? "
            "WHERE id=?");
        update.bind(1, edit.projectId);
        update.bind(2, edit.title);
        update.bind(3, edit.type);
        update.bind(4, edit.priority);
        update.bind(5, edit.dueDate);
        update.bind(6, edit.reviewDate);
        update.bind(7, edit.blockedReason);
        update.bind(8, edit.body);
        update.bind(9, nowIsoUtc());
        update.bind(10, itemId);
        if (update.exec() != 1)
            throw std::runtime_error("item changed before it could be saved");

        if (moved) {
            SQLite::Statement events(db->raw(held),
                "UPDATE events SET project_id=? "
                "WHERE item_id=? AND kind='completion'");
            events.bind(1, edit.projectId);
            events.bind(2, itemId);
            events.exec();
        }

        setItemStatusLocked(held, db, itemId, edit.status, bot);
        if (moved) {
            db->logActivity(held,
                "item_moved", edit.projectId,
                "I" + std::to_string(itemId) + " " + edit.title + " (" +
                    currentProjectName + " -> " + destinationName + ")");
        }
        tx.commit();

        if (moved && bot) bot->requestNotifyCardPost();
        if (projectChanged) *projectChanged = moved;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (projectChanged) *projectChanged = false;
        return false;
    }
}

void setItemStatusLocked(Db::Held held, Db* db, long long itemId, const std::string& status,
                         DiscordBot* bot) {
    std::string prevStatus, title;
    long long projectId = 0;
    {
        SQLite::Statement q(db->raw(held),
            "SELECT status,title,project_id FROM items WHERE id=?");
        q.bind(1, itemId);
        if (!q.executeStep()) return;
        prevStatus = q.getColumn(0).getString();
        title = q.getColumn(1).getString();
        projectId = q.getColumn(2).getInt64();
    }
    // A merged source row is an immutable audit pointer. Its contributors,
    // messages, cards, images, and completion event have already moved to the
    // target item, so reopening it would manufacture a second live ticket.
    if (prevStatus == "merged" && status != "merged") return;
    if (prevStatus == status) return;

    {
        SQLite::Statement up(db->raw(held),
            "UPDATE items SET status=?, updated_at=? WHERE id=?");
        up.bind(1, status);
        up.bind(2, nowIsoUtc());
        up.bind(3, itemId);
        up.exec();
    }
    if (status == "completed") {
        SQLite::Statement up(db->raw(held),
            "UPDATE items SET completed_at=? WHERE id=?");
        up.bind(1, nowIsoUtc());
        up.bind(2, itemId);
        up.exec();
        SQLite::Statement ev(db->raw(held),
            "INSERT INTO events(project_id,item_id,title,kind,date,created_at) "
            "VALUES(?,?,?,'completion',?,?)");
        ev.bind(1, projectId);
        ev.bind(2, itemId);
        ev.bind(3, title);
        ev.bind(4, todayLocal());
        ev.bind(5, nowIsoUtc());
        ev.exec();
        db->logActivity(held, "item_completed", projectId, title);
    } else if (prevStatus == "completed") {
        SQLite::Statement up(db->raw(held),
            "UPDATE items SET completed_at='' WHERE id=?");
        up.bind(1, itemId);
        up.exec();
        SQLite::Statement del(db->raw(held),
            "DELETE FROM events WHERE item_id=? AND kind='completion'");
        del.bind(1, itemId);
        del.exec();
    }
    updateNotifyCardForItemLocked(held, db, bot, itemId, status);
}

} // namespace devhub
