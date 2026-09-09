#include "Db.h"
#include "Ingest.h"
#include "devhub/Util.h"

#include <algorithm>
#include <utility>

namespace devhub {

std::vector<TicketMenuEntry> ticketMenuProjects(Db* db) {
    std::vector<TicketMenuEntry> out;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT p.id, p.name, COUNT(i.id) "
        "FROM projects p "
        "JOIN items i ON i.project_id=p.id "
        " AND i.status IN ('open','in_progress','blocked') "
        " AND i.type IN ('fix','implementation') "
        "WHERE p.archived=0 AND p.discord_tickets=1 "
        "GROUP BY p.id, p.name HAVING COUNT(i.id)>0 "
        "ORDER BY p.name COLLATE NOCASE");
    while (q.executeStep()) {
        TicketMenuEntry entry;
        entry.projectId = q.getColumn(0).getInt64();
        entry.name = q.getColumn(1).getString();
        entry.openCount = q.getColumn(2).getInt();
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<TicketLine> ticketTitlesForProject(Db* db, long long projectId) {
    std::vector<TicketLine> out;
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT type, title FROM items WHERE project_id=? "
        "AND status IN ('open','in_progress','blocked') "
        "AND type IN ('fix','implementation') "
        "ORDER BY CASE type WHEN 'fix' THEN 0 ELSE 1 END, id");
    q.bind(1, projectId);
    while (q.executeStep()) {
        TicketLine line;
        line.type = q.getColumn(0).getString();
        line.title = q.getColumn(1).getString();
        out.push_back(std::move(line));
    }
    return out;
}

TicketSearchResult searchActiveTicketTitles(
    Db* db, const std::string& query, std::size_t maxResults) {
    TicketSearchResult out;
    const std::string needle = trim(query);
    if (needle.empty() || maxResults == 0) return out;
    constexpr std::size_t kMaximumResults = 500;
    const int boundedResults = static_cast<int>(
        std::min(maxResults, kMaximumResults));

    auto lk = db->guard();
    SQLite::Statement count(db->raw(lk.token()), R"sql(
SELECT COUNT(*)
FROM items i JOIN projects p ON p.id=i.project_id
WHERE p.archived=0 AND p.discord_tickets=1
 AND i.status IN ('open','in_progress','blocked')
 AND i.type IN ('fix','implementation')
 AND (instr(lower(i.title),lower(?))>0
      OR instr(lower(IFNULL(i.body,'')),lower(?))>0))sql");
    count.bind(1, needle);
    count.bind(2, needle);
    if (count.executeStep())
        out.totalMatches = static_cast<std::size_t>(
            count.getColumn(0).getInt64());

    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT i.id,p.name,i.title
FROM items i JOIN projects p ON p.id=i.project_id
WHERE p.archived=0 AND p.discord_tickets=1
 AND i.status IN ('open','in_progress','blocked')
 AND i.type IN ('fix','implementation')
 AND (instr(lower(i.title),lower(?))>0
      OR instr(lower(IFNULL(i.body,'')),lower(?))>0)
ORDER BY CASE WHEN instr(lower(i.title),lower(?))>0 THEN 0 ELSE 1 END,
 p.name COLLATE NOCASE,i.id
LIMIT ?)sql");
    q.bind(1, needle);
    q.bind(2, needle);
    q.bind(3, needle);
    q.bind(4, boundedResults);
    while (q.executeStep()) {
        TicketSearchEntry entry;
        entry.itemId = q.getColumn(0).getInt64();
        entry.projectName = q.getColumn(1).getString();
        entry.title = q.getColumn(2).getString();
        out.entries.push_back(std::move(entry));
    }
    return out;
}

static std::size_t validUtf8CharacterWidth(
    const std::string& value, std::size_t offset) {
    const unsigned char lead = static_cast<unsigned char>(value[offset]);
    std::size_t width = 1;
    if ((lead & 0xe0) == 0xc0) width = 2;
    else if ((lead & 0xf0) == 0xe0) width = 3;
    else if ((lead & 0xf8) == 0xf0) width = 4;
    if (width == 1 || offset + width > value.size()) return 1;
    for (std::size_t i = 1; i < width; ++i) {
        if ((static_cast<unsigned char>(value[offset + i]) & 0xc0) != 0x80)
            return 1;
    }
    return width;
}

std::string ticketSearchTitlePreview(
    const std::string& title, std::size_t maxCharacters) {
    if (maxCharacters == 0) return {};
    std::string clean = normalizeUtf8(title);
    for (char& ch : clean) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) ch = ' ';
    }
    clean = trim(clean);

    std::size_t cursor = 0;
    std::size_t characters = 0;
    while (cursor < clean.size() && characters < maxCharacters) {
        cursor += validUtf8CharacterWidth(clean, cursor);
        ++characters;
    }
    if (cursor == clean.size()) return clean;

    if (maxCharacters == 1) return "\xE2\x80\xA6";
    cursor = 0;
    characters = 0;
    while (cursor < clean.size() && characters < maxCharacters - 1) {
        cursor += validUtf8CharacterWidth(clean, cursor);
        ++characters;
    }
    return clean.substr(0, cursor) + "\xE2\x80\xA6";
}

std::vector<std::string> ticketSearchPages(
    const std::vector<TicketSearchEntry>& entries, std::size_t pageSize,
    std::size_t maxTitleCharacters) {
    std::vector<std::string> pages;
    if (entries.empty() || pageSize == 0 || maxTitleCharacters == 0)
        return pages;
    for (std::size_t offset = 0; offset < entries.size();
         offset += pageSize) {
        std::string page;
        const std::size_t finish = std::min(entries.size(), offset + pageSize);
        for (std::size_t i = offset; i < finish; ++i) {
            page += "I" + std::to_string(entries[i].itemId) + " | " +
                    ticketSearchTitlePreview(entries[i].projectName, 30) +
                    " | " + ticketSearchTitlePreview(
                        entries[i].title, maxTitleCharacters) + "\n";
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

} // namespace devhub
