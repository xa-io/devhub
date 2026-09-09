#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DiscordAttachment.h"

namespace dpp { class cluster; }

namespace devhub {

class Db;
struct IngestOutcome;
struct ManualCaptureCommand;
struct ManualCaptureEffect;

// One notification embed the bot posts/edits in the configured notify
// channel (plain std types only - this header stays dpp- and json-free).
struct NotifyCard {
    std::string title, description, url, footer;
    uint32_t color = 0x4f8ff7;
    std::vector<std::pair<std::string, std::string>> fields; // name -> value
};

// Embedded Discord gateway client (D++). Connects with the token saved on
// the Discord settings page, watches every text channel of monitored guilds
// plus individually monitored channels, backfills history since each
// channel's last-read timestamp, and feeds messages straight into the
// ingest pipeline. No external bot process required.
class DiscordBot {
public:
    explicit DiscordBot(Db* db);
    ~DiscordBot();

    // (Re)connect with this token. Empty token is ignored.
    void start(const std::string& token);
    void stop();
    bool running() const { return lockCluster() != nullptr; }

    struct Status {
        std::string state = "offline"; // offline | connecting | connected
        std::string botUser;
        std::string error; // sticky - needs user action (intents/token)
        std::string warn;  // transient connection hiccup - auto-expires
        std::string lastLog; // most recent gateway log line (diagnostics)
        int guilds = 0;
    };
    Status status();

    // ---- outbound (notification cards) ------------------------------------
    // The bot only ever creates/edits ITS OWN notification cards - it never
    // deletes or modifies other users' messages. Safe to call from any
    // thread; silent no-ops (with an appLog note) when the bot is offline.
    // Callbacks run on dpp threads.
    void postCard(const std::string& channelId, const NotifyCard& card,
                  std::function<void(const std::string& messageId)> onPosted,
                  std::function<void(const std::string& error,
                                     bool retryable)> onFailed = nullptr);
    void editCard(const std::string& channelId, const std::string& messageId,
                  const NotifyCard& card,
                  std::function<void()> onEdited = nullptr,
                  std::function<void(const std::string& error,
                                     bool retryable)> onFailed = nullptr);
    // Used only with an id returned by this bot's own create callback when the
    // durable lineage vanished or a newer outbox attempt won the lease.
    void deleteCard(const std::string& channelId,
                    const std::string& messageId,
                    std::function<void()> onDeleted = nullptr,
                    std::function<void(const std::string& error,
                                       bool retryable)> onFailed = nullptr);

    // Refetch one Discord message so signed attachment URLs are current, then
    // download only the exact requested attachment IDs. This is called after
    // item promotion; pending captures never invoke it.
    std::vector<DiscordAttachmentDownload> downloadAttachments(
        const std::string& channelId, const std::string& messageId,
        const std::vector<std::string>& attachmentIds,
        std::chrono::seconds timeout = std::chrono::seconds(30));

    // Promotion only stages DB work and signals the bot-owned worker, keeping
    // GUI/API callers off Discord REST waits.
    void requestQueuedTicketImageSave() { resumeTicketImages_ = true; }
    void requestNotifyCardPost() { resumeNotifyCards_ = true; }
    // Seed the approve/reject controls for one exact pending bot-owned card.
    // Requests are serialized because Discord rate-limits reaction adds.
    void queuePendingReviewReactions(const std::string& channelId,
                                     const std::string& messageId);
    // Remove every reaction from the exact bot-owned review card after the
    // durable edit has rendered a terminal state. Callers may bind completion
    // and heartbeat callbacks to the dedicated cleanup outbox; quiet recovery
    // work suppresses one success log per historical card.
    void queueTerminalReviewReactionCleanup(
        const std::string& channelId, const std::string& messageId,
        std::function<void()> onCleared,
        std::function<void(const std::string& error, bool retryable)> onFailed,
        std::function<void()> onHeartbeat,
        bool logSuccess = true);
    bool stopRequested() const { return rescanStop_.load(); }

private:
    void handleMessage(const void* msg, bool liveEvent); // dpp::message*
    void handleMessageUpdate(const void* msg); // dpp::message*
    // An authorized admin mentioned the bot: a reply force-captures the target
    // (reply text is its admin note), while a direct mention force-captures the
    // ping itself. New sources stay in the approval inbox; an exact promoted
    // source is enriched in place. Returns true when consumed.
    bool maybeManualCapture(const void* msg); // dpp::message*
    // A whitelisted admin replied to an exact pending notification card with
    // a non-empty note. This route runs before watched-channel filtering so a
    // private notification channel (or configured existing DM channel) works.
    bool maybeNotifyCardReviewReply(const void* msg); // dpp::message*
    // Authorized mention commands are staged durably before any target REST
    // fetch. One bot-owned pump processes due commands so a transient fetch
    // failure or process restart cannot turn the command receipt into a
    // permanent replay gate.
    void pumpManualCapture();
    void finishManualCapturePump(uint64_t generation);
    uint64_t beginManualCaptureRestAttempt(
        const ManualCaptureCommand& command, uint64_t generation);
    bool claimManualCaptureRestAttempt(uint64_t token, uint64_t generation);
    void resetManualCaptureRestAttempt();
    void expireManualCaptureRestAttempt();
    void manualCaptureWatchdogWorker();
    bool completeManualCaptureTarget(
        const ManualCaptureCommand& command,
        const std::string& targetAuthor,
        const std::string& targetAuthorId,
        const std::string& targetContent,
        const std::string& targetPostedAt,
        const std::vector<DiscordAttachmentMeta>& attachments,
        uint64_t generation);
    bool recordManualCaptureFailure(const ManualCaptureCommand& command,
                                    const std::string& error,
                                    bool retryable,
                                    uint64_t generation);
    void reconcileManualCaptureEffect(const ManualCaptureEffect& effect,
                                      uint64_t generation);
    void addManualReaction(const std::string& commandMessageId,
                           const std::string& channelId,
                           const std::string& emoji,
                           std::function<void(bool ok,
                                              const std::string& error,
                                              bool retryable)> completed = nullptr);
    // Admin typed "!tickets": post the numbered project menu and seed the
    // number reactions. Returns true when consumed as a command.
    bool maybeTicketsCommand(const void* msg, bool liveEvent); // dpp::message*
    // Admin typed "!leaderboard": post the bounded public contributor table.
    bool maybeLeaderboardCommand(const void* msg, bool liveEvent); // dpp::message*
    // A number reaction landed on one of our menus: flip the card to that
    // project's open fix/implementation tickets.
    void handleReaction(const void* ev); // dpp::message_reaction_add_t*
    // Seed keycap idx on the menu, then chain idx+1 after a delay - the
    // reactions endpoint rate-limits hard when adds go out concurrently.
    void seedTicketReactions(uint64_t msgId, uint64_t chanId, size_t idx,
                             size_t count, int retriesLeft);
    void seedTicketSearchReactions(uint64_t msgId, uint64_t chanId,
                                   size_t idx, size_t pageCount,
                                   int retriesLeft);
    void recoverPendingReviewReactions();
    struct ReviewReactionTask {
        uint64_t taskId = 0;
        uint64_t channelId = 0;
        uint64_t messageId = 0;
        bool cleanup = false;
        bool logSuccess = true;
        std::chrono::steady_clock::time_point deadline{};
        std::chrono::steady_clock::time_point nextHeartbeat{};
        std::function<void()> onCleared;
        std::function<void(const std::string&, bool)> onFailed;
        std::function<void()> onHeartbeat;
    };
    void queueReviewReactionTask(ReviewReactionTask task);
    bool reviewReactionTaskActive(uint64_t generation, uint64_t taskId);
    void expireReviewReactionTask();
    void pumpReviewReactionQueue(uint64_t generation);
    void seedReviewReactionStep(uint64_t msgId, uint64_t chanId,
                                size_t step, int retriesLeft,
                                uint64_t generation, uint64_t taskId);
    void clearAllReviewReactions(uint64_t msgId, uint64_t chanId,
                                 int retriesLeft, uint64_t generation,
                                 uint64_t taskId);
    void finishReviewReactionTask(uint64_t generation, uint64_t taskId,
                                  const std::string& error = {},
                                  bool retryable = false);
    void backfillGuild(const void* guild); // dpp::guild*
    // History sweeps run through a serial queue - one messages_get in
    // flight, spaced out, with retries. Firing every watched channel's
    // request concurrently trips D++'s https client ("Malformed HTTP
    // response") and Discord's limiter; serialized they just work.
    struct SweepJob { uint64_t channel; uint64_t after; int page; int retries; };
    struct QueuedSweepPage;
    void enqueueSweep(const SweepJob& job, bool front = false);
    void pumpSweep();          // start the next queued job if none in flight
    void runSweep(SweepJob job);
    bool enqueueSweepPage(std::shared_ptr<QueuedSweepPage> page);
    void drainSweepMessages(size_t limit = 25);
    void rescanWorker(); // periodic re-check of every watched channel
    std::shared_ptr<dpp::cluster> lockCluster() const;

    Db* db_;
    mutable std::mutex clusterMu_; // guards the pointer, never a D++ call
    std::shared_ptr<dpp::cluster> cluster_;
    std::mutex mu_; // guards st_, seenGuilds_ and warnAt_
    Status st_;
    std::chrono::steady_clock::time_point warnAt_{}; // when st_.warn was set
    std::set<uint64_t> seenGuilds_;
    std::thread rescanTh_;
    std::thread manualCaptureWatchdogTh_;
    std::atomic<bool> rescanStop_{false};
    std::atomic<bool> resumeTicketImages_{false};
    std::atomic<bool> resumeNotifyCards_{false};
    std::atomic<bool> resumeManualCaptures_{false};
    std::atomic<bool> manualCaptureBusy_{false};
    // Incremented before every stop and on each gateway-ready epoch. Late
    // callbacks cannot clear or drive the new session's single-flight state.
    std::atomic<uint64_t> manualCaptureGeneration_{0};
    // Serializes a session-generation rollover with the final SQLite outcome
    // of target/effect callbacks. It is never held across an outbound D++ call.
    std::mutex manualCaptureOutcomeMu_;
    struct ManualCaptureRestAttempt {
        bool active = false;
        uint64_t token = 0;
        uint64_t generation = 0;
        long long commandRowId = 0;
        std::string commandMessageId;
        std::string channelId;
        std::chrono::steady_clock::time_point deadline{};
    };
    std::mutex manualCaptureAttemptMu_;
    ManualCaptureRestAttempt manualCaptureRestAttempt_;
    uint64_t manualCaptureAttemptSequence_ = 0; // guarded by attempt mutex
    std::string logPath_ = "discord.log"; // set from data dir in ctor
    std::mutex logFileMu_;

    std::mutex sweepMu_; // guards sweepQ_ and sweepBusy_
    std::deque<SweepJob> sweepQ_;
    bool sweepBusy_ = false;
    std::atomic<std::chrono::steady_clock::time_point> sweepResumeAt_{};
    std::mutex sweepMessagesMu_;
    std::deque<std::shared_ptr<QueuedSweepPage>> sweepPages_;
    size_t queuedSweepMessages_ = 0;

    // !tickets menus this session: menu message id -> ordered (project id,
    // name) as posted. Bounded (oldest pruned) - a menu stops reacting
    // after a restart, just run !tickets again.
    std::mutex menusMu_;
    std::map<uint64_t, std::vector<std::pair<long long, std::string>>>
        ticketMenus_;
    struct TicketSearchMenu {
        std::string query;
        std::vector<std::string> pages;
        size_t page = 0;
        size_t shownMatches = 0;
        size_t totalMatches = 0;
    };
    // !xatickets searches this session: bounded and non-persistent like the
    // project menus. Pages contain title-only projections, never ticket bodies.
    std::map<uint64_t, TicketSearchMenu> ticketSearchMenus_;

    // Notification-card reaction work. Pending control seeding and terminal
    // all-reaction cleanup share one lane so an in-flight add cannot win after
    // a terminal delete. Cleanup tasks are prioritized after the active request.
    std::mutex reviewReactionMu_;
    std::deque<ReviewReactionTask> reviewReactionQ_;
    bool reviewReactionBusy_ = false;
    uint64_t reviewReactionTaskSequence_ = 0; // guarded by reaction mutex
    std::atomic<uint64_t> reviewReactionGeneration_{0};
};

} // namespace devhub
