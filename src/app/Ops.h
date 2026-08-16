#pragma once
#include "Db.h"
#include "Ingest.h"
#include "devhub/SuggestionDetector.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace devhub {

class DiscordBot;

struct ItemEdit {
    long long projectId = 0;
    std::string title;
    std::string type;
    std::string status;
    int priority = 2;
    std::string dueDate;
    std::string reviewDate;
    std::string blockedReason;
    std::string body;
};

struct PendingSuggestionEdit {
    long long projectId = 0; // zero keeps the source pending and unassigned
    std::string title;
    std::string type;
    int priority = 2;
    std::string content;
    std::string adminNote;
};

// Shared operations used by both the native GUI and the HTTP API (bot).
//
// Functions with the "Locked" suffix require a Db::Held proof from the
// caller's live guard. The others acquire their own guard. Db uses a recursive
// mutex so snapshot builders can safely call existing guard-taking helpers.

long long ensureSourceLocked(Db::Held held, Db* db, const std::string& name,
                             const std::string& platform,
                             const std::string& handle);

long long insertItemLocked(Db::Held held, Db* db, long long projectId, const std::string& type,
                           const std::string& title, const std::string& body,
                           int priority, long long sourceId,
                           const std::string& origin, const std::string& dueDate,
                           const std::string& tags);

// A work item may represent the same request from several people. These
// helpers maintain that contributor list and its per-person credit state.
bool addItemSourceLocked(Db::Held held, Db* db, long long itemId, long long sourceId);
bool removeItemSourceLocked(Db::Held held, Db* db, long long itemId, long long sourceId);
bool setItemSourceCreditedLocked(Db::Held held, Db* db, long long itemId, long long sourceId,
                                 bool credited);

// Moves every ticket attribution from one canonical contributor to another.
// The old source row remains as a durable identity alias so future messages
// from its stable platform/handle resolve to the surviving contributor.
bool mergeSourcesLocked(Db::Held held, Db* db, long long sourceId, long long targetSourceId,
                        std::string& error);

// Consolidates duplicate feedback without losing either item or contributor.
// The source item becomes an audit-only `merged` record linked to the target.
bool mergeItemsLocked(Db::Held held, Db* db, long long sourceItemId, long long targetItemId,
                      std::string& error);

// Saves the native item editor as one transaction. A changed project keeps the
// global item ID and item-linked evidence, moves its completion event, and lets
// the existing item trigger refresh any posted Discord card. Missing, archived,
// or merge-linked destinations fail closed without partially saving other
// fields. An unchanged project remains a normal idempotent edit.
bool saveItemEditLocked(Db::Held held, Db* db, long long itemId, const ItemEdit& edit,
                        bool* projectChanged, std::string& error,
                        DiscordBot* bot = nullptr);

// Handles completed_at bookkeeping, calendar completion events and the
// activity log for any status transition. When the item came from Discord
// and a notification card exists, `bot` (optional) keeps the card in sync
// (completed / reopened).
void setItemStatusLocked(Db::Held held, Db* db, long long itemId, const std::string& status,
                         DiscordBot* bot = nullptr);

SuggestionDetector makeDetectorLocked(Db::Held held, Db* db);

// Which project does this message talk about? Longest case-insensitive match
// of any project name or alias (projects.aliases, comma separated) wins.
// Returns 0 when nothing matches - the message stays a general flag.
long long inferProjectLocked(Db::Held held, Db* db, const std::string& content);

// Same matcher, fed the CHANNEL name: plugin-specific channels usually carry
// the project name ("#xa-dashboard" -> XA Dashboard). Separators are
// normalized to spaces before matching. Used as the fallback when the
// message content itself names no project.
long long inferProjectFromChannelLocked(Db::Held held, Db* db, const std::string& channelName);

// ---- guard-taking top level operations ----
// (Discord ingest + bot monitoring helpers live in Ingest.h, which is kept
// free of nlohmann includes for the dpp translation unit.)

// Markdown bundle of a project's open work for AI review sessions.
// Returns "" when the project does not exist.
std::string buildReviewMarkdown(Db* db, long long projectId);

// Explicit operator-selected subset. IDs are normalized deterministically and
// must all still be active members of this project; invalid/stale/empty or
// over-200 selections fail closed with an empty result.
std::string buildReviewMarkdown(Db* db, long long projectId,
                                const std::vector<long long>& selectedItemIds);

// Markdown release-note and contributor draft built from recently completed
// work. Returns "" when the project does not exist.
std::string buildReleaseDraft(Db* db, long long projectId, int days);

// Additive, snapshot-backed historical ticket reconciliation. The payload
// selects explicit item IDs from source_data_root\devhub.db and may explicitly
// authorize only the missing source projects needed by those items. Existing
// work is never updated or deleted: complete exact replays are no-ops and any
// same-ID/title/body or lineage mismatch rejects the whole batch.
nlohmann::json importWorkItems(Db* db, const nlohmann::json& payload);

// Promote a detected Discord message into a project item with source credit.
// projectIdOverride/typeOverride may be 0/"" to use the channel defaults.
// priorityOverride must be a canonical P1-P4 value; Discord review reactions
// intentionally use only P1-P3 while manual editing retains P4 critical.
// Updates the message's notification card via `bot` (optional).
// Returns {ok, item_id} or {ok:false, error}.
nlohmann::json promoteSuggestion(Db* db, DiscordBot* bot, long long msgId,
                                 long long projectIdOverride,
                                 const std::string& typeOverride,
                                 const std::string& titleOverride,
                                 const std::string& adminNoteAppend = {},
                                 int priorityOverride = 2);

// Edits one exact pending Discord suggestion. Without a selected project the
// revised content/admin note are saved in place and the source remains in the
// inbox. With an active project, the edits and canonical promotion share the
// existing item/credit/attachment/card transaction.
nlohmann::json savePendingSuggestionEdit(
    Db* db, DiscordBot* bot, long long msgId,
    const PendingSuggestionEdit& edit);

// Requeue one item-linked failed attachment through the same bounded Discord
// refetch/validation worker. Captured, queued, and saved rows are untouched.
bool retryFailedTicketImage(Db* db, DiscordBot* bot, long long attachmentRowId);

// One-time compatibility repair for image rows failed by the removed Discord-MIME
// gate. Returns the number moved back to queued; all other failures stay put.
int requeueLegacyTicketImageFailures(Db* db);

// Dismiss: marks the capture dismissed and grays out its notification card.
// The bot only ever edits ITS OWN messages - the user's original message on
// Discord is never touched.
nlohmann::json dismissSuggestion(Db* db, DiscordBot* bot, long long msgId);

// Re-run the detector over unhandled messages (after pattern edits).
nlohmann::json rescanNewMessages(Db* db);

// Calendar entries (events, due items, builds) between two YYYY-MM-DD dates.
nlohmann::json calendarEntries(Db* db, const std::string& from,
                               const std::string& to);

} // namespace devhub
