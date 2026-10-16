#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace devhub {

class Db;

// Plain values across DiscordBot.cpp's D++/nlohmann ABI boundary.
struct DiscordAccessRule {
    std::string channelId;
    std::string filenamePrefix;
    bool enabled = false;
    std::uint64_t revision = 0;
    std::string watchAfterMessageId;
    std::string scanAfterMessageId;
    std::string latestGuildId;
    std::string latestMessageId;
    std::string latestAuthorId;
    std::string latestFilename;
    std::string pinnedMessageId;
    std::string lastSuccessAt;
    std::string status = "waiting"; // waiting | pending | retry | failed | synced
    std::string lastError;
    std::string lastScanError;
    std::string nextRetryAt;
    unsigned attempts = 0;
};

bool parseDiscordAccessSnowflake(std::string_view value, std::uint64_t& parsed);
bool discordAccessFilenameMatches(std::string_view filename,
                                  std::string_view prefix);

// Reads fail closed on malformed stored policy. Mutations preserve that value
// and return an error instead of replacing it with an empty/default document.
std::vector<DiscordAccessRule> discordAccessRules(
    Db* db, std::string* error = nullptr);
bool saveDiscordAccessRule(Db* db, const std::string& channelId,
                           const std::string& filenamePrefix, bool enabled,
                           std::uint64_t expectedRevision,
                           std::string* error = nullptr);
bool removeDiscordAccessRule(Db* db, const std::string& channelId,
                             std::uint64_t expectedRevision,
                             std::string* error = nullptr);
bool retryDiscordAccessRule(Db* db, const std::string& channelId,
                            std::uint64_t expectedRevision,
                            std::string* error = nullptr);

// Live creates and bounded catch-up after the activation watermark share this
// path. History callers bind their configuration revision; edits are not intake.
bool queueDiscordAccessUpload(Db* db, const std::string& channelId,
                              const std::string& guildId,
                              const std::string& messageId,
                              const std::string& authorId,
                              const std::vector<std::string>& filenames,
                              std::string* error = nullptr,
                              std::uint64_t expectedRevision = 0);
bool discordAccessConfigCurrent(Db* db, const DiscordAccessRule& work,
                                std::string* error = nullptr);
bool advanceDiscordAccessScan(Db* db, const DiscordAccessRule& work,
                              const std::string& newestScannedMessageId,
                              std::string* error = nullptr);
bool recordDiscordAccessScanError(Db* db, const DiscordAccessRule& work,
                                  const std::string& failure,
                                  std::string* error = nullptr);
std::vector<DiscordAccessRule> dueDiscordAccessRules(
    Db* db, const std::string& now = {}, std::string* error = nullptr);
// Check before each REST stage and before committing a callback. Configuration
// revisions never repeat, including after remove/recreate or explicit retry.
bool discordAccessWorkCurrent(Db* db, const DiscordAccessRule& work,
                              std::string* error = nullptr);
bool finishDiscordAccessWork(Db* db, const DiscordAccessRule& work,
                             bool success, const std::string& failure = {},
                             bool retryable = false,
                             std::string* error = nullptr);

} // namespace devhub
