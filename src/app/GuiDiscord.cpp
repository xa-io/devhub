#include "GuiInternal.h"

namespace devhub {

static void drawDiscordBotSection(App& a, const DiscordBot::Status& bs) {
    if (bs.state == "connected") {
        ImGui::TextColored(C_GREEN, "connected");
        ImGui::SameLine();
        ImGui::Text("as %s", bs.botUser.c_str());
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "- sees %d server(s)", bs.guilds);
    } else if (bs.state == "connecting") {
        ImGui::TextColored(C_ORANGE, "connecting...");
    } else {
        ImGui::TextColored(C_DIM, "offline");
    }
    ImGui::SameLine(0, 20);
    if (bs.state == "offline") {
        if (ImGui::SmallButton("Connect")) {
            std::string token;
            {
                auto lk = a.db->guard();
                token = a.db->getSetting(lk.token(), "discord_bot_token");
            }
            if (trim(token).empty()) toast(a, "save a bot token first", true);
            else { a.discord->start(token); toast(a, "connecting..."); }
        }
    } else {
        if (ImGui::SmallButton("Disconnect")) a.discord->stop();
        ImGui::SameLine();
        if (ImGui::SmallButton("Reconnect")) {
            std::string token;
            {
                auto lk = a.db->guard();
                token = a.db->getSetting(lk.token(), "discord_bot_token");
            }
            a.discord->start(token);
        }
    }
    if (!bs.error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_RED, "%s", bs.error.c_str());
        ImGui::PopTextWrapPos();
        if (bs.error.find("4014") != std::string::npos ||
            bs.error.find("Disallowed") != std::string::npos) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(C_ORANGE,
                "Fix: discord.com/developers -> your application -> Bot -> "
                "Privileged Gateway Intents -> enable MESSAGE CONTENT INTENT "
                "-> Save. The bot retries automatically and will connect "
                "within seconds of the toggle being saved.");
            ImGui::PopTextWrapPos();
        }
        if (bs.error.find("4004") != std::string::npos ||
            bs.error.find("Authentication") != std::string::npos) {
            ImGui::TextColored(C_ORANGE,
                "Fix: the token is wrong or was reset - copy a fresh one from "
                "the developer portal and Save token + connect.");
        }
    }
    // Transient connection hiccups (dropped socket, rate limit) retry on
    // their own and this notice expires by itself - orange, not red.
    if (!bs.warn.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_ORANGE, "%s", bs.warn.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextColored(C_DIM,
            "transient connection hiccup during a history sweep - retried "
            "automatically, nothing to do");
    }
    if (bs.state != "connected" && bs.error.empty()) {
        ImGui::TextColored(C_DIM,
            "Checklist: bot invited to the server, and the MESSAGE CONTENT "
            "INTENT toggle enabled under Bot settings in the Discord developer "
            "portal (required - the gateway refuses the connection without it).");
    }

    ImGui::SetNextItemWidth(420);
    ImGui::InputText("bot token", a.dcToken, sizeof(a.dcToken),
                     a.dcShowToken ? 0 : ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    ImGui::Checkbox("show", &a.dcShowToken);
    ImGui::SameLine();
    if (ImGui::Button("Save token + connect")) {
        {
            auto lk = a.db->guard();
            a.db->setSetting(lk.token(), "discord_bot_token", a.dcToken);
        }
        a.discord->start(a.dcToken);
        toast(a, "token saved - connecting");
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(C_DIM,
        "Enter Discord user IDs, not usernames. Separate multiple IDs with "
        "commas. This global whitelist can approve/capture messages in watched "
        "channels; approve or reject pending notification cards; reply with an "
        "approval note; and use !tickets, !xatickets, !leaderboard, and menu "
        "reactions in every server the bot can read. Clear and save to disable "
        "these privileged actions; changes apply without reconnecting.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(420);
    if (a.dcAdminIdsValue.truncated) {
        ImGui::TextColored(
            C_ORANGE,
            "The saved whitelist is %zu bytes and cannot fit in this editor. "
            "It will be preserved unchanged.",
            a.dcAdminIdsValue.original.size());
        ImGui::BeginDisabled();
    }
    ImGui::InputTextWithHint("admin/developer whitelist",
                             "Discord user IDs, comma-separated",
                             a.dcAdminIds, sizeof(a.dcAdminIds));
    if (a.dcAdminIdsValue.truncated) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save whitelist")) {
        std::vector<std::string> ids;
        std::string error;
        const std::string whitelist =
            preservedText(a.dcAdminIds, a.dcAdminIdsValue);
        if (!parseDiscordAdminUserIds(whitelist, ids, &error)) {
            toast(a, "whitelist not saved - " + error, true);
        } else {
            std::ostringstream normalized;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) normalized << ", ";
                normalized << ids[i];
            }
            const std::string saved = normalized.str();
            {
                auto lk = a.db->guard();
                a.db->setSetting(lk.token(), "discord_admin_user_ids", saved);
            }
            loadPreservedText(a.dcAdminIds, sizeof(a.dcAdminIds), saved,
                              a.dcAdminIdsValue);
            toast(a, ids.empty()
                ? "whitelist cleared - privileged Discord actions disabled"
                : "admin/developer whitelist saved");
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(C_DIM,
        "In a watched channel, a listed admin/developer can @mention the bot "
        "with a suggestion, or reply to another message and @mention it. New "
        "suggestions enter the normal approval inbox. If that exact replied-to "
        "message already belongs to a promoted ticket, its follow-up note and "
        "only new target/reply attachments are appended to the existing ticket. "
        "Files are never saved before the first Promote approval.");
    ImGui::PopTextWrapPos();
}

// Body of the collapsible "Monitors" section: add-monitor inputs plus the
// per-server channel groups (each group's open state persists too).
static void drawDiscordMonitors(App& a) {
    ImGui::TextColored(C_DIM,
        "Server ID alone = monitor every channel in that server. Add a channel "
        "ID to monitor just that channel. Channels the bot reads are listed "
        "below automatically.");
    ImGui::SetNextItemWidth(190);
    ImGui::InputTextWithHint("##msrv", "server id", a.dcServerId, sizeof(a.dcServerId));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    ImGui::InputTextWithHint("##mchan", "channel id (optional)", a.dcChanId,
                             sizeof(a.dcChanId));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##mname", "name (optional)", a.dcMonName,
                             sizeof(a.dcMonName));
    ImGui::SameLine();
    if (ImGui::Button("Add monitor")) {
        std::string srv = trim(a.dcServerId), chan = trim(a.dcChanId);
        if (!chan.empty()) {
            auto lk = a.db->guard();
            SQLite::Statement ins(a.db->raw(lk.token()),
                "INSERT INTO discord_channels(channel_id,guild_name,channel_name,"
                "created_at) VALUES(?,?,?,?) ON CONFLICT(channel_id) DO NOTHING");
            ins.bind(1, chan);
            ins.bind(2, "");
            ins.bind(3, a.dcMonName);
            ins.bind(4, nowIsoUtc());
            ins.exec();
            lk.unlock();
            a.needChannels = true;
            toast(a, "channel monitor added");
        } else if (!srv.empty()) {
            auto lk = a.db->guard();
            SQLite::Statement ins(a.db->raw(lk.token()),
                "INSERT INTO discord_guilds(guild_id,guild_name,created_at) "
                "VALUES(?,?,?) ON CONFLICT(guild_id) DO NOTHING");
            ins.bind(1, srv);
            ins.bind(2, a.dcMonName);
            ins.bind(3, nowIsoUtc());
            ins.exec();
            lk.unlock();
            a.needGuilds = true;
            toast(a, "server monitor added - all channels will be watched");
        } else {
            toast(a, "enter a server id (and optionally a channel id)", true);
        }
        a.dcServerId[0] = a.dcChanId[0] = a.dcMonName[0] = 0;
    }

    auto drawChannelTable = [&](std::vector<Channel*>& list) {
        if (list.empty()) {
            ImGui::TextColored(C_DIM,
                "no channels seen yet - they appear as soon as the bot reads "
                "them");
            return;
        }
        if (!ImGui::BeginTable("channels", 8,
                               ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("channel", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("project", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("msgs", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("pending", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        // last read = newest message the bot has ingested;
        // last checked = the last time the bot actually looked at the channel.
        ImGui::TableSetupColumn("last read", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("last checked", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableHeadersRow();
        for (Channel* pc : list) {
            Channel& c = *pc;
            ImGui::PushID((int)c.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("#%s", c.name.empty() ? c.channelId.c_str() : c.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("id %s", c.channelId.c_str());
            ImGui::TableNextColumn();
            int idx = -1;
            for (int i = 0; i < (int)a.projects.size(); ++i)
                if (a.projects[i].id == c.projectId) idx = i;
            if (projectCombo(a, "##chp", &idx, -1)) {
                auto lk = a.db->guard();
                SQLite::Statement up(a.db->raw(lk.token()),
                    "UPDATE discord_channels SET project_id=? WHERE id=?");
                up.bind(1, a.projects[idx].id);
                up.bind(2, c.id);
                up.exec();
                lk.unlock();
                a.needChannels = true;
                a.needInbox = true;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%lld", c.msgs);
            ImGui::TableNextColumn();
            if (c.pending > 0) ImGui::TextColored(C_ORANGE, "%lld", c.pending);
            else ImGui::TextColored(C_DIM, "0");
            ImGui::TableNextColumn();
            ImGui::TextColored(C_DIM, "%s",
                               c.lastRead.empty() ? "never" : shortTs(c.lastRead).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("timestamp of the newest message ingested");
            ImGui::TableNextColumn();
            ImGui::TextColored(C_DIM, "%s",
                               c.lastScan.empty() ? "never" : shortTs(c.lastScan).c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("last time the bot checked this channel "
                                  "(connect backfill, live event, or the "
                                  "5-minute re-scan)");
            ImGui::TableNextColumn();
            bool on = c.enabled != 0;
            if (ImGui::Checkbox("##on", &on)) {
                auto lk = a.db->guard();
                SQLite::Statement up(a.db->raw(lk.token()),
                    "UPDATE discord_channels SET enabled=? WHERE id=?");
                up.bind(1, on ? 1 : 0);
                up.bind(2, c.id);
                up.exec();
                lk.unlock();
                a.needChannels = true;
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("del")) {
                bool deleted = false;
                bool hasNotifyCard = false;
                bool hasDeletedSourceMarker = false;
                auto lk = a.db->guard();
                SQLite::Statement lineage(a.db->raw(lk.token()),
                    "SELECT EXISTS(SELECT 1 FROM discord_notify_cards n "
                    "JOIN discord_messages m ON m.id=n.discord_message_row_id "
                    "WHERE m.channel_row_id=?),"
                    "EXISTS(SELECT 1 FROM discord_messages m "
                    "WHERE m.channel_row_id=? AND m.state='deleted')");
                lineage.bind(1, c.id);
                lineage.bind(2, c.id);
                lineage.executeStep();
                hasNotifyCard = lineage.getColumn(0).getInt() != 0;
                hasDeletedSourceMarker = lineage.getColumn(1).getInt() != 0;
                if (!hasNotifyCard && !hasDeletedSourceMarker) {
                    SQLite::Statement del(a.db->raw(lk.token()),
                        "DELETE FROM discord_channels WHERE id=?");
                    del.bind(1, c.id);
                    deleted = del.exec() == 1;
                }
                lk.unlock();
                if (hasDeletedSourceMarker) {
                    toast(a,
                        "channel contains retained Discord deletion markers; disable it instead",
                        true);
                } else if (hasNotifyCard) {
                    toast(a,
                        "channel owns Discord notification cards; disable it instead",
                        true);
                } else if (deleted) {
                    a.needChannels = true;
                    a.needInbox = true;
                } else {
                    toast(a, "channel monitor was not deleted", true);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    };

    std::map<std::string, std::vector<Channel*>> byGuild;
    for (auto& c : a.channels) byGuild[c.guild].push_back(&c);
    std::set<std::string> shownGuilds;

    for (auto& g : a.guilds) {
        ImGui::PushID((int)(g.id + 100000));
        auto& chans = byGuild[g.name];
        shownGuilds.insert(g.name);
        char hdr[320];
        std::snprintf(hdr, sizeof(hdr), "%s  -  %s  (%d channel%s)###guildhdr",
                      g.name.empty() ? "(server - name appears on connect)"
                                     : g.name.c_str(),
                      g.guildId.c_str(), (int)chans.size(),
                      chans.size() == 1 ? "" : "s");
        bool open = sectionHeader(a, hdr, "guild_" + g.guildId, true,
                                  ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 110);
        bool on = g.enabled != 0;
        if (ImGui::Checkbox("on##gon", &on)) {
            auto lk = a.db->guard();
            SQLite::Statement up(a.db->raw(lk.token()),
                "UPDATE discord_guilds SET enabled=? WHERE id=?");
            up.bind(1, on ? 1 : 0);
            up.bind(2, g.id);
            up.exec();
            lk.unlock();
            a.needGuilds = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("del")) {
            auto lk = a.db->guard();
            SQLite::Statement del(a.db->raw(lk.token()),
                "DELETE FROM discord_guilds WHERE id=?");
            del.bind(1, g.id);
            del.exec();
            lk.unlock();
            a.needGuilds = true;
        }
        if (open) drawChannelTable(chans);
        ImGui::PopID();
    }

    // Channel monitors that live outside any server-wide monitor.
    for (auto& [gname, list] : byGuild) {
        if (shownGuilds.count(gname)) continue;
        ImGui::PushID(gname.empty() ? "#nogld" : gname.c_str());
        char hdr[320];
        std::snprintf(hdr, sizeof(hdr), "%s  (%d channel%s)###chanhdr",
                      gname.empty() ? "Individual channel monitors"
                                    : (gname + "  -  channel monitors").c_str(),
                      (int)list.size(), list.size() == 1 ? "" : "s");
        if (sectionHeader(a, hdr,
                          "changrp_" + (gname.empty() ? std::string("none")
                                                      : slugify(gname)),
                          true))
            drawChannelTable(list);
        ImGui::PopID();
    }
}

void drawDiscord(App& a) {
    // one-time buffer loads
    if (!a.dcLoaded) {
        auto lk = a.db->guard();
        copyBuf(a.dcToken, sizeof(a.dcToken),
                a.db->getSetting(lk.token(), "discord_bot_token"));
        loadPreservedText(a.dcAdminIds, sizeof(a.dcAdminIds),
                          a.db->getSetting(lk.token(), "discord_admin_user_ids"),
                          a.dcAdminIdsValue);
        a.dcLoaded = true;
    }
    if (!a.dcPatternsLoaded) {
        SuggestionDetector d;
        {
            auto lk = a.db->guard();
            d = makeDetectorLocked(lk.token(), a.db);
        }
        copyBuf(a.dcPatterns, sizeof(a.dcPatterns),
                SuggestionDetector::patternsToJson(d.patterns()).dump(1));
        a.dcPatternsLoaded = true;
    }

    // Bot: collapsible; the connection summary stays visible in the header.
    DiscordBot::Status bs = a.discord->status();
    std::string botHdr = "Bot  -  ";
    if (bs.state == "connected")
        botHdr += "connected as " + bs.botUser + " (" +
                  std::to_string(bs.guilds) +
                  (bs.guilds == 1 ? " server)" : " servers)");
    else
        botHdr += bs.state;
    if (sectionHeader(a, (botHdr + "###secbot").c_str(), "discord_bot", true))
        drawDiscordBotSection(a, bs);

    // ---- bot notifications -------------------------------------------------
    if (sectionHeader(a, "Bot notifications", "discord_notify", false)) {
        if (!a.dcNotifyLoaded) {
            auto lk = a.db->guard();
            copyBuf(a.dcNotifyGuild, sizeof(a.dcNotifyGuild),
                    a.db->getSetting(lk.token(), "notify_guild_id"));
            copyBuf(a.dcNotifyChan, sizeof(a.dcNotifyChan),
                    a.db->getSetting(lk.token(), "notify_channel_id"));
            a.dcNotifyLoaded = true;
        }
        ImGui::TextColored(C_DIM,
            "The bot posts a card into this channel for every detected "
            "suggestion/bug, and keeps editing the SAME card as you handle "
            "it: promoted (project + type), completed, dismissed, or deleted. "
            "Whitelisted admins can react with the check mark to approve, react "
            "with X to reject, or reply with a non-empty note to approve and "
            "carry that note into the ticket. Approval waits for a mapped or "
            "inferred project; controls are restored after reconnect. "
            "It only ever edits its own cards - users' messages are never "
            "touched. Leave the channel id blank to disable.");
        ImGui::SetNextItemWidth(190);
        ImGui::InputTextWithHint("##ng", "server id", a.dcNotifyGuild,
                                 sizeof(a.dcNotifyGuild));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(190);
        ImGui::InputTextWithHint("##nc", "channel id", a.dcNotifyChan,
                                 sizeof(a.dcNotifyChan));
        ImGui::SameLine();
        auto saveNotify = [&]() {
            auto lk = a.db->guard();
            a.db->setSetting(lk.token(), "notify_guild_id", trim(a.dcNotifyGuild));
            a.db->setSetting(lk.token(), "notify_channel_id", trim(a.dcNotifyChan));
        };
        if (ImGui::Button("Save##notify")) {
            saveNotify();
            toast(a, trim(a.dcNotifyChan).empty()
                          ? "bot notifications disabled"
                          : "notify channel saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save + send test")) {
            saveNotify();
            if (trim(a.dcNotifyChan).empty()) {
                toast(a, "enter the channel id first", true);
            } else if (a.discord->status().state != "connected") {
                toast(a, "bot is not connected - connect it first", true);
            } else {
                sendNotifyTest(a.db, a.discord);
                toast(a, "test card sent - check the channel");
            }
        }

        const bool reloadNotifyCounters = a.needNotifyCounters;
        if (reloadNotifyCounters) {
            a.notifyFailures = {};
            auto lk = a.db->guard();
            SQLite::Statement total(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,'create' AS operation,post_revision AS revision
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,'edit',edit_revision
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT COUNT(*) FROM failures f
 WHERE NOT EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision))sql");
            total.executeStep();
            a.notifyFailures.activeTotal = total.getColumn(0).getInt();

            SQLite::Statement dismissedTotal(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,'create' AS operation,post_revision AS revision
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,'edit',edit_revision
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT COUNT(*) FROM failures f
 WHERE EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision))sql");
            dismissedTotal.executeStep();
            a.notifyFailures.dismissedTotal =
                dismissedTotal.getColumn(0).getInt();

            SQLite::Statement oldMessageTotal(a.db->raw(lk.token()), R"sql(
SELECT COUNT(*) FROM discord_notify_cards n
 WHERE n.edit_state='failed'
   AND (instr(n.edit_last_error,
       'Maximum number of edits to messages older than 1 hour reached')>0
     OR instr(n.edit_last_error,'30046')>0)
   AND NOT EXISTS(
     SELECT 1 FROM discord_notify_failure_dismissals d
      WHERE d.card_row_id=n.id AND d.operation='edit'
        AND d.revision=n.edit_revision))sql");
            oldMessageTotal.executeStep();
            a.notifyFailures.oldMessageTotal =
                oldMessageTotal.getColumn(0).getInt();
            SQLite::Statement oldMessageTargets(a.db->raw(lk.token()), R"sql(
SELECT n.id,n.edit_revision
  FROM discord_notify_cards n
 WHERE n.edit_state='failed'
   AND (instr(n.edit_last_error,
       'Maximum number of edits to messages older than 1 hour reached')>0
     OR instr(n.edit_last_error,'30046')>0)
   AND NOT EXISTS(
     SELECT 1 FROM discord_notify_failure_dismissals d
      WHERE d.card_row_id=n.id AND d.operation='edit'
        AND d.revision=n.edit_revision)
 ORDER BY n.updated_at,n.id
 LIMIT ?)sql");
            oldMessageTargets.bind(
                1, static_cast<int>(kMaxNotifyFailureDismissBatch));
            while (oldMessageTargets.executeStep()) {
                a.notifyFailures.oldMessageTargets.push_back({
                    oldMessageTargets.getColumn(0).getInt64(),
                    oldMessageTargets.getColumn(1).getInt()});
            }

            auto readFailureRows = [](SQLite::Statement& statement,
                                      std::vector<NotifyFailureRow>& rows) {
                while (statement.executeStep()) {
                    NotifyFailureRow row;
                    row.id = statement.getColumn(0).getInt64();
                    row.itemId = statement.getColumn(1).getInt64();
                    row.messageRowId = statement.getColumn(2).getInt64();
                    row.operation = statement.getColumn(3).getString();
                    row.attempts = statement.getColumn(4).getInt();
                    row.revision = statement.getColumn(5).getInt();
                    row.error =
                        redactHttpUrls(statement.getColumn(6).getString());
                    row.channelId = statement.getColumn(7).getString();
                    row.messageId = statement.getColumn(8).getString();
                    rows.push_back(std::move(row));
                }
            };
            SQLite::Statement failed(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,COALESCE(item_id,0) AS item_id,
        COALESCE(discord_message_row_id,0) AS message_row_id,
        'create' AS operation,post_attempts AS attempts,
        post_revision AS revision,post_last_error AS error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,COALESCE(item_id,0),
        COALESCE(discord_message_row_id,0),
        'edit',edit_attempts,edit_revision,edit_last_error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT id,item_id,message_row_id,operation,attempts,revision,error,
       notify_channel_id,notify_message_id
  FROM failures f
 WHERE NOT EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision)
 ORDER BY updated_at DESC,id DESC,operation LIMIT 20)sql");
            readFailureRows(failed, a.notifyFailures.active);

            SQLite::Statement dismissed(a.db->raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,COALESCE(item_id,0) AS item_id,
        COALESCE(discord_message_row_id,0) AS message_row_id,
        'create' AS operation,post_attempts AS attempts,
        post_revision AS revision,post_last_error AS error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,COALESCE(item_id,0),
        COALESCE(discord_message_row_id,0),
        'edit',edit_attempts,edit_revision,edit_last_error,
        notify_channel_id,notify_message_id,updated_at
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT id,item_id,message_row_id,operation,attempts,revision,error,
       notify_channel_id,notify_message_id
  FROM failures f
 WHERE EXISTS(
   SELECT 1 FROM discord_notify_failure_dismissals d
    WHERE d.card_row_id=f.id AND d.operation=f.operation
      AND d.revision=f.revision)
 ORDER BY updated_at DESC,id DESC,operation LIMIT 20)sql");
            readFailureRows(dismissed, a.notifyFailures.dismissed);
        }
        const auto& notifyFailures = a.notifyFailures.active;
        const auto& dismissedNotifyFailures = a.notifyFailures.dismissed;
        const int notifyFailureTotal = a.notifyFailures.activeTotal;
        const int dismissedNotifyFailureTotal =
            a.notifyFailures.dismissedTotal;
        const int oldMessageNotifyFailureTotal =
            a.notifyFailures.oldMessageTotal;
        const auto& oldMessageNotifyFailureTargets =
            a.notifyFailures.oldMessageTargets;
        if (notifyFailureTotal > 0 || dismissedNotifyFailureTotal > 0) {
            ImGui::SeparatorText(notifyFailureTotal > 0
                ? "Delivery failures" : "Delivery history");
        }
        if (!notifyFailures.empty()) {
            ImGui::TextColored(C_ORANGE,
                "%d active notification delivery failure(s)",
                notifyFailureTotal);
            ImGui::TextColored(C_DIM,
                "Retry transient failures, or recreate a card when Discord "
                "cannot edit its stored message. Dismiss hides only this "
                "failed revision locally; it does not delete the card or "
                "contact Discord.");
            if (oldMessageNotifyFailureTotal > 0) {
                const int batchCount = static_cast<int>(
                    oldMessageNotifyFailureTargets.size());
                const std::string bulkLabel =
                    "dismiss old-message alerts (" +
                    std::to_string(batchCount) +
                    (batchCount < oldMessageNotifyFailureTotal
                        ? " of " +
                          std::to_string(oldMessageNotifyFailureTotal)
                        : "") + ")";
                if (batchCount > 0 &&
                    ImGui::SmallButton(bulkLabel.c_str())) {
                    a.dismissOldNotifyAlertTargets =
                        oldMessageNotifyFailureTargets;
                    a.dismissOldNotifyAlertTotal =
                        oldMessageNotifyFailureTotal;
                }
                ImGui::SameLine();
                ImGui::TextColored(C_DIM,
                    "exact Discord edit-limit failures only");
            }
            for (const NotifyFailureRow& row : notifyFailures) {
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::PushID(row.operation.c_str());
                const std::string owner = row.itemId > 0
                    ? "item I" + std::to_string(row.itemId)
                    : "source D" + std::to_string(row.messageRowId);
                ImGui::TextColored(C_RED,
                    "card N%lld - %s %s failed (%d attempts, revision %d)",
                    row.id, owner.c_str(), row.operation.c_str(),
                    row.attempts, row.revision);
                if (!row.error.empty())
                    ImGui::TextWrapped("%s", row.error.c_str());
                ImGui::TextColored(C_DIM, "stored target: channel %s%s%s",
                    row.channelId.empty() ? "(none)" : row.channelId.c_str(),
                    row.messageId.empty() ? "" : ", message ",
                    row.messageId.empty() ? "" : row.messageId.c_str());
                const bool oldMessageLimit =
                    row.operation == "edit" &&
                    isDiscordOldMessageEditLimitText(row.error);
                if (oldMessageLimit) {
                    ImGui::TextColored(C_ORANGE,
                        "Discord will not accept another edit for this older "
                        "message. Recreate it or dismiss this alert.");
                } else {
                    if (ImGui::SmallButton("retry delivery")) {
                        if (retryFailedNotifyCard(a.db, a.discord, row.id))
                            toast(a, "notification card N" +
                                      std::to_string(row.id) +
                                      " queued for retry");
                        else
                            toast(a, "notification retry was not applied",
                                  true);
                        a.needNotifyCounters = true;
                    }
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("recreate in saved channel")) {
                    a.recreateNotifyCardId = row.id;
                    a.recreateNotifyCardOperation = row.operation;
                    a.recreateNotifyCardRevision = row.revision;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("dismiss alert")) {
                    if (dismissFailedNotifyCardAlert(
                            a.db, row.id, row.operation, row.revision))
                        toast(a, "notification alert dismissed; delivery "
                                  "history retained");
                    else
                        toast(a, "alert changed before it could be dismissed",
                              true);
                    a.needNotifyCounters = true;
                }
                ImGui::PopID();
                ImGui::PopID();
            }
            if (notifyFailureTotal > static_cast<int>(notifyFailures.size()))
                ImGui::TextColored(C_DIM, "%d older active failure(s) omitted",
                    notifyFailureTotal -
                    static_cast<int>(notifyFailures.size()));
        }
        if (!dismissedNotifyFailures.empty()) {
            const std::string acknowledgedLabel =
                "Acknowledged current failures (" +
                std::to_string(dismissedNotifyFailureTotal) +
                ")###acknowledged_notify_failures";
            if (ImGui::TreeNode(acknowledgedLabel.c_str())) {
                ImGui::TextColored(C_DIM,
                    "These failed revisions remain recorded but no longer "
                    "raise an alert. Restore one to make it active again.");
                for (const NotifyFailureRow& row :
                     dismissedNotifyFailures) {
                    ImGui::PushID(static_cast<int>(row.id));
                    ImGui::PushID(row.operation.c_str());
                    const std::string owner = row.itemId > 0
                        ? "item I" + std::to_string(row.itemId)
                        : "source D" + std::to_string(row.messageRowId);
                    ImGui::TextColored(C_DIM,
                        "card N%lld - %s %s revision %d (%d attempts)",
                        row.id, owner.c_str(), row.operation.c_str(),
                        row.revision, row.attempts);
                    if (!row.error.empty())
                        ImGui::TextWrapped("%s", row.error.c_str());
                    if (ImGui::SmallButton("restore alert")) {
                        if (restoreFailedNotifyCardAlert(
                                a.db, row.id, row.operation, row.revision))
                            toast(a, "notification alert restored");
                        else
                            toast(a, "dismissed revision is no longer current",
                                  true);
                        a.needNotifyCounters = true;
                    }
                    ImGui::SameLine();
                    const bool oldMessageLimit =
                        row.operation == "edit" &&
                        isDiscordOldMessageEditLimitText(row.error);
                    if (!oldMessageLimit) {
                        if (ImGui::SmallButton("retry delivery")) {
                            if (retryFailedNotifyCard(
                                    a.db, a.discord, row.id))
                                toast(a, "notification card N" +
                                          std::to_string(row.id) +
                                          " queued for retry");
                            else
                                toast(a,
                                    "notification retry was not applied",
                                    true);
                            a.needNotifyCounters = true;
                        }
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("recreate in saved channel")) {
                        a.recreateNotifyCardId = row.id;
                        a.recreateNotifyCardOperation = row.operation;
                        a.recreateNotifyCardRevision = row.revision;
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (dismissedNotifyFailureTotal >
                    static_cast<int>(dismissedNotifyFailures.size()))
                    ImGui::TextColored(C_DIM,
                        "%d older acknowledged failure(s) omitted",
                        dismissedNotifyFailureTotal -
                        static_cast<int>(dismissedNotifyFailures.size()));
                ImGui::TreePop();
            }
        }

        if (reloadNotifyCounters) {
            auto lk = a.db->guard();
            SQLite::Statement total(a.db->raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_orphans "
                "WHERE cleanup_state='failed'");
            total.executeStep();
            a.notifyFailures.orphanTotal = total.getColumn(0).getInt();
            SQLite::Statement failed(a.db->raw(lk.token()),
                "SELECT id,card_row_id,notify_channel_id,notify_message_id,"
                "cleanup_attempts,cleanup_last_error "
                "FROM discord_notify_orphans WHERE cleanup_state='failed' "
                "ORDER BY updated_at DESC,id DESC LIMIT 20");
            while (failed.executeStep()) {
                OrphanFailureRow row;
                row.id = failed.getColumn(0).getInt64();
                row.cardRowId = failed.getColumn(1).getInt64();
                row.channelId = failed.getColumn(2).getString();
                row.messageId = failed.getColumn(3).getString();
                row.attempts = failed.getColumn(4).getInt();
                row.error = redactHttpUrls(failed.getColumn(5).getString());
                a.notifyFailures.orphans.push_back(std::move(row));
            }
        }
        const auto& orphanFailures = a.notifyFailures.orphans;
        const int orphanFailureTotal = a.notifyFailures.orphanTotal;
        if (!orphanFailures.empty()) {
            ImGui::SeparatorText("Orphan cleanup failures");
            ImGui::TextColored(C_RED,
                "%d exact callback-generated card cleanup failure(s)",
                orphanFailureTotal);
            ImGui::TextColored(C_DIM,
                "These IDs came only from this bot's create callbacks. Correct "
                "connectivity/permissions, then retry cleanup.");
            ImGui::PushID("orphan_cleanup");
            for (const OrphanFailureRow& row : orphanFailures) {
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::TextColored(C_RED,
                    "cleanup O%lld from card N%lld failed (%d attempts)",
                    row.id, row.cardRowId, row.attempts);
                ImGui::TextColored(C_DIM, "exact target: channel %s, message %s",
                    row.channelId.c_str(), row.messageId.c_str());
                if (!row.error.empty())
                    ImGui::TextWrapped("%s", row.error.c_str());
                if (ImGui::SmallButton("retry orphan cleanup")) {
                    if (retryFailedNotifyOrphan(a.db, a.discord, row.id))
                        toast(a, "orphan cleanup O" + std::to_string(row.id) +
                                  " queued for retry");
                    else
                        toast(a, "orphan cleanup retry was not applied", true);
                    a.needNotifyCounters = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            if (orphanFailureTotal > static_cast<int>(orphanFailures.size()))
                ImGui::TextColored(C_DIM, "%d older cleanup failure(s) omitted",
                    orphanFailureTotal -
                    static_cast<int>(orphanFailures.size()));
        }

        if (reloadNotifyCounters) {
            auto lk = a.db->guard();
            SQLite::Statement total(a.db->raw(lk.token()),
                "SELECT COUNT(*) FROM discord_review_control_cleanups "
                "WHERE cleanup_state='failed'");
            total.executeStep();
            a.notifyFailures.reviewCleanupTotal =
                total.getColumn(0).getInt();
            SQLite::Statement failed(a.db->raw(lk.token()),
                "SELECT id,card_row_id,notify_channel_id,notify_message_id,"
                "cleanup_attempts,cleanup_last_error "
                "FROM discord_review_control_cleanups "
                "WHERE cleanup_state='failed' "
                "ORDER BY updated_at DESC,id DESC LIMIT 20");
            while (failed.executeStep()) {
                ReviewCleanupFailureRow row;
                row.id = failed.getColumn(0).getInt64();
                row.cardRowId = failed.getColumn(1).getInt64();
                row.channelId = failed.getColumn(2).getString();
                row.messageId = failed.getColumn(3).getString();
                row.attempts = failed.getColumn(4).getInt();
                row.error = redactHttpUrls(failed.getColumn(5).getString());
                a.notifyFailures.reviewCleanups.push_back(std::move(row));
            }
            a.needNotifyCounters = false;
        }
        const auto& reviewCleanupFailures =
            a.notifyFailures.reviewCleanups;
        const int reviewCleanupFailureTotal =
            a.notifyFailures.reviewCleanupTotal;
        if (!reviewCleanupFailures.empty()) {
            ImGui::SeparatorText("Review-reaction cleanup failures");
            ImGui::TextColored(C_RED,
                "%d all-reaction cleanup failure(s)",
                reviewCleanupFailureTotal);
            ImGui::TextColored(C_DIM,
                "Every reaction on the exact terminal bot card is targeted; "
                "the card, source, and item are retained. Requires Manage "
                "Messages.");
            ImGui::PushID("review_control_cleanup");
            for (const ReviewCleanupFailureRow& row :
                 reviewCleanupFailures) {
                ImGui::PushID(static_cast<int>(row.id));
                ImGui::TextColored(C_RED,
                    "cleanup C%lld from card N%lld failed (%d attempts)",
                    row.id, row.cardRowId, row.attempts);
                ImGui::TextColored(C_DIM,
                    "exact target: channel %s, message %s",
                    row.channelId.c_str(), row.messageId.c_str());
                if (!row.error.empty())
                    ImGui::TextWrapped("%s", row.error.c_str());
                if (ImGui::SmallButton("retry reaction cleanup")) {
                    if (retryFailedReviewControlCleanup(
                            a.db, a.discord, row.id))
                        toast(a, "review-reaction cleanup C" +
                                  std::to_string(row.id) +
                                  " queued for retry");
                    else
                        toast(a, "review-reaction cleanup retry was not applied",
                              true);
                    a.needNotifyCounters = true;
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            if (reviewCleanupFailureTotal >
                static_cast<int>(reviewCleanupFailures.size()))
                ImGui::TextColored(C_DIM,
                    "%d older reaction cleanup failure(s) omitted",
                    reviewCleanupFailureTotal -
                    static_cast<int>(reviewCleanupFailures.size()));
        }
    }

    if (sectionHeader(a, "Monitors", "discord_monitors", true))
        drawDiscordMonitors(a);

    ImGui::SeparatorText("Inbox");
    ImGui::TextColored(C_DIM,
        "Manual capture (admin/developer whitelist only): directly mention "
        "the bot with a suggestion, or mention it while replying to a message. "
        "The submitted message lands here at 100%%; reply text after the "
        "mention becomes the admin note. Replies to an exact source that is "
        "already promoted enrich that existing ticket instead of returning "
        "to this inbox.");
    if (a.inbox.empty())
        ImGui::TextColored(C_DIM, "inbox zero - nothing detected");
    for (auto& m : a.inbox) {
        ImGui::PushID((int)m.id);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG2);
        ImGui::BeginChild("msg", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        chip((m.kind + " " + std::to_string((int)(m.score * 100)) + "%").c_str(),
             m.kind == "bug" ? C_RED : C_ACCENT);
        ImGui::SameLine();
        ImGui::TextUnformatted(m.author.c_str());
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "#%s - %s", m.channelName.c_str(),
                           shortTs(m.posted).c_str());
        if (!m.guildId.empty() && !m.messageId.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("open in Discord"))
                openUrl("https://discord.com/channels/" + m.guildId + "/" +
                        m.channelId + "/" + m.messageId);
        }
        // Project attribution: channel mapping first, then name/alias
        // inference, otherwise a general flag the user assigns by hand.
        if (m.selProjectId <= 0) {
            long long want = m.channelProjectId > 0 ? m.channelProjectId
                                                    : m.inferredProjectId;
            m.selProjectId = want;
            m.selProject = projectIndexForId(a, want);
            if (m.selProject < 0) m.selProjectId = 0;
        }
        ImGui::SameLine();
        if (m.inferredProjectId > 0 && m.channelProjectId <= 0) {
            Project* ip = findProject(a, m.inferredProjectId);
            if (ip) chip(("auto: " + ip->name).c_str(), C_GREEN);
        } else if (m.channelProjectId <= 0 && m.inferredProjectId <= 0) {
            chip("general - pick a project", C_DIM);
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(m.content.c_str());
        if (!m.adminNote.empty()) {
            ImGui::TextColored(C_GREEN, "admin note:");
            ImGui::SameLine();
            ImGui::TextUnformatted(m.adminNote.c_str());
        }
        if (m.attachmentCount > 0) {
            ImGui::TextColored(C_ORANGE,
                "%d file%s attached - files are saved only when you Promote",
                m.attachmentCount, m.attachmentCount == 1 ? "" : "s");
        }
        ImGui::PopTextWrapPos();
        ImGui::TextColored(C_DIM, "matched: %s", m.matched.c_str());

        projectCombo(a, "##pm", &m.selProject);
        m.selProjectId =
            (m.selProject >= 0 &&
             m.selProject < static_cast<int>(a.projects.size()))
                ? a.projects[m.selProject].id : 0;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##pt", &m.selType, kPromoteTypeNames, 3);
        ImGui::SameLine();
        if (ImGui::Button("Edit pending"))
            openEditPendingSuggestion(a, m);
        ImGui::SameLine();
        bool noProject = m.selProjectId <= 0;
        if (noProject) ImGui::BeginDisabled();
        if (ImGui::Button("Promote to item")) {
            nlohmann::json r = promoteSuggestion(a.db, a.discord, m.id,
                                                 m.selProjectId,
                                                 kPromoteTypeNames[m.selType], "");
            if (r.value("ok", false)) {
                std::string result = "promoted with credit to " + m.author;
                const int saved = r.value("attachments_saved", 0);
                const int failed = r.value("attachments_failed", 0);
                const int pendingAttachments =
                    r.value("attachments_pending", 0);
                if (saved || failed || pendingAttachments) {
                    result += " - attachments: " + std::to_string(saved) +
                              " saved";
                    if (failed) result += ", " + std::to_string(failed) + " failed";
                    if (pendingAttachments)
                        result += ", " +
                                  std::to_string(pendingAttachments) +
                                  " queued for save";
                }
                toast(a, result, failed > 0);
                a.needInbox = true;
                a.needProjects = true;
                a.needItems = true;
                a.needSources = true;
                a.needWork = true;
            } else {
                toast(a, r.value("error", "promote failed"), true);
            }
        }
        if (noProject) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Dismiss")) {
            dismissSuggestion(a.db, a.discord, m.id);
            toast(a, "dismissed");
            a.needInbox = true;
            a.needChannels = true;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::Spacing();
    if (sectionHeader(a, "Detection templates", "discord_templates", false)) {
        ImGui::TextColored(C_DIM,
            "One entry per pattern: {\"phrase\",\"kind\":suggestion|bug,\"weight\"}. "
            "Phrases starting with ^ anchor to the message start. Score >= 0.5 lands "
            "in the inbox.");
        ImGui::InputTextMultiline("##pat", a.dcPatterns, sizeof(a.dcPatterns),
                                  ImVec2(-1, 220));
        auto savePatterns = [&]() -> bool {
            nlohmann::json j = nlohmann::json::parse(a.dcPatterns, nullptr, false);
            if (j.is_discarded()) { toast(a, "invalid JSON", true); return false; }
            auto pats = SuggestionDetector::patternsFromJson(j);
            if (pats.empty()) { toast(a, "no valid patterns", true); return false; }
            auto lk = a.db->guard();
            a.db->setSetting(lk.token(), "detection_patterns",
                             SuggestionDetector::patternsToJson(pats).dump());
            return true;
        };
        if (ImGui::Button("Save patterns")) {
            if (savePatterns()) toast(a, "patterns saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Save + rescan inbox")) {
            if (savePatterns()) {
                nlohmann::json r = rescanNewMessages(a.db);
                toast(a, "rescanned " + std::to_string(r.value("rescanned", 0)) + ", " +
                             std::to_string(r.value("candidates", 0)) + " candidates");
                a.needInbox = true;
                a.needChannels = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) {
            {
                auto lk = a.db->guard();
                a.db->setSetting(lk.token(), "detection_patterns", "");
            }
            a.dcPatternsLoaded = false;
            toast(a, "patterns reset");
        }
    }
}


} // namespace devhub
