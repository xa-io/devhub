// Keep the D++ bundled JSON ABI separate from Db.h / application JSON.
#include "DiscordBot.h"
#include "DiscordAccess.h"
#include "Ingest.h"
#include "devhub/DiscordRetry.h"
#include "devhub/Util.h"

#include <dpp/cluster.h>
#include <dpp/discordclient.h>
#include <dpp/json.h>
#include <dpp/utility.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace devhub {
namespace {

struct AccessResponse {
    bool ok = false;
    bool stopped = false;
    bool retryable = false;
    unsigned status = 0;
    dpp::json body;
};

// D++ owns rate limiting and HTTP timeouts. Only this dedicated worker waits;
// callbacks retain the promise, never the bot or DB. Stop remains responsive.
AccessResponse accessRequest(const std::shared_ptr<dpp::cluster>& cluster,
                             const std::atomic<bool>& stop,
                             const std::string& channel,
                             const std::string& path, dpp::http_method method) {
    if (stop) { AccessResponse result; result.stopped = true; return result; }
    auto promise = std::make_shared<std::promise<AccessResponse>>();
    auto future = promise->get_future();
    cluster->post_rest(API_PATH "/channels", channel, path, method, "",
        [promise](dpp::json& body, const dpp::http_request_completion_t& http) {
            AccessResponse result;
            result.status = http.status;
            result.ok = http.error == dpp::h_success &&
                        http.status >= 200 && http.status < 300;
            result.retryable = http.error != dpp::h_success ||
                               isRetryableDiscordHttpStatus(http.status);
            result.body = body;
            promise->set_value(std::move(result));
        });
    while (future.wait_for(std::chrono::milliseconds(100)) !=
           std::future_status::ready) {
        if (stop) { AccessResponse result; result.stopped = true; return result; }
    }
    return future.get();
}

std::string accessText(const dpp::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : "";
}

std::uint64_t accessId(const std::string& text) {
    std::uint64_t value = 0;
    parseDiscordAccessSnowflake(text, value);
    return value;
}

std::vector<std::string> accessFilenames(const dpp::json& message) {
    std::vector<std::string> names;
    const auto it = message.find("attachments");
    if (it == message.end() || !it->is_array()) return names;
    for (const auto& attachment : *it) {
        if (!attachment.is_object() || attachment.value("ephemeral", false))
            continue;
        const std::string name = accessText(attachment, "filename");
        if (!name.empty()) names.push_back(name);
    }
    return names;
}

bool accessMessageMatches(Db* db, const DiscordAccessRule& rule,
                          const dpp::json& message) {
    if (!message.is_object() || !accessId(accessText(message, "id")) ||
        accessText(message, "channel_id") != rule.channelId ||
        !accessText(message, "webhook_id").empty()) return false;
    const auto author = message.find("author");
    if (author == message.end() || !author->is_object() ||
        author->value("bot", false) ||
        !discordAdminUserAllowed(db, accessText(*author, "id"))) return false;
    for (const auto& filename : accessFilenames(message))
        if (discordAccessFilenameMatches(filename, rule.filenamePrefix))
            return true;
    return false;
}

std::string accessFailure(const char* stage, const AccessResponse& response) {
    // Do not retain raw Discord responses, tokens, message text or signed URLs.
    std::string result = std::string(stage) + " failed (HTTP " +
                         std::to_string(response.status) + ").";
    if (response.status == 403)
        result += " Check View Channel, Read Message History and Pin Messages permissions.";
    else if (response.status == 404)
        result += " The channel or message is no longer available.";
    else if (response.status == 400)
        result += " Check the channel pin limit and request permissions.";
    else if (response.retryable)
        result += " Discord is temporarily unavailable; retry is scheduled.";
    return result;
}

struct AccessScanProgress {
    std::uint64_t revision = 0;
    std::uint64_t floor = 0;
    std::uint64_t newest = 0;
    std::string before;
    std::string guildId;
};

} // namespace

void DiscordBot::accessManagementWorker() {
    std::map<std::string, AccessScanProgress> scans;
    std::map<std::string, std::chrono::steady_clock::time_point> nextPoll;
    std::map<std::string, std::string> observedWork;
    while (!rescanStop_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (rescanStop_) break;
        const auto cluster = lockCluster();
        if (!cluster || status().state != "connected") continue;
        const auto rules = discordAccessRules(db_);
        std::set<std::string> configured;
        for (const auto& rule : rules) configured.insert(rule.channelId);
        // Rules are capped at 100; removing them also releases worker caches.
        for (auto it = scans.begin(); it != scans.end();)
            if (!configured.count(it->first)) it = scans.erase(it); else ++it;
        for (auto it = nextPoll.begin(); it != nextPoll.end();)
            if (!configured.count(it->first)) it = nextPoll.erase(it); else ++it;
        for (auto it = observedWork.begin(); it != observedWork.end();)
            if (!configured.count(it->first)) it = observedWork.erase(it); else ++it;

        for (const auto& rule : rules) {
            if (rescanStop_) break;
            if (!rule.enabled) { scans.erase(rule.channelId); continue; }
            const auto now = std::chrono::steady_clock::now();
            const std::string identity = std::to_string(rule.revision) + ":" +
                                        rule.latestMessageId + ":" + rule.status;
            const bool changed = observedWork[rule.channelId] != identity;
            if (!changed && now < nextPoll[rule.channelId]) continue;
            observedWork[rule.channelId] = identity;
            nextPoll[rule.channelId] = now + std::chrono::seconds(30);
            std::optional<DiscordAccessRule> activeWork;
            try {
                auto currentConfig = [&]() {
                    return !rescanStop_ && discordAccessConfigCurrent(db_, rule);
                };
                if (!currentConfig()) continue;
                auto& scan = scans[rule.channelId];
                if (scan.revision != rule.revision) {
                    scan = {};
                    scan.revision = rule.revision;
                    scan.floor = accessId(rule.scanAfterMessageId);
                    scan.newest = scan.floor;
                    scan.guildId = rule.latestGuildId;
                }
                if (scan.guildId.empty()) {
                    const auto channel = accessRequest(cluster, rescanStop_,
                        rule.channelId, "", dpp::m_get);
                    if (channel.stopped) break;
                    if (!currentConfig()) continue;
                    if (!channel.ok) {
                        recordDiscordAccessScanError(db_, rule,
                            accessFailure("Read channel", channel));
                        continue;
                    }
                    scan.guildId = accessText(channel.body, "guild_id");
                    if (!accessId(scan.guildId)) {
                        recordDiscordAccessScanError(db_, rule,
                            "Choose a server channel; direct messages are not supported.");
                        continue;
                    }
                }

                bool scanComplete = false;
                bool scanFailed = false;
                // Search newest first. Never advance the durable high-water
                // until this window is exhausted or its newest match is found.
                // A long backlog continues next pass; restart re-reads safely.
                for (unsigned page = 0; page < 10 && currentConfig(); ++page) {
                    std::string path = "messages?limit=100";
                    if (!scan.before.empty()) path += "&before=" + scan.before;
                    const auto response = accessRequest(cluster, rescanStop_,
                        rule.channelId, path, dpp::m_get);
                    if (response.stopped) break;
                    if (!currentConfig()) break;
                    if (!response.ok) {
                        recordDiscordAccessScanError(db_, rule,
                            accessFailure("Read missed uploads", response));
                        scanFailed = true;
                        break;
                    }
                    if (!response.body.is_array() || response.body.size() > 100)
                        throw std::runtime_error("Unexpected message history response.");
                    std::vector<dpp::json> messages;
                    for (const auto& message : response.body) messages.push_back(message);
                    std::sort(messages.begin(), messages.end(), [](const auto& a, const auto& b) {
                        return accessId(accessText(a, "id")) > accessId(accessText(b, "id"));
                    });
                    std::uint64_t oldest = (std::numeric_limits<std::uint64_t>::max)();
                    for (const auto& message : messages) {
                        const auto id = accessId(accessText(message, "id"));
                        if (!id) throw std::runtime_error("Message history contained an invalid ID.");
                        if (!scan.before.empty() && id >= accessId(scan.before))
                            throw std::runtime_error("Message history pagination did not advance.");
                        oldest = (std::min)(oldest, id);
                        scan.newest = (std::max)(scan.newest, id);
                        if (id <= scan.floor) { scanComplete = true; break; }
                        if (accessMessageMatches(db_, rule, message)) {
                            std::string error;
                            queueDiscordAccessUpload(db_, rule.channelId, scan.guildId,
                                accessText(message, "id"), accessText(message["author"], "id"),
                                accessFilenames(message), &error, rule.revision);
                            if (!error.empty()) throw std::runtime_error(error);
                            scanComplete = true;
                            break;
                        }
                    }
                    if (messages.size() < 100) scanComplete = true;
                    if (scanComplete) break;
                    scan.before = std::to_string(oldest);
                }
                if (rescanStop_) break;
                if (!currentConfig() || scanFailed) continue;
                if (!scanComplete) {
                    recordDiscordAccessScanError(db_, rule,
                        "Catching up on missed posts; continuing with the next history page.");
                    nextPoll[rule.channelId] = now + std::chrono::seconds(1);
                    continue;
                }
                advanceDiscordAccessScan(db_, rule, std::to_string(scan.newest));
                recordDiscordAccessScanError(db_, rule, "");
                scans.erase(rule.channelId);

                // Refresh after catch-up: a live upload can supersede any page.
                DiscordAccessRule work;
                bool due = false;
                for (const auto& candidate : dueDiscordAccessRules(db_)) {
                    if (candidate.channelId == rule.channelId &&
                        candidate.revision == rule.revision) {
                        work = candidate; due = true; break;
                    }
                }
                if (!due) continue;
                activeWork = work;
                auto currentWork = [&]() {
                    return !rescanStop_ && discordAccessWorkCurrent(db_, work);
                };
                auto fail = [&](const char* stage, const AccessResponse& response) {
                    if (!response.stopped)
                        finishDiscordAccessWork(db_, work, false,
                            accessFailure(stage, response), response.retryable);
                };
                if (!currentWork()) {
                    finishDiscordAccessWork(db_, work, false,
                        "The upload author is no longer in the admin/developer whitelist.");
                    continue;
                }
                auto latest = accessRequest(cluster, rescanStop_, work.channelId,
                    "messages/" + work.latestMessageId, dpp::m_get);
                if (latest.stopped) break;
                if (!latest.ok) { fail("Read release", latest); continue; }
                if (!currentWork()) continue;
                if (accessText(latest.body, "id") != work.latestMessageId ||
                    !accessMessageMatches(db_, work, latest.body)) {
                    finishDiscordAccessWork(db_, work, false,
                        "The latest release no longer has an authorized matching ZIP attachment.");
                    continue;
                }

                std::vector<std::string> olderPins;
                std::string before;
                bool pinsComplete = false;
                bool superseded = false;
                bool pinsFailed = false;
                bool latestPinned = false;
                // The typed D++ pin map drops has_more and truncates timestamps
                // to seconds. Preserve raw ISO cursor precision via post_rest.
                for (unsigned page = 0; page < 20 && currentWork(); ++page) {
                    std::string path = "messages/pins?limit=50";
                    if (!before.empty()) path += "&before=" + dpp::utility::url_encode(before);
                    const auto pins = accessRequest(cluster, rescanStop_,
                        work.channelId, path, dpp::m_get);
                    if (pins.stopped) break;
                    if (!pins.ok) { fail("Read pins", pins); pinsFailed = true; break; }
                    if (!currentWork()) break;
                    if (!pins.body.is_object() || !pins.body.contains("items") ||
                        !pins.body["items"].is_array() || pins.body["items"].size() > 50 ||
                        !pins.body.contains("has_more") || !pins.body["has_more"].is_boolean())
                        throw std::runtime_error("Unexpected pin list response.");
                    const auto& items = pins.body["items"];
                    for (const auto& pin : items) {
                        if (!pin.is_object() || !pin.contains("message"))
                            throw std::runtime_error("Pin list contained an invalid entry.");
                        const auto& message = pin["message"];
                        if (!accessMessageMatches(db_, work, message)) continue;
                        const std::string id = accessText(message, "id");
                        if (accessId(id) > accessId(work.latestMessageId)) {
                            queueDiscordAccessUpload(db_, work.channelId, work.latestGuildId,
                                id, accessText(message["author"], "id"),
                                accessFilenames(message), nullptr, work.revision);
                            superseded = true;
                        } else if (id == work.latestMessageId) latestPinned = true;
                        else olderPins.push_back(id);
                    }
                    if (superseded) break;
                    if (!pins.body["has_more"].get<bool>()) { pinsComplete = true; break; }
                    const std::string cursor = items.empty() ? "" :
                        accessText(items.back(), "pinned_at");
                    if (cursor.empty() || cursor == before)
                        throw std::runtime_error("Pin pagination did not advance.");
                    before = cursor;
                }
                if (rescanStop_) break;
                if (superseded || pinsFailed || !currentWork()) continue;
                if (!pinsComplete) {
                    finishDiscordAccessWork(db_, work, false,
                        "The pin list exceeded the safe scan limit; no pins were changed.");
                    continue;
                }
                if (!latestPinned) {
                    const auto pinned = accessRequest(cluster, rescanStop_, work.channelId,
                        "messages/pins/" + work.latestMessageId, dpp::m_put);
                    if (pinned.stopped) break;
                    if (!pinned.ok) { fail("Pin latest release", pinned); continue; }
                }
                if (!currentWork()) continue;
                std::sort(olderPins.begin(), olderPins.end());
                olderPins.erase(std::unique(olderPins.begin(), olderPins.end()), olderPins.end());
                bool cleanupFailed = false;
                for (const auto& oldId : olderPins) {
                    if (!currentWork()) { cleanupFailed = true; break; }
                    // Revalidate both ends immediately before each removal:
                    // edits, permission revocation and manual unpins fail safe.
                    latest = accessRequest(cluster, rescanStop_, work.channelId,
                        "messages/" + work.latestMessageId, dpp::m_get);
                    if (latest.stopped) { cleanupFailed = true; break; }
                    if (!latest.ok) { fail("Verify latest pin", latest); cleanupFailed = true; break; }
                    if (!currentWork()) { cleanupFailed = true; break; }
                    if (!latest.body.value("pinned", false) ||
                        !accessMessageMatches(db_, work, latest.body)) {
                        finishDiscordAccessWork(db_, work, false,
                            "The replacement pin changed; older pins were retained.");
                        cleanupFailed = true; break;
                    }
                    const auto old = accessRequest(cluster, rescanStop_, work.channelId,
                        "messages/" + oldId, dpp::m_get);
                    if (old.stopped) { cleanupFailed = true; break; }
                    if (!old.ok) {
                        if (old.status == 404 && old.body.is_object() &&
                            old.body.value("code", 0) == 10008) continue;
                        fail("Verify previous release", old); cleanupFailed = true; break;
                    }
                    if (!currentWork()) { cleanupFailed = true; break; }
                    if (accessText(old.body, "id") != oldId ||
                        !accessMessageMatches(db_, work, old.body) ||
                        !old.body.value("pinned", false)) continue;
                    const auto removed = accessRequest(cluster, rescanStop_, work.channelId,
                        "messages/pins/" + oldId, dpp::m_delete);
                    if (removed.stopped) { cleanupFailed = true; break; }
                    if (!removed.ok) { fail("Unpin previous release", removed); cleanupFailed = true; break; }
                }
                if (!cleanupFailed && currentWork())
                    finishDiscordAccessWork(db_, work, true);
            } catch (const std::exception&) {
                // Fail visibly without persisting remote payloads or exception
                // strings that could contain private message data.
                recordDiscordAccessScanError(db_, rule,
                    "Access Management could not finish this pass; check the channel and retry.");
                if (activeWork)
                    finishDiscordAccessWork(db_, *activeWork, false,
                        "Access Management received an unexpected response or local error.", true);
            }
        }
    }
}

} // namespace devhub
