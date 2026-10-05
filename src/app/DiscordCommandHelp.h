#pragma once

#include <string>

namespace devhub {

// Shared by the native Commands section and the bot's help card. Keep this
// header independent of JSON and D++ so both translation units can include it.
struct DiscordCommandHelpEntry {
    const char* command;
    const char* description;
};

inline constexpr const char* kDiscordCommandHelpAccess =
    "Commands and review actions require your Discord user ID in Discord > "
    "Bot > admin/developer whitelist. The ! commands work anywhere the bot "
    "can read. Ticket lists include only projects with Discord tickets enabled.";

inline constexpr DiscordCommandHelpEntry kDiscordCommandHelp[] = {
    {"!xahelp", "Show this command guide."},
    {"!tickets", "Open the interactive project/ticket menu. Use number reactions to select a project; X closes the menu."},
    {"!xatickets", "Show a summary of open tickets."},
    {"!xatickets search text", "Search active ticket titles and descriptions. Example: !xatickets submarine. Use arrow reactions to change pages."},
    {"!leaderboard", "Show the top 10 community contributors."},
    {"@bot suggestion text", "Submit feedback in a monitored channel. Direct submissions in a project-mapped channel can auto-promote; otherwise choose a project and approve in DevHub."},
    {"Reply to a message with @bot", "Capture the replied-to message in a monitored channel. Extra reply text becomes a note; an existing ticket receives a follow-up instead of a duplicate."},
    {"Pending approval card reactions", "Warning = approve high priority; check mark = approve normal; sleeping face = approve low; X = reject."},
    {"Reply to a pending approval card", "Save text, files, or images and approve when a project is assigned or mapped. Without a project, evidence stays pending. A bot mention is optional."},
    {"Reply to a promoted ticket card", "Append text, files, or images to the linked ticket. A bot mention is optional."},
};

inline constexpr const char* kDiscordCommandHelpMention =
    "@bot means an actual Discord mention of your connected bot (for example, @FixIt).";

inline std::string discordCommandHelpText() {
    std::string text = kDiscordCommandHelpAccess;
    for (const auto& entry : kDiscordCommandHelp) {
        text += "\n\n";
        text += entry.command;
        text += " - ";
        text += entry.description;
    }
    text += "\n\n";
    text += kDiscordCommandHelpMention;
    return text;
}

} // namespace devhub
