// Embedded Discord gateway client.
//
// IMPORTANT: this translation unit must not include anything that pulls in
// vcpkg's nlohmann/json (Db.h, Ops.h, crow, ...). dpp.lib is built against
// its bundled nlohmann copy; both share the same include guard, so mixing
// them here produces ABI-mismatched unresolved externals. All database
// access goes through the plain-string bridge in Ingest.h.
#include "DiscordBot.h"
#include "Ingest.h"
#include "Version.h"
#include "devhub/DiscordRetry.h"
#include "devhub/TicketImage.h"
#include "devhub/Util.h"

// Targeted includes instead of <dpp/dpp.h>: the umbrella header pulls in
// bignum.h, whose nested deleter is not dll-exported and breaks the link.
#include <dpp/cluster.h>
#include <dpp/cache.h>
#include <dpp/dispatcher.h>
#include <dpp/intents.h>
#include <dpp/message.h>
#include <dpp/misc-enum.h>
#include <dpp/once.h>

#include <windows.h> // after dpp (WIN32_LEAN_AND_MEAN keeps winsock1 out)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace devhub {

// Discord epoch: 2015-01-01T00:00:00Z in unix milliseconds.
static constexpr uint64_t kDiscordEpochMs = 1420070400000ULL;

static std::string isoFromSnowflake(uint64_t id) {
    std::time_t secs = (std::time_t)(((id >> 22) + kDiscordEpochMs) / 1000ULL);
    std::tm tm{};
    gmtime_s(&tm, &secs);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static uint64_t snowflakeFromIso(const std::string& iso) {
    std::tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d",
                 &y, &mo, &d, &h, &mi, &s) != 6)
        return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    std::time_t t = _mkgmtime(&tm);
    if (t <= 0) return 0;
    uint64_t ms = (uint64_t)t * 1000ULL;
    if (ms <= kDiscordEpochMs) return 0;
    return (ms - kDiscordEpochMs) << 22;
}

static std::string displayName(const dpp::user& u) {
    return u.global_name.empty() ? u.username : u.global_name;
}

static std::string responseContentType(
    const dpp::http_request_completion_t& response) {
    for (const auto& [name, rawValue] : response.headers) {
        if (toLower(name) != "content-type") continue;
        std::string value = trim(rawValue);
        for (char& ch : value) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte == 0x7f) ch = ' ';
        }
        if (value.size() > 127) value.resize(127);
        return value;
    }
    return {};
}

static std::size_t responseContentLength(
    const dpp::http_request_completion_t& response) {
    for (const auto& [name, rawValue] : response.headers) {
        if (toLower(name) != "content-length") continue;
        const std::string value = trim(rawValue);
        size_t used = 0;
        try {
            const unsigned long long parsed = std::stoull(value, &used);
            if (used != value.size() ||
                parsed > (std::numeric_limits<std::size_t>::max)())
                return (std::numeric_limits<std::size_t>::max)();
            return static_cast<std::size_t>(parsed);
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

static bool isDiscordAttachmentUrl(const std::string& url) {
    const std::string lower = toLower(url);
    return lower.rfind("https://cdn.discordapp.com/", 0) == 0 ||
           lower.rfind("https://media.discordapp.net/", 0) == 0;
}

static std::vector<DiscordAttachmentMeta> attachmentMetadata(
    const dpp::message& message, const std::string& role) {
    std::vector<DiscordAttachmentMeta> out;
    for (const dpp::attachment& attachment : message.attachments) {
        const std::string attachmentId =
            std::to_string((uint64_t)attachment.id);
        const std::string messageId = std::to_string((uint64_t)message.id);
        if (!isDecimalDiscordSnowflake(attachmentId) ||
            !isDecimalDiscordSnowflake(messageId) ||
            !isSupportedTicketAttachmentMetadata(attachment.content_type,
                                                 attachment.filename,
                                                 attachment.ephemeral))
            continue;
        DiscordAttachmentMeta meta;
        meta.attachmentId = attachmentId;
        meta.sourceChannelId = std::to_string((uint64_t)message.channel_id);
        meta.sourceMessageId = messageId;
        meta.sourceRole = role;
        meta.filename = attachment.filename;
        meta.contentType = attachment.content_type;
        meta.sizeBytes = attachment.size;
        meta.width = attachment.width;
        meta.height = attachment.height;
        meta.ephemeral = attachment.ephemeral;
        out.push_back(std::move(meta));
    }
    return out;
}

static bool retryableDiscordRestFailure(
    const dpp::confirmation_callback_t& response) {
    return response.http_info.error != dpp::h_success ||
           isRetryableDiscordHttpStatus(response.http_info.status);
}

static std::string redactExactSecret(std::string value,
                                     const std::string& secret) {
    if (!secret.empty()) value = replaceAll(std::move(value), secret, "[redacted]");
    return redactHttpUrls(std::move(value));
}

template <typename Fn>
static void guardedDiscordHandler(const char* name, Fn&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        appLog(std::string("[discord] ") + name + " failed: " +
               redactHttpUrls(e.what()));
    } catch (...) {
        appLog(std::string("[discord] ") + name +
               " failed with an unknown exception");
    }
}

static bool discordOldMessageEditLimit(
    const dpp::confirmation_callback_t& response) {
    if (!response.is_error()) return false;
    return isDiscordOldMessageEditLimitText(response.get_error().message);
}

struct DiscordBot::QueuedSweepPage {
    std::deque<dpp::message> messages;
};

DiscordBot::DiscordBot(Db* db) : db_(db) {
    char buf[512]{};
    if (GetModuleFileNameA(nullptr, buf, sizeof(buf))) {
        std::string p(buf);
        auto slash = p.find_last_of("/\\");
        if (slash != std::string::npos)
            logPath_ = p.substr(0, slash) + "\\data\\discord.log";
    }
}
DiscordBot::~DiscordBot() { stop(); }

std::shared_ptr<dpp::cluster> DiscordBot::lockCluster() const {
    std::lock_guard<std::mutex> lk(clusterMu_);
    return cluster_;
}

void DiscordBot::stop() {
    // Invalidate callbacks before touching either worker or cluster state.
    // Pending commands remain pending in SQLite and the next session resumes
    // them; an old callback must never release a new session's busy flag.
    {
        std::lock_guard<std::mutex> outcomeLock(manualCaptureOutcomeMu_);
        ++manualCaptureGeneration_;
        resetManualCaptureRestAttempt();
    }
    ++reviewReactionGeneration_;
    rescanStop_ = true;
    resumeTicketImages_ = false;
    resumeNotifyCards_ = false;
    resumeManualCaptures_ = false;
    // New callers see offline immediately. Existing calls retain their local
    // shared owner until their current D++ operation/callback is complete.
    std::shared_ptr<dpp::cluster> dying;
    {
        std::lock_guard<std::mutex> lk(clusterMu_);
        dying.swap(cluster_);
    }
    if (rescanTh_.joinable()) rescanTh_.join();
    if (manualCaptureWatchdogTh_.joinable())
        manualCaptureWatchdogTh_.join();
    if (dying) {
        try { dying->shutdown(); } catch (...) {}
        dying.reset();
        appLog("[discord] bot stopped");
    }
    {
        std::lock_guard<std::mutex> lk(sweepMu_);
        sweepQ_.clear();
        sweepBusy_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(sweepMessagesMu_);
        sweepPages_.clear();
        queuedSweepMessages_ = 0;
    }
    sweepResumeAt_ = std::chrono::steady_clock::time_point{};
    {
        std::lock_guard<std::mutex> lk(reviewReactionMu_);
        reviewReactionQ_.clear();
        reviewReactionBusy_ = false;
    }
    manualCaptureBusy_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    st_ = Status{};
    seenGuilds_.clear();
}

DiscordBot::Status DiscordBot::status() {
    std::lock_guard<std::mutex> lk(mu_);
    // A transient hiccup that hasn't recurred in 2 minutes recovered long
    // ago (sweeps retry within seconds) - don't keep alarming the user.
    if (!st_.warn.empty() && std::chrono::steady_clock::now() - warnAt_ >
                               std::chrono::minutes(2))
        st_.warn.clear();
    Status s = st_;
    s.guilds = (int)seenGuilds_.size();
    return s;
}

void DiscordBot::start(const std::string& token) {
    std::string tok = trim(token);
    if (tok.empty()) return;
    stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.state = "connecting";
        st_.error.clear();
    }
    std::shared_ptr<dpp::cluster> cluster;
    try {
        cluster = std::make_shared<dpp::cluster>(
            tok, dpp::i_guilds | dpp::i_guild_messages |
                      dpp::i_guild_message_reactions | dpp::i_direct_messages |
                      dpp::i_direct_message_reactions | dpp::i_message_content);

        cluster->on_log([this](const dpp::log_t& ev) {
            guardedDiscordHandler("log handler", [&]() {
            // D++ includes the requested endpoint in some HTTP timeout and
            // malformed-response messages. Attachment endpoints are signed,
            // so sanitize before any status, application, or file log sink.
            const std::string safeMessage =
                logSafe(redactHttpUrls(ev.message), 2048);
            const std::string lowerMessage = toLower(safeMessage);
            {
                std::lock_guard<std::mutex> lk(mu_);
                st_.lastLog = safeMessage;
                // dpp logs gateway close reasons at debug level; promote the
                // ones the user must act on.
                bool critical =
                    lowerMessage.find("4014") != std::string::npos ||
                    lowerMessage.find("disallowed intent") != std::string::npos ||
                    lowerMessage.find("4004") != std::string::npos ||
                    lowerMessage.find("authentication failed") != std::string::npos ||
                    lowerMessage.find("invalid token") != std::string::npos;
                // Connection-level hiccups (a dropped keep-alive socket
                // surfaces as "Malformed HTTP response", plus timeouts and
                // 429s). The sweep queue retries these within seconds - a
                // passing warning, not a sticky error.
                bool transient =
                    !critical &&
                    (lowerMessage.find("malformed http response") !=
                         std::string::npos ||
                     lowerMessage.find("rate limited") != std::string::npos ||
                     lowerMessage.find("timed out") != std::string::npos ||
                     lowerMessage.find("connection") != std::string::npos);
                if (transient && ev.severity >= dpp::ll_warning) {
                    if (st_.warn.empty()) // first of a burst -> activity log
                        appLog("[discord] transient: " + safeMessage);
                    st_.warn = safeMessage;
                    warnAt_ = std::chrono::steady_clock::now();
                } else if (critical || ev.severity >= dpp::ll_warning) {
                    st_.error = safeMessage;
                    appLog("[discord] " + safeMessage);
                }
            }
            // Full gateway log for diagnostics, capped at 8 MiB with one
            // previous generation. The callback can run on multiple D++ pool
            // threads, so rotation and append share one file lock.
            {
                std::lock_guard<std::mutex> fileLock(logFileMu_);
                std::error_code ec;
                if (std::filesystem::file_size(logPath_, ec) >=
                    8ull * 1024 * 1024 && !ec) {
                    const std::filesystem::path previous = logPath_ + ".1";
                    std::filesystem::remove(previous, ec);
                    ec.clear();
                    std::filesystem::rename(logPath_, previous, ec);
                }
                FILE* f = nullptr;
                if (fopen_s(&f, logPath_.c_str(), "a") == 0 && f) {
                    std::fprintf(f, "[%d] %s\n", (int)ev.severity,
                                 safeMessage.c_str());
                    std::fclose(f);
                }
            }
            });
        });

        cluster->on_ready([this](const dpp::ready_t&) {
            guardedDiscordHandler("ready handler", [&]() {
            const std::shared_ptr<dpp::cluster> live = lockCluster();
            if (!live) return;
            const std::string botUsername = live->me.username;
            // Treat each gateway-ready epoch as a new REST work session. If a
            // disconnect swallowed an earlier callback, its durable row stays
            // pending and an eventual late callback cannot release this
            // session's single-flight flag.
            {
                std::lock_guard<std::mutex> outcomeLock(
                    manualCaptureOutcomeMu_);
                ++manualCaptureGeneration_;
                resetManualCaptureRestAttempt();
                manualCaptureBusy_ = false;
            }
            {
                std::lock_guard<std::mutex> reviewLock(reviewReactionMu_);
                ++reviewReactionGeneration_;
                reviewReactionQ_.clear();
                reviewReactionBusy_ = false;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                st_.state = "connected";
                st_.botUser = botUsername;
                st_.error.clear();
                st_.warn.clear();
            }
            appLog("[discord] connected as " + botUsername);
            // Promotion while offline leaves metadata queued but never writes a
            // file. Ask the owned worker to retry it so the gateway callback
            // never blocks on REST/download futures.
            resumeTicketImages_ = true;
            // Notification creates and revisioned edits are durable. Reclaim
            // pending or expired in-flight delivery once Discord is reachable.
            resumeNotifyCards_ = true;
            // Authorized mention commands are durable. Resume any command
            // whose target fetch was interrupted by disconnect or shutdown.
            resumeManualCaptures_ = true;
            // Re-seed controls for cards that were already pending before
            // this build/reconnect. Adding the same bot reaction is
            // idempotent and the queue serializes Discord's strict bucket.
            recoverPendingReviewReactions();
            });
        });

        cluster->on_guild_create([this](const dpp::guild_create_t& ev) {
            guardedDiscordHandler("guild-create handler", [&]() {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (!seenGuilds_.insert((uint64_t)ev.created.id).second)
                        return; // reconnect replay - already handled
                }
                // Show the server's real name in the monitors table.
                discordSetGuildName(db_,
                                    std::to_string((uint64_t)ev.created.id),
                                    ev.created.name);
                appLog("[discord] sees server '" +
                       logSafe(ev.created.name, 100) +
                       "' - backfilling watched channels");
                backfillGuild(&ev.created);
            });
        });

        cluster->on_message_create([this](const dpp::message_create_t& ev) {
            guardedDiscordHandler("message-create handler",
                                  [&]() { handleMessage(&ev.msg, true); });
        });

        cluster->on_message_update([this](const dpp::message_update_t& ev) {
            guardedDiscordHandler("message-update handler",
                                  [&]() { handleMessageUpdate(&ev.msg); });
        });

        // Number reactions drive the !tickets project menus.
        cluster->on_message_reaction_add(
            [this](const dpp::message_reaction_add_t& ev) {
                guardedDiscordHandler("reaction-add handler",
                                      [&]() { handleReaction(&ev); });
            });

        // Deleted Discord sources become scrubbed terminal markers so late
        // capture/retry events cannot replay them. Item-owned evidence and
        // notification-card lineage retain their lifecycle without source text.
        cluster->on_message_delete([this](const dpp::message_delete_t& ev) {
            guardedDiscordHandler("message-delete handler", [&]() {
                discordMessageDeleted(db_, this,
                                      std::to_string((uint64_t)ev.id));
            });
        });
        cluster->on_message_delete_bulk(
            [this](const dpp::message_delete_bulk_t& ev) {
                guardedDiscordHandler("message-delete-bulk handler", [&]() {
                    for (dpp::snowflake id : ev.deleted)
                        discordMessageDeleted(
                            db_, this, std::to_string((uint64_t)id));
                });
            });

        {
            std::lock_guard<std::mutex> lk(clusterMu_);
            cluster_ = cluster;
        }
        cluster->start(dpp::st_return);

        rescanStop_ = false;
        rescanTh_ = std::thread([this]() {
            guardedDiscordHandler("rescan worker",
                                  [this]() { rescanWorker(); });
        });
        manualCaptureWatchdogTh_ = std::thread([this]() {
            guardedDiscordHandler(
                "manual-capture watchdog worker",
                [this]() { manualCaptureWatchdogWorker(); });
        });
    } catch (const std::exception& e) {
        rescanStop_ = true;
        if (rescanTh_.joinable()) rescanTh_.join();
        if (manualCaptureWatchdogTh_.joinable())
            manualCaptureWatchdogTh_.join();
        {
            std::lock_guard<std::mutex> clusterLock(clusterMu_);
            if (cluster_ == cluster) cluster_.reset();
        }
        if (cluster) {
            try { cluster->shutdown(); } catch (...) {}
            cluster.reset();
        }
        std::lock_guard<std::mutex> lk(mu_);
        st_.state = "offline";
        st_.error = redactExactSecret(e.what(), tok);
    }
}

void DiscordBot::handleMessage(const void* msgPtr, bool liveEvent) {
    const dpp::message& m = *static_cast<const dpp::message*>(msgPtr);
    if (m.author.is_bot() || m.content.empty()) return;

    std::string chanId = std::to_string((uint64_t)m.channel_id);
    std::string guildId =
        m.guild_id ? std::to_string((uint64_t)m.guild_id) : std::string();

    // Admin commands work in ANY channel the bot can read (gated inside).
    if (maybeLeaderboardCommand(msgPtr, liveEvent)) return;
    if (maybeTicketsCommand(msgPtr, liveEvent)) return;

    // Exact replies to our pending review card must work even when the
    // configured private admin/DM channel is not a watched ingest channel.
    if (maybeNotifyCardReviewReply(msgPtr)) return;

    if (!discordWatch(db_, guildId, chanId)) return;

    // Admin mention command? Consumed entirely - its durable lifecycle row is
    // not itself detected and never posts its own card.
    if (maybeManualCapture(msgPtr)) return;

    dpp::channel* ch = dpp::find_channel(m.channel_id);
    dpp::guild* g = m.guild_id ? dpp::find_guild(m.guild_id) : nullptr;
    IngestOutcome out = ingestDiscordMessage(
        db_, chanId, ch ? ch->name : "", guildId, g ? g->name : "",
        std::to_string((uint64_t)m.id), displayName(m.author),
        std::to_string((uint64_t)m.author.id), m.content,
        isoFromSnowflake((uint64_t)m.id), /*asCommand=*/false,
        attachmentMetadata(m, "suggestion"));
    if (out.ingested && out.kind != "none")
        notifyDetection(db_, this, out.messageRowId, guildId, chanId,
                        ch ? ch->name : "", std::to_string((uint64_t)m.id),
                        displayName(m.author), m.content, out.kind, out.score);
}

void DiscordBot::handleMessageUpdate(const void* msgPtr) {
    const dpp::message& m = *static_cast<const dpp::message*>(msgPtr);
    if (m.author.is_bot() || !m.id || !m.channel_id) return;

    const std::string channelId = std::to_string((uint64_t)m.channel_id);
    const std::string guildId = m.guild_id
        ? std::to_string((uint64_t)m.guild_id) : std::string();
    if (!discordWatch(db_, guildId, channelId)) return;

    // Editing a watched message to add an authorized bot mention follows the
    // same durable command lifecycle as a create event. Its replay key makes a
    // repeated MESSAGE_UPDATE harmless.
    if (maybeManualCapture(msgPtr)) return;

    dpp::channel* channel = dpp::find_channel(m.channel_id);
    dpp::guild* guild = m.guild_id ? dpp::find_guild(m.guild_id) : nullptr;
    IngestOutcome outcome = reconcileDiscordMessageEdit(
        db_, channelId, channel ? channel->name : "", guildId,
        guild ? guild->name : "", std::to_string((uint64_t)m.id),
        displayName(m.author), std::to_string((uint64_t)m.author.id),
        m.content, isoFromSnowflake((uint64_t)m.id),
        attachmentMetadata(m, "suggestion"));
    if (outcome.failed || !outcome.ingested) return;

    if (outcome.imagesQueued > 0) requestQueuedTicketImageSave();
    if (outcome.itemId > 0) {
        ensureTicketNotifyCard(db_, this, outcome.messageRowId,
                               outcome.itemId);
        refreshTicketNotifyCard(db_, this, outcome.itemId);
    } else if (outcome.kind == "suggestion" || outcome.kind == "bug") {
        if (outcome.hasCard) {
            requestNotifyCardPost();
        } else {
            notifyDetection(
                db_, this, outcome.messageRowId, guildId, channelId,
                channel ? channel->name : "",
                std::to_string((uint64_t)m.id), displayName(m.author),
                m.content, outcome.kind, outcome.score);
        }
    } else if (outcome.hasCard) {
        requestNotifyCardPost();
    }
}

// Remove every <@id> / <@!id> mention of the bot from the reply; what
// remains (trimmed) is the admin's note.
static std::string stripBotMention(std::string s, uint64_t botId) {
    const std::string id = std::to_string(botId);
    for (const std::string& tok : {"<@!" + id + ">", "<@" + id + ">"}) {
        size_t pos;
        while ((pos = s.find(tok)) != std::string::npos) s.erase(pos, tok.size());
    }
    return trim(s);
}

bool DiscordBot::maybeNotifyCardReviewReply(const void* msgPtr) {
    const dpp::message& m = *static_cast<const dpp::message*>(msgPtr);
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster || m.message_reference.message_id == 0 ||
        m.message_reference.type != dpp::mrt_default ||
        (m.message_reference.channel_id != 0 &&
         m.message_reference.channel_id != m.channel_id))
        return false;

    const std::string channelId = std::to_string((uint64_t)m.channel_id);
    const std::string targetMessageId =
        std::to_string((uint64_t)m.message_reference.message_id);
    const std::string actorId = std::to_string((uint64_t)m.author.id);
    const std::string note = stripBotMention(
        m.content, static_cast<uint64_t>(cluster->me.id));

    if (note.empty()) {
        // Probe exact ownership with an intentionally unauthorized actor so an
        // empty reply can never approve. Exact-card replies are consumed; an
        // authorized author receives a visible retry cue.
        DiscordReviewOutcome probe = reviewNotificationCard(
            db_, this, channelId, targetMessageId, "", true, "");
        if (!probe.matched) return false;
        if (discordAdminUserAllowed(db_, actorId))
            addManualReaction(std::to_string((uint64_t)m.id), channelId,
                              "\xE2\x9D\x8C");
        return true;
    }

    DiscordReviewOutcome outcome = reviewNotificationCard(
        db_, this, channelId, targetMessageId, actorId, true, note);
    if (!outcome.matched) return false;
    if (!outcome.authorized) return true; // exact but unauthorized: inert

    addManualReaction(std::to_string((uint64_t)m.id), channelId,
                      outcome.ok ? "\xE2\x9C\x85" : "\xE2\x9D\x8C");
    if (outcome.ok) {
        appLog("[review] admin reply approved " +
               (outcome.itemId > 0
                    ? "I" + std::to_string(outcome.itemId)
                    : "an already-terminal card") +
               (outcome.duplicate ? " (replay ignored)" : ""));
    } else {
        appLog("[review] admin reply could not approve pending card: " +
               redactHttpUrls(outcome.error));
    }
    return true;
}

void DiscordBot::addManualReaction(const std::string& commandMessageId,
                                   const std::string& channelId,
                                   const std::string& emoji,
                                   std::function<void(bool, const std::string&,
                                                      bool)> completed) {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster || !isDecimalDiscordSnowflake(commandMessageId) ||
        !isDecimalDiscordSnowflake(channelId)) {
        if (completed)
            completed(false, "bot offline or invalid acknowledgement ids",
                      cluster == nullptr);
        return;
    }
    try {
        cluster->message_add_reaction(
            dpp::snowflake(std::stoull(commandMessageId)),
            dpp::snowflake(std::stoull(channelId)), emoji,
            [cluster, completed](const dpp::confirmation_callback_t& response) {
                if (!completed) return;
                if (response.is_error()) {
                    completed(false, response.get_error().message,
                              retryableDiscordRestFailure(response));
                } else {
                    completed(true, {}, false);
                }
            });
    } catch (const std::exception& e) {
        appLog("[manual] couldn't add command acknowledgement: " +
               redactHttpUrls(e.what()));
        if (completed) completed(false, e.what(), true);
    }
}

bool DiscordBot::recordManualCaptureFailure(
    const ManualCaptureCommand& command, const std::string& error,
    bool retryable, uint64_t generation) {
    const std::string safeError = redactHttpUrls(error);
    ManualCommandFailureOutcome failed;
    try {
        {
            std::lock_guard<std::mutex> outcomeLock(manualCaptureOutcomeMu_);
            if (manualCaptureGeneration_.load() != generation) return false;
            failed = recordManualCaptureCommandFailure(
                db_, command.commandRowId, safeError, retryable);
        }
        if (failed.terminal) {
            appLog("[manual] command " + command.commandMessageId +
                   " failed after " + std::to_string(failed.attempts) +
                   " attempt(s): " + failed.error);
            addManualReaction(command.commandMessageId, command.channelId,
                              "\xE2\x9D\x8C"); // cross mark: terminal failure
        } else if (failed.retryScheduled) {
            appLog("[manual] command " + command.commandMessageId +
                   " retry scheduled after attempt " +
                   std::to_string(failed.attempts) + ": " + failed.error);
        }
    } catch (const std::exception& e) {
        // The row is still pending. The periodic pump will retry the durable
        // receipt instead of losing it because failure accounting itself hit
        // a transient database problem.
        appLog("[manual] couldn't record command failure: " +
               redactHttpUrls(e.what()));
    }
    resumeManualCaptures_ = true;
    return true;
}

void DiscordBot::reconcileManualCaptureEffect(
    const ManualCaptureEffect& effect, uint64_t generation) {
    try {
        if (effect.resultKind == "captured") {
            if (effect.messageRowId <= 0)
                throw std::runtime_error(
                    "completed capture has no persisted target row");
            // notifyDetection now stages a durable outbox receipt. Repeating
            // this after a crash is safe and never creates a second lineage.
            notifyDetection(
                db_, this, effect.messageRowId, effect.guildId,
                effect.channelId, effect.channelName, effect.targetMessageId,
                effect.targetAuthor, effect.targetContent,
                effect.targetKind.empty() ? "suggestion" : effect.targetKind,
                effect.targetScore);
        } else if (effect.resultKind == "appended") {
            if (effect.itemId <= 0)
                throw std::runtime_error(
                    "completed follow-up has no persisted target item");
            if (effect.imagesQueued > 0) requestQueuedTicketImageSave();
            ensureTicketNotifyCard(
                db_, this, effect.messageRowId, effect.itemId);
            refreshTicketNotifyCard(db_, this, effect.itemId);
        } else if (effect.resultKind == "repeat") {
            // Replays never append the note or evidence again, but they may
            // be the first post-crash opportunity to restore a missing card.
            if (effect.itemId > 0) {
                ensureTicketNotifyCard(
                    db_, this, effect.messageRowId, effect.itemId);
                refreshTicketNotifyCard(db_, this, effect.itemId);
            } else if (effect.messageRowId > 0) {
                notifyDetection(
                    db_, this, effect.messageRowId, effect.guildId,
                    effect.channelId, effect.channelName,
                    effect.targetMessageId, effect.targetAuthor,
                    effect.targetContent,
                    effect.targetKind.empty() ? "suggestion"
                                              : effect.targetKind,
                    effect.targetScore);
            }
        } else {
            throw std::runtime_error("completed command has an invalid result kind");
        }
    } catch (const std::exception& e) {
        ManualCommandFailureOutcome failed;
        {
            std::lock_guard<std::mutex> outcomeLock(manualCaptureOutcomeMu_);
            if (manualCaptureGeneration_.load() != generation) return;
            failed = recordManualCaptureEffectFailure(
                db_, effect.commandRowId, effect.attempt,
                std::string("manual effect reconciliation failed: ") + e.what(),
                /*retryable=*/true);
        }
        if (failed.terminal)
            appLog("[manual] post-commit effect failed: " + failed.error);
        resumeManualCaptures_ = true;
        return;
    }

    const std::string emoji = effect.resultKind == "appended"
        ? "\xF0\x9F\x93\x8E" // paperclip: existing item gained evidence
        : (effect.resultKind == "captured" ? "\xE2\x9C\x85"
                                            : "\xF0\x9F\x94\x81");
    addManualReaction(
        effect.commandMessageId, effect.channelId, emoji,
        [this, effect, generation](bool ok, const std::string& error,
                                   bool retryable) {
            // A reconnect invalidates old callbacks. The posting lease will
            // reclaim the effect; it must not complete a newer attempt.
            ManualCommandFailureOutcome failed;
            try {
                {
                    std::lock_guard<std::mutex> outcomeLock(
                        manualCaptureOutcomeMu_);
                    if (manualCaptureGeneration_.load() != generation) return;
                    if (ok) {
                        completeManualCaptureEffect(
                            db_, effect.commandRowId, effect.attempt);
                    } else {
                        failed = recordManualCaptureEffectFailure(
                            db_, effect.commandRowId, effect.attempt, error,
                            retryable);
                    }
                }
                if (failed.terminal)
                    appLog("[manual] acknowledgement failed: " + failed.error);
            } catch (const std::exception& e) {
                appLog("[manual] couldn't persist acknowledgement result: " +
                       redactHttpUrls(e.what()));
            }
            resumeManualCaptures_ = true;
        });
}

bool DiscordBot::completeManualCaptureTarget(
    const ManualCaptureCommand& command, const std::string& targetAuthor,
    const std::string& targetAuthorId, const std::string& targetContent,
    const std::string& targetPostedAt,
    const std::vector<DiscordAttachmentMeta>& attachments,
    uint64_t generation) {
    // This call is the exactly-once boundary: target mutation and command
    // state transition to done share one SQLite transaction.
    IngestOutcome outcome;
    {
        std::lock_guard<std::mutex> outcomeLock(manualCaptureOutcomeMu_);
        if (manualCaptureGeneration_.load() != generation) return false;
        outcome = completeManualCaptureCommand(
            db_, command, targetAuthor, targetAuthorId, targetContent,
            targetPostedAt, attachments);
    }
    if (!(outcome.ingested || outcome.duplicate || outcome.appendedToItem))
        throw std::runtime_error(
            "manual capture completion returned no durable receipt");

    // Notification/card refresh and acknowledgement are separate durable
    // effects. The pump can replay them after a crash without reopening this
    // transaction or appending the note a second time.
    resumeManualCaptures_ = true;
    return true;
}

bool DiscordBot::maybeManualCapture(const void* msgPtr) {
    const dpp::message& m = *static_cast<const dpp::message*>(msgPtr);
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster) return false;

    // Both direct and reply submissions must explicitly ping the bot.
    const uint64_t me = (uint64_t)cluster->me.id;
    bool mentionsMe = false;
    for (const auto& [u, gm] : m.mentions)
        if ((uint64_t)u.id == me) { mentionsMe = true; break; }
    if (!mentionsMe) return false;

    // ... from an authorized admin/developer. Anyone else's bot ping is NOT
    // a command - it flows through normal ingest like any other message.
    const std::string authorId = std::to_string((uint64_t)m.author.id);
    if (!discordAdminUserAllowed(db_, authorId)) return false;

    const bool isReply = m.message_reference.message_id != 0;
    if (isReply && (m.message_reference.type != dpp::mrt_default ||
                    (m.message_reference.channel_id != 0 &&
                     m.message_reference.channel_id != m.channel_id)))
        return false;

    std::string chanId = std::to_string((uint64_t)m.channel_id);
    std::string guildId =
        m.guild_id ? std::to_string((uint64_t)m.guild_id) : std::string();
    dpp::channel* ch = dpp::find_channel(m.channel_id);
    dpp::guild* g = m.guild_id ? dpp::find_guild(m.guild_id) : nullptr;
    std::string chanName = ch ? ch->name : "";
    std::string guildName = g ? g->name : "";

    const std::string stripped = stripBotMention(m.content, me);
    const std::string adminName = displayName(m.author);
    const std::string commandId = std::to_string((uint64_t)m.id);
    const std::string targetId = isReply
        ? std::to_string((uint64_t)m.message_reference.message_id)
        : commandId;
    std::vector<DiscordAttachmentMeta> commandImages =
        attachmentMetadata(m, isReply ? "admin_reply" : "direct_ping");

    ManualCommandStageOutcome staged;
    try {
        staged = stageManualCaptureCommand(
            db_, chanId, chanName, guildId, guildName, commandId, adminName,
            authorId, m.content, isoFromSnowflake((uint64_t)m.id), targetId,
            isReply ? stripped : std::string(), commandImages);
    } catch (const std::exception& e) {
        appLog("[manual] couldn't stage durable command: " +
               redactHttpUrls(e.what()));
        addManualReaction(commandId, chanId,
                          "\xE2\x9D\x8C"); // terminal-looking operator signal
        return true;
    }

    // A row receipt is no longer treated as proof of completion. Replays
    // inspect the durable lifecycle and never reapply a note after done.
    if (staged.state == "done") {
        // Reconcile the persisted result; the correct success emoji depends on
        // whether this command captured, appended, or was a true no-op.
        resumeManualCaptures_ = true;
        return true;
    }
    if (staged.state == "failed") {
        addManualReaction(commandId, chanId,
                          "\xE2\x9D\x8C"); // terminal failure
        return true;
    }
    if (staged.state != "pending" ||
        !(staged.accepted || staged.duplicate)) {
        appLog("[manual] command was not accepted for durable processing");
        return true;
    }

    // A direct authorized ping submits that message itself to the same inbox.
    // Complete it from the live payload immediately. If the process stops in
    // the middle, its pending self-target row is recoverable by the same REST
    // pump used for reply commands. Attachment files still wait for Promote.
    if (!isReply && !staged.duplicate) {
        bool idle = false;
        if (!manualCaptureBusy_.compare_exchange_strong(idle, true)) {
            // Another command owns the exactly-once lane. The durable direct
            // command will be fetched from its self-target when the lane frees.
            resumeManualCaptures_ = true;
            return true;
        }
        const uint64_t generation = manualCaptureGeneration_.load();
        std::string content = stripped;
        if (content.empty() && !staged.command.commandAttachments.empty())
            content = "File attachment submitted from Discord";
        if (content.empty()) {
            appLog("[manual] direct bot mention had no suggestion text or durable attachment");
            recordManualCaptureFailure(
                staged.command,
                "direct bot mention has no suggestion text or durable attachment",
                /*retryable=*/false, generation);
            finishManualCapturePump(generation);
            return true;
        }
        try {
            completeManualCaptureTarget(
                staged.command, adminName, authorId, content,
                isoFromSnowflake((uint64_t)m.id),
                staged.command.commandAttachments, generation);
        } catch (const std::exception& e) {
            recordManualCaptureFailure(
                staged.command,
                std::string("direct capture could not complete: ") + e.what(),
                /*retryable=*/true, generation);
        }
        finishManualCapturePump(generation);
        return true;
    }

    // Reply targets require a REST fetch. A replay of a pending direct command
    // also comes through the pump so it honors the stored retry deadline.
    resumeManualCaptures_ = true;
    return true;
}

void DiscordBot::finishManualCapturePump(uint64_t generation) {
    {
        // Keep the generation check and lane release atomic with stop/on_ready.
        // An old callback must not clear a newer session's active fetch.
        std::lock_guard<std::mutex> outcomeLock(manualCaptureOutcomeMu_);
        if (manualCaptureGeneration_.load() != generation) return;
        manualCaptureBusy_ = false;
    }
    // Let the owned worker select the next due row. Deferring by one worker
    // tick avoids recursive retry loops if failure accounting itself failed.
    resumeManualCaptures_ = true;
}

uint64_t DiscordBot::beginManualCaptureRestAttempt(
    const ManualCaptureCommand& command, uint64_t generation) {
    std::lock_guard<std::mutex> lock(manualCaptureAttemptMu_);
    if (manualCaptureGeneration_.load() != generation || rescanStop_)
        return 0;
    ManualCaptureRestAttempt attempt;
    attempt.active = true;
    attempt.token = ++manualCaptureAttemptSequence_;
    attempt.generation = generation;
    attempt.commandRowId = command.commandRowId;
    attempt.commandMessageId = command.commandMessageId;
    attempt.channelId = command.channelId;
    attempt.deadline = std::chrono::steady_clock::now() +
                       std::chrono::seconds(30);
    manualCaptureRestAttempt_ = std::move(attempt);
    return manualCaptureRestAttempt_.token;
}

bool DiscordBot::claimManualCaptureRestAttempt(uint64_t token,
                                               uint64_t generation) {
    std::lock_guard<std::mutex> lock(manualCaptureAttemptMu_);
    if (!manualCaptureRestAttempt_.active || token == 0 ||
        manualCaptureRestAttempt_.token != token ||
        manualCaptureRestAttempt_.generation != generation ||
        manualCaptureGeneration_.load() != generation)
        return false;
    manualCaptureRestAttempt_.active = false;
    return true;
}

void DiscordBot::resetManualCaptureRestAttempt() {
    std::lock_guard<std::mutex> lock(manualCaptureAttemptMu_);
    manualCaptureRestAttempt_.active = false;
}

void DiscordBot::expireManualCaptureRestAttempt() {
    ManualCaptureRestAttempt expired;
    {
        std::lock_guard<std::mutex> lock(manualCaptureAttemptMu_);
        if (!manualCaptureRestAttempt_.active ||
            std::chrono::steady_clock::now() <
                manualCaptureRestAttempt_.deadline)
            return;
        expired = manualCaptureRestAttempt_;
        manualCaptureRestAttempt_.active = false;
    }
    if (manualCaptureGeneration_.load() != expired.generation || rescanStop_)
        return;
    ManualCaptureCommand command;
    command.commandRowId = expired.commandRowId;
    command.commandMessageId = expired.commandMessageId;
    command.channelId = expired.channelId;
    recordManualCaptureFailure(
        command, "Discord target fetch timed out before its callback arrived",
        /*retryable=*/true, expired.generation);
    finishManualCapturePump(expired.generation);
}

void DiscordBot::manualCaptureWatchdogWorker() {
    while (!rescanStop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (rescanStop_) break;
        expireManualCaptureRestAttempt();
        expireReviewReactionTask();
        // Sweep pacing belongs to this owned/joined thread. D++ pool callbacks
        // only enqueue work and publish the next eligible time.
        if (std::chrono::steady_clock::now() >= sweepResumeAt_.load())
            pumpSweep();
    }
}

void DiscordBot::pumpManualCapture() {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (rescanStop_ || !cluster || status().state != "connected") return;
    bool idle = false;
    if (!manualCaptureBusy_.compare_exchange_strong(idle, true)) return;

    const uint64_t generation = manualCaptureGeneration_.load();
    ManualCaptureEffect effect;
    try {
        if (nextPendingManualCaptureEffect(db_, effect)) {
            reconcileManualCaptureEffect(effect, generation);
            // The DB lease, rather than this in-memory lane, owns the async
            // reaction callback. Other due commands can continue meanwhile.
            finishManualCapturePump(generation);
            return;
        }
    } catch (const std::exception& e) {
        manualCaptureBusy_ = false;
        appLog("[manual] couldn't select pending effect: " +
               redactHttpUrls(e.what()));
        return;
    }
    ManualCaptureCommand command;
    try {
        if (!nextPendingManualCaptureCommand(db_, command)) {
            manualCaptureBusy_ = false;
            return;
        }
    } catch (const std::exception& e) {
        manualCaptureBusy_ = false;
        appLog("[manual] couldn't select pending command: " +
               redactHttpUrls(e.what()));
        return;
    }

    // A persisted privileged command must still be authorized when it is
    // executed. Revocation, restart, or a delayed retry must never inherit the
    // permission that existed only when the row was staged.
    if (!discordAdminUserAllowed(db_, command.adminId)) {
        recordManualCaptureFailure(
            command, "author is no longer an authorized admin",
            /*retryable=*/false, generation);
        finishManualCapturePump(generation);
        return;
    }

    if (!isDecimalDiscordSnowflake(command.targetMessageId) ||
        !isDecimalDiscordSnowflake(command.channelId)) {
        recordManualCaptureFailure(
            command, "persisted command has an invalid target or channel ID",
            /*retryable=*/false, generation);
        finishManualCapturePump(generation);
        return;
    }

    const uint64_t attemptToken =
        beginManualCaptureRestAttempt(command, generation);
    if (attemptToken == 0) {
        finishManualCapturePump(generation);
        return;
    }
    try {
        const uint64_t botUserId = (uint64_t)cluster->me.id;
        cluster->message_get(
            dpp::snowflake(std::stoull(command.targetMessageId)),
            dpp::snowflake(std::stoull(command.channelId)),
            [this, cluster, command, generation, attemptToken, botUserId](
                const dpp::confirmation_callback_t& response) {
                // The callback and watchdog race for this token. Exactly one
                // owns the lifecycle update; a timed-out or old-session
                // callback cannot release a newer lane.
                if (!claimManualCaptureRestAttempt(attemptToken, generation))
                    return;

                if (response.is_error()) {
                    const bool retryable =
                        retryableDiscordRestFailure(response);
                    const std::string error =
                        "could not fetch Discord target message (HTTP " +
                        std::to_string(response.http_info.status) + "): " +
                        response.get_error().message;
                    recordManualCaptureFailure(
                        command, error, retryable, generation);
                    finishManualCapturePump(generation);
                    return;
                }

                try {
                    const dpp::message& target =
                        std::get<dpp::message>(response.value);
                    if (std::to_string((uint64_t)target.id) !=
                            command.targetMessageId ||
                        std::to_string((uint64_t)target.channel_id) !=
                            command.channelId) {
                        recordManualCaptureFailure(
                            command,
                            "Discord returned a different message than the exact target",
                            /*retryable=*/false, generation);
                        finishManualCapturePump(generation);
                        return;
                    }
                    const bool direct = command.targetMessageId ==
                                        command.commandMessageId;
                    std::vector<DiscordAttachmentMeta> images;
                    std::string content;
                    if (direct) {
                        // This attachment metadata was staged from the live
                        // command and retains its direct_ping provenance.
                        images = command.commandAttachments;
                        content = stripBotMention(target.content, botUserId);
                    } else {
                        images = attachmentMetadata(target, "suggestion");
                        images.insert(images.end(),
                                      command.commandAttachments.begin(),
                                      command.commandAttachments.end());
                        content = target.content;
                    }

                    if (target.author.is_bot() ||
                        (content.empty() && images.empty())) {
                        recordManualCaptureFailure(
                            command,
                            target.author.is_bot()
                                ? "target message was posted by a bot"
                                : "target message has no text or durable attachment",
                            /*retryable=*/false, generation);
                        finishManualCapturePump(generation);
                        return;
                    }
                    if (content.empty())
                        content = "File attachment submitted from Discord";

                    if (manualCaptureGeneration_.load() != generation)
                        return;
                    completeManualCaptureTarget(
                        command, displayName(target.author),
                        std::to_string((uint64_t)target.author.id), content,
                        isoFromSnowflake((uint64_t)target.id), images,
                        generation);
                } catch (const std::exception& e) {
                    recordManualCaptureFailure(
                        command,
                        std::string("manual capture could not complete: ") +
                            e.what(),
                        /*retryable=*/true, generation);
                }
                finishManualCapturePump(generation);
            });
    } catch (const std::exception& e) {
        if (!claimManualCaptureRestAttempt(attemptToken, generation)) return;
        recordManualCaptureFailure(
            command,
            std::string("Discord target fetch could not start: ") + e.what(),
            /*retryable=*/true, generation);
        finishManualCapturePump(generation);
    }
}

std::vector<DiscordAttachmentDownload> DiscordBot::downloadAttachments(
    const std::string& channelId, const std::string& messageId,
    const std::vector<std::string>& attachmentIds,
    std::chrono::seconds timeout) {
    std::vector<std::string> wanted;
    std::set<std::string> seen;
    for (const std::string& id : attachmentIds) {
        if (isDecimalDiscordSnowflake(id) && seen.insert(id).second)
            wanted.push_back(id);
    }
    if (wanted.empty()) return {};
    auto allErrors = [&](const std::string& error, bool retryable) {
        std::vector<DiscordAttachmentDownload> out;
        for (const std::string& id : wanted) {
            DiscordAttachmentDownload result;
            result.meta.attachmentId = id;
            result.meta.sourceChannelId = channelId;
            result.meta.sourceMessageId = messageId;
            result.error = error;
            result.retryable = retryable;
            out.push_back(std::move(result));
        }
        return out;
    };
    const std::shared_ptr<dpp::cluster> requestCluster = lockCluster();
    if (!requestCluster || status().state != "connected")
        return allErrors("Discord bot is not connected", true);

    struct DownloadState {
        std::mutex mutex;
        std::promise<std::vector<DiscordAttachmentDownload>> promise;
        std::vector<DiscordAttachmentDownload> results;
        std::size_t remaining = 0;
        bool settled = false;
    };
    auto state = std::make_shared<DownloadState>();
    std::future<std::vector<DiscordAttachmentDownload>> future =
        state->promise.get_future();
    auto settle = [state]() {
        std::vector<DiscordAttachmentDownload> ready;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->settled || state->remaining != 0) return;
            state->settled = true;
            ready = std::move(state->results);
        }
        state->promise.set_value(std::move(ready));
    };
    try {
        requestCluster->message_get(
            dpp::snowflake(std::stoull(messageId)),
            dpp::snowflake(std::stoull(channelId)),
            [requestCluster, state, settle, wanted, channelId, messageId](
                const dpp::confirmation_callback_t& cc) {
                if (cc.is_error()) {
                    const bool retryable =
                        cc.http_info.error != dpp::h_success ||
                        isRetryableDiscordHttpStatus(cc.http_info.status);
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        for (const std::string& id : wanted) {
                            DiscordAttachmentDownload result;
                            result.meta.attachmentId = id;
                            result.meta.sourceChannelId = channelId;
                            result.meta.sourceMessageId = messageId;
                            result.error = "could not refetch Discord message: " +
                                           cc.get_error().message;
                            result.retryable = retryable;
                            state->results.push_back(std::move(result));
                        }
                    }
                    settle();
                    return;
                }
                const dpp::message& message = std::get<dpp::message>(cc.value);
                std::map<std::string, const dpp::attachment*> available;
                for (const dpp::attachment& attachment : message.attachments)
                    available[std::to_string((uint64_t)attachment.id)] = &attachment;

                // Copy the original attachment URL while the REST message is
                // alive. DiscordChatExporter follows this same source-URL
                // path; avoiding attachment::download also avoids depending
                // on D++'s back-pointer surviving REST message copies.
                std::vector<std::pair<std::string, DiscordAttachmentMeta>> downloads;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    for (const std::string& id : wanted) {
                        auto found = available.find(id);
                        if (found == available.end()) {
                            DiscordAttachmentDownload result;
                            result.meta.attachmentId = id;
                            result.meta.sourceChannelId = channelId;
                            result.meta.sourceMessageId = messageId;
                            result.error = "attachment is no longer available on Discord";
                            state->results.push_back(std::move(result));
                            continue;
                        }
                        const dpp::attachment& attachment = *found->second;
                        DiscordAttachmentMeta meta;
                        meta.attachmentId = id;
                        meta.sourceChannelId = channelId;
                        meta.sourceMessageId = messageId;
                        meta.filename = attachment.filename;
                        meta.contentType = attachment.content_type;
                        meta.sizeBytes = attachment.size;
                        meta.width = attachment.width;
                        meta.height = attachment.height;
                        meta.ephemeral = attachment.ephemeral;
                        if (!isSupportedTicketAttachmentMetadata(
                                meta.contentType, meta.filename, meta.ephemeral)) {
                            DiscordAttachmentDownload result;
                            result.meta = meta;
                            result.error = "ephemeral Discord attachments cannot be retained";
                            state->results.push_back(std::move(result));
                            continue;
                        }
                        if (meta.sizeBytes > kMaxTicketAttachmentBytes) {
                            DiscordAttachmentDownload result;
                            result.meta = meta;
                            result.error = "attachment exceeds the 10 MiB per-file limit";
                            state->results.push_back(std::move(result));
                            continue;
                        }
                        if (attachment.url.empty()) {
                            DiscordAttachmentDownload result;
                            result.meta = meta;
                            result.error = "Discord attachment has no downloadable URL";
                            state->results.push_back(std::move(result));
                            continue;
                        }
                        downloads.push_back({attachment.url, std::move(meta)});
                    }
                    std::size_t plannedBytes = 0;
                    bool batchTooLarge = false;
                    for (const auto& download : downloads) {
                        const std::size_t bytes = download.second.sizeBytes;
                        if (bytes > kMaxTicketAttachmentTotalBytes -
                                        (std::min)(plannedBytes,
                                                   kMaxTicketAttachmentTotalBytes)) {
                            batchTooLarge = true;
                            break;
                        }
                        plannedBytes += bytes;
                    }
                    if (batchTooLarge) {
                        for (const auto& download : downloads) {
                            DiscordAttachmentDownload result;
                            result.meta = download.second;
                            result.error =
                                "attachment batch exceeds the 50 MiB per-message limit";
                            state->results.push_back(std::move(result));
                        }
                        downloads.clear();
                    }
                    state->remaining = downloads.size();
                }
                if (downloads.empty()) {
                    settle();
                    return;
                }
                for (const auto& pair : downloads) {
                    const std::string sourceUrl = pair.first;
                    const DiscordAttachmentMeta meta = pair.second;
                    if (!isDiscordAttachmentUrl(sourceUrl)) {
                        DiscordAttachmentDownload result;
                        result.meta = meta;
                        result.error =
                            "attachment URL is not an allowed Discord CDN URL";
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->results.push_back(std::move(result));
                            if (state->remaining > 0) --state->remaining;
                        }
                        settle();
                        continue;
                    }
                    try {
                        requestCluster->request(
                            sourceUrl, dpp::m_get,
                            [state, settle, meta](const dpp::http_request_completion_t& http) {
                                DiscordAttachmentDownload result;
                                result.meta = meta;
                                result.responseContentType = responseContentType(http);
                                if (http.error != dpp::h_success || http.status != 200) {
                                    result.error = "Discord attachment download failed (HTTP " +
                                                   std::to_string(http.status) + ")";
                                    result.retryable =
                                        http.error != dpp::h_success ||
                                        isRetryableDiscordHttpStatus(http.status);
                                } else if (responseContentLength(http) >
                                               kMaxTicketAttachmentBytes ||
                                           http.body.size() >
                                               kMaxTicketAttachmentBytes) {
                                    result.error = "download exceeds the 10 MiB per-file limit";
                                } else {
                                    result.bytes = http.body;
                                }
                                {
                                    std::lock_guard<std::mutex> lock(state->mutex);
                                    state->results.push_back(std::move(result));
                                    if (state->remaining > 0) --state->remaining;
                                }
                                settle();
                            });
                    } catch (const std::exception& e) {
                        DiscordAttachmentDownload result;
                        result.meta = meta;
                        result.error = std::string("Discord attachment download could not start: ") +
                                       e.what();
                        result.retryable = true;
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->results.push_back(std::move(result));
                            if (state->remaining > 0) --state->remaining;
                        }
                        settle();
                    }
                }
            });
    } catch (const std::exception& e) {
        return allErrors(std::string("Discord message refetch could not start: ") + e.what(),
                         true);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (future.wait_for(std::chrono::milliseconds(250)) ==
            std::future_status::ready)
            return future.get();
        if (stopRequested())
            return allErrors("Discord attachment download interrupted by shutdown",
                             true);
        if (std::chrono::steady_clock::now() >= deadline)
            return allErrors("Discord attachment download timed out", true);
    }
}

// ---------------------------------------------------------------------------
// !leaderboard / !tickets: admin-only operator cards
// ---------------------------------------------------------------------------

bool DiscordBot::maybeLeaderboardCommand(const void* msgPtr, bool liveEvent) {
    const dpp::message& m = *static_cast<const dpp::message*>(msgPtr);
    if (!lockCluster() || toLower(trim(m.content)) != "!leaderboard") return false;
    const std::string authorId = std::to_string((uint64_t)m.author.id);
    if (!discordAdminUserAllowed(db_, authorId)) return false;

    const std::string channelId = std::to_string((uint64_t)m.channel_id);
    const std::string guildId = m.guild_id
        ? std::to_string((uint64_t)m.guild_id) : std::string();
    if (discordWatch(db_, guildId, channelId)) {
        dpp::channel* channel = dpp::find_channel(m.channel_id);
        dpp::guild* guild = m.guild_id ? dpp::find_guild(m.guild_id) : nullptr;
        IngestOutcome command = ingestDiscordMessage(
            db_, channelId, channel ? channel->name : "", guildId,
            guild ? guild->name : "", std::to_string((uint64_t)m.id),
            displayName(m.author), authorId, m.content,
            isoFromSnowflake((uint64_t)m.id), /*asCommand=*/true);
        if (command.duplicate) return true;
    }

    // A history page records the command as its replay key, but it must never
    // post a fresh card or delete an old user message. Only the gateway create
    // event is authorized to perform those live side effects.
    if (!liveEvent) return true;

    std::string error;
    std::vector<LeaderboardEntry> entries;
    try {
        entries = contributorLeaderboard(db_, 10, &error);
    } catch (const std::exception& e) {
        error = e.what();
    }
    NotifyCard card;
    card.title = "\xF0\x9F\x8F\x86 Community contributors";
    card.footer = DEVHUB_APP_NAME;
    if (!error.empty()) {
        card.description = "leaderboard unavailable until its exclusion "
                           "preference is repaired in DevHub";
        card.color = 0xf85149;
    } else if (entries.empty()) {
        card.description = "no public fix/implementation contributors are listed yet";
    } else {
        card.description = compactLeaderboardDescription(entries);
    }

    const uint64_t channel = (uint64_t)m.channel_id;
    const uint64_t commandMessage = (uint64_t)m.id;
    postCard(channelId, card,
        [this, channel, commandMessage](const std::string&) {
            const std::shared_ptr<dpp::cluster> cluster = lockCluster();
            if (!cluster) return;
            cluster->message_delete(
                dpp::snowflake(commandMessage), dpp::snowflake(channel),
                [](const dpp::confirmation_callback_t& response) {
                    if (response.is_error())
                        appLog("[leaderboard] couldn't remove the command "
                               "(needs Manage Messages): " +
                               response.get_error().message);
                });
        });
    appLog("[leaderboard] requested by " + displayName(m.author));
    return true;
}

// Keycap emojis the bot seeds under the menu (1..9 + ten).
static const char* kKeycaps[10] = {
    "1\xEF\xB8\x8F\xE2\x83\xA3", "2\xEF\xB8\x8F\xE2\x83\xA3",
    "3\xEF\xB8\x8F\xE2\x83\xA3", "4\xEF\xB8\x8F\xE2\x83\xA3",
    "5\xEF\xB8\x8F\xE2\x83\xA3", "6\xEF\xB8\x8F\xE2\x83\xA3",
    "7\xEF\xB8\x8F\xE2\x83\xA3", "8\xEF\xB8\x8F\xE2\x83\xA3",
    "9\xEF\xB8\x8F\xE2\x83\xA3", "\xF0\x9F\x94\x9F"};
// Seeded last: the admin clicks it to delete the menu card.
static const char* kXEmoji = "\xE2\x9D\x8C";
static const char* kHighApproveEmoji =
    "\xE2\x9A\xA0\xEF\xB8\x8F"; // warning + VS16
static const char* kHighApproveEmojiWithoutVariation = "\xE2\x9A\xA0";
static const char* kNormalApproveEmoji = "\xE2\x9C\x85";
static const char* kLowApproveEmoji = "\xF0\x9F\x98\xB4";
static const char* kReviewEmojis[] = {
    kHighApproveEmoji, kNormalApproveEmoji, kLowApproveEmoji, kXEmoji};
static constexpr size_t kReviewEmojiCount =
    sizeof(kReviewEmojis) / sizeof(kReviewEmojis[0]);

static int reviewApprovalPriority(const std::string& emoji) {
    if (emoji == kHighApproveEmoji ||
        emoji == kHighApproveEmojiWithoutVariation)
        return kDiscordReviewHighPriority;
    if (emoji == kNormalApproveEmoji)
        return kDiscordReviewNormalPriority;
    if (emoji == kLowApproveEmoji)
        return kDiscordReviewLowPriority;
    if (emoji == kXEmoji) return 0;
    return -1;
}

static int keycapIndex(const std::string& name) {
    if (name == "\xF0\x9F\x94\x9F") return 9; // ten
    // digit + (optional VS16) + combining keycap
    if (!name.empty() && name[0] >= '1' && name[0] <= '9' &&
        name.find("\xE2\x83\xA3") != std::string::npos)
        return name[0] - '1';
    return -1;
}

// Menu numbers as a compact field so the mapping stays visible on the
// per-project ticket view.
static std::string menuFieldText(
    const std::vector<std::pair<long long, std::string>>& menu) {
    std::string s;
    for (size_t i = 0; i < menu.size() && s.size() < 950; ++i)
        s += std::to_string(i + 1) + ". " + menu[i].second + "\n";
    return s;
}

bool DiscordBot::maybeTicketsCommand(const void* msgPtr, bool liveEvent) {
    const dpp::message& m = *static_cast<const dpp::message*>(msgPtr);
    if (!lockCluster()) return false;
    // "!tickets" = interactive menu with reaction paging;
    // "!xatickets" = plain numbered summary, no reactions, no drill-down.
    const std::string cmd = toLower(trim(m.content));
    const bool interactive = cmd == "!tickets";
    if (!interactive && cmd != "!xatickets") return false;

    // Admin-only. Anyone else typing "!tickets" is just chatting.
    const std::string authorId = std::to_string((uint64_t)m.author.id);
    if (!discordAdminUserAllowed(db_, authorId)) return false;

    std::string chanId = std::to_string((uint64_t)m.channel_id);
    std::string guildId =
        m.guild_id ? std::to_string((uint64_t)m.guild_id) : std::string();

    // In watched channels the command lands in history sweeps too - record
    // it (detection off) and let the duplicate flag stop replays. Unwatched
    // channels only ever see live events, no gate needed.
    if (discordWatch(db_, guildId, chanId)) {
        dpp::channel* ch = dpp::find_channel(m.channel_id);
        dpp::guild* g = m.guild_id ? dpp::find_guild(m.guild_id) : nullptr;
        IngestOutcome ingest = ingestDiscordMessage(
            db_, chanId, ch ? ch->name : "", guildId, g ? g->name : "",
            std::to_string((uint64_t)m.id), displayName(m.author), authorId,
            m.content, isoFromSnowflake((uint64_t)m.id), /*asCommand=*/true);
        if (ingest.duplicate) return true;
    }

    // Backfill is observation, not command execution. The durable command row
    // above prevents a later replay, while skipping all post/delete effects.
    if (!liveEvent) return true;

    std::vector<TicketMenuEntry> entries = ticketMenuProjects(db_);

    // The card replaces the command - once it posts, the admin's "!tickets"
    // message is removed (theirs ONLY, and only because they asked for the
    // menu; nobody else's messages are ever touched).
    const uint64_t chan = (uint64_t)m.channel_id;
    const uint64_t cmdMsg = (uint64_t)m.id;
    auto deleteCommand = [this, chan, cmdMsg]() {
        const std::shared_ptr<dpp::cluster> cluster = lockCluster();
        if (!cluster) return;
        cluster->message_delete(
            dpp::snowflake(cmdMsg), dpp::snowflake(chan),
            [](const dpp::confirmation_callback_t& cc) {
                if (cc.is_error())
                    appLog("[tickets] couldn't remove the !tickets command "
                           "(needs Manage Messages): " +
                           cc.get_error().message);
            });
    };

    NotifyCard card;
    card.title = "\xF0\x9F\x8E\xAB Open tickets";
    card.footer = DEVHUB_APP_NAME;
    if (entries.empty()) {
        card.description =
            "no opted-in projects currently have active fix or implementation tickets";
        postCard(chanId, card,
                 [deleteCommand](const std::string&) { deleteCommand(); });
        return true;
    }
    if (entries.size() > 10) entries.resize(10); // one keycap each

    std::vector<std::pair<long long, std::string>> menu;
    for (size_t i = 0; i < entries.size(); ++i) {
        card.description += std::to_string(i + 1) + ". " + entries[i].name +
                            " (" + std::to_string(entries[i].openCount) +
                            " open)\n";
        menu.emplace_back(entries[i].projectId, entries[i].name);
    }

    // Plain summary: post the list, remove the command, done.
    if (!interactive) {
        postCard(chanId, card,
                 [deleteCommand](const std::string&) { deleteCommand(); });
        appLog("[tickets] summary requested by " + displayName(m.author));
        return true;
    }

    card.description += "\nreact a number for that project's open tickets - "
                        "\xE2\x9D\x8C closes this menu";

    const size_t count = entries.size();
    postCard(chanId, card,
             [this, chan, count, menu,
              deleteCommand](const std::string& mid) {
                  uint64_t menuId = std::strtoull(mid.c_str(), nullptr, 10);
                  if (menuId == 0 || !lockCluster()) return;
                 {
                     std::lock_guard<std::mutex> lk(menusMu_);
                     ticketMenus_[menuId] = menu;
                     while (ticketMenus_.size() > 8) // oldest first
                         ticketMenus_.erase(ticketMenus_.begin());
                 }
                 deleteCommand();
                 seedTicketReactions(menuId, chan, 0, count,
                                     /*retriesLeft=*/2);
             });
    appLog("[tickets] menu requested by " + displayName(m.author));
    return true;
}

// One reaction at a time: the next add is issued from the previous add's
// completion callback after a pause (Discord's reactions bucket allows
// roughly one add per ~300ms per channel; concurrent adds all 429).
// Steps 0..count-1 are the keycaps, step count is the closing X.
void DiscordBot::seedTicketReactions(uint64_t msgId, uint64_t chanId,
                                     size_t idx, size_t count,
                                     int retriesLeft) {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster || count > 10 || idx > count) return;
    const char* emoji = idx < count ? kKeycaps[idx] : kXEmoji;
    cluster->message_add_reaction(
        dpp::snowflake(msgId), dpp::snowflake(chanId), emoji,
        [this, cluster, msgId, chanId, idx, count,
         retriesLeft](const dpp::confirmation_callback_t& cc) {
            if (!lockCluster()) return;
            if (cc.is_error()) {
                if (retriesLeft > 0) { // rate limited - back off and retry
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(900));
                    seedTicketReactions(msgId, chanId, idx, count,
                                        retriesLeft - 1);
                    return;
                }
                appLog("[tickets] reaction " + std::to_string(idx + 1) +
                       " failed: " + cc.get_error().message);
                // fall through - still try the remaining reactions
            }
            if (idx + 1 > count) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            seedTicketReactions(msgId, chanId, idx + 1, count,
                                /*retriesLeft=*/2);
        });
}

void DiscordBot::queuePendingReviewReactions(
    const std::string& channelId, const std::string& messageId) {
    if (!isDecimalDiscordSnowflake(channelId) ||
        !isDecimalDiscordSnowflake(messageId))
        return;
    ReviewReactionTask task;
    task.channelId = std::strtoull(channelId.c_str(), nullptr, 10);
    task.messageId = std::strtoull(messageId.c_str(), nullptr, 10);
    if (task.channelId == 0 || task.messageId == 0) return;
    queueReviewReactionTask(std::move(task));
}

void DiscordBot::queueTerminalReviewReactionCleanup(
    const std::string& channelId, const std::string& messageId,
    std::function<void()> onCleared,
    std::function<void(const std::string&, bool)> onFailed,
    std::function<void()> onHeartbeat,
    bool logSuccess) {
    if (!isDecimalDiscordSnowflake(channelId) ||
        !isDecimalDiscordSnowflake(messageId)) {
        if (onFailed)
            onFailed("notification all-reaction cleanup ids are invalid", false);
        return;
    }
    ReviewReactionTask task;
    task.channelId = std::strtoull(channelId.c_str(), nullptr, 10);
    task.messageId = std::strtoull(messageId.c_str(), nullptr, 10);
    task.cleanup = true;
    task.logSuccess = logSuccess;
    task.onCleared = std::move(onCleared);
    task.onFailed = std::move(onFailed);
    task.onHeartbeat = std::move(onHeartbeat);
    if (task.channelId == 0 || task.messageId == 0) {
        if (task.onFailed)
            task.onFailed("notification all-reaction cleanup ids are invalid",
                          false);
        return;
    }
    queueReviewReactionTask(std::move(task));
}

void DiscordBot::queueReviewReactionTask(ReviewReactionTask task) {
    uint64_t generation = 0;
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(reviewReactionMu_);
        // Bind this task and its pump to the same ready-session generation.
        // The ready handler advances the generation while holding this lock,
        // so an old task cannot be inserted behind a freshly reset queue.
        generation = reviewReactionGeneration_.load();
        task.taskId = ++reviewReactionTaskSequence_;
        if (task.taskId == 0) task.taskId = ++reviewReactionTaskSequence_;
        if (task.cleanup) {
            // A durable edit can wait behind the active REST request longer
            // than its SQLite lease. Heartbeat queued cleanup from the next
            // watchdog tick, not only after it reaches the front.
            task.nextHeartbeat = std::chrono::steady_clock::now();
            // Keep the active request at the front, then place cleanup ahead
            // of pending seed backlog. Durable target uniqueness prevents the
            // recovery pump from queueing duplicate historical work.
            auto insertAt = reviewReactionQ_.begin();
            if (reviewReactionBusy_ && insertAt != reviewReactionQ_.end())
                ++insertAt;
            while (insertAt != reviewReactionQ_.end() && insertAt->cleanup)
                ++insertAt;
            reviewReactionQ_.insert(insertAt, std::move(task));
        } else {
            auto duplicateBegin = reviewReactionQ_.begin();
            if (reviewReactionBusy_ &&
                duplicateBegin != reviewReactionQ_.end())
                ++duplicateBegin;
            // Never collapse a new pending-state request into the active
            // task: that task may already be clearing controls for an older
            // terminal state. Keep one follow-up seed so the latest state wins.
            const auto duplicate = std::find_if(
                duplicateBegin, reviewReactionQ_.end(),
                [&](const ReviewReactionTask& queued) {
                    return !queued.cleanup &&
                           queued.channelId == task.channelId &&
                           queued.messageId == task.messageId;
                });
            if (duplicate == reviewReactionQ_.end())
                reviewReactionQ_.push_back(std::move(task));
        }
        if (!reviewReactionBusy_ && !reviewReactionQ_.empty()) {
            reviewReactionBusy_ = true;
            start = true;
        }
    }
    if (start) pumpReviewReactionQueue(generation);
}

bool DiscordBot::reviewReactionTaskActive(uint64_t generation,
                                           uint64_t taskId) {
    std::lock_guard<std::mutex> lk(reviewReactionMu_);
    return generation == reviewReactionGeneration_.load() &&
           reviewReactionBusy_ && !reviewReactionQ_.empty() &&
           reviewReactionQ_.front().taskId == taskId;
}

void DiscordBot::expireReviewReactionTask() {
    uint64_t generation = 0;
    uint64_t taskId = 0;
    std::vector<std::function<void()>> heartbeats;
    {
        std::lock_guard<std::mutex> lk(reviewReactionMu_);
        if (!reviewReactionBusy_ || reviewReactionQ_.empty())
            return;
        const auto now = std::chrono::steady_clock::now();
        ReviewReactionTask& active = reviewReactionQ_.front();
        const bool expired =
            active.deadline != std::chrono::steady_clock::time_point{} &&
            now >= active.deadline;
        if (expired) {
            generation = reviewReactionGeneration_.load();
            taskId = active.taskId;
        }
        for (std::size_t i = 0; i < reviewReactionQ_.size(); ++i) {
            ReviewReactionTask& queued = reviewReactionQ_[i];
            if (i == 0 && expired) continue;
            if (!queued.cleanup || !queued.onHeartbeat ||
                now < queued.nextHeartbeat)
                continue;
            queued.nextHeartbeat = now + std::chrono::seconds(15);
            heartbeats.push_back(queued.onHeartbeat);
        }
    }
    for (const auto& heartbeat : heartbeats) {
        try {
            heartbeat();
        } catch (const std::exception& e) {
            appLog("[review] couldn't refresh cleanup lease: " +
                   redactHttpUrls(e.what()));
        }
    }
    if (taskId == 0) return;
    // A late D++ callback carries the same task id and is ignored after this
    // timeout wins. Terminal work returns to its dedicated durable cleanup
    // surface; pending seed work simply releases the lane for later recovery.
    finishReviewReactionTask(
        generation, taskId,
        "Discord reaction-control callback timed out", true);
}

void DiscordBot::recoverPendingReviewReactions() {
    try {
        const int reconciled = reconcileLegacyTerminalReviewControls(db_);
        if (reconciled > 0) {
            appLog("[review] queued all-reaction cleanup for " +
                   std::to_string(reconciled) + " legacy terminal card(s)");
            requestNotifyCardPost();
        }
    } catch (const std::exception& e) {
        appLog("[review] couldn't reconcile legacy terminal controls: " +
               redactHttpUrls(e.what()));
    }
    try {
        const auto cards = pendingDiscordReviewCards(db_, 10000);
        for (const auto& card : cards)
            queuePendingReviewReactions(card.channelId, card.messageId);
        if (cards.size() == 10000)
            appLog("[review] reaction-control recovery reached its "
                   "10,000-card safety bound");
    } catch (const std::exception& e) {
        appLog("[review] couldn't recover pending card controls: " +
               redactHttpUrls(e.what()));
    }
}

void DiscordBot::pumpReviewReactionQueue(uint64_t generation) {
    if (generation != reviewReactionGeneration_.load()) return;
    ReviewReactionTask target;
    {
        std::lock_guard<std::mutex> lk(reviewReactionMu_);
        if (generation != reviewReactionGeneration_.load()) return;
        if (reviewReactionQ_.empty()) {
            reviewReactionBusy_ = false;
            return;
        }
        if (reviewReactionQ_.front().deadline ==
            std::chrono::steady_clock::time_point{}) {
            const auto now = std::chrono::steady_clock::now();
            reviewReactionQ_.front().deadline =
                now + std::chrono::seconds(120);
            if (reviewReactionQ_.front().nextHeartbeat ==
                std::chrono::steady_clock::time_point{})
                reviewReactionQ_.front().nextHeartbeat =
                    now + std::chrono::seconds(15);
        }
        target = reviewReactionQ_.front();
    }
    bool stillPending = false;
    bool terminal = false;
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (cluster) {
        try {
            stillPending = pendingDiscordReviewCard(
                db_, std::to_string(target.channelId),
                std::to_string(target.messageId));
            if (!stillPending)
                terminal = terminalDiscordReviewCard(
                    db_, std::to_string(target.channelId),
                    std::to_string(target.messageId));
        } catch (const std::exception& e) {
            appLog("[review] couldn't validate reaction-control target: " +
                   redactHttpUrls(e.what()));
            finishReviewReactionTask(
                generation, target.taskId,
                "reaction-control state could not be validated", true);
            return;
        }
    }
    if (!cluster) {
        finishReviewReactionTask(generation, target.taskId, "bot is offline",
                                 true);
        return;
    }
    if (target.cleanup) {
        if (!terminal) {
            // A newer pending revision won before this older edit callback.
            // Completing the callback is safe: finishNotifyEdit detects the
            // revision mismatch and queues the current card state again.
            finishReviewReactionTask(generation, target.taskId);
            return;
        }
        clearAllReviewReactions(target.messageId, target.channelId, 2,
                                generation, target.taskId);
        return;
    }
    if (stillPending) {
        seedReviewReactionStep(target.messageId, target.channelId, 0, 2,
                               generation, target.taskId);
    } else if (terminal) {
        // A promotion can win while this seed task waits in the queue. Clear
        // every reaction that may have landed before the terminal transition.
        clearAllReviewReactions(target.messageId, target.channelId, 2,
                                generation, target.taskId);
    } else {
        finishReviewReactionTask(generation, target.taskId);
    }
}

void DiscordBot::seedReviewReactionStep(uint64_t msgId, uint64_t chanId,
                                        size_t step, int retriesLeft,
                                        uint64_t generation,
    uint64_t taskId) {
    if (!reviewReactionTaskActive(generation, taskId)) return;
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster || step >= kReviewEmojiCount) {
        finishReviewReactionTask(generation, taskId);
        return;
    }
    try {
        if (!pendingDiscordReviewCard(
                db_, std::to_string(chanId), std::to_string(msgId))) {
            if (terminalDiscordReviewCard(
                    db_, std::to_string(chanId), std::to_string(msgId)))
                clearAllReviewReactions(msgId, chanId, 2, generation, taskId);
            else
                finishReviewReactionTask(generation, taskId);
            return;
        }
    } catch (const std::exception& e) {
        appLog("[review] couldn't revalidate reaction seed: " +
               redactHttpUrls(e.what()));
        finishReviewReactionTask(generation, taskId);
        return;
    }
    const char* emoji = kReviewEmojis[step];
    cluster->message_add_reaction(
        dpp::snowflake(msgId), dpp::snowflake(chanId), emoji,
        [this, cluster, msgId, chanId, step, retriesLeft, generation,
         taskId](const dpp::confirmation_callback_t& response) {
            if (!reviewReactionTaskActive(generation, taskId)) return;
            if (!lockCluster()) {
                finishReviewReactionTask(generation, taskId);
                return;
            }
            if (response.is_error() && retriesLeft > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(900));
                seedReviewReactionStep(msgId, chanId, step, retriesLeft - 1,
                                       generation, taskId);
                return;
            }
            if (response.is_error())
                appLog("[review] reaction control failed: " +
                       redactHttpUrls(response.get_error().message));
            if (step + 1 < kReviewEmojiCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                seedReviewReactionStep(msgId, chanId, step + 1, 2, generation,
                                       taskId);
            } else {
                finishReviewReactionTask(generation, taskId);
            }
        });
}

void DiscordBot::clearAllReviewReactions(
    uint64_t msgId, uint64_t chanId, int retriesLeft, uint64_t generation,
    uint64_t taskId) {
    if (!reviewReactionTaskActive(generation, taskId)) return;
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster) {
        finishReviewReactionTask(generation, taskId, "bot is offline", true);
        return;
    }
    try {
        if (!terminalDiscordReviewCard(
                db_, std::to_string(chanId), std::to_string(msgId))) {
            // A newer pending revision won while an earlier delete was queued.
            // Release this task before touching the message; the compensating
            // pending edit will seed both controls afterward.
            finishReviewReactionTask(generation, taskId);
            return;
        }
    } catch (const std::exception& e) {
        appLog("[review] couldn't revalidate reaction cleanup: " +
               redactHttpUrls(e.what()));
        finishReviewReactionTask(
            generation, taskId,
            "reaction cleanup state could not be validated", true);
        return;
    }
    try {
        cluster->message_delete_all_reactions(
            dpp::snowflake(msgId), dpp::snowflake(chanId),
            [this, cluster, msgId, chanId, retriesLeft, generation,
             taskId](const dpp::confirmation_callback_t& response) {
                if (!reviewReactionTaskActive(generation, taskId)) return;
                if (!lockCluster()) {
                    finishReviewReactionTask(generation, taskId,
                                             "bot is offline", true);
                    return;
                }
                if (response.is_error() &&
                    isDiscordDeleteAlreadyAbsent(response.http_info.status)) {
                    // An absent message/reaction collection already satisfies
                    // the exact terminal-card cleanup contract.
                    finishReviewReactionTask(generation, taskId);
                    return;
                }
                const bool retryable = response.is_error() &&
                    retryableDiscordRestFailure(response);
                if (response.is_error() && retryable && retriesLeft > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(900));
                    clearAllReviewReactions(
                        msgId, chanId, retriesLeft - 1, generation, taskId);
                    return;
                }
                if (response.is_error()) {
                    const std::string error =
                        redactHttpUrls(response.get_error().message);
                    appLog("[review] all-reaction cleanup failed: " + error);
                    finishReviewReactionTask(
                        generation, taskId,
                        error.empty()
                            ? "Discord rejected all-reaction cleanup"
                            : error,
                        retryable);
                    return;
                }
                finishReviewReactionTask(generation, taskId);
            });
    } catch (const std::exception& e) {
        const std::string error = redactHttpUrls(e.what());
        if (retriesLeft > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
            clearAllReviewReactions(msgId, chanId, retriesLeft - 1,
                                    generation, taskId);
            return;
        }
        appLog("[review] all-reaction cleanup could not start: " + error);
        finishReviewReactionTask(
            generation, taskId,
            error.empty() ? "Discord all-reaction cleanup could not start"
                          : error,
            true);
    }
}

void DiscordBot::finishReviewReactionTask(uint64_t generation,
                                           uint64_t taskId,
                                           const std::string& error,
                                           bool retryable) {
    if (generation != reviewReactionGeneration_.load()) return;
    bool more = false;
    ReviewReactionTask completed;
    bool haveCompleted = false;
    {
        std::lock_guard<std::mutex> lk(reviewReactionMu_);
        if (generation != reviewReactionGeneration_.load()) return;
        if (!reviewReactionQ_.empty() &&
            reviewReactionQ_.front().taskId == taskId) {
            completed = std::move(reviewReactionQ_.front());
            reviewReactionQ_.pop_front();
            haveCompleted = true;
        } else return;
        more = !reviewReactionQ_.empty();
        if (!more) reviewReactionBusy_ = false;
    }
    if (haveCompleted && completed.cleanup) {
        try {
            if (error.empty()) {
                if (completed.logSuccess)
                    appLog("[review] all reactions removed from terminal card");
                if (completed.onCleared) completed.onCleared();
            } else if (completed.onFailed) {
                completed.onFailed(error, retryable);
            }
        } catch (const std::exception& e) {
            // The durable cleanup row remains deleting when its completion
            // callback cannot persist; its lease reclaims it later.
            appLog("[review] reaction cleanup completion failed: " +
                   redactHttpUrls(e.what()));
        }
    }
    if (more) pumpReviewReactionQueue(generation);
}

void DiscordBot::handleReaction(const void* evPtr) {
    const dpp::message_reaction_add_t& ev =
        *static_cast<const dpp::message_reaction_add_t*>(evPtr);
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster) return;
    // The bot seeding its own keycaps fires this too - skip self and bots.
    if (ev.reacting_user.id == cluster->me.id || ev.reacting_user.is_bot())
        return;
    // Pure in-memory disqualifiers come before the per-reaction SQLite admin
    // lookup. Unrelated remote reactions must not be able to schedule DB work.
    if (reviewApprovalPriority(ev.reacting_emoji.name) < 0 &&
        keycapIndex(ev.reacting_emoji.name) < 0)
        return;
    // Only whitelisted admins/developers drive menus; other reactions are inert.
    if (!discordAdminUserAllowed(
            db_, std::to_string((uint64_t)ev.reacting_user.id)))
        return;

    // Check exact durable notification-card lineage before the in-memory
    // !tickets menu. The same X emoji rejects a pending card but still closes
    // a ticket menu when no exact review card exists.
    const int approvalPriority =
        reviewApprovalPriority(ev.reacting_emoji.name);
    if (approvalPriority >= 0) {
        const bool approve = approvalPriority > 0;
        DiscordReviewOutcome review = reviewNotificationCard(
            db_, this, std::to_string((uint64_t)ev.channel_id),
            std::to_string((uint64_t)ev.message_id),
            std::to_string((uint64_t)ev.reacting_user.id), approve, {},
            approve ? approvalPriority : kDiscordReviewNormalPriority);
        if (review.matched) {
            if (review.ok) {
                if (review.duplicate) {
                    appLog("[review] terminal card already " + review.state +
                           "; opposite/replayed action ignored");
                } else {
                    appLog(std::string("[review] ") +
                           (approve
                                ? "approved P" +
                                      std::to_string(approvalPriority) + " "
                                : "rejected ") +
                           (review.itemId > 0
                                ? "I" + std::to_string(review.itemId)
                                : "pending Discord suggestion"));
                }
            } else {
                // A general/unmapped source stays pending. Remove only this
                // admin's attempted reaction so it can be retried after the
                // project is assigned in DevHub.
                cluster->message_delete_reaction(
                    ev.message_id, ev.channel_id, ev.reacting_user.id,
                    ev.reacting_emoji.name,
                    [](const dpp::confirmation_callback_t& response) {
                        if (response.is_error())
                            appLog("[review] couldn't reset failed action: " +
                                   response.get_error().message);
                    });
                appLog("[review] action left pending: " +
                       redactHttpUrls(review.error));
            }
            return;
        }
    }

    std::vector<std::pair<long long, std::string>> menu;
    {
        std::lock_guard<std::mutex> lk(menusMu_);
        auto it = ticketMenus_.find((uint64_t)ev.message_id);
        if (it == ticketMenus_.end()) return;
        menu = it->second;
    }

    // X closes the menu: delete OUR OWN card (never anyone's message).
    if (ev.reacting_emoji.name == kXEmoji) {
        {
            std::lock_guard<std::mutex> lk(menusMu_);
            ticketMenus_.erase((uint64_t)ev.message_id);
        }
        cluster->message_delete(ev.message_id, ev.channel_id);
        appLog("[tickets] menu closed");
        return;
    }

    int idx = keycapIndex(ev.reacting_emoji.name);
    if (idx < 0 || idx >= (int)menu.size()) return;

    // Reset the admin's keycap right away so every number sits at 1 and the
    // same project can be re-pulled with a single click.
    cluster->message_delete_reaction(
        ev.message_id, ev.channel_id, ev.reacting_user.id,
        ev.reacting_emoji.name, [](const dpp::confirmation_callback_t& cc) {
            if (cc.is_error())
                appLog("[tickets] couldn't reset the reaction (needs Manage "
                       "Messages): " + cc.get_error().message);
        });

    const long long projectId = menu[idx].first;
    const std::string& name = menu[idx].second;
    std::vector<TicketLine> lines = ticketTitlesForProject(db_, projectId);

    NotifyCard card;
    card.title = "\xF0\x9F\x8E\xAB " + name + " - open tickets (" +
                 std::to_string(lines.size()) + ")";
    card.footer = DEVHUB_APP_NAME;
    if (lines.empty()) {
        card.description = "no open fix/implementation tickets";
    } else {
        size_t shown = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (card.description.size() > 3800) break;
            card.description += std::to_string(i + 1) + ". [" +
                                (lines[i].type == "fix" ? "fix" : "impl") +
                                "] " + lines[i].title + "\n";
            ++shown;
        }
        if (shown < lines.size())
            card.description +=
                "... +" + std::to_string(lines.size() - shown) + " more";
    }
    card.fields = {{"projects", menuFieldText(menu)}};
    editCard(std::to_string((uint64_t)ev.channel_id),
             std::to_string((uint64_t)ev.message_id), card);
    appLog("[tickets] " + name + " - " + std::to_string(lines.size()) +
           " open shown");
}

// ---------------------------------------------------------------------------
// outbound: notification cards + moderation
// ---------------------------------------------------------------------------
static dpp::embed cardToEmbed(const NotifyCard& c) {
    dpp::embed e;
    e.set_title(escapeDiscordMarkdown(c.title));
    if (!c.description.empty())
        e.set_description(escapeDiscordMarkdown(c.description));
    if (c.url.rfind("https://discord.com/channels/", 0) == 0)
        e.set_url(c.url);
    e.set_color(c.color);
    for (const auto& f : c.fields)
        e.add_field(escapeDiscordMarkdown(f.first),
                    escapeDiscordMarkdown(f.second), true);
    if (!c.footer.empty())
        e.set_footer(dpp::embed_footer().set_text(
            escapeDiscordMarkdown(c.footer)));
    return e;
}

static bool parseSnowflake(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtoull(s.c_str(), &end, 10);
    return end && *end == '\0' && out != 0;
}

void DiscordBot::postCard(const std::string& channelId, const NotifyCard& card,
                          std::function<void(const std::string&)> onPosted,
                          std::function<void(const std::string&, bool)> onFailed) {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    uint64_t chan = 0;
    if (!cluster || !parseSnowflake(channelId, chan)) {
        appLog("[notify] card NOT posted (bot offline or bad channel id)");
        if (onFailed)
            onFailed("bot offline or notification channel id is invalid",
                     cluster == nullptr);
        return;
    }
    try {
        dpp::message m(dpp::snowflake(chan), "");
        m.add_embed(cardToEmbed(card));
        cluster->message_create(m, [cluster, onPosted, onFailed](
                                       const dpp::confirmation_callback_t& cc) {
            if (cc.is_error()) {
                const std::string error = redactHttpUrls(cc.get_error().message);
                appLog("[notify] post FAILED: " + error);
                if (onFailed)
                    onFailed(error, retryableDiscordRestFailure(cc));
                return;
            }
            const auto& posted = std::get<dpp::message>(cc.value);
            appLog("[notify] card posted (message " +
                   std::to_string((uint64_t)posted.id) + ")");
            if (onPosted) onPosted(std::to_string((uint64_t)posted.id));
        });
    } catch (const std::exception& e) {
        const std::string error = redactHttpUrls(e.what());
        appLog("[notify] post could not start: " + error);
        if (onFailed) onFailed(error, true);
    }
}

void DiscordBot::deleteCard(const std::string& channelId,
                            const std::string& messageId,
                            std::function<void()> onDeleted,
                            std::function<void(const std::string&, bool)> onFailed) {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    uint64_t chan = 0, msg = 0;
    if (!cluster || !parseSnowflake(channelId, chan) ||
        !parseSnowflake(messageId, msg)) {
        appLog("[notify] orphan card NOT removed (bot offline or ids missing)");
        if (onFailed)
            onFailed("bot offline or orphan card ids are invalid",
                     cluster == nullptr);
        return;
    }
    try {
        cluster->message_delete(
            dpp::snowflake(msg), dpp::snowflake(chan),
            [cluster, onDeleted, onFailed](
                const dpp::confirmation_callback_t& cc) {
                if (cc.is_error()) {
                    // Discord 404 means the exact callback-generated orphan is
                    // already gone, which satisfies the cleanup contract.
                    if (isDiscordDeleteAlreadyAbsent(cc.http_info.status)) {
                        appLog("[notify] orphan card already absent");
                        if (onDeleted) onDeleted();
                        return;
                    }
                    const std::string error =
                        redactHttpUrls(cc.get_error().message);
                    appLog("[notify] orphan card removal FAILED: " + error);
                    if (onFailed)
                        onFailed(error, retryableDiscordRestFailure(cc));
                    return;
                }
                appLog("[notify] orphan card removed");
                if (onDeleted) onDeleted();
            });
    } catch (const std::exception& e) {
        const std::string error = redactHttpUrls(e.what());
        appLog("[notify] orphan card removal could not start: " + error);
        if (onFailed) onFailed(error, true);
    }
}

void DiscordBot::editCard(const std::string& channelId,
                          const std::string& messageId,
                          const NotifyCard& card,
                          std::function<void()> onEdited,
                          std::function<void(const std::string&, bool)> onFailed) {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    uint64_t chan = 0, msg = 0;
    if (!cluster || !parseSnowflake(channelId, chan) ||
        !parseSnowflake(messageId, msg)) {
        appLog("[notify] card NOT updated (bot offline or ids missing)");
        if (onFailed)
            onFailed("bot offline or notification card ids are invalid",
                     cluster == nullptr);
        return;
    }
    try {
        dpp::message m;
        m.id = dpp::snowflake(msg);
        m.channel_id = dpp::snowflake(chan);
        m.add_embed(cardToEmbed(card));
        cluster->message_edit(m, [cluster, onEdited, onFailed](
                                    const dpp::confirmation_callback_t& cc) {
            if (cc.is_error()) {
                appLog("[notify] edit FAILED: " +
                       redactHttpUrls(cc.get_error().message));
                if (onFailed)
                    onFailed(redactHttpUrls(cc.get_error().message),
                             retryableDiscordRestFailure(cc) &&
                                 !discordOldMessageEditLimit(cc));
            } else if (onEdited) {
                onEdited();
            }
        });
    } catch (const std::exception& e) {
        appLog("[notify] card edit could not start: " +
               redactHttpUrls(e.what()));
        if (onFailed) onFailed(redactHttpUrls(e.what()), true);
    }
}


void DiscordBot::backfillGuild(const void* guildPtr) {
    const dpp::guild* g = static_cast<const dpp::guild*>(guildPtr);
    std::string guildId = std::to_string((uint64_t)g->id);
    bool wholeGuild = discordGuildEnabled(db_, guildId);

    for (dpp::snowflake cid : g->channels) {
        dpp::channel* ch = dpp::find_channel(cid);
        if (!ch || !ch->is_text_channel()) continue;
        std::string cidStr = std::to_string((uint64_t)cid);
        if (!wholeGuild && !discordChannelEnabled(db_, cidStr)) continue;
        std::string lastRead = discordChannelLastRead(db_, cidStr);
        uint64_t after = lastRead.empty() ? 0 : snowflakeFromIso(lastRead);
        enqueueSweep({(uint64_t)cid, after, 0, /*retries=*/2});
    }
    pumpSweep();
}

void DiscordBot::enqueueSweep(const SweepJob& job, bool front) {
    std::lock_guard<std::mutex> lk(sweepMu_);
    for (auto it = sweepQ_.begin(); it != sweepQ_.end(); ++it) {
        if (it->channel != job.channel) continue;
        if (!front) return; // already scheduled this round
        sweepQ_.erase(it);  // an exact retry/page-forward takes precedence
        break;
    }
    if (front) sweepQ_.push_front(job);
    else sweepQ_.push_back(job);
}

void DiscordBot::pumpSweep() {
    if (std::chrono::steady_clock::now() < sweepResumeAt_.load()) return;
    SweepJob job{};
    {
        std::lock_guard<std::mutex> lk(sweepMu_);
        if (sweepBusy_ || sweepQ_.empty()) return;
        job = sweepQ_.front();
        sweepQ_.pop_front();
        sweepBusy_ = true;
    }
    runSweep(job);
}

bool DiscordBot::enqueueSweepPage(std::shared_ptr<QueuedSweepPage> page) {
    if (!page || page->messages.empty()) return true;
    constexpr size_t kMaxQueuedHistoryMessages = 2000;
    std::lock_guard<std::mutex> lk(sweepMessagesMu_);
    if (page->messages.size() >
        kMaxQueuedHistoryMessages - queuedSweepMessages_)
        return false;
    queuedSweepMessages_ += page->messages.size();
    sweepPages_.push_back(std::move(page));
    return true;
}

void DiscordBot::drainSweepMessages(size_t limit) {
    std::vector<dpp::message> batch;
    batch.reserve(limit);
    {
        std::lock_guard<std::mutex> lk(sweepMessagesMu_);
        while (batch.size() < limit && !sweepPages_.empty()) {
            std::shared_ptr<QueuedSweepPage>& page = sweepPages_.front();
            while (batch.size() < limit && !page->messages.empty()) {
                batch.push_back(std::move(page->messages.front()));
                page->messages.pop_front();
                --queuedSweepMessages_;
            }
            if (page->messages.empty()) sweepPages_.pop_front();
        }
    }
    for (const dpp::message& message : batch) {
        guardedDiscordHandler("history message ingest",
                              [&]() { handleMessage(&message, false); });
    }
}

void DiscordBot::runSweep(SweepJob job) {
    const std::shared_ptr<dpp::cluster> cluster = lockCluster();
    if (!cluster) {
        std::lock_guard<std::mutex> lk(sweepMu_);
        sweepBusy_ = false;
        sweepQ_.clear();
        return;
    }
    cluster->messages_get(
        job.channel, 0, 0, job.after, 100,
        [this, cluster, job](const dpp::confirmation_callback_t& cc) {
            if (!lockCluster()) return; // stop() resets the queue and busy flag
            if (cc.is_error()) {
                {
                    std::lock_guard<std::mutex> lk(sweepMu_);
                    sweepBusy_ = false;
                }
                if (job.retries > 0) {
                    // Transient (dropped socket / 429): the owned watchdog
                    // resumes this retry; never sleep on a D++ pool thread.
                    enqueueSweep({job.channel, job.after, job.page,
                                  job.retries - 1}, /*front=*/true);
                    sweepResumeAt_ = std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(1200);
                } else {
                    appLog("[discord] history sweep failed for channel " +
                           std::to_string(job.channel) +
                           " - retrying on the next rescan");
                }
                return;
            }
            const auto& mm = std::get<dpp::message_map>(cc.value);
            auto page = std::make_shared<QueuedSweepPage>();
            uint64_t maxId = 0;
            for (const auto& [id, msg] : mm) {
                if ((uint64_t)id > maxId) maxId = (uint64_t)id;
                if (msg.author.is_bot() || msg.content.empty()) continue;
                page->messages.push_back(msg);
            }
            if (!enqueueSweepPage(page)) {
                {
                    std::lock_guard<std::mutex> lk(sweepMu_);
                    sweepBusy_ = false;
                }
                enqueueSweep(job, /*front=*/true);
                sweepResumeAt_ = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(250);
                appLog("[discord] history ingest queue reached its 2,000-message "
                       "bound; deferring the page without advancing");
                return;
            }
            // The bot just looked at this channel - even when nothing new
            // was there. This is what "last checked" shows in the UI.
            discordMarkChannelChecked(db_, std::to_string(job.channel));
            {
                // Sweeps flow again - retire any lingering hiccup warning.
                std::lock_guard<std::mutex> lk(mu_);
                st_.warn.clear();
            }
            {
                std::lock_guard<std::mutex> lk(sweepMu_);
                sweepBusy_ = false;
            }
            // Page forward (bounded) when the window was full; the front of
            // the queue keeps one channel's pages contiguous.
            if (mm.size() == 100 && job.page < 4 && maxId != 0)
                enqueueSweep({job.channel, maxId, job.page + 1, job.retries},
                             /*front=*/true);
            // Space requests out, but let the owned watchdog start the next
            // one so this D++ pool callback returns immediately.
            sweepResumeAt_ = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(250);
        });
}

// Every 5 minutes, sweep the history of every watched channel again (same
// paths as the connect-time backfill). Catches messages a dropped gateway
// missed and keeps each channel's "last checked" stamp honest.
void DiscordBot::rescanWorker() {
    constexpr int kIntervalSec = 300;
    constexpr int kManualCommandPollSec = 5;
    int elapsed = 0;
    int manualElapsed = 0;
    while (!rescanStop_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (rescanStop_) break;
        drainSweepMessages();
        const bool imageWake = resumeTicketImages_.exchange(false);
        const bool notifyWake = resumeNotifyCards_.exchange(false);
        const bool manualWake = resumeManualCaptures_.exchange(false);
        const bool manualPoll = ++manualElapsed >= kManualCommandPollSec;
        if (manualPoll) manualElapsed = 0;
        // Commands/effects and card outbox claims stay responsive even when a
        // queued-image pass performs synchronous Discord downloads.
        if (notifyWake || imageWake || manualPoll)
            resumePendingNotifyCards(db_, this);
        if (manualWake || imageWake || manualPoll)
            pumpManualCapture();
        if (imageWake && !rescanStop_ && lockCluster())
            resumeQueuedTicketImages(db_, this, /*recoverySweep=*/false);
        if (++elapsed < kIntervalSec) continue;
        elapsed = 0;
        if (!lockCluster()) continue;
        // Also retry bounded queued work during the normal five-minute
        // recovery sweep in case a transient REST/CDN error did not require a
        // full gateway reconnect.
        resumePendingNotifyCards(db_, this);
        pumpManualCapture();
        resumeQueuedTicketImages(db_, this, /*recoverySweep=*/true);
        // Reaction adds are idempotent. This also recovers cards whose
        // bounded Discord REST retries failed without requiring a reconnect.
        recoverPendingReviewReactions();
        std::set<uint64_t> guilds;
        {
            std::lock_guard<std::mutex> lk(mu_);
            guilds = seenGuilds_;
        }
        for (uint64_t gid : guilds) {
            if (rescanStop_ || !lockCluster()) break;
            if (dpp::guild* g = dpp::find_guild(gid)) backfillGuild(g);
        }
    }
}

} // namespace devhub
