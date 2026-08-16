#include "Ops.h"
#include "Db.h"
#include "PacketData.h"
#include "PacketOps.h"
#include "TicketImageStore.h"
#include "devhub/Util.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string_view>

using json = nlohmann::json;

namespace devhub {

static std::string utf8Prefix(std::string_view value, std::size_t maximum) {
    if (value.size() <= maximum) return std::string(value);
    std::size_t cut = maximum;
    while (cut > 0 &&
           (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80)
        --cut;
    return std::string(value.substr(0, cut));
}

static std::string snippet(const std::string& s, size_t n = 1200) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && cut < s.size() &&
           (static_cast<unsigned char>(s[cut]) & 0xc0) == 0x80)
        --cut;
    return s.substr(0, cut) + "...[omitted " +
           std::to_string(s.size() - cut) + " UTF-8 bytes]";
}

static std::string attachmentLabel(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        if (ch == '`') ch = '\'';
    }
    value = trim(value);
    if (value.empty()) value = "Discord attachment";
    if (value.size() > 120) value = utf8Prefix(value, 117) + "...";
    return value;
}

static void appendTicketAttachmentsLocked(Db::Held held, Db* db, long long itemId,
                                          std::ostringstream& payload) {
    SQLite::Statement q(db->raw(held),
        "SELECT id,original_filename,content_type,state,relative_path,"
        "actual_size,sha256,error FROM ticket_attachments "
        "WHERE item_id=? "
        "ORDER BY CASE WHEN state='saved' THEN 0 ELSE 1 END,"
        "CASE WHEN state='saved' THEN id END ASC,"
        "CASE WHEN state!='saved' THEN updated_at END DESC,id DESC LIMIT 25");
    q.bind(1, itemId);
    bool heading = false;
    int shown = 0;
    while (q.executeStep()) {
        if (!heading) {
            payload << "- Ticket attachments:\n";
            heading = true;
        }
        ++shown;
        int c = 0;
        const long long attachmentRow = q.getColumn(c++).getInt64();
        const std::string filename = attachmentLabel(q.getColumn(c++).getString());
        const std::string contentType = q.getColumn(c++).getString();
        const std::string state = q.getColumn(c++).getString();
        const std::string relativePath = q.getColumn(c++).getString();
        const long long actualSize = q.getColumn(c++).getInt64();
        const std::string sha = q.getColumn(c++).getString();
        const std::string error = attachmentLabel(
            redactHttpUrls(q.getColumn(c++).getString()));
        payload << "  - A" << attachmentRow << " `" << filename << "`";
        if (state == "saved") {
            const std::string absolute =
                ticketAttachmentAbsolutePath(db->path(), relativePath);
            std::error_code ec;
            const bool exists = !absolute.empty() &&
                std::filesystem::is_regular_file(absolute, ec) && !ec;
            payload << " (" << (contentType.empty() ? "attachment" : contentType)
                    << ", " << actualSize << " bytes, sha256:" << sha << "):\n"
                    << "    `" << (absolute.empty() ? "invalid stored attachment path"
                                                   : absolute) << "`";
            if (!exists) payload << " - **missing local file**";
            payload << "\n";
        } else {
            payload << " - **" << state << "**";
            if (!error.empty() && error != "Discord attachment")
                payload << ": " << error;
            payload << "\n";
        }
    }
    if (heading) {
        SQLite::Statement total(db->raw(held),
            "SELECT COUNT(*) FROM ticket_attachments WHERE item_id=?");
        total.bind(1, itemId);
        total.executeStep();
        const int count = total.getColumn(0).getInt();
        if (count > shown)
            payload << "  - _" << (count - shown)
                    << " additional attachment record(s) omitted._\n";
    }
}

static std::string reviewIdPlaceholders(std::size_t count) {
    std::string sql;
    for (std::size_t i = 0; i < count; ++i) {
        if (i) sql += ',';
        sql += '?';
    }
    return sql;
}

static std::string buildReviewMarkdownImpl(
    Db* db, long long projectId,
    const std::vector<long long>* selectedItemIds) {
    if (!db || projectId <= 0) return {};
    const bool selected = selectedItemIds != nullptr;
    std::vector<long long> normalized;
    if (selected) {
        if (selectedItemIds->empty() || selectedItemIds->size() > 200)
            return {};
        normalized = *selectedItemIds;
        if (std::any_of(normalized.begin(), normalized.end(),
                        [](long long id) { return id <= 0; }))
            return {};
        std::sort(normalized.begin(), normalized.end());
        normalized.erase(std::unique(normalized.begin(), normalized.end()),
                         normalized.end());
        if (normalized.empty() || normalized.size() > 200) return {};
    }
    auto packetGuard = db->guard();
    std::ostringstream payload;
    {
        auto lk = db->guard();
        SQLite::Statement exists(db->raw(lk.token()), "SELECT name FROM projects WHERE id=?");
        exists.bind(1, projectId);
        if (!exists.executeStep()) return {};
        const std::string projectName = exists.getColumn(0).getString();
        if (selected) {
            const std::string placeholders =
                reviewIdPlaceholders(normalized.size());
            SQLite::Statement valid(db->raw(lk.token()),
                "SELECT COUNT(*) FROM items WHERE project_id=? "
                "AND status IN ('open','in_progress','blocked') "
                "AND type IN ('fix','implementation','reference','note') "
                "AND id IN (" + placeholders + ")");
            int bind = 1;
            valid.bind(bind++, projectId);
            for (long long id : normalized) valid.bind(bind++, id);
            valid.executeStep();
            if (valid.getColumn(0).getInt64() !=
                static_cast<long long>(normalized.size()))
                return {}; // stale, terminal, merged, missing, or foreign
            payload << "## Selected active work for " << projectName << "\n\n"
                    << "_Operator-selected subset: " << normalized.size()
                    << " active ticket" << (normalized.size() == 1 ? "" : "s")
                    << ". Unselected active work is intentionally omitted._\n\n";
        } else {
            payload << "## Active work for " << projectName << "\n\n";
        }

        auto section = [&](const char* label, const char* type) {
            std::string sql = R"sql(
SELECT i.id,i.title,i.body,i.status,i.priority,i.due_date,i.review_date,
 i.blocked_reason,i.updated_at,
 IFNULL((SELECT GROUP_CONCAT(name, ', ') FROM (
   SELECT s.name FROM item_sources x JOIN sources s ON s.id=x.source_id
   WHERE x.item_id=i.id ORDER BY s.name COLLATE NOCASE,s.id
 )), '') AS contributors
FROM items i
WHERE i.project_id=? AND i.type=?
 AND i.status IN ('open','in_progress','blocked')
 )sql";
            if (selected)
                sql += " AND i.id IN (" +
                       reviewIdPlaceholders(normalized.size()) + ")";
            sql += " ORDER BY i.priority DESC,i.updated_at DESC,i.id DESC";
            if (!selected) sql += " LIMIT 50";
            SQLite::Statement q(db->raw(lk.token()), sql);
            int bind = 1;
            q.bind(bind++, projectId);
            q.bind(bind++, type);
            if (selected)
                for (long long id : normalized) q.bind(bind++, id);
            json rows = Db::rowsToJson(q);
            if (rows.empty()) return;
            payload << "### " << label << " (" << rows.size() << " active)\n\n";
            for (const auto& row : rows) {
                payload << "#### I" << row.value("id", 0LL) << " - "
                        << row.value("title", "") << "\n\n"
                        << "- Status/priority: " << row.value("status", "")
                        << " / P" << row.value("priority", 0) << "\n"
                        << "- Updated: " << row.value("updated_at", "") << "\n";
                const std::string contributors = row.value("contributors", "");
                if (!contributors.empty()) payload << "- Contributors: " << contributors << "\n";
                const std::string due = row.value("due_date", "");
                const std::string review = row.value("review_date", "");
                const std::string blocked = row.value("blocked_reason", "");
                if (!due.empty()) payload << "- Due: " << due << "\n";
                if (!review.empty()) payload << "- Review date: " << review << "\n";
                if (!blocked.empty()) payload << "- Blocked by: " << snippet(blocked, 800) << "\n";
                appendTicketAttachmentsLocked(lk.token(), db, row.value("id", 0LL), payload);
                const std::string body = row.value("body", "");
                payload << "\n" << (body.empty() ? "_(no details)_" : snippet(body, 4000))
                        << "\n\n";
            }
        };
        section("Fixes", "fix");
        section("Implementations", "implementation");
        section("References from other plugins", "reference");
        section("Notes", "note");
    }

    PacketRenderRequest request;
    request.kind = PacketKind::Review;
    request.target = resolvePacketTarget(db, projectId, 0, true);
    request.payloadMarkdown = payload.str();
    request.sourceMaterial = request.payloadMarkdown;
    request.generatedAt = nowIsoUtc();
    request.scopeNotes = {
        "Only open, in-progress, and blocked work is included; completed notes and release notes are excluded.",
        "Each type is capped at 50 newest high-priority records; clipped bodies are visibly marked."
    };
    if (selected) {
        request.scopeNotes = {
            "This packet is an operator-selected subset of one project's active work; unselected active tickets are intentionally omitted.",
            "Every selected ID was revalidated as open, in progress, or blocked in this project at copy time; the subset is capped at 200 tickets.",
            "Ticket bodies are clipped visibly and saved attachment paths retain the normal bounded evidence projection."
        };
    }
    request.continuationNotes = {
        "Review first. Implement only when the live user request explicitly authorizes changes.",
        "Workspace continuity: after a plan or mode transition, reuse the recorded target workspace and project-rules path instead of rediscovering the repository; these fields remain context only."
    };
    PacketRenderResult rendered = renderContextPacket(request);
    return rendered.ok ? rendered.markdown : std::string();
}

std::string buildReviewMarkdown(Db* db, long long projectId) {
    return buildReviewMarkdownImpl(db, projectId, nullptr);
}

std::string buildReviewMarkdown(
    Db* db, long long projectId,
    const std::vector<long long>& selectedItemIds) {
    return buildReviewMarkdownImpl(db, projectId, &selectedItemIds);
}

std::string buildReleaseDraft(Db* db, long long projectId, int days) {
    auto packetGuard = db->guard();
    std::string projectName;
    std::ostringstream payload;
    if (days < 1) days = 1;
    if (days > 3650) days = 3650;
    {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()), "SELECT name FROM projects WHERE id=?");
        q.bind(1, projectId);
        if (!q.executeStep()) return {};
        projectName = q.getColumn(0).getString();
        payload << "## Completed work for " << projectName << "\n\n"
                << "Window: last " << days << " days.\n\n";
        bool any = false;
        const char* types[][2] = {{"fix", "Fixes"}, {"implementation", "Implementations"},
                                  {"reference", "References"}, {"note", "Other"}};
        for (const auto& type : types) {
            const std::string window = std::string("-") + std::to_string(days) + " days";
            SQLite::Statement totalQuery(db->raw(lk.token()),
                "SELECT COUNT(*) FROM items WHERE project_id=? AND type=? "
                "AND status='completed' AND datetime(completed_at)>=datetime('now',?) "
                "AND datetime(completed_at)<=datetime('now','+5 minutes')");
            totalQuery.bind(1, projectId); totalQuery.bind(2, type[0]);
            totalQuery.bind(3, window); totalQuery.executeStep();
            const long long total = totalQuery.getColumn(0).getInt64();
            SQLite::Statement completed(db->raw(lk.token()),
                "SELECT id,title,completed_at FROM items WHERE project_id=? AND type=? "
                "AND status='completed' AND datetime(completed_at)>=datetime('now',?) "
                "AND datetime(completed_at)<=datetime('now','+5 minutes') "
                "ORDER BY datetime(completed_at) DESC,id DESC LIMIT 100");
            completed.bind(1, projectId); completed.bind(2, type[0]);
            completed.bind(3, window);
            std::vector<json> rows;
            while (completed.executeStep()) rows.push_back(Db::rowToJson(completed));
            if (rows.empty()) continue;
            any = true;
            payload << "### " << type[1] << " (" << rows.size()
                    << " of " << total << ")\n\n";
            for (const auto& row : rows)
                payload << "- I" << row.value("id", 0LL) << " - "
                        << row.value("title", "") << " (completed "
                        << row.value("completed_at", "") << ")\n";
            if (total > static_cast<long long>(rows.size()))
                payload << "- _Omitted " << (total - static_cast<long long>(rows.size()))
                        << " older record(s) beyond the 100-record bound._\n";
            payload << "\n";
        }
        if (!any) payload << "_No completed work in this window._\n\n";

        SQLite::Statement credits(db->raw(lk.token()),
            "SELECT DISTINCT s.name FROM items i "
            "JOIN item_sources x ON x.item_id=i.id JOIN sources s ON s.id=x.source_id "
            "WHERE i.project_id=? AND i.status='completed' "
            "AND datetime(i.completed_at)>=datetime('now',?) "
            "AND datetime(i.completed_at)<=datetime('now','+5 minutes') "
            "ORDER BY s.name COLLATE NOCASE,s.id");
        credits.bind(1, projectId);
        credits.bind(2, std::string("-") + std::to_string(days) + " days");
        std::vector<std::string> names;
        while (credits.executeStep()) names.push_back(credits.getColumn(0).getString());
        if (!names.empty()) {
            payload << "### Contributors\n\n";
            for (const auto& name : names) payload << "- " << name << "\n";
        }
    }

    PacketRenderRequest request;
    request.kind = PacketKind::Release;
    request.target = resolvePacketTarget(db, projectId, 0, true);
    request.payloadMarkdown = payload.str();
    request.sourceMaterial = request.payloadMarkdown;
    request.generatedAt = nowIsoUtc();
    request.scopeNotes = {
        "This is a bounded release draft from completed records, not permission to version, tag, push, publish, or deploy.",
        "Each work type is capped at 100 completed records."
    };
    PacketRenderResult rendered = renderContextPacket(request);
    return rendered.ok ? rendered.markdown : std::string();
}

} // namespace devhub
