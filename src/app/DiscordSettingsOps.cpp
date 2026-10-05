#include "Ingest.h"
#include "Db.h"
#include "devhub/Util.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

namespace devhub {

bool parseDiscordAdminUserIds(const std::string& value,
                              std::vector<std::string>& userIds,
                              std::string* error) {
    userIds.clear();
    if (error) error->clear();

    std::string token;
    auto flush = [&]() {
        if (token.empty()) return true;
        std::uint64_t parsed = 0;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end || parsed == 0)
            return false;
        std::string canonical = std::to_string(parsed);
        if (std::find(userIds.begin(), userIds.end(), canonical) == userIds.end())
            userIds.push_back(std::move(canonical));
        token.clear();
        return true;
    };

    for (unsigned char ch : value) {
        if (ch == ',' || ch == ';' || ch <= ' ') {
            if (!flush()) {
                userIds.clear();
                if (error)
                    *error = "use positive decimal Discord user IDs only";
                return false;
            }
        } else {
            token.push_back(static_cast<char>(ch));
        }
    }
    if (!flush() || (!trim(value).empty() && userIds.empty())) {
        userIds.clear();
        if (error)
            *error = "use positive decimal Discord user IDs separated by commas, semicolons, or spaces";
        return false;
    }
    return true;
}

bool discordAdminUserAllowed(Db* db, const std::string& userId) {
    if (!db || userId.empty()) return false;
    std::string configured;
    {
        auto lk = db->guard();
        configured = db->getSetting(lk.token(), "discord_admin_user_ids");
    }
    std::vector<std::string> userIds;
    if (!parseDiscordAdminUserIds(configured, userIds)) return false;
    return std::find(userIds.begin(), userIds.end(), userId) != userIds.end();
}

bool discordGuildEnabled(Db* db, const std::string& guildId) {
    if (guildId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT enabled FROM discord_guilds WHERE guild_id=?");
    q.bind(1, guildId);
    return q.executeStep() && q.getColumn(0).getInt() != 0;
}

bool discordChannelEnabled(Db* db, const std::string& channelId) {
    if (channelId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT enabled FROM discord_channels WHERE channel_id=?");
    q.bind(1, channelId);
    return q.executeStep() && q.getColumn(0).getInt() != 0;
}

bool discordWatch(Db* db, const std::string& guildId,
                  const std::string& channelId) {
    if (channelId.empty()) return false;
    auto lk = db->guard();
    SQLite::Statement channel(db->raw(lk.token()),
        "SELECT enabled FROM discord_channels WHERE channel_id=?");
    channel.bind(1, channelId);
    // Explicit channel choices always win, including disabled channels.
    if (channel.executeStep()) return channel.getColumn(0).getInt() != 0;
    if (db->getSetting(lk.token(), "discord_auto_add_channels") == "0" ||
        guildId.empty()) return false;
    SQLite::Statement guild(db->raw(lk.token()),
        "SELECT enabled FROM discord_guilds WHERE guild_id=?");
    guild.bind(1, guildId);
    return guild.executeStep() && guild.getColumn(0).getInt() != 0;
}

void discordSetChannelMetadata(Db* db, const std::string& channelId,
                               const std::string& name,
                               const std::string& guildId,
                               const std::string& guildName) {
    if (channelId.empty() || name.empty()) return;
    auto lk = db->guard();
    // Metadata refresh never adds or enables a monitor.
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_channels SET channel_name=?,"
        "guild_id=CASE WHEN ?='' THEN guild_id ELSE ? END,"
        "guild_name=CASE WHEN ?='' THEN guild_name ELSE ? END "
        "WHERE channel_id=?");
    up.bind(1, name);
    up.bind(2, guildId);
    up.bind(3, guildId);
    up.bind(4, guildName);
    up.bind(5, guildName);
    up.bind(6, channelId);
    up.exec();
}

std::string discordChannelLastRead(Db* db, const std::string& channelId) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT last_read_ts FROM discord_channels WHERE channel_id=?");
    q.bind(1, channelId);
    if (q.executeStep()) return q.getColumn(0).getString();
    return {};
}

std::string discordNextUnnamedChannel(Db* db, long long& afterRowId) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT id,channel_id FROM discord_channels WHERE id>? "
        "AND (channel_name='' OR channel_name=channel_id) ORDER BY id LIMIT 1");
    q.bind(1, afterRowId);
    if (!q.executeStep()) return {};
    afterRowId = q.getColumn(0).getInt64();
    return q.getColumn(1).getString();
}

void discordSetGuildName(Db* db, const std::string& guildId,
                         const std::string& name) {
    auto lk = db->guard();
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_guilds SET guild_name=? WHERE guild_id=?");
    up.bind(1, name);
    up.bind(2, guildId);
    up.exec();
}

void discordMarkChannelChecked(Db* db, const std::string& channelId) {
    if (channelId.empty()) return;
    auto lk = db->guard();
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE discord_channels SET last_scan_at=? WHERE channel_id=?");
    up.bind(1, nowIsoUtc());
    up.bind(2, channelId);
    up.exec();
}

} // namespace devhub
