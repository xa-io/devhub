#include "DiscordAccess.h"
#include "Db.h"
#include "Ingest.h"
#include "devhub/Util.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace devhub {
namespace {

using json = nlohmann::json;
constexpr const char* kSettingsKey = "discord_access_management";
constexpr std::size_t kMaxRules = 100;
constexpr std::size_t kMaxSettingsBytes = 512 * 1024;
constexpr unsigned kMaxAttempts = 5;

struct AccessDocument {
    std::uint64_t nextRevision = 1;
    std::vector<DiscordAccessRule> rules;
};

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

char folded(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

bool validPrefix(const std::string& prefix) {
    return !prefix.empty() && prefix.size() <= 200 && prefix == trim(prefix) &&
           std::none_of(prefix.begin(), prefix.end(), [](unsigned char ch) {
               return ch < 32 || ch == 127 || ch == '/' || ch == '\\' ||
                      ch == '*' || ch == '?' || ch == '[' || ch == ']';
           });
}

bool validTimestamp(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19)
            continue;
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

bool validId(const std::string& value, bool allowEmpty = false) {
    if (value.empty()) return allowEmpty;
    std::uint64_t parsed = 0;
    return parseDiscordAccessSnowflake(value, parsed) &&
           std::to_string(parsed) == value;
}

std::string activationWatermark() {
    constexpr std::int64_t kDiscordEpochMs = 1420070400000LL;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (elapsed <= kDiscordEpochMs)
        throw std::runtime_error("The system clock predates Discord; correct it before enabling Access Management");
    const auto millis = static_cast<std::uint64_t>(elapsed - kDiscordEpochMs);
    if (millis > ((std::numeric_limits<std::uint64_t>::max)() >> 22))
        throw std::runtime_error("The system clock cannot produce a Discord activation watermark");
    // Exclude all snowflakes from the activation millisecond, including a post
    // created just before the operator enabled the rule in that same tick.
    return std::to_string((millis << 22) | ((1ULL << 22) - 1));
}

std::string retryTime(unsigned attempts) {
    static constexpr int delays[] = {5, 15, 60, 300};
    const auto index = std::min<std::size_t>(attempts - 1, 3);
    const auto when = std::chrono::system_clock::now() +
                      std::chrono::seconds(delays[index]);
    const std::time_t raw = std::chrono::system_clock::to_time_t(when);
    std::tm utc{};
    if (gmtime_s(&utc, &raw) != 0)
        throw std::runtime_error("Could not compute Access Management retry time");
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        throw std::runtime_error("Could not format Access Management retry time");
    return buffer;
}

AccessDocument readDocument(Db* db, Db::Held held) {
    const std::string value = db->getSetting(held, kSettingsKey);
    if (value.empty()) return {};
    if (value.size() > kMaxSettingsBytes)
        throw std::runtime_error("Access Management settings exceed their size limit; saved data was preserved");
    const json root = json::parse(value);
    if (!root.is_object() || !root.contains("version") ||
        !root["version"].is_number_integer() || root["version"] != 1 ||
        !root.contains("next_revision") || !root["next_revision"].is_number_unsigned() ||
        !root.contains("rules") || !root["rules"].is_array() ||
        root["rules"].size() > kMaxRules)
        throw std::runtime_error("Access Management settings have an unsupported structure; saved data was preserved");
    AccessDocument document;
    document.nextRevision = root["next_revision"].get<std::uint64_t>();
    if (document.nextRevision == 0)
        throw std::runtime_error("Access Management revision is invalid; saved data was preserved");
    std::set<std::string> channels;
    for (const auto& row : root["rules"]) {
        if (!row.is_object())
            throw std::runtime_error("Access Management rule is invalid; saved data was preserved");
        DiscordAccessRule rule;
        rule.channelId = row.at("channel_id").get<std::string>();
        rule.filenamePrefix = row.at("filename_prefix").get<std::string>();
        rule.enabled = row.at("enabled").get<bool>();
        if (!row.at("revision").is_number_unsigned() ||
            !row.at("attempts").is_number_unsigned())
            throw std::runtime_error("Access Management rule counters are invalid; saved data was preserved");
        rule.revision = row.at("revision").get<std::uint64_t>();
        const auto attempts = row.at("attempts").get<std::uint64_t>();
        if (attempts > kMaxAttempts)
            throw std::runtime_error("Access Management retry count is invalid; saved data was preserved");
        rule.attempts = static_cast<unsigned>(attempts);
        rule.watchAfterMessageId = row.at("watch_after_message_id").get<std::string>();
        rule.scanAfterMessageId = row.at("scan_after_message_id").get<std::string>();
        rule.latestGuildId = row.at("latest_guild_id").get<std::string>();
        rule.latestMessageId = row.at("latest_message_id").get<std::string>();
        rule.latestAuthorId = row.at("latest_author_id").get<std::string>();
        rule.latestFilename = row.at("latest_filename").get<std::string>();
        rule.pinnedMessageId = row.at("pinned_message_id").get<std::string>();
        rule.lastSuccessAt = row.at("last_success_at").get<std::string>();
        rule.status = row.at("status").get<std::string>();
        rule.lastError = row.at("last_error").get<std::string>();
        rule.lastScanError = row.at("last_scan_error").get<std::string>();
        rule.nextRetryAt = row.at("next_retry_at").get<std::string>();
        const bool hasCandidate = !rule.latestMessageId.empty();
        const bool knownStatus = rule.status == "waiting" || rule.status == "pending" ||
            rule.status == "retry" || rule.status == "failed" || rule.status == "synced";
        std::uint64_t baseline = 0, cursor = 0, candidate = 0;
        parseDiscordAccessSnowflake(rule.watchAfterMessageId, baseline);
        parseDiscordAccessSnowflake(rule.scanAfterMessageId, cursor);
        parseDiscordAccessSnowflake(rule.latestMessageId, candidate);
        if (!validId(rule.channelId) || !validPrefix(rule.filenamePrefix) ||
            rule.revision == 0 || rule.revision >= document.nextRevision ||
            !channels.insert(rule.channelId).second ||
            !validId(rule.watchAfterMessageId) || !validId(rule.scanAfterMessageId) ||
            cursor < baseline || !validId(rule.latestGuildId, !hasCandidate) ||
            !validId(rule.latestMessageId, true) ||
            !validId(rule.latestAuthorId, !hasCandidate) ||
            !validId(rule.pinnedMessageId, true) || !knownStatus ||
            !validTimestamp(rule.lastSuccessAt) || !validTimestamp(rule.nextRetryAt) ||
            rule.lastError.size() > 1024 || rule.lastScanError.size() > 1024 ||
            rule.latestFilename.size() > 1024 ||
            (hasCandidate && (candidate <= baseline ||
                !discordAccessFilenameMatches(rule.latestFilename, rule.filenamePrefix))) ||
            (!hasCandidate && (rule.status != "waiting" ||
                !rule.latestAuthorId.empty() || !rule.latestGuildId.empty() ||
                !rule.latestFilename.empty())) ||
            (rule.status == "retry" && (rule.nextRetryAt.empty() || rule.attempts == 0)) ||
            (rule.status == "synced" && rule.pinnedMessageId != rule.latestMessageId))
            throw std::runtime_error("Access Management rule validation failed; saved data was preserved");
        document.rules.push_back(std::move(rule));
    }
    return document;
}

void writeDocument(Db* db, Db::Held held, const AccessDocument& document) {
    json rows = json::array();
    for (const auto& rule : document.rules) {
        rows.push_back({{"channel_id", rule.channelId},
            {"filename_prefix", rule.filenamePrefix}, {"enabled", rule.enabled},
            {"revision", rule.revision}, {"watch_after_message_id", rule.watchAfterMessageId},
            {"scan_after_message_id", rule.scanAfterMessageId},
            {"latest_guild_id", rule.latestGuildId}, {"latest_message_id", rule.latestMessageId},
            {"latest_author_id", rule.latestAuthorId}, {"latest_filename", rule.latestFilename},
            {"pinned_message_id", rule.pinnedMessageId}, {"last_success_at", rule.lastSuccessAt},
            {"status", rule.status}, {"last_error", rule.lastError},
            {"last_scan_error", rule.lastScanError},
            {"next_retry_at", rule.nextRetryAt}, {"attempts", rule.attempts}});
    }
    const std::string encoded = json{{"version", 1},
        {"next_revision", document.nextRevision}, {"rules", std::move(rows)}}.dump();
    if (encoded.size() > kMaxSettingsBytes)
        throw std::runtime_error("Access Management settings exceed their size limit");
    db->setSetting(held, kSettingsKey, encoded);
}

std::uint64_t nextRevision(AccessDocument& document) {
    if (document.nextRevision == (std::numeric_limits<std::uint64_t>::max)())
        throw std::runtime_error("Access Management revision limit reached");
    return document.nextRevision++;
}

auto findRule(AccessDocument& document, const std::string& channelId) {
    return std::find_if(document.rules.begin(), document.rules.end(),
        [&](const DiscordAccessRule& rule) { return rule.channelId == channelId; });
}

bool sameConfiguration(const DiscordAccessRule& current, const DiscordAccessRule& work) {
    return current.enabled && current.channelId == work.channelId &&
           current.revision == work.revision &&
           current.filenamePrefix == work.filenamePrefix;
}

bool sameWork(const DiscordAccessRule& current, const DiscordAccessRule& work) {
    return sameConfiguration(current, work) && !work.latestMessageId.empty() &&
           current.latestMessageId == work.latestMessageId &&
           current.latestAuthorId == work.latestAuthorId &&
           current.latestFilename == work.latestFilename &&
           current.attempts == work.attempts &&
           (current.status == "pending" || current.status == "retry");
}

template <typename Fn>
bool mutate(Db* db, std::string* error, Fn&& fn) {
    if (error) error->clear();
    if (!db) return fail(error, "Access Management database is unavailable");
    try {
        auto guard = db->guard();
        AccessDocument document = readDocument(db, guard.token());
        if (!fn(document)) return false;
        writeDocument(db, guard.token(), document);
        return true;
    } catch (const json::exception&) {
        return fail(error, "Access Management settings are malformed; saved data was preserved");
    } catch (const std::exception& ex) {
        return fail(error, redactHttpUrls(ex.what()).substr(0, 1024));
    }
}

} // namespace

bool parseDiscordAccessSnowflake(std::string_view value, std::uint64_t& parsed) {
    parsed = 0;
    if (value.empty() || value.size() > 20 ||
        std::any_of(value.begin(), value.end(), [](char ch) { return ch < '0' || ch > '9'; }))
        return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed != 0)
        return true;
    parsed = 0;
    return false;
}

bool discordAccessFilenameMatches(std::string_view filename, std::string_view prefix) {
    if (prefix.empty() || filename.size() < prefix.size() || filename.size() < 4 ||
        filename.size() > 1024 ||
        std::any_of(filename.begin(), filename.end(), [](unsigned char ch) {
            return ch < 32 || ch == 127 || ch == '/' || ch == '\\';
        })) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (folded(filename[i]) != folded(prefix[i])) return false;
    const auto extension = filename.substr(filename.size() - 4);
    return folded(extension[0]) == '.' && folded(extension[1]) == 'z' &&
           folded(extension[2]) == 'i' && folded(extension[3]) == 'p';
}

std::vector<DiscordAccessRule> discordAccessRules(Db* db, std::string* error) {
    if (error) error->clear();
    if (!db) { fail(error, "Access Management database is unavailable"); return {}; }
    try {
        auto guard = db->guard();
        return readDocument(db, guard.token()).rules;
    } catch (const json::exception&) {
        fail(error, "Access Management settings are malformed; saved data was preserved");
    } catch (const std::exception& ex) {
        fail(error, redactHttpUrls(ex.what()).substr(0, 1024));
    }
    return {};
}

bool saveDiscordAccessRule(Db* db, const std::string& channelId,
                           const std::string& filenamePrefix, bool enabled,
                           std::uint64_t expectedRevision, std::string* error) {
    std::uint64_t channel = 0;
    const std::string prefix = trim(filenamePrefix);
    const std::string enteredId = trim(channelId);
    if (!parseDiscordAccessSnowflake(enteredId, channel))
        return fail(error, "Enter a positive decimal Discord channel ID");
    if (!validPrefix(prefix))
        return fail(error, "Enter a literal filename prefix of 1-200 characters without paths, wildcards or control characters");
    const std::string canonicalId = std::to_string(channel);
    return mutate(db, error, [&](AccessDocument& document) {
        auto found = findRule(document, canonicalId);
        if (found == document.rules.end()) {
            if (expectedRevision != 0)
                return fail(error, "This access rule changed or was removed; reload it before saving");
            if (document.rules.size() >= kMaxRules)
                return fail(error, "Access Management supports up to 100 channel rules");
            DiscordAccessRule rule;
            rule.channelId = canonicalId;
            document.rules.push_back(std::move(rule));
            found = std::prev(document.rules.end());
        } else {
            if (expectedRevision != found->revision)
                return fail(error, "This access rule changed; reload it before saving");
            if (found->filenamePrefix == prefix && found->enabled == enabled)
                return true;
        }
        found->filenamePrefix = prefix;
        found->enabled = enabled;
        found->revision = nextRevision(document);
        found->watchAfterMessageId = activationWatermark();
        found->scanAfterMessageId = found->watchAfterMessageId;
        found->latestGuildId.clear();
        found->latestMessageId.clear();
        found->latestAuthorId.clear();
        found->latestFilename.clear();
        found->status = "waiting";
        found->lastError.clear();
        found->lastScanError.clear();
        found->nextRetryAt.clear();
        found->attempts = 0;
        return true;
    });
}

bool removeDiscordAccessRule(Db* db, const std::string& channelId,
                             std::uint64_t expectedRevision, std::string* error) {
    return mutate(db, error, [&](AccessDocument& document) {
        const auto found = findRule(document, channelId);
        if (found == document.rules.end() || found->revision != expectedRevision)
            return fail(error, "This access rule changed or was removed; reload it before removing");
        document.rules.erase(found);
        return true;
    });
}

bool retryDiscordAccessRule(Db* db, const std::string& channelId,
                            std::uint64_t expectedRevision, std::string* error) {
    return mutate(db, error, [&](AccessDocument& document) {
        const auto found = findRule(document, channelId);
        if (found == document.rules.end() || found->revision != expectedRevision)
            return fail(error, "This access rule changed or was removed; reload it before retrying");
        if (!found->enabled || found->latestMessageId.empty() ||
            (found->status != "failed" && found->status != "retry"))
            return fail(error, "Only an enabled rule with a failed or waiting retry can be retried");
        if (!discordAdminUserAllowed(db, found->latestAuthorId))
            return fail(error, "The upload author is no longer on the admin/developer whitelist");
        found->revision = nextRevision(document);
        found->status = "pending";
        found->attempts = 0;
        found->lastError.clear();
        found->nextRetryAt.clear();
        return true;
    });
}

bool queueDiscordAccessUpload(Db* db, const std::string& channelId,
                              const std::string& guildId, const std::string& messageId,
                              const std::string& authorId,
                              const std::vector<std::string>& filenames,
                              std::string* error, std::uint64_t expectedRevision) {
    if (error) error->clear();
    if (!validId(channelId) || !validId(guildId) || !validId(messageId) ||
        !validId(authorId) || filenames.empty()) return false;
    return mutate(db, error, [&](AccessDocument& document) {
        const auto found = findRule(document, channelId);
        if (found == document.rules.end() || !found->enabled ||
            (expectedRevision != 0 && expectedRevision != found->revision) ||
            !discordAdminUserAllowed(db, authorId)) return false;
        std::uint64_t incoming = 0, previous = 0, baseline = 0;
        parseDiscordAccessSnowflake(messageId, incoming);
        parseDiscordAccessSnowflake(found->latestMessageId, previous);
        parseDiscordAccessSnowflake(found->watchAfterMessageId, baseline);
        if (incoming <= previous || incoming <= baseline) return false;
        const auto filename = std::find_if(filenames.begin(), filenames.end(),
            [&](const std::string& value) {
                return discordAccessFilenameMatches(value, found->filenamePrefix);
            });
        if (filename == filenames.end()) return false;
        found->latestGuildId = guildId;
        found->latestMessageId = messageId;
        found->latestAuthorId = authorId;
        found->latestFilename = *filename;
        found->status = "pending";
        found->lastError.clear();
        found->nextRetryAt.clear();
        found->attempts = 0;
        return true;
    });
}

std::vector<DiscordAccessRule> dueDiscordAccessRules(
    Db* db, const std::string& now, std::string* error) {
    auto rules = discordAccessRules(db, error);
    const std::string deadline = now.empty() ? nowIsoUtc() : now;
    rules.erase(std::remove_if(rules.begin(), rules.end(),
        [&](const DiscordAccessRule& rule) {
            return !rule.enabled || (rule.status != "pending" && rule.status != "retry") ||
                   (!rule.nextRetryAt.empty() && rule.nextRetryAt > deadline);
        }), rules.end());
    return rules;
}

bool discordAccessConfigCurrent(Db* db, const DiscordAccessRule& work, std::string* error) {
    const auto rules = discordAccessRules(db, error);
    return std::any_of(rules.begin(), rules.end(),
        [&](const DiscordAccessRule& rule) { return sameConfiguration(rule, work); });
}

bool advanceDiscordAccessScan(Db* db, const DiscordAccessRule& work,
                              const std::string& newestScannedMessageId,
                              std::string* error) {
    if (error) error->clear();
    std::uint64_t incoming = 0;
    if (!parseDiscordAccessSnowflake(newestScannedMessageId, incoming)) return false;
    return mutate(db, error, [&](AccessDocument& document) {
        const auto found = findRule(document, work.channelId);
        if (found == document.rules.end() || !sameConfiguration(*found, work)) return false;
        std::uint64_t previous = 0;
        parseDiscordAccessSnowflake(found->scanAfterMessageId, previous);
        if (incoming <= previous) return false;
        found->scanAfterMessageId = std::to_string(incoming);
        found->lastScanError.clear();
        return true;
    });
}

bool recordDiscordAccessScanError(Db* db, const DiscordAccessRule& work,
                                  const std::string& failure, std::string* error) {
    return mutate(db, error, [&](AccessDocument& document) {
        const auto found = findRule(document, work.channelId);
        if (found == document.rules.end() || !sameConfiguration(*found, work)) return false;
        const std::string safeFailure = redactHttpUrls(failure).substr(0, 1024);
        if (found->lastScanError == safeFailure) return false;
        found->lastScanError = safeFailure;
        return true;
    });
}

bool discordAccessWorkCurrent(Db* db, const DiscordAccessRule& work, std::string* error) {
    if (!db) return fail(error, "Access Management database is unavailable");
    // Keep policy and authorization in one guarded snapshot. The recursive DB
    // guard lets the established whitelist helper remain the single authority.
    auto guard = db->guard();
    const auto rules = discordAccessRules(db, error);
    return std::any_of(rules.begin(), rules.end(),
        [&](const DiscordAccessRule& rule) {
            return sameWork(rule, work) && discordAdminUserAllowed(db, rule.latestAuthorId);
        });
}

bool finishDiscordAccessWork(Db* db, const DiscordAccessRule& work,
                             bool success, const std::string& failure,
                             bool retryable, std::string* error) {
    return mutate(db, error, [&](AccessDocument& document) {
        const auto found = findRule(document, work.channelId);
        if (found == document.rules.end() || !sameWork(*found, work)) return false;
        if (success && !discordAdminUserAllowed(db, found->latestAuthorId)) return false;
        found->attempts = std::min(found->attempts + 1, kMaxAttempts);
        if (success) {
            found->pinnedMessageId = found->latestMessageId;
            found->lastSuccessAt = nowIsoUtc();
            found->status = "synced";
            found->lastError.clear();
            found->nextRetryAt.clear();
        } else {
            found->lastError = redactHttpUrls(failure.empty()
                ? "Discord pin update failed" : failure).substr(0, 1024);
            const bool retry = retryable && found->attempts < kMaxAttempts;
            found->status = retry ? "retry" : "failed";
            found->nextRetryAt = retry ? retryTime(found->attempts) : std::string();
        }
        return true;
    });
}

} // namespace devhub
