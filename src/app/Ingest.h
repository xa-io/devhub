#pragma once
// Discord ingest bridge - deliberately free of nlohmann/json includes.
//
// DiscordBot.cpp must not see vcpkg's nlohmann (dpp.lib is built against its
// bundled copy, and both use the same include guard - mixing them in one TU
// causes ABI-mismatched link errors). Everything the bot needs from the
// database goes through these plain-string helpers, implemented in Ops.cpp.
#include <cstddef>
#include <string>
#include <vector>

#include "DiscordAttachment.h"

namespace devhub {

class Db;
class DiscordBot;

struct IngestOutcome {
    bool ingested = false;
    bool duplicate = false;
    bool failed = false;
    bool appendedToItem = false; // existing promoted item gained note/evidence
    bool skippedDisabled = false;
    bool hasCard = false;      // a notify card already exists for this row
    std::string kind = "none"; // suggestion | bug | none (when ingested)
    double score = 0;          // detector confidence (when ingested)
    long long messageRowId = 0; // discord_messages.id (when ingested)
    long long itemId = 0;       // existing item enriched by a follow-up reply
    int imageCount = 0;         // eligible metadata rows associated with capture
    int imagesQueued = 0;       // newly queued rows on an existing item
    std::string error;          // local diagnostic when failed is true
};

// Durable representation of one authorized mention command. The command row
// is the replay key; pending work survives reconnect/restart and is completed
// in the same SQLite transaction as the target mutation.
struct ManualCaptureCommand {
    long long commandRowId = 0;
    std::string commandMessageId;
    std::string targetMessageId;
    std::string channelId;
    std::string channelName;
    std::string guildId;
    std::string guildName;
    std::string adminName;
    std::string adminId;
    std::string note;
    std::string postedAt;
    int attempts = 0;
    std::vector<DiscordAttachmentMeta> commandAttachments;
};

struct ManualCommandStageOutcome {
    bool accepted = false;
    bool duplicate = false;
    std::string state; // pending | done | failed
    ManualCaptureCommand command;
};

struct ManualCommandFailureOutcome {
    bool retryScheduled = false;
    bool terminal = false;
    int attempts = 0;
    std::string error;
};

// Durable post-commit work for a completed manual command. The target mutation
// is already exactly-once; this record only reconciles idempotent Discord-side
// effects and never re-applies the note or attachment transfer.
struct ManualCaptureEffect {
    long long commandRowId = 0;
    std::string commandMessageId;
    std::string channelId;
    std::string channelName;
    std::string guildId;
    std::string guildName;
    std::string adminName;
    std::string resultKind; // captured | appended | repeat
    long long messageRowId = 0;
    long long itemId = 0;
    int imagesQueued = 0;
    int attempt = 0;
    std::string targetMessageId;
    std::string targetAuthor;
    std::string targetContent;
    std::string targetKind;
    double targetScore = 1.0;
};

// Result of the direct-admin fast path. Only an exact active project mapping
// on the source Discord channel enables automatic normal-priority promotion;
// inferred/common-channel projects remain in the approval inbox.
struct MappedManualPromotionOutcome {
    bool mapped = false;
    bool ok = false;
    bool duplicate = false;
    long long itemId = 0;
    std::string error;
};

// One Discord message through the full pipeline: channel row find/create,
// enabled check, suggestion/bug detection, project inference, insert,
// last-read advance. Used by the embedded bot and the HTTP ingest API.
// asCommand records the message but skips detection entirely (kind stays
// "none"). The dedicated manual-command helpers below own authorized mention
// lifecycle/replay handling; asCommand remains for simple commands such as
// !tickets.
IngestOutcome ingestDiscordMessage(Db* db,
                                   const std::string& channelId,
                                   const std::string& channelName,
                                   const std::string& guildId,
                                   const std::string& guildName,
                                   const std::string& messageId,
                                   const std::string& author,
                                   const std::string& authorId,
                                   const std::string& content,
                                   const std::string& postedAt,
                                   bool asCommand = false,
                                   const std::vector<DiscordAttachmentMeta>& attachments = {});

// Reconciles one Discord MESSAGE_UPDATE against its durable source row.
// Replays are no-ops, deleted markers never reopen, and a promoted source keeps
// its item ownership while its still-available source text/card is refreshed.
IngestOutcome reconcileDiscordMessageEdit(
    Db* db, const std::string& channelId, const std::string& channelName,
    const std::string& guildId, const std::string& guildName,
    const std::string& messageId, const std::string& author,
    const std::string& authorId, const std::string& content,
    const std::string& postedAt,
    const std::vector<DiscordAttachmentMeta>& attachments = {});

// Parse the editable Discord admin/developer whitelist. Accepted separators
// are commas, semicolons and ASCII whitespace. Every entry must be a positive
// decimal uint64 Discord user ID; malformed nonempty lists fail as a whole.
bool parseDiscordAdminUserIds(const std::string& value,
                              std::vector<std::string>& userIds,
                              std::string* error = nullptr);

// Exact membership in settings "discord_admin_user_ids". The setting is read
// for every privileged event so additions and revocations need no reconnect.
bool discordAdminUserAllowed(Db* db, const std::string& userId);

// Public/community leaderboard projection. Exclusions are a presentation
// preference only; contributor attribution, credit, release history, and
// ticket drill-down remain untouched.
struct LeaderboardEntry {
    long long sourceId = 0;
    std::string name;
    long long submitted = 0;
    long long shipped = 0;
};
bool parseLeaderboardExcludedSourceIds(
    const std::string& value, std::vector<long long>& sourceIds,
    std::string* error = nullptr);
bool loadLeaderboardExcludedSourceIds(
    Db* db, std::vector<long long>& sourceIds, std::string* error = nullptr);
bool setLeaderboardSourceExcluded(Db* db, long long sourceId, bool excluded,
                                  std::string* error = nullptr);
std::vector<LeaderboardEntry> contributorLeaderboard(
    Db* db, std::size_t limit = 10, std::string* error = nullptr);
// Discord-safe compact rendering used by !leaderboard. At most ten rows are
// emitted, and contributor names cannot introduce mentions or Markdown.
std::string compactLeaderboardDescription(
    const std::vector<LeaderboardEntry>& entries);

// Exact bot-owned notification-card moderation. Callers supply the reacting
// Discord user for a fresh whitelist check on every event. Terminal/replayed
// actions are successful no-ops; a reply note is appended only inside the
// same transaction that first promotes the source. Reaction approval accepts
// only P1-P3; the default keeps reply-with-note approval at P2 normal.
struct DiscordReviewOutcome {
    bool matched = false;
    bool authorized = false;
    bool ok = false;
    bool duplicate = false;
    long long messageRowId = 0;
    long long itemId = 0;
    std::string state;
    std::string error;
};
inline constexpr int kDiscordReviewLowPriority = 1;
inline constexpr int kDiscordReviewNormalPriority = 2;
inline constexpr int kDiscordReviewHighPriority = 3;
struct DiscordReviewCard {
    std::string channelId;
    std::string messageId;
};
DiscordReviewOutcome reviewNotificationCard(
    Db* db, DiscordBot* bot, const std::string& notifyChannelId,
    const std::string& notifyMessageId, const std::string& actorUserId,
    bool approve, const std::string& note = {},
    int approvalPriority = kDiscordReviewNormalPriority);
bool pendingDiscordReviewCard(Db* db, const std::string& notifyChannelId,
                               const std::string& notifyMessageId);
// Replies approve pending cards when possible, then append to the exact ticket.
// Without a project, the note stays on the pending source for later promotion.
// The reply's Discord ID, note, and attachment metadata commit together for
// replay safety. Attachment-only replies are supported.
DiscordReviewOutcome replyToNotificationCard(
    Db* db, DiscordBot* bot, const std::string& notifyChannelId,
    const std::string& notifyMessageId, const std::string& actorUserId,
    const std::string& replyMessageId, const std::string& note,
    const std::vector<DiscordAttachmentMeta>& attachments = {});
// Exact terminal review-card lookup used before removing every reaction from
// that bot-owned message. Missing, pending, and unrelated cards fail closed.
bool terminalDiscordReviewCard(Db* db, const std::string& notifyChannelId,
                               const std::string& notifyMessageId);
// One-time upgrade reconciliation: terminal cards rendered by an older build
// enter the dedicated all-reaction cleanup outbox. This never queues a Discord
// message edit or targets a message outside exact notification-card lineage.
int reconcileLegacyTerminalReviewControls(Db* db);
std::vector<DiscordReviewCard> pendingDiscordReviewCards(
    Db* db, std::size_t limit = 50);

// Atomically stage an authorized mention command, its stripped note and safe
// attachment metadata. Row uniqueness is a receipt, not proof of completion:
// duplicates report the persisted pending/done/failed lifecycle.
ManualCommandStageOutcome stageManualCaptureCommand(
    Db* db, const std::string& channelId, const std::string& channelName,
    const std::string& guildId, const std::string& guildName,
    const std::string& commandMessageId, const std::string& adminName,
    const std::string& adminId, const std::string& commandContent,
    const std::string& postedAt, const std::string& targetMessageId,
    const std::string& note,
    const std::vector<DiscordAttachmentMeta>& attachments = {});

// Select one due command. The bot keeps only one REST fetch in flight; no
// persistent "claimed" state is needed, so a process exit leaves it pending.
bool nextPendingManualCaptureCommand(Db* db, ManualCaptureCommand& command);

// Record a bounded/redacted fetch failure. Transient failures receive bounded
// backoff and at most five attempts; terminal/exhausted work remains visible as
// failed and a fresh Discord command can be submitted.
ManualCommandFailureOutcome recordManualCaptureCommandFailure(
    Db* db, long long commandRowId, const std::string& error, bool retryable);

// Apply the target capture and mark its command done in one transaction. This
// is the exactly-once boundary for non-empty follow-up notes.
IngestOutcome completeManualCaptureCommand(
    Db* db, const ManualCaptureCommand& command,
    const std::string& targetAuthor, const std::string& targetAuthorId,
    const std::string& targetContent, const std::string& targetPostedAt,
    const std::vector<DiscordAttachmentMeta>& targetAttachments = {});

// Claim one due post-commit effect with a bounded lease. Missing callbacks are
// reclaimed after 45 seconds; five failed/expired attempts become visible as
// terminal state without reopening the capture command.
bool nextPendingManualCaptureEffect(Db* db, ManualCaptureEffect& effect);
ManualCommandFailureOutcome recordManualCaptureEffectFailure(
    Db* db, long long commandRowId, int attempt, const std::string& error,
    bool retryable);
bool completeManualCaptureEffect(Db* db, long long commandRowId, int attempt);

// Promote a completed direct admin capture at normal P2 when its Discord
// channel has an exact active project mapping. Promotion retains the existing
// atomic contributor-credit and notification-lineage transaction.
MappedManualPromotionOutcome promoteMappedManualCapture(
    Db* db, DiscordBot* bot, long long messageRowId);

// Manual capture: an authorized admin directly mentions the bot or replies to
// a user's message while mentioning it. Force-registers the direct ping or
// replied-to message as an explicit candidate (score 100%, matched with
// "manual"). Configured bug detection retains `bug` so mapped cold entries
// promote as fixes; unmatched content uses the suggestion fallback. Eligible
// target/reply image metadata is attached. adminNote (reply text minus
// the mention) is stored on the row, shown in the inbox, and carried onto the
// item at promote time. Idempotent for already captured messages: kind is
// upgraded / the note appended, never duplicated; a dismissed capture is
// resurrected to the inbox. If the exact source is already promoted, an
// authorized reply appends its note and only previously unseen target/reply
// images to that existing item instead of creating another inbox entry.
IngestOutcome manualCaptureSuggestion(Db* db,
                                      const std::string& channelId,
                                      const std::string& channelName,
                                      const std::string& guildId,
                                      const std::string& guildName,
                                      const std::string& messageId,
                                      const std::string& author,
                                      const std::string& authorId,
                                      const std::string& content,
                                      const std::string& postedAt,
                                      const std::string& adminNote,
                                      const std::vector<DiscordAttachmentMeta>& attachments = {});

// Finishes queued image saves after the embedded bot reconnects. Only rows
// already linked to real promoted items are eligible. The exact legacy
// Discord-MIME mismatch failure is requeued once through the corrected path.
void resumeQueuedTicketImages(Db* db, DiscordBot* bot, bool recoverySweep);

// Recompose the bot-owned notification card from the item's current status
// and image saved/queued/failed counts. Never modifies a user's Discord post.
void refreshTicketNotifyCard(Db* db, DiscordBot* bot, long long itemId);

// Restore a missing notification lineage for a persisted Discord result. A
// live or scrubbed terminal source row is preferred; an item-only row remains
// the compatibility fallback when an older database already removed it.
void ensureTicketNotifyCard(Db* db, DiscordBot* bot, long long messageRowId,
                            long long itemId);

// Durable notification delivery pump. It claims at most one exact-orphan
// cleanup, one create, and one revisioned edit per call; late callbacks cannot
// overwrite newer state, and stale create IDs enter the bounded cleanup queue.
void resumePendingNotifyCards(Db* db, DiscordBot* bot);

// Operator recovery for a terminal notification create/edit row. A create is
// returned to pending; an edit receives a new revision so stale callbacks from
// the exhausted attempt cannot acknowledge the retry.
bool retryFailedNotifyCard(Db* db, DiscordBot* bot, long long cardRowId);

// Explicit replacement recovery for a deleted bot message or retired target
// channel. Retargets a terminal row to the currently saved notification
// channel and starts a fresh generation. The GUI confirms this separately and
// tells the operator to remove any surviving old card manually; DevHub does not
// delete a persisted remote target that this callback did not just create.
// When supplied, expectedOperation/revision bind the confirmation to the
// exact failed generation the operator reviewed.
bool recreateFailedNotifyCard(Db* db, DiscordBot* bot, long long cardRowId,
                              const std::string& expectedOperation = {},
                              int expectedRevision = -1);

// Locally acknowledge one exact failed create/edit generation without
// changing transport state or contacting Discord. The operation and revision
// guard prevents a stale UI action from hiding a newer failure.
bool dismissFailedNotifyCardAlert(Db* db, long long cardRowId,
                                  const std::string& operation,
                                  int expectedRevision);

// Undo a local acknowledgement only while that exact generation is still the
// card's current failed operation.
bool restoreFailedNotifyCardAlert(Db* db, long long cardRowId,
                                  const std::string& operation,
                                  int expectedRevision);

struct NotifyFailureAlertTarget {
    long long cardRowId = 0;
    int revision = 0;
};

struct NotifyFailureDismissResult {
    bool ok = false;
    int changed = 0;
};

inline constexpr std::size_t kMaxNotifyFailureDismissBatch = 200;

// Acknowledge only the exact old-message edit-limit generations in the
// operator-confirmed snapshot. Other/newer delivery and cleanup failures are
// never included, even if they appear while the confirmation is open.
NotifyFailureDismissResult dismissOldMessageNotifyCardAlerts(
    Db* db, const std::vector<NotifyFailureAlertTarget>& targets);

// Requeue a terminal exact callback-generated orphan cleanup. Successful
// cleanup (including Discord 404/already absent) removes the queue row; stale
// failures cannot overwrite a newer operator retry generation.
bool retryFailedNotifyOrphan(Db* db, DiscordBot* bot, long long orphanRowId);

// Requeue one terminal exact-card all-reaction cleanup. The durable row owns
// the exact bot-message identity and remains independent from card create/edit
// delivery failures.
bool retryFailedReviewControlCleanup(Db* db, DiscordBot* bot,
                                     long long cleanupRowId);

// Idempotently stage one exact terminal card for all-reaction cleanup.
// Re-staging an in-flight target advances its generation so an older callback
// cannot delete the newer request.
bool stageReviewControlCleanup(Db* db, long long cardRowId,
                               const std::string& notifyChannelId,
                               const std::string& notifyMessageId);

// ---- !tickets command (admin-only queue status in Discord) ----------------
// Projects the menu lists: discord_tickets=1, not archived, and at least one
// open/in_progress/blocked fix or implementation, ordered by name. openCount
// is the exact active-ticket count used to decide whether the project appears.
struct TicketMenuEntry {
    long long projectId = 0;
    std::string name;
    int openCount = 0;
};
std::vector<TicketMenuEntry> ticketMenuProjects(Db* db);

// One project's open/in_progress/blocked fix+implementation titles, fixes first.
struct TicketLine {
    std::string type;  // fix | implementation
    std::string title;
};
std::vector<TicketLine> ticketTitlesForProject(Db* db, long long projectId);

// Cross-project active-ticket search for the authorized !xatickets command.
// Matching includes title and body, but the projection deliberately carries
// only ticket identity, project, and title into the Discord presentation path.
struct TicketSearchEntry {
    long long itemId = 0;
    std::string projectName;
    std::string title;
};
struct TicketSearchResult {
    std::vector<TicketSearchEntry> entries;
    std::size_t totalMatches = 0;
};
TicketSearchResult searchActiveTicketTitles(
    Db* db, const std::string& query, std::size_t maxResults = 500);

// Search cards use bounded, UTF-8-safe title previews and deterministic pages.
// The ellipsis counts toward maxCharacters; controls/newlines become spaces.
std::string ticketSearchTitlePreview(
    const std::string& title, std::size_t maxCharacters = 50);
std::vector<std::string> ticketSearchPages(
    const std::vector<TicketSearchEntry>& entries,
    std::size_t pageSize = 20, std::size_t maxTitleCharacters = 50);

// Monitoring lookups for the embedded bot (each takes the DB guard itself).
bool discordGuildEnabled(Db* db, const std::string& guildId);
bool discordChannelEnabled(Db* db, const std::string& channelId);
bool discordWatch(Db* db, const std::string& guildId,
                  const std::string& channelId);
void discordSetChannelMetadata(Db* db, const std::string& channelId,
                               const std::string& name,
                               const std::string& guildId,
                               const std::string& guildName);
std::string discordNextUnnamedChannel(Db* db, long long& afterRowId);
std::string discordChannelLastRead(Db* db, const std::string& channelId);
void discordSetGuildName(Db* db, const std::string& guildId,
                         const std::string& name);

// The bot just looked at this channel (history fetch completed or a live
// event arrived) - stamp discord_channels.last_scan_at. This is what the
// "last checked" column shows; last_read_ts stays the newest message time.
void discordMarkChannelChecked(Db* db, const std::string& channelId);

// The message was deleted on Discord - scrub captured text immediately so it
// disappears from inbox/recent activity, and durably gray its bot card when one
// exists. Minimal message/channel plus item/card identity is retained as a
// terminal marker so delayed gateway/history/reply/command callbacks cannot
// resurrect it and a late older card edit can still trigger compensation. No
// author, content, note, posted/local timestamp, manual-command result, match,
// score, or unapproved attachment metadata remains; promoted items keep their
// approved evidence without deleted source text or jump URLs.
void discordMessageDeleted(Db* db, DiscordBot* bot,
                           const std::string& messageId);

// Post a notification card for a fresh detection to the configured notify
// channel (settings "notify_channel_id"; no-op when blank or bot offline).
// Card lineage is pre-created in discord_notify_cards before the async post;
// promote / dismiss / completion can therefore edit the same card even after
// the original Discord source row is deleted.
void notifyDetection(Db* db, DiscordBot* bot, long long messageRowId,
                     const std::string& guildId, const std::string& channelId,
                     const std::string& channelName,
                     const std::string& messageId, const std::string& author,
                     const std::string& content, const std::string& kind,
                     double score);

// Post a test card to the configured notify channel (GUI "Send test").
void sendNotifyTest(Db* db, DiscordBot* bot);

} // namespace devhub
