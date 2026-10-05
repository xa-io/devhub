#include "GuiInternal.h"

namespace devhub {
// ---------------------------------------------------------------------------
static int dashboardCompletedPresetIndex(int days) {
    for (int i = 0; i < kDashboardCompletedPresetCount; ++i) {
        if (kDashboardCompletedPresetDays[i] == days) return i;
    }
    return 1; // 30 days is the fail-closed default for missing/invalid values.
}

static int normalizeDashboardCompletedDays(int days) {
    return kDashboardCompletedPresetDays[dashboardCompletedPresetIndex(days)];
}

void toast(App& a, const std::string& msg, bool err) {
    a.toastMsg = msg;
    a.toastErr = err;
    a.toastUntil = ImGui::GetTime() + 3.5;
    appLog(std::string(err ? "[ui] ERROR: " : "[ui] ") + msg);
}

bool copyPacket(App& a, const std::string& packet,
                const std::string& successMessage) {
    if (packet.empty()) {
        toast(a, "packet generation failed; clipboard was not changed", true);
        return false;
    }
    ImGui::SetClipboardText(packet.c_str());
    const char* copied = ImGui::GetClipboardText();
    if (!copied || packet != copied) {
        toast(a, "clipboard verification failed; packet was not confirmed", true);
        return false;
    }
    toast(a, successMessage);
    return true;
}

nlohmann::json nonEmptyLines(const char* value) {
    nlohmann::json result = nlohmann::json::array();
    std::set<std::string> seen;
    std::istringstream input(value ? value : "");
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (!line.empty() && seen.insert(toLower(line)).second)
            result.push_back(line);
    }
    return result;
}

static void openFolder(const std::string& path) {
    if (!path.empty())
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void openUrl(const std::string& url) {
    if (!url.empty())
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Word-wrapped body text with clickable links: tokens are laid out against
// the available width and http(s):// tokens render as accent-colored links
// (hover = underline + full-URL tooltip, click = default browser). Long URLs
// display shortened; the click always uses the full string. Needed because
// InputTextMultiline cannot word-wrap - edit popups read their text through
// this and keep the raw editor one tab away.
void drawBodyWithLinks(const std::string& text, const ImVec4& color) {
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const float wrapRight =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        std::string line = text.substr(
            lineStart, lineEnd == std::string::npos ? std::string::npos
                                                    : lineEnd - lineStart);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        bool first = true;
        size_t pos = 0;
        while (pos < line.size()) {
            size_t ws = line.find_first_of(" \t", pos);
            std::string word = line.substr(
                pos, ws == std::string::npos ? std::string::npos : ws - pos);
            pos = ws == std::string::npos ? line.size() : ws + 1;
            if (word.empty()) continue;

            // Links keep trailing punctuation out of the URL.
            std::string url, tail;
            if (word.rfind("http://", 0) == 0 || word.rfind("https://", 0) == 0) {
                size_t cut = word.find_last_not_of(".,;:)]>\"'");
                url = word.substr(0, cut + 1);
                tail = word.substr(cut + 1);
            }
            std::string shown = url.empty() ? word : url;
            if (!url.empty() && shown.size() > 64)
                shown = shown.substr(0, 44) + "..." +
                        shown.substr(shown.size() - 16);

            const float w = ImGui::CalcTextSize(shown.c_str()).x;
            if (!first) {
                ImGui::SameLine(0.0f, spaceW);
                if (ImGui::GetCursorPosX() + w > wrapRight) ImGui::NewLine();
            }
            first = false;

            if (url.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(word.c_str());
                ImGui::PopStyleColor();
                continue;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
            ImGui::TextUnformatted(shown.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f),
                    ImGui::GetColorU32(C_ACCENT));
                ImGui::SetTooltip("%s", url.c_str());
            }
            if (ImGui::IsItemClicked()) openUrl(url);
            if (!tail.empty()) {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(tail.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (first) ImGui::TextUnformatted(""); // blank source line

        if (lineEnd == std::string::npos) break;
        lineStart = lineEnd + 1;
    }
}

struct ItemAttachmentRow {
    long long id = 0;
    std::string filename, contentType, state, relativePath, sha256, error;
    long long actualSize = 0;
};

void drawItemAttachments(App& a, long long itemId) {
    std::vector<ItemAttachmentRow> rows;
    int total = 0;
    {
        auto lk = a.db->guard();
        SQLite::Statement count(a.db->raw(lk.token()),
            "SELECT COUNT(*) FROM ticket_attachments WHERE item_id=?");
        count.bind(1, itemId);
        count.executeStep();
        total = count.getColumn(0).getInt();
        if (total == 0) return;
        SQLite::Statement q(a.db->raw(lk.token()),
            "SELECT id,original_filename,content_type,state,relative_path,"
            "actual_size,sha256,error FROM ticket_attachments "
            "WHERE item_id=? "
            "ORDER BY CASE WHEN state='saved' THEN 0 ELSE 1 END,"
            "CASE WHEN state='saved' THEN id END ASC,"
            "CASE WHEN state!='saved' THEN updated_at END DESC,id DESC LIMIT 25");
        q.bind(1, itemId);
        while (q.executeStep()) {
            ItemAttachmentRow row;
            int c = 0;
            row.id = q.getColumn(c++).getInt64();
            row.filename = q.getColumn(c++).getString();
            row.contentType = q.getColumn(c++).getString();
            row.state = q.getColumn(c++).getString();
            row.relativePath = q.getColumn(c++).getString();
            row.actualSize = q.getColumn(c++).getInt64();
            row.sha256 = q.getColumn(c++).getString();
            // Older persisted failures may predate write-time URL scrubbing.
            // Redact again at this display boundary before the text reaches UI.
            row.error = redactHttpUrls(q.getColumn(c++).getString());
            rows.push_back(std::move(row));
        }
    }

    ImGui::SeparatorText("Ticket attachments");
    ImGui::TextColored(C_DIM,
        "Saved paths match the AI review bundle; queued/failed rows never claim a file.");
    ImGui::BeginChild("##itemTicketAttachments", ImVec2(0, 210),
                      ImGuiChildFlags_Borders);
    for (const ItemAttachmentRow& row : rows) {
        ImGui::PushID(static_cast<int>(row.id));
        const ImVec4& stateColor = row.state == "saved" ? C_GREEN
            : (row.state == "failed" ? C_RED : C_ORANGE);
        const std::string label = row.filename.empty()
            ? "Discord attachment" : row.filename;
        ImGui::TextColored(stateColor, "A%lld  %s  [%s]", row.id,
                           label.c_str(), row.state.c_str());
        if (row.state == "saved") {
            const std::string absolute =
                ticketAttachmentAbsolutePath(a.db->path(), row.relativePath);
            std::error_code ec;
            const bool exists = !absolute.empty() &&
                std::filesystem::is_regular_file(absolute, ec) && !ec;
            ImGui::PushStyleColor(ImGuiCol_Text, exists ? C_TEXT : C_RED);
            ImGui::TextWrapped("%s", absolute.empty()
                ? "invalid stored attachment path" : absolute.c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
            ImGui::TextWrapped("%s, %lld bytes, sha256:%s",
                row.contentType.empty() ? "attachment" : row.contentType.c_str(),
                row.actualSize, row.sha256.c_str());
            ImGui::PopStyleColor();
            if (!exists) ImGui::TextColored(C_RED, "missing local file");
            if (!absolute.empty()) {
                if (ImGui::SmallButton("copy path"))
                    ImGui::SetClipboardText(absolute.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("open folder"))
                    openFolder(std::filesystem::path(absolute).parent_path().string());
            }
        } else {
            if (!row.error.empty()) ImGui::TextWrapped("%s", row.error.c_str());
            if (row.state == "failed") {
                if (ImGui::SmallButton("retry save")) {
                    if (retryFailedTicketImage(a.db, a.discord, row.id))
                        toast(a, "attachment A" + std::to_string(row.id) +
                                  " queued for retry");
                    else
                        toast(a, "attachment retry was not applied", true);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Refetch this exact Discord attachment and run the same safety checks again.");
            }
        }
        ImGui::PopID();
    }
    if (total > static_cast<int>(rows.size()))
        ImGui::TextColored(C_DIM, "%d additional attachment record(s) omitted",
                           total - static_cast<int>(rows.size()));
    ImGui::EndChild();
}

static bool copyBufChecked(char* dst, size_t cap, const std::string& src) {
    if (cap == 0) return src.empty();
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    while (n > 0 && n < src.size() &&
           (static_cast<unsigned char>(src[n]) & 0xc0) == 0x80)
        --n;
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
    return n == src.size();
}

void copyBuf(char* dst, size_t cap, const std::string& src) {
    (void)copyBufChecked(dst, cap, src);
}

void loadPreservedText(char* dst, size_t cap, const std::string& src,
                       PreservedText& value) {
    value.original = src;
    value.truncated = !copyBufChecked(dst, cap, src);
}

std::string preservedText(const char* buffer, const PreservedText& value) {
    return value.truncated ? value.original : std::string(buffer ? buffer : "");
}

static void saveAppDisplayName(App& a, const char* configured) {
    std::string normalized, error;
    if (!normalizeAppDisplayName(configured ? configured : "", normalized, &error)) {
        toast(a, "visual display name: " + error, true);
        return;
    }
    {
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), kAppDisplayNameSetting, normalized);
    }
    copyBuf(a.appDisplayNameInput, sizeof(a.appDisplayNameInput), normalized);
    a.appDisplayName = normalized.empty() ? DEVHUB_APP_NAME : normalized;
    a.appDisplayNameInvalid = false;

    const std::wstring caption = widen(a.appDisplayName);
    const bool captionUpdated = !a.hwnd || SetWindowTextW(a.hwnd, caption.c_str());
    if (!captionUpdated) {
        toast(a, "display name saved; Windows title update failed", true);
    } else {
        toast(a, normalized.empty()
            ? "visual display name reset to XA DevHub"
            : "visual display name saved");
    }
}

static std::string shortDate(const std::string& iso) { return iso.substr(0, 10); }
std::string shortTs(const std::string& iso) {
    std::string s = iso;
    if (s.size() >= 19) { s = s.substr(0, 16); s[10] = ' '; }
    return s;
}

std::string dateAfterDays(int days) {
    std::time_t t = std::time(nullptr) + static_cast<std::time_t>(days) * 86400;
    std::tm tm{}; localtime_s(&tm, &t);
    char buf[16]{}; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

bool activeItemStatus(const std::string& status) {
    return status == "open" || status == "in_progress" || status == "blocked";
}

Project* findProject(App& a, long long id) {
    for (auto& p : a.projects) if (p.id == id) return &p;
    return nullptr;
}

int projectIndexForId(const App& a, long long id) {
    if (id <= 0) return -1;
    for (size_t i = 0; i < a.projects.size(); ++i)
        if (a.projects[i].id == id) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// data loading
// ---------------------------------------------------------------------------
static void loadProjects(App& a) {
    a.projects.clear();
    a.activity.clear();
    auto lk = a.db->guard();
    {
        SQLite::Statement q(a.db->raw(lk.token()), R"sql(
SELECT p.id, p.name, p.description, p.path, p.rules_path, p.codex_skills, p.build_cwd,
 p.build_command, p.prep_command, p.release_command, p.version_command,
 p.local_version_file, p.remote_version_url, p.remote_version_key,
 p.github_url, p.aliases, p.release_notes, p.archived, p.discord_tickets,
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status IN ('open','in_progress','blocked') AND i.type='fix'),
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status IN ('open','in_progress','blocked') AND i.type='implementation'),
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status IN ('open','in_progress','blocked') AND i.type NOT IN ('fix','implementation')),
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status='completed'),
 IFNULL((SELECT b.status FROM builds b WHERE b.project_id=p.id ORDER BY b.id DESC LIMIT 1),''),
 IFNULL((SELECT b.kind FROM builds b WHERE b.project_id=p.id ORDER BY b.id DESC LIMIT 1),''),
 IFNULL((SELECT b.started_at FROM builds b WHERE b.project_id=p.id ORDER BY b.id DESC LIMIT 1),'')
FROM projects p ORDER BY p.archived, p.sort_order, p.name COLLATE NOCASE)sql");
        while (q.executeStep()) {
            Project p;
            int c = 0;
            p.id = q.getColumn(c++).getInt64();
            p.name = q.getColumn(c++).getString();
            p.desc = q.getColumn(c++).getString();
            p.path = q.getColumn(c++).getString();
            p.rules = q.getColumn(c++).getString();
            p.codexSkills = q.getColumn(c++).getString();
            p.buildCwd = q.getColumn(c++).getString();
            p.buildCmd = q.getColumn(c++).getString();
            p.prepCmd = q.getColumn(c++).getString();
            p.releaseCmd = q.getColumn(c++).getString();
            p.versionCmd = q.getColumn(c++).getString();
            p.localVerFile = q.getColumn(c++).getString();
            p.remoteUrl = q.getColumn(c++).getString();
            p.remoteKey = q.getColumn(c++).getString();
            p.githubUrl = q.getColumn(c++).getString();
            p.aliases = q.getColumn(c++).getString();
            p.releaseNotes = q.getColumn(c++).getString();
            p.archived = q.getColumn(c++).getInt();
            p.discordTickets = q.getColumn(c++).getInt();
            p.openFix = q.getColumn(c++).getInt64();
            p.openImpl = q.getColumn(c++).getInt64();
            p.openOther = q.getColumn(c++).getInt64();
            p.done = q.getColumn(c++).getInt64();
            p.lastBuildStatus = q.getColumn(c++).getString();
            p.lastBuildKind = q.getColumn(c++).getString();
            p.lastBuildAt = q.getColumn(c++).getString();
            a.projects.push_back(std::move(p));
        }
    }
    {
        // App activity plus every captured Discord message still sitting in
        // the inbox. Captures drop out when promoted, dismissed, or replaced
        // by a scrubbed terminal deletion marker.
        SQLite::Statement q(a.db->raw(lk.token()), R"sql(
SELECT ts, kind, project, detail, mid FROM (
 SELECT a.ts AS ts, a.kind AS kind, IFNULL(p.name,'') AS project,
        a.detail AS detail, 0 AS mid
 FROM activity_log a LEFT JOIN projects p ON p.id=a.project_id
 WHERE a.kind!='discord_manual_capture'
 UNION ALL
 SELECT m.ingested_at, 'discord '||m.kind, IFNULL(p2.name,''),
        m.author||' in #'||IFNULL(NULLIF(c.channel_name,''),c.channel_id)||
        ': '||substr(replace(m.content,char(10),' '),1,140), m.id
 FROM discord_messages m
 JOIN discord_channels c ON c.id=m.channel_row_id
 LEFT JOIN projects p2 ON p2.id=COALESCE(c.project_id,m.inferred_project_id)
 WHERE m.state='new' AND m.kind!='none'
) ORDER BY ts DESC LIMIT 30)sql");
        while (q.executeStep())
            a.activity.push_back({q.getColumn(0).getString(),
                                  q.getColumn(1).getString(),
                                  q.getColumn(2).getString(),
                                  q.getColumn(3).getString(),
                                  q.getColumn(4).getInt64()});
    }
    {
        SQLite::Statement q(a.db->raw(lk.token()),
            "SELECT COUNT(*) FROM discord_messages WHERE state='new' AND kind!='none'");
        q.executeStep();
        a.statPending = q.getColumn(0).getInt64();
    }
    {
        SQLite::Statement q(a.db->raw(lk.token()),
            "SELECT COUNT(*) FROM item_sources x JOIN items i ON i.id=x.item_id "
            "WHERE i.status='completed' AND x.credited=0");
        q.executeStep();
        a.statUncredited = q.getColumn(0).getInt64();
    }
    {
        a.dashboardCompletedDays =
            normalizeDashboardCompletedDays(a.dashboardCompletedDays);
        std::time_t t = std::time(nullptr) -
            static_cast<std::time_t>(a.dashboardCompletedDays - 1) * 86400;
        std::tm tm{};
        gmtime_s(&tm, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        SQLite::Statement q(a.db->raw(lk.token()),
            "SELECT COUNT(*) FROM items WHERE status='completed' AND completed_at>=?");
        q.bind(1, buf);
        q.executeStep();
        a.statCompletedWindow = q.getColumn(0).getInt64();
    }
    {
        SQLite::Statement q(a.db->raw(lk.token()),
            "SELECT COUNT(*) FROM items i JOIN projects p ON p.id=i.project_id "
            "WHERE p.archived=0 AND i.status IN ('open','in_progress','blocked') "
            "AND i.due_date!='' AND i.due_date<?");
        q.bind(1, todayLocal());
        q.executeStep();
        a.statOverdue = q.getColumn(0).getInt64();
    }
    // Project order is mutable (archive, rename, sort, add, delete). Rebuild
    // every display index from its durable project id after each cache reload.
    for (auto& message : a.inbox) {
        message.selProject = projectIndexForId(a, message.selProjectId);
        if (message.selProject < 0) message.selProjectId = 0;
    }
    if (a.editPendingMessageId != 0) {
        a.editPendingProject =
            projectIndexForId(a, a.editPendingProjectId);
        if (a.editPendingProject < 0) a.editPendingProjectId = 0;
    }
    a.needProjects = false;
}

static void loadItems(App& a) {
    a.items.clear();
    if (a.selProject <= 0) { a.needItems = false; return; }
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()),
        "SELECT i.id, i.project_id, i.type, i.title, i.body, i.status, "
        "IFNULL(GROUP_CONCAT(s.name, ', '),''), i.origin, i.due_date, i.created_at, i.tags, "
        "i.priority, CASE WHEN COUNT(x.source_id)>0 AND "
        "IFNULL(SUM(CASE WHEN x.credited=0 THEN 1 ELSE 0 END),0)=0 THEN 1 ELSE 0 END, "
        "i.updated_at, i.review_date, i.blocked_reason, "
        "COUNT(x.source_id), IFNULL(SUM(CASE WHEN x.credited=0 THEN 1 ELSE 0 END),0), "
        "CASE WHEN i.status='completed' THEN "
        "IFNULL(strftime('%Y-%m-%dT%H:%M:%fZ', i.completed_at),'') ELSE '' END "
        "FROM items i LEFT JOIN item_sources x ON x.item_id=i.id "
        "LEFT JOIN sources s ON s.id=x.source_id WHERE i.project_id=? GROUP BY i.id "
        "ORDER BY CASE i.status WHEN 'in_progress' THEN 0 WHEN 'open' THEN 1 "
        "WHEN 'blocked' THEN 2 WHEN 'completed' THEN 3 ELSE 4 END, i.priority DESC, i.id DESC");
    q.bind(1, a.selProject);
    while (q.executeStep()) {
        Item it;
        int c = 0;
        it.id = q.getColumn(c++).getInt64();
        it.projectId = q.getColumn(c++).getInt64();
        it.type = q.getColumn(c++).getString();
        it.title = normalizeUtf8(q.getColumn(c++).getString());
        it.body = normalizeUtf8(q.getColumn(c++).getString());
        it.status = q.getColumn(c++).getString();
        it.source = q.getColumn(c++).getString();
        it.origin = q.getColumn(c++).getString();
        it.due = q.getColumn(c++).getString();
        it.created = q.getColumn(c++).getString();
        it.tags = q.getColumn(c++).getString();
        it.priority = q.getColumn(c++).getInt();
        it.credited = q.getColumn(c++).getInt();
        it.updated = q.getColumn(c++).getString();
        it.reviewDate = q.getColumn(c++).getString();
        it.blockedReason = q.getColumn(c++).getString();
        it.sourceCount = q.getColumn(c++).getInt();
        it.uncreditedCount = q.getColumn(c++).getInt();
        it.completedAt = q.getColumn(c++).getString();
        a.items.push_back(std::move(it));
    }
    a.needItems = false;
}

static void loadWorkItems(App& a) {
    a.workItems.clear();
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()), R"sql(
SELECT i.id,i.project_id,p.name,i.type,i.title,i.body,i.status,
 IFNULL(GROUP_CONCAT(s.name, ', '),''),i.origin,i.due_date,i.created_at,i.tags,
 i.priority,CASE WHEN COUNT(x.source_id)>0 AND
 IFNULL(SUM(CASE WHEN x.credited=0 THEN 1 ELSE 0 END),0)=0 THEN 1 ELSE 0 END,
 i.updated_at,i.review_date,i.blocked_reason,
 COUNT(x.source_id),IFNULL(SUM(CASE WHEN x.credited=0 THEN 1 ELSE 0 END),0),
 CASE WHEN julianday('now')-julianday(i.updated_at)>=? THEN 1 ELSE 0 END,
 CASE WHEN i.due_date!='' AND i.due_date<? THEN 1 ELSE 0 END,
 CASE WHEN i.review_date!='' AND i.review_date<=? THEN 1 ELSE 0 END
FROM items i JOIN projects p ON p.id=i.project_id
LEFT JOIN item_sources x ON x.item_id=i.id LEFT JOIN sources s ON s.id=x.source_id
WHERE p.archived=0 AND i.status IN ('open','in_progress','blocked')
GROUP BY i.id)sql");
    q.bind(1, a.staleDays); q.bind(2, todayLocal()); q.bind(3, todayLocal());
    while (q.executeStep()) {
        Item it; int c = 0;
        it.id=q.getColumn(c++).getInt64(); it.projectId=q.getColumn(c++).getInt64();
        it.project=q.getColumn(c++).getString(); it.type=q.getColumn(c++).getString();
        it.title=q.getColumn(c++).getString(); it.body=q.getColumn(c++).getString();
        it.status=q.getColumn(c++).getString(); it.source=q.getColumn(c++).getString();
        it.origin=q.getColumn(c++).getString(); it.due=q.getColumn(c++).getString();
        it.created=q.getColumn(c++).getString(); it.tags=q.getColumn(c++).getString();
        it.priority=q.getColumn(c++).getInt(); it.credited=q.getColumn(c++).getInt();
        it.updated=q.getColumn(c++).getString(); it.reviewDate=q.getColumn(c++).getString();
        it.blockedReason=q.getColumn(c++).getString(); it.sourceCount=q.getColumn(c++).getInt();
        it.uncreditedCount=q.getColumn(c++).getInt(); it.stale=q.getColumn(c++).getInt();
        it.overdue=q.getColumn(c++).getInt(); it.reviewDue=q.getColumn(c++).getInt();
        a.workItems.push_back(std::move(it));
    }
    a.needWork = false;
}

static void loadSources(App& a) {
    a.sources.clear();
    a.leaderboard.clear();
    a.sourceItems.clear();
    std::vector<long long> excluded;
    a.leaderboardConfigError.clear();
    a.leaderboardConfigValid = loadLeaderboardExcludedSourceIds(
        a.db, excluded, &a.leaderboardConfigError);
    {
        auto lk = a.db->guard();
        SQLite::Statement q(a.db->raw(lk.token()), R"sql(
SELECT s.id, s.name, s.platform,
 (SELECT COUNT(*) FROM item_sources x WHERE x.source_id=s.id),
 (SELECT COUNT(*) FROM item_sources x JOIN items i ON i.id=x.item_id
  WHERE x.source_id=s.id AND i.status='completed'),
 (SELECT COUNT(*) FROM item_sources x JOIN items i ON i.id=x.item_id
  WHERE x.source_id=s.id AND i.status='completed' AND x.credited=0)
FROM sources s
WHERE NOT EXISTS(SELECT 1 FROM source_merges m WHERE m.source_id=s.id)
ORDER BY 4 DESC, s.name COLLATE NOCASE)sql");
        while (q.executeStep()) {
            Source s;
            s.id = q.getColumn(0).getInt64();
            s.name = q.getColumn(1).getString();
            s.platform = q.getColumn(2).getString();
            s.items = q.getColumn(3).getInt64();
            s.completed = q.getColumn(4).getInt64();
            s.uncredited = q.getColumn(5).getInt64();
            s.leaderboardVisible = a.leaderboardConfigValid &&
                !std::binary_search(excluded.begin(), excluded.end(), s.id);
            a.sources.push_back(std::move(s));
        }
    }
    std::string leaderboardError;
    a.leaderboard = contributorLeaderboard(a.db, 10, &leaderboardError);
    if (!leaderboardError.empty()) {
        a.leaderboardConfigValid = false;
        a.leaderboardConfigError = leaderboardError;
    }
    a.needSources = false;
}

void openReviewSelection(App& a, long long projectId) {
    a.reviewFor = 0;
    a.reviewSearch[0] = '\0';
    a.reviewChoices.clear();
    if (projectId <= 0) return;
    auto lk = a.db->guard();
    SQLite::Statement project(a.db->raw(lk.token()), "SELECT 1 FROM projects WHERE id=?");
    project.bind(1, projectId);
    if (!project.executeStep()) return;
    for (const char* type : kTypeNames) {
        SQLite::Statement q(a.db->raw(lk.token()), R"sql(
SELECT id,type,title,body,status,priority FROM items
 WHERE project_id=? AND type=?
   AND status IN ('open','in_progress','blocked')
 ORDER BY priority DESC,updated_at DESC,id DESC LIMIT 50
)sql");
        q.bind(1, projectId);
        q.bind(2, type);
        while (q.executeStep()) {
            ReviewChoice choice;
            choice.id = q.getColumn(0).getInt64();
            choice.type = q.getColumn(1).getString();
            choice.title = normalizeUtf8(q.getColumn(2).getString());
            choice.body = normalizeUtf8(q.getColumn(3).getString());
            choice.status = q.getColumn(4).getString();
            choice.priority = q.getColumn(5).getInt();
            choice.selected = true;
            a.reviewChoices.push_back(std::move(choice));
        }
    }
    a.reviewFor = projectId;
}

static void loadChannels(App& a) {
    a.channels.clear();
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()), R"sql(
SELECT c.id, c.channel_id, c.guild_name, c.channel_name, c.last_read_ts,
 c.last_scan_at, IFNULL(c.project_id,0), c.enabled,
 (SELECT COUNT(*) FROM discord_messages m WHERE m.channel_row_id=c.id),
 (SELECT COUNT(*) FROM discord_messages m WHERE m.channel_row_id=c.id AND m.state='new' AND m.kind!='none')
FROM discord_channels c ORDER BY c.guild_name, c.channel_name)sql");
    while (q.executeStep()) {
        Channel c;
        c.id = q.getColumn(0).getInt64();
        c.channelId = q.getColumn(1).getString();
        c.guild = q.getColumn(2).getString();
        c.name = q.getColumn(3).getString();
        c.lastRead = q.getColumn(4).getString();
        c.lastScan = q.getColumn(5).getString();
        c.projectId = q.getColumn(6).getInt64();
        c.enabled = q.getColumn(7).getInt();
        c.msgs = q.getColumn(8).getInt64();
        c.pending = q.getColumn(9).getInt64();
        a.channels.push_back(std::move(c));
    }
    a.needChannels = false;
}

static void loadInbox(App& a) {
    // Preserve in-progress categorization (project/type picks the user made
    // but hasn't promoted yet) across live refreshes.
    std::map<long long, std::pair<long long, int>> pending;
    for (auto& m : a.inbox) pending[m.id] = {m.selProjectId, m.selType};
    a.inbox.clear();
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()),
        "SELECT m.id, m.author, m.content, m.posted_at, m.kind, m.matched, m.score, "
        "IFNULL(c.project_id,0), IFNULL(c.channel_name,''), "
        "IFNULL(m.inferred_project_id,0), c.guild_id, c.channel_id, m.message_id, "
        "IFNULL(m.admin_note,''), "
        "(SELECT COUNT(*) FROM ticket_attachments t "
        " WHERE t.discord_message_row_id=m.id AND t.state='captured') "
        "FROM discord_messages m JOIN discord_channels c ON c.id=m.channel_row_id "
        "WHERE m.state='new' AND m.kind!='none' ORDER BY m.posted_at DESC LIMIT 200");
    while (q.executeStep()) {
        Msg m;
        m.id = q.getColumn(0).getInt64();
        m.author = q.getColumn(1).getString();
        m.content = q.getColumn(2).getString();
        m.posted = q.getColumn(3).getString();
        m.kind = q.getColumn(4).getString();
        m.matched = q.getColumn(5).getString();
        m.score = q.getColumn(6).getDouble();
        m.channelProjectId = q.getColumn(7).getInt64();
        m.channelName = q.getColumn(8).getString();
        m.inferredProjectId = q.getColumn(9).getInt64();
        m.guildId = q.getColumn(10).getString();
        m.channelId = q.getColumn(11).getString();
        m.messageId = q.getColumn(12).getString();
        m.adminNote = q.getColumn(13).getString();
        m.attachmentCount = q.getColumn(14).getInt();
        m.selType = (m.kind == "bug") ? 1 : 0;
        auto it = pending.find(m.id);
        if (it != pending.end()) {
            m.selProjectId = it->second.first;
            m.selType = it->second.second;
        } else {
            m.selProjectId = m.channelProjectId > 0
                ? m.channelProjectId : m.inferredProjectId;
        }
        m.selProject = projectIndexForId(a, m.selProjectId);
        if (m.selProject < 0 && !a.needProjects) m.selProjectId = 0;
        a.inbox.push_back(std::move(m));
    }
    a.needInbox = false;
}

static void loadGuilds(App& a) {
    a.guilds.clear();
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()),
        "SELECT id, guild_id, guild_name, enabled FROM discord_guilds "
        "ORDER BY guild_name, guild_id");
    while (q.executeStep()) {
        Guild g;
        g.id = q.getColumn(0).getInt64();
        g.guildId = q.getColumn(1).getString();
        g.name = q.getColumn(2).getString();
        g.enabled = q.getColumn(3).getInt();
        a.guilds.push_back(std::move(g));
    }
    a.needGuilds = false;
}

static void loadCalendar(App& a) {
    a.calendar.clear();
    char from[16], to[16];
    std::snprintf(from, sizeof(from), "%04d-%02d-01", a.calYear, a.calMonth + 1);
    std::snprintf(to, sizeof(to), "%04d-%02d-31", a.calYear, a.calMonth + 1);
    nlohmann::json entries = calendarEntries(a.db, from, to);
    auto numField = [](nlohmann::json& e, const char* key) -> long long {
        return (e.contains(key) && e[key].is_number()) ? e[key].get<long long>()
                                                       : 0;
    };
    auto strField = [](nlohmann::json& e, const char* key) -> std::string {
        return (e.contains(key) && e[key].is_string())
                   ? e[key].get<std::string>() : std::string();
    };
    for (auto& e : entries) {
        CalEntry ce;
        ce.date = e.value("date", "");
        ce.title = e.value("title", "");
        ce.kind = e.value("kind", "event");
        ce.entry = e.value("entry", "");
        ce.project = strField(e, "project_name");
        if (ce.entry == "event" && e.contains("id") && e["id"].is_number())
            ce.eventId = e["id"].get<long long>();
        ce.itemId = numField(e, "item_id");
        ce.buildId = numField(e, "build_id");
        ce.notes = strField(e, "notes");
        ce.created = strField(e, "created_at");
        ce.status = strField(e, "status");
        ce.itemType = strField(e, "type");
        ce.ts = strField(e, "started_at");
        ce.exitCode = e.contains("exit_code") && e["exit_code"].is_number()
                          ? e["exit_code"].get<int>() : -1;
        a.calendar[ce.date].push_back(std::move(ce));
    }
    a.needCal = false;
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
void launchAction(App& a, const Project& p, const std::string& kind,
                  const std::string& command, const std::string& stdinData) {
    std::string cwd = p.buildCwd.empty() ? p.path : p.buildCwd;
    std::string error;
    long long id = a.builds->start(p.id, kind, command, cwd, stdinData, error);
    if (id == 0) { toast(a, error, true); return; }
    a.console = Console{};
    a.console.open = true;
    a.console.buildId = id;
    a.console.title = p.name + " - " + kind;
    a.console.status = "running";
    a.needProjects = true;
    appLog("[build] started: " + p.name + " " + kind);
}

static void startAction(App& a, const Project& p, const std::string& kind) {
    std::string cmd = kind == "prep"      ? p.prepCmd
                      : kind == "release" ? p.releaseCmd
                      : kind == "version" ? p.versionCmd
                                          : p.buildCmd;
    if (trim(cmd).empty()) {
        toast(a, "no " + kind + " command set for " + p.name +
                 " - edit the project to add one", true);
        return;
    }
    if (a.builds->isRunning(p.id)) {
        toast(a, "an action is already running for " + p.name, true);
        return;
    }
    if (kind == "release") {
        a.releaseFor = p.id;
        a.releaseBuf[0] = 0;
    } else if (kind == "version") {
        a.versionFor = p.id;
        a.versionChoice = 0;
        VersionInfo vi = a.versions->get(p.id);
        copyBuf(a.versionBuf, sizeof(a.versionBuf),
                vi.local.empty() ? "0.0.0.0" : vi.local);
    } else {
        launchAction(a, p, kind, cmd, "\n\n\n");
    }
}

static void pollConsole(App& a) {
    if (!a.console.open || a.console.buildId == 0 || a.console.done) return;
    nlohmann::json s = a.builds->status(a.console.buildId, a.console.offset);
    if (s.contains("error")) { a.console.done = true; return; }
    const size_t nextOffset =
        s.value("next_offset", static_cast<size_t>(a.console.offset));
    if (s.value("truncated", false)) {
        const size_t retainedFrom = s.value("dropped_before", size_t{0});
        a.console.text +=
            "\n[devhub] live output before absolute byte " +
            std::to_string(retainedFrom) +
            " is no longer retained; streaming resumed at the current window.\n";
    }
    std::string chunk = s.value("output", "");
    if (!chunk.empty()) a.console.text += chunk;
    constexpr size_t kConsoleCap = 512 * 1024;
    if (a.console.text.size() > kConsoleCap) {
        size_t cut = a.console.text.size() - kConsoleCap;
        const size_t newline = a.console.text.find('\n', cut);
        if (newline != std::string::npos) cut = newline + 1;
        a.console.text.erase(0, cut);
        a.console.truncated = true;
    }
    // Offsets are absolute stream positions. Advance even for an empty chunk
    // so a ring-window clamp cannot leave the console polling a stale byte.
    a.console.offset = nextOffset;
    a.console.status = s.value("status", "running");
    if (s.contains("exit_code") && s["exit_code"].is_number())
        a.console.exitCode = s["exit_code"].get<int>();
    if (s.value("done", false)) {
        a.console.done = true;
        if (!a.console.reloaded) {
            a.console.reloaded = true;
            a.needProjects = true;
            a.needCal = true;
            appLog("[build] finished: " + a.console.title + " -> " +
                   a.console.status +
                   (a.console.exitCode >= 0
                        ? " (exit " + std::to_string(a.console.exitCode) + ")"
                        : ""));
            toast(a, a.console.title + ": " + a.console.status,
                  a.console.status != "success");
        }
    }
}

// ---------------------------------------------------------------------------
// ticket detail (calendar drill-down) + jump-to-ticket
// ---------------------------------------------------------------------------
Ticket& ticketInfo(App& a, long long itemId) {
    Ticket& t = a.ticketCache[itemId];
    if (t.loaded) return t;
    t.loaded = true;
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()),
        "SELECT i.title, i.type, i.status, i.priority, i.created_at, "
        "i.completed_at, i.project_id, i.origin, i.body, "
        "IFNULL((SELECT GROUP_CONCAT(s.name, ', ') FROM item_sources x "
        "JOIN sources s ON s.id=x.source_id WHERE x.item_id=i.id),''), "
        "IFNULL((SELECT m.state FROM discord_messages m WHERE m.item_id=i.id "
        "LIMIT 1),'') "
        "FROM items i WHERE i.id=?");
    q.bind(1, itemId);
    if (!q.executeStep()) return t;
    t.found = true;
    int c = 0;
    t.title = q.getColumn(c++).getString();
    t.type = q.getColumn(c++).getString();
    t.status = q.getColumn(c++).getString();
    t.priority = q.getColumn(c++).getInt();
    t.created = q.getColumn(c++).getString();
    t.completed = q.getColumn(c++).getString();
    t.projectId = q.getColumn(c++).getInt64();
    t.origin = q.getColumn(c++).getString();
    t.body = q.getColumn(c++).getString();
    t.source = q.getColumn(c++).getString();
    t.discordState = q.getColumn(c++).getString();
    return t;
}

// Navigate to the project page with this ticket's tab selected and its
// details expanded (the calendar popup's "open ticket" hyperlink).
void jumpToItem(App& a, long long projectId, long long itemId) {
    a.page = Page::Project;
    a.selProject = projectId;
    a.projectItemSearch[0] = 0;
    a.projectItemPriority = 0;
    a.needItems = true;
    a.focusItemId = itemId;
}

// ---------------------------------------------------------------------------
// shared widgets
// ---------------------------------------------------------------------------
void chip(const char* text, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::Text("[%s]", text);
    ImGui::PopStyleColor();
}

// CollapsingHeader whose open/collapsed state survives restarts: stored in
// the settings table as ui_sec_<key> ("1"/"0"), loaded lazily, written only
// when the user actually toggles the section.
bool sectionHeader(App& a, const char* label, const std::string& key,
                   bool defaultOpen, ImGuiTreeNodeFlags flags) {
    std::string k = "ui_sec_" + key;
    auto it = a.secOpen.find(k);
    if (it == a.secOpen.end()) {
        std::string v;
        {
            auto lk = a.db->guard();
            v = a.db->getSetting(lk.token(), k);
        }
        it = a.secOpen.emplace(k, v.empty() ? defaultOpen : v == "1").first;
    }
    ImGui::SetNextItemOpen(it->second, ImGuiCond_Always);
    bool open = ImGui::CollapsingHeader(label, flags);
    if (open != it->second) {
        it->second = open;
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), k, open ? "1" : "0");
    }
    return open;
}

bool projectCombo(App& a, const char* label, int* index, float width) {
    ImGui::SetNextItemWidth(width);
    std::string preview = (*index >= 0 && *index < (int)a.projects.size())
                              ? a.projects[*index].name : "select project";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (int i = 0; i < (int)a.projects.size(); ++i) {
            if (a.projects[i].archived) continue;
            if (ImGui::Selectable(a.projects[i].name.c_str(), *index == i)) {
                *index = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool projectIdCombo(App& a, const char* label, long long* projectId,
                    float width) {
    ImGui::SetNextItemWidth(width);
    Project* selected = findProject(a, *projectId);
    const char* preview = selected ? selected->name.c_str() : "select project";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (auto& project : a.projects) {
            if (project.archived) continue;
            if (ImGui::Selectable(project.name.c_str(),
                                  *projectId == project.id)) {
                *projectId = project.id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

long long knowledgeProjectId(const App& a, int selector) {
    int index = selector - 1;
    return index >= 0 && index < static_cast<int>(a.projects.size())
        ? a.projects[index].id : 0;
}

bool knowledgeProjectCombo(App& a, const char* label, int* selector,
                           float width) {
    ImGui::SetNextItemWidth(width);
    int index = *selector - 1;
    std::string preview = index >= 0 && index < static_cast<int>(a.projects.size())
        ? a.projects[index].name : "all / cross-project";
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (ImGui::Selectable("all / cross-project", *selector == 0)) {
            *selector = 0;
            changed = true;
        }
        for (int i = 0; i < static_cast<int>(a.projects.size()); ++i) {
            if (a.projects[i].archived) continue;
            if (ImGui::Selectable(a.projects[i].name.c_str(), *selector == i + 1)) {
                *selector = i + 1;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void loadKnowledgeWorkspace(App& a) {
    long long projectId = knowledgeProjectId(a, a.kbProject);
    const std::string kind = a.kbKindFilter > 0
        ? kKnowledgeKinds[a.kbKindFilter - 1] : "";
    nlohmann::json nodes = listKnowledgeNodes(
        a.db, projectId, a.kbSearch, kind, "active", 100);
    nlohmann::json conflicts = listKnowledgeConflicts(a.db, projectId, "open", 100);
    nlohmann::json reports = listReports(a.db, projectId, "", "", 100);
    nlohmann::json workflows = listWorkflows(a.db, projectId, "", 100);
    a.knowledgeNodes = nodes.contains("items") ? nodes["items"] : nlohmann::json::array();
    a.knowledgeConflicts = conflicts.contains("items") ? conflicts["items"] : nlohmann::json::array();
    a.knowledgeReports = reports.contains("items") ? reports["items"] : nlohmann::json::array();
    a.knowledgeWorkflows = workflows.contains("items") ? workflows["items"] : nlohmann::json::array();
    a.knowledgeHealthState = knowledgeHealth(a.db, projectId);
    a.needKnowledge = false;
}

static void versionCell(App& a, const Project& p) {
    VersionInfo vi = a.versions->get(p.id);
    if (!vi.checked) {
        ImGui::TextColored(C_DIM, a.versions->busy() ? "checking..." : "-");
        return;
    }
    // Local-only projects (nothing published anywhere yet): show the version
    // plainly instead of an "unknown" comparison.
    if (!vi.local.empty() && p.remoteUrl.empty()) {
        ImGui::TextUnformatted(vi.local.c_str());
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "(local)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("No published-version URL configured - showing "
                              "the in-development version only.");
        return;
    }
    ImVec4 col = verStateColor(vi.state);
    std::string l = vi.local.empty() ? "?" : vi.local;
    std::string r = vi.remote.empty() ? "?" : vi.remote;
    ImGui::TextColored(col, "%s / %s", l.c_str(), r.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("local (in dev): %s", l.c_str());
        ImGui::Text("published:      %s", r.c_str());
        ImGui::TextColored(col, "%s", versionStateLabel(vi.state));
        if (!vi.error.empty()) ImGui::TextColored(C_RED, "%s", vi.error.c_str());
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    ImGui::TextColored(col, "(%s)", versionStateLabel(vi.state));
}

// Only commands that are actually configured get a button.
static void actionButtons(App& a, const Project& p, bool compact) {
    ImGui::PushID((int)p.id);
    bool running = a.builds->isRunning(p.id);
    bool first = true;
    auto pre = [&]() { if (!first) ImGui::SameLine(); first = false; };

    if (running) ImGui::BeginDisabled();
    if (!trim(p.buildCmd).empty()) {
        pre();
        if (ImGui::SmallButton("Build")) startAction(a, p, "build");
    }
    if (!trim(p.prepCmd).empty()) {
        pre();
        if (ImGui::SmallButton("Prep")) startAction(a, p, "prep");
    }
    if (!trim(p.releaseCmd).empty()) {
        pre();
        ImGui::PushStyleColor(ImGuiCol_Text, C_ORANGE);
        if (ImGui::SmallButton("Release")) startAction(a, p, "release");
        ImGui::PopStyleColor();
    }
    if (!trim(p.versionCmd).empty()) {
        pre();
        if (ImGui::SmallButton("Ver")) startAction(a, p, "version");
    }
    if (running) ImGui::EndDisabled();

    if (compact) {
        pre();
        if (ImGui::SmallButton("AI")) openReviewSelection(a, p.id);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Choose active tickets for this project's AI review bundle");
    }

    if (!p.path.empty()) {
        pre();
        if (ImGui::SmallButton(ICON_FOLDER)) openFolder(p.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open folder in Explorer");
    }
    if (!p.githubUrl.empty() || !p.remoteUrl.empty()) {
        pre();
        if (ImGui::SmallButton(ICON_GLOBE))
            openUrl(!p.githubUrl.empty() ? p.githubUrl : p.remoteUrl);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", !p.githubUrl.empty() ? p.githubUrl.c_str()
                                                         : p.remoteUrl.c_str());
    }
    if (first) ImGui::TextColored(C_DIM, "-"); // nothing configured
    if (!compact && running) {
        ImGui::SameLine();
        ImGui::TextColored(C_ORANGE, "running...");
    }
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// panels
// ---------------------------------------------------------------------------
static void drawDashboard(App& a) {
    // stat tiles
    struct Tile {
        const char* label;
        long long v;
        ImVec4 col;
        const char* help;
        bool navigates;
        Page target;
    };
    long long openTotal = 0, fixes = 0, implementations = 0;
    for (auto& p : a.projects) {
        if (p.archived) continue;
        openTotal += p.openFix + p.openImpl + p.openOther;
        fixes += p.openFix;
        implementations += p.openImpl;
    }
    const int completedPreset =
        dashboardCompletedPresetIndex(a.dashboardCompletedDays);
    const std::string completedLabel = std::string("completed (") +
        kDashboardCompletedPresetCompact[completedPreset] + ")";
    const std::string completedHelp = std::string("Completed work in the last ") +
        kDashboardCompletedPresetLabels[completedPreset] +
        ". Open the calendar to review it.";
    Tile tiles[] = {
        {"open items", openTotal, C_TEXT,
         "All active work across active projects.", true, Page::Work},
        {"open fixes", fixes, C_RED,
         "Active fixes across active projects.", true, Page::Work},
        {"open implementations", implementations, C_ACCENT,
         "Active implementations across active projects.", true, Page::Work},
        {"overdue", a.statOverdue, C_RED,
         "Review active work past its due date.", true, Page::Work},
        {completedLabel.c_str(), a.statCompletedWindow, C_GREEN,
         completedHelp.c_str(), true, Page::Calendar},
        {"discord pending", a.statPending, C_ORANGE,
         "Open the Discord inbox to process pending feedback.", true,
         Page::Discord},
        {"credits owed", a.statUncredited, C_PURPLE,
         "Open Credits to review shipped suggestions awaiting credit.", true,
         Page::Credits},
    };
    for (int i = 0; i < 7; ++i) {
        if (i) ImGui::SameLine(0, 18);
        ImGui::BeginGroup();
        ImGui::PushFont(a.fontBig);
        ImGui::TextColored(tiles[i].col, "%lld", tiles[i].v);
        ImGui::PopFont();
        ImGui::TextColored(C_DIM, "%s", tiles[i].label);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tiles[i].help);
            if (tiles[i].navigates)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (tiles[i].navigates && ImGui::IsItemClicked()) {
            a.page = tiles[i].target;
            if (tiles[i].target == Page::Work) {
                a.workType = i == 1 ? 1 : i == 2 ? 2 : 0;
                a.workFocus = i == 3 ? 1 : 0;
                a.needWork = true;
            }
            if (tiles[i].target == Page::Discord)
                a.needChannels = a.needGuilds = a.needInbox = true;
            if (tiles[i].target == Page::Credits) a.needSources = true;
            if (tiles[i].target == Page::Calendar) a.needCal = true;
        }
    }
    ImGui::SameLine(0, 26);
    ImGui::BeginGroup();
    if (a.versions->busy()) ImGui::BeginDisabled();
    if (ImGui::Button(ICON_REFRESH " versions")) a.versions->refreshAsync();
    if (a.versions->busy()) ImGui::EndDisabled();
    ImGui::TextColored(C_DIM, a.versions->busy() ? "checking..." : "local / published");
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // A small client-side filter keeps large project lists manageable without
    // changing their stored sort order or requiring a database round trip.
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##projectFilter", "find project / path / alias...",
                             a.dashboardProjectFilter,
                             sizeof(a.dashboardProjectFilter));
    ImGui::SameLine();
    ImGui::Checkbox("active projects only", &a.dashboardActiveOnly);
    if (a.dashboardProjectFilter[0]) {
        ImGui::SameLine();
        if (ImGui::SmallButton("clear filter"))
            a.dashboardProjectFilter[0] = 0;
    }
    ImGui::Spacing();

    // project table
    if (ImGui::BeginTable("projects", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingFixedFit)) {
        // Keep the Dashboard compact instead of distributing spare width
        // between every column. Pipeline has room for the longest configured
        // action strip: Build, Prep, Release, Ver, AI, folder, and repo.
        // Version and Last action stay only as wide as their visible content,
        // so Done remains beside the action strip with a readable header.
        ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Version (local / published)",
                                ImGuiTableColumnFlags_WidthFixed, 225.0f);
        ImGui::TableSetupColumn("Open", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Last action", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Pipeline", ImGuiTableColumnFlags_WidthFixed, 355.0f);
        ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableHeadersRow();

        const std::string filter = trim(a.dashboardProjectFilter);
        int visibleProjects = 0;
        for (auto& p : a.projects) {
            if (p.archived) continue;
            const long long active = p.openFix + p.openImpl + p.openOther;
            if (a.dashboardActiveOnly && active == 0) continue;
            if (!filter.empty() && !containsCI(p.name, filter) &&
                !containsCI(p.desc, filter) && !containsCI(p.path, filter) &&
                !containsCI(p.aliases, filter))
                continue;
            ++visibleProjects;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)p.id);
            if (ImGui::Selectable(p.name.c_str(), false,
                                  ImGuiSelectableFlags_None)) {
                a.page = Page::Project;
                a.selProject = p.id;
                a.needItems = true;
            }
            if (ImGui::IsItemHovered() && !p.path.empty())
                ImGui::SetTooltip("%s", p.path.c_str());
            ImGui::PopID();

            ImGui::TableNextColumn();
            versionCell(a, p);

            ImGui::TableNextColumn();
            ImGui::TextColored(C_RED, "%lldF", p.openFix);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open this project's Fixes tab");
            }
            if (ImGui::IsItemClicked()) {
                a.page = Page::Project;
                a.selProject = p.id;
                a.pendingTab = 0;
                a.needItems = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(C_ACCENT, "%lldI", p.openImpl);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open this project's Implementations tab");
            }
            if (ImGui::IsItemClicked()) {
                a.page = Page::Project;
                a.selProject = p.id;
                a.pendingTab = 1;
                a.needItems = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(C_CYAN, "%lldO", p.openOther);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open references + notes count");

            ImGui::TableNextColumn();
            if (!p.lastBuildStatus.empty()) {
                ImGui::TextColored(statusColor(p.lastBuildStatus), "%s %s",
                                   p.lastBuildKind.c_str(), p.lastBuildStatus.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", shortTs(p.lastBuildAt).c_str());
            } else {
                ImGui::TextColored(C_DIM, "-");
            }

            ImGui::TableNextColumn();
            actionButtons(a, p, true);

            ImGui::TableNextColumn();
            ImGui::TextColored(C_GREEN, "%lld", p.done);
        }
        if (visibleProjects == 0) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(C_DIM, "No projects match the current filters.");
        }
        ImGui::EndTable();
    }

    // new project
    ImGui::Spacing();
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##np", "new project name...", a.npName, sizeof(a.npName));
    ImGui::SameLine();
    if (ImGui::Button("Add project") && a.npName[0]) {
        auto lk = a.db->guard();
        try {
            SQLite::Statement ins(a.db->raw(lk.token()),
                "INSERT INTO projects(name,slug,created_at,updated_at) VALUES(?,?,?,?)");
            ins.bind(1, a.npName);
            ins.bind(2, slugify(a.npName));
            ins.bind(3, nowIsoUtc());
            ins.bind(4, nowIsoUtc());
            ins.exec();
            a.db->logActivity(lk.token(), "project_created", a.db->raw(lk.token()).getLastInsertRowid(),
                              a.npName);
            a.npName[0] = 0;
            a.needProjects = true;
        } catch (const std::exception&) {
            lk.unlock();
            toast(a, "project already exists", true);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Recent activity");
    int actIdx = 0;
    for (auto& act : a.activity) {
        ImGui::PushID(actIdx++);
        bool capture = act.msgId != 0;
        ImGui::TextColored(C_DIM, "%s", shortTs(act.ts).c_str());
        ImGui::SameLine();
        ImGui::TextColored(capture ? C_ORANGE : C_ACCENT, "%s", act.kind.c_str());
        ImGui::SameLine();
        if (capture) {
            // Captured Discord message waiting in the inbox - jump to it.
            // (Button sits before the text so long messages cannot push it
            // off the right edge.)
            if (ImGui::SmallButton("open")) {
                a.page = Page::Discord;
                a.needChannels = a.needGuilds = a.needInbox = true;
            }
            ImGui::SameLine();
        }
        if (!act.project.empty()) {
            ImGui::TextUnformatted(act.project.c_str());
            ImGui::SameLine();
        }
        ImGui::TextColored(C_DIM, "%s", act.detail.c_str());
        ImGui::PopID();
    }
}

static void drawWork(App& a) {
    ImGui::PushFont(a.fontBig);
    ImGui::TextUnformatted("Work Queue");
    ImGui::PopFont();
    ImGui::TextColored(C_DIM,
        "One active queue across every project. Sort any column and combine filters.");

    ImGui::SetNextItemWidth(280);
    ImGui::InputTextWithHint("##workSearch", "find title, details, contributor...",
                             a.workSearch, sizeof(a.workSearch));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    const char* projectPreview = "all projects";
    if (a.workProject > 0) {
        if (Project* p = findProject(a, a.workProject)) projectPreview = p->name.c_str();
    }
    if (ImGui::BeginCombo("##workProject", projectPreview)) {
        if (ImGui::Selectable("all projects", a.workProject == 0)) a.workProject = 0;
        for (auto& p : a.projects) {
            if (p.archived) continue;
            if (ImGui::Selectable(p.name.c_str(), a.workProject == p.id))
                a.workProject = static_cast<int>(p.id);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    static const char* types[] = {"all types", "fix", "implementation", "reference", "note"};
    ImGui::SetNextItemWidth(145); ImGui::Combo("##workType", &a.workType, types, 5);
    ImGui::SameLine();
    static const char* statuses[] = {"all statuses", "open", "in progress", "blocked"};
    ImGui::SetNextItemWidth(135); ImGui::Combo("##workStatus", &a.workStatus, statuses, 4);
    ImGui::SameLine();
    static const char* priorities[] = {"all priority", "low", "normal", "high", "critical"};
    ImGui::SetNextItemWidth(125); ImGui::Combo("##workPrio", &a.workPriority, priorities, 5);
    ImGui::SameLine();
    static const char* focuses[] = {"all work", "overdue", "stale", "review due", "blocked"};
    ImGui::SetNextItemWidth(125); ImGui::Combo("##workFocus", &a.workFocus, focuses, 5);

    std::vector<Item*> rows;
    const std::string search = trim(a.workSearch);
    for (auto& it : a.workItems) {
        if (a.workProject > 0 && it.projectId != a.workProject) continue;
        if (a.workType > 0 && it.type != kTypeNames[a.workType - 1]) continue;
        if (a.workStatus > 0) {
            const char* wanted = a.workStatus == 1 ? "open" :
                                 a.workStatus == 2 ? "in_progress" : "blocked";
            if (it.status != wanted) continue;
        }
        if (a.workPriority > 0 && it.priority != a.workPriority) continue;
        if (a.workFocus == 1 && !it.overdue) continue;
        if (a.workFocus == 2 && !it.stale) continue;
        if (a.workFocus == 3 && !it.reviewDue) continue;
        if (a.workFocus == 4 && it.status != "blocked") continue;
        if (!search.empty() && !containsCI(it.title, search) &&
            !containsCI(it.body, search) && !containsCI(it.project, search) &&
            !containsCI(it.source, search) && !containsCI(it.tags, search)) continue;
        rows.push_back(&it);
    }
    ImGui::TextColored(C_DIM, "%d shown / %d active", (int)rows.size(),
                       (int)a.workItems.size());

    if (ImGui::BeginTable("workQueue", 8,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_DefaultSort, 1.25f, 0);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch, 2.6f, 1);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 95, 2);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100, 3);
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 72, 4);
        ImGui::TableSetupColumn("Due / review", ImGuiTableColumnFlags_WidthFixed, 105, 5);
        ImGui::TableSetupColumn("Updated", ImGuiTableColumnFlags_WidthFixed, 90, 6);
        ImGui::TableSetupColumn("Contributors", ImGuiTableColumnFlags_WidthStretch, 1.25f, 7);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
            specs && specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs s = specs->Specs[0];
            std::stable_sort(rows.begin(), rows.end(), [&](const Item* l, const Item* r) {
                int cmp = 0;
                switch (s.ColumnUserID) {
                    case 0: cmp = l->project.compare(r->project); break;
                    case 1: cmp = l->title.compare(r->title); break;
                    case 2: cmp = l->type.compare(r->type); break;
                    case 3: cmp = l->status.compare(r->status); break;
                    case 4: cmp = l->priority - r->priority; break;
                    case 5: {
                        std::string ld = !l->due.empty() ? l->due : l->reviewDate;
                        std::string rd = !r->due.empty() ? r->due : r->reviewDate;
                        cmp = ld.compare(rd); break;
                    }
                    case 6: cmp = l->updated.compare(r->updated); break;
                    case 7: cmp = l->source.compare(r->source); break;
                }
                if (cmp == 0) cmp = l->id < r->id ? -1 : l->id > r->id ? 1 : 0;
                return s.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
            });
        }
        for (Item* it : rows) {
            ImGui::PushID((int)it->id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->project.c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable((it->title + "##workitem").c_str(), false,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                jumpToItem(a, it->projectId, it->id);
                ImGui::PopID(); ImGui::EndTable(); return;
            }
            if (ImGui::IsItemHovered() && !it->body.empty())
                ImGui::SetTooltip("%s", it->body.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(typeColor(it->type), "%s", it->type.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(statusColor(it->status), "%s", it->status.c_str());
            if (ImGui::IsItemHovered() && !it->blockedReason.empty())
                ImGui::SetTooltip("blocked: %s", it->blockedReason.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(it->priority >= 4 ? C_RED : it->priority == 3 ? C_ORANGE : C_TEXT,
                               "P%d", it->priority);
            ImGui::TableNextColumn();
            if (!it->due.empty())
                ImGui::TextColored(it->overdue ? C_RED : C_ORANGE, "%s", it->due.c_str());
            else if (!it->reviewDate.empty())
                ImGui::TextColored(it->reviewDue ? C_ORANGE : C_PURPLE, "R %s", it->reviewDate.c_str());
            else ImGui::TextColored(C_DIM, "-");
            ImGui::TableNextColumn();
            ImGui::TextColored(it->stale ? C_ORANGE : C_DIM, "%s%s",
                               shortDate(it->updated).c_str(), it->stale ? " *" : "");
            ImGui::TableNextColumn();
            ImGui::TextColored(it->source.empty() ? C_DIM : C_PURPLE, "%s",
                               it->source.empty() ? "-" : it->source.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void openEditProject(App& a, const Project& p) {
    a.editProject = true;
    copyBuf(a.epName, sizeof(a.epName), p.name);
    copyBuf(a.epDesc, sizeof(a.epDesc), p.desc);
    loadPreservedText(a.epPath, sizeof(a.epPath), p.path, a.epPathValue);
    copyBuf(a.epRules, sizeof(a.epRules), p.rules);
    copyBuf(a.epSkills, sizeof(a.epSkills), p.codexSkills);
    loadPreservedText(a.epBuild, sizeof(a.epBuild), p.buildCmd,
                      a.epBuildValue);
    loadPreservedText(a.epPrep, sizeof(a.epPrep), p.prepCmd,
                      a.epPrepValue);
    loadPreservedText(a.epRelease, sizeof(a.epRelease), p.releaseCmd,
                      a.epReleaseValue);
    loadPreservedText(a.epVersion, sizeof(a.epVersion), p.versionCmd,
                      a.epVersionValue);
    loadPreservedText(a.epCwd, sizeof(a.epCwd), p.buildCwd, a.epCwdValue);
    copyBuf(a.epVerFile, sizeof(a.epVerFile), p.localVerFile);
    copyBuf(a.epRemoteUrl, sizeof(a.epRemoteUrl), p.remoteUrl);
    copyBuf(a.epRemoteKey, sizeof(a.epRemoteKey), p.remoteKey);
    copyBuf(a.epGithub, sizeof(a.epGithub), p.githubUrl);
    copyBuf(a.epAliases, sizeof(a.epAliases), p.aliases);
    a.epTickets = p.discordTickets != 0;
}

static void openEditItem(App& a, const Item& it) {
    a.editItemId = it.id;
    a.editItemProjectId = it.projectId;
    a.eiProjectId = it.projectId;
    copyBuf(a.eiTitle, sizeof(a.eiTitle), it.title);
    loadPreservedText(a.eiBody, sizeof(a.eiBody), it.body, a.eiBodyValue);
    copyBuf(a.eiDue, sizeof(a.eiDue), it.due);
    copyBuf(a.eiReview, sizeof(a.eiReview), it.reviewDate);
    copyBuf(a.eiBlocked, sizeof(a.eiBlocked), it.blockedReason);
    a.eiAddSource[0] = 0;
    a.eiMergeSourceIds.clear();
    a.eiType = 0;
    for (int i = 0; i < 4; ++i)
        if (it.type == kTypeNames[i]) a.eiType = i;
    a.eiPrio = it.priority >= 1 && it.priority <= 4 ? it.priority - 1 : 1;
    a.eiStatus = 0;
    for (int i = 0; i < 5; ++i)
        if (it.status == kStatusNames[i]) a.eiStatus = i;
    a.editContributors.clear();
    auto lk = a.db->guard();
    SQLite::Statement q(a.db->raw(lk.token()),
        "SELECT s.id,s.name,x.credited FROM item_sources x "
        "JOIN sources s ON s.id=x.source_id WHERE x.item_id=? "
        "ORDER BY s.name COLLATE NOCASE");
    q.bind(1, it.id);
    while (q.executeStep())
        a.editContributors.push_back({q.getColumn(0).getInt64(),
                                      q.getColumn(1).getString(),
                                      q.getColumn(2).getInt()});
}

void openEditPendingSuggestion(App& a, const Msg& message) {
    a.editPendingMessageId = message.id;
    a.editPendingProjectId = message.selProjectId;
    a.editPendingProject = projectIndexForId(a, a.editPendingProjectId);
    a.editPendingType = message.selType;
    a.editPendingPrio = 1;
    copyBuf(a.editPendingContent, sizeof(a.editPendingContent),
            message.content);
    copyBuf(a.editPendingAdminNote, sizeof(a.editPendingAdminNote),
            message.adminNote);
    std::string title = trim(message.content);
    for (char& ch : title)
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    while (title.find("  ") != std::string::npos)
        title.replace(title.find("  "), 2, " ");
    if (title.size() > 180) {
        std::size_t cut = 177;
        while (cut > 0 &&
               (static_cast<unsigned char>(title[cut]) & 0xc0) == 0x80)
            --cut;
        title = title.substr(0, cut) + "...";
    }
    copyBuf(a.editPendingTitle, sizeof(a.editPendingTitle), title);
}

static void drawItemRow(App& a, Item& it, std::set<long long>& expanded) {
    ImGui::PushID((int)it.id);
    bool done = it.status == "completed";
    bool check = done;
    if (ImGui::Checkbox("##done", &check)) {
        {
            auto lk = a.db->guard();
            setItemStatusLocked(lk.token(), a.db, it.id, check ? "completed" : "open",
                                a.discord);
        }
        a.needItems = true;
        a.needProjects = true;
        a.needWork = true;
        a.needCal = true;
        toast(a, check ? "completed - logged on calendar" : "reopened");
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (done) ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
    bool isOpen = expanded.count(it.id) > 0;
    const std::string rowTitle =
        "I" + std::to_string(it.id) + "  " + it.title;
    if (ImGui::Selectable(rowTitle.c_str(), isOpen)) {
        if (isOpen) expanded.erase(it.id); else expanded.insert(it.id);
    }
    if (done) ImGui::PopStyleColor();
    if (ImGui::BeginPopupContextItem(
            nullptr, ImGuiPopupFlags_MouseButtonRight)) {
        const bool canCopyReview = activeItemStatus(it.status);
        if (!canCopyReview) ImGui::BeginDisabled();
        if (ImGui::MenuItem("Copy AI review bundle")) {
            copyPacket(
                a, buildReviewMarkdown(a.db, it.projectId, {it.id}),
                "review bundle copied - I" + std::to_string(it.id) + " only");
        }
        if (!canCopyReview) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("Active tickets only");
        }
        ImGui::EndPopup();
    }

    chip(it.type.c_str(), typeColor(it.type));
    ImGui::SameLine();
    if (it.priority == 3) { chip("high", C_ORANGE); ImGui::SameLine(); }
    if (it.priority == 4) { chip("critical", C_RED); ImGui::SameLine(); }
    if (it.priority == 1) { chip("low", C_DIM); ImGui::SameLine(); }
    if (!it.source.empty()) {
        chip(("by " + it.source).c_str(), C_PURPLE);
        ImGui::SameLine();
        if (done && !it.credited) {
            if (ImGui::SmallButton("credit")) {
                auto lk = a.db->guard();
                SQLite::Statement up(a.db->raw(lk.token()),
                    "UPDATE item_sources SET credited=1 WHERE item_id=?");
                up.bind(1, it.id);
                up.exec();
                SQLite::Statement legacy(a.db->raw(lk.token()),
                    "UPDATE items SET credited=1 WHERE id=?");
                legacy.bind(1, it.id); legacy.exec();
                lk.unlock();
                a.needItems = true;
                a.needProjects = true;
                a.needSources = true;
                a.needWork = true;
            }
            ImGui::SameLine();
        } else if (done && it.credited) {
            chip("credited", C_GREEN);
            ImGui::SameLine();
        }
    }
    if (it.origin != "manual") { chip(it.origin.c_str(), C_DIM); ImGui::SameLine(); }
    if (it.status == "blocked") { chip("blocked", C_ORANGE); ImGui::SameLine(); }
    if (!it.due.empty()) { chip(("due " + it.due).c_str(), C_ORANGE); ImGui::SameLine(); }
    if (!it.reviewDate.empty()) {
        chip(("review " + it.reviewDate).c_str(),
             it.reviewDate <= todayLocal() ? C_ORANGE : C_PURPLE);
        ImGui::SameLine();
    }
    ImGui::TextColored(C_DIM, "%s", shortDate(it.created).c_str());
    ImGui::SameLine();
    if (done) {
        const std::string completedDate = it.completedAt.empty()
            ? "unknown" : shortDate(it.completedAt);
        chip(("completed: " + completedDate).c_str(), C_GREEN);
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("edit")) openEditItem(a, it);
    ImGui::SameLine();
    if (ImGui::SmallButton("del")) a.deleteItemId = it.id;

    if (isOpen) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG2);
        ImGui::BeginChild(("body" + std::to_string(it.id)).c_str(),
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        if (it.body.empty())
            ImGui::TextColored(C_DIM, "(no details)");
        else
            drawBodyWithLinks(it.body, C_DIM);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    ImGui::Separator();
    ImGui::PopID();
}

static void drawProject(App& a) {
    Project* pp = findProject(a, a.selProject);
    if (!pp) { a.page = Page::Dashboard; return; }
    Project& p = *pp;

    if (ImGui::Button("< back")) { a.page = Page::Dashboard; return; }
    ImGui::SameLine();
    ImGui::PushFont(a.fontBig);
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    versionCell(a, p);

    if (!p.path.empty()) {
        ImGui::TextColored(C_DIM, "%s", p.path.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FOLDER "##hdr")) openFolder(p.path);
    }
    if (!p.codexSkills.empty())
        ImGui::TextColored(C_PURPLE, "Codex routes: %s", p.codexSkills.c_str());

    actionButtons(a, p, false);
    ImGui::SameLine(0, 18);
    if (ImGui::SmallButton("Edit project")) openEditProject(a, p);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy AI review bundle"))
        openReviewSelection(a, p.id);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy release draft")) {
        std::string md = buildReleaseDraft(a.db, p.id, a.releaseDraftDays);
        copyPacket(a, md, "release notes + contributor draft copied");
    }

    if (!p.releaseNotes.empty()) {
        if (ImGui::CollapsingHeader("Release notes")) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(C_DIM, "%s", p.releaseNotes.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // quick add
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputTextWithHint("##qa", "quick add: title...", a.qaTitle, sizeof(a.qaTitle));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##qat", &a.qaType, kTypeNames, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("##qap", &a.qaPrio, kPrioNames, 4);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##qas", "suggested by", a.qaSource, sizeof(a.qaSource));
    ImGui::SameLine();
    const std::string quickTitle = trim(a.qaTitle);
    if (ImGui::Button("Add") && !quickTitle.empty()) {
        const int createdType = a.qaType;
        const int createdPriority = a.qaPrio + 1;
        const std::string createdSource = trim(a.qaSource);
        long long createdItemId = 0;
        {
            auto lk = a.db->guard();
            long long src = ensureSourceLocked(lk.token(), a.db, createdSource, "", "");
            createdItemId = insertItemLocked(lk.token(),
                a.db, p.id, kTypeNames[createdType], quickTitle, "",
                createdPriority, src, "manual", "", "");
            a.db->logActivity(lk.token(), "item_added", p.id, quickTitle);
        }
        a.qaTitle[0] = 0;
        a.qaSource[0] = 0;
        a.projectItemSearch[0] = 0;
        a.projectItemPriority = 0;
        a.pendingTab = createdType;
        a.focusItemId = createdItemId;
        a.needItems = true;
        a.needProjects = true;
        a.needWork = true;
        a.needSources = true;

        // The insert is committed and its DB guard released before the editor
        // reloads contributor state. Canceling the editor keeps the one ticket
        // created by Quick add; Save enriches that exact row with its details.
        Item created;
        created.id = createdItemId;
        created.projectId = p.id;
        created.type = kTypeNames[createdType];
        created.title = quickTitle;
        created.status = "open";
        created.source = createdSource;
        created.priority = createdPriority;
        openEditItem(a, created);
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##projectItemSearch",
                             "search title, details, or contributor...",
                             a.projectItemSearch,
                             sizeof(a.projectItemSearch));
    ImGui::SameLine();
    static const char* projectSorts[] = {
        "priority / status",
        "title A-Z",
        "title Z-A",
        "newest submitted",
        "oldest submitted",
        "contributor A-Z",
        "contributor Z-A",
        "newest completed",
        "oldest completed",
    };
    ImGui::SetNextItemWidth(175.0f);
    ImGui::Combo("##projectItemSort", &a.projectItemSort, projectSorts,
                 static_cast<int>(sizeof(projectSorts) / sizeof(projectSorts[0])));
    ImGui::SameLine();
    static const char* projectPriorities[] = {
        "all priorities",
        "critical",
        "normal",
        "low",
    };
    ImGui::SetNextItemWidth(165.0f);
    ImGui::Combo("##projectItemPriority", &a.projectItemPriority,
                 projectPriorities,
                 static_cast<int>(sizeof(projectPriorities) /
                                  sizeof(projectPriorities[0])));
    std::string projectSearch = trim(a.projectItemSearch);
    if (!projectSearch.empty() || a.projectItemSort != 0 ||
        a.projectItemPriority != 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
            a.projectItemSearch[0] = 0;
            a.projectItemSort = 0;
            a.projectItemPriority = 0;
            projectSearch.clear();
        }
    }

    // tabs
    static std::set<long long> expanded;
    auto matchesProjectSearch = [&](const Item& it) {
        return projectSearch.empty() || containsCI(it.title, projectSearch) ||
               containsCI(it.body, projectSearch) ||
               containsCI(it.source, projectSearch);
    };
    auto matchesProjectPriority = [&](const Item& it) {
        switch (a.projectItemPriority) {
            case 1: return it.priority == 4 || it.priority == 3;
            case 2: return it.priority == 2;
            case 3: return it.priority == 1;
            default: return true;
        }
    };
    std::vector<Item*> projectRows;
    for (auto& it : a.items) {
        if (!matchesProjectSearch(it)) continue;
        if (!matchesProjectPriority(it)) continue;
        projectRows.push_back(&it);
    }
    if (a.projectItemSort > 0) {
        std::stable_sort(
            projectRows.begin(), projectRows.end(),
            [&](const Item* left, const Item* right) {
                int comparison = 0;
                switch (a.projectItemSort) {
                    case 1:
                    case 2:
                        comparison =
                            toLower(left->title).compare(toLower(right->title));
                        break;
                    case 3:
                    case 4:
                        comparison = left->created.compare(right->created);
                        break;
                    case 5:
                    case 6:
                        comparison =
                            toLower(left->source).compare(toLower(right->source));
                        break;
                    case 7:
                    case 8:
                        // Missing completion dates follow dated rows in both directions.
                        if (left->completedAt.empty() != right->completedAt.empty())
                            return !left->completedAt.empty();
                        comparison = left->completedAt.compare(right->completedAt);
                        break;
                    default:
                        break;
                }
                if (comparison == 0)
                    comparison = left->id < right->id
                        ? -1
                        : left->id > right->id ? 1 : 0;
                const bool descending =
                    a.projectItemSort == 2 || a.projectItemSort == 3 ||
                    a.projectItemSort == 6 || a.projectItemSort == 7;
                return descending ? comparison > 0 : comparison < 0;
            });
    }
    // Arriving via a calendar "open ticket" link: expand the ticket and
    // force-select the tab it lives on.
    if (a.focusItemId != 0 && !a.needItems) {
        for (auto& it : a.items) {
            if (it.id != a.focusItemId) continue;
            expanded.insert(it.id);
            if (it.status == "completed") a.pendingTab = 4;
            else
                for (int t = 0; t < 4; ++t)
                    if (it.type == kTypeNames[t]) a.pendingTab = t;
            break;
        }
        a.focusItemId = 0;
    }
    auto countType = [&](const char* t) {
        int n = 0;
        for (const Item* it : projectRows)
            if (it->type == t && activeItemStatus(it->status))
                ++n;
        return n;
    };
    int openCount = 0;
    int doneCount = 0;
    for (const Item* it : projectRows) {
        if (activeItemStatus(it->status)) ++openCount;
        if (it->status == "completed") ++doneCount;
    }
    const int allCount = static_cast<int>(projectRows.size());
    if (!projectSearch.empty() || a.projectItemPriority != 0) {
        ImGui::SameLine();
        ImGui::TextColored(C_DIM, "%d matching ticket%s", allCount,
                           allCount == 1 ? "" : "s");
    }

    if (ImGui::BeginTabBar("itemTabs")) {
        struct Tab { std::string label; const char* type; };
        char lbl[64];
        for (int t = 0; t < 4; ++t) {
            std::snprintf(lbl, sizeof(lbl), "%s (%d)###tab%d",
                          t == 0 ? "Fixes" : t == 1 ? "Implementations"
                          : t == 2 ? "References" : "Notes",
                          countType(kTypeNames[t]), t);
            if (ImGui::BeginTabItem(lbl, nullptr,
                                    a.pendingTab == t
                                        ? ImGuiTabItemFlags_SetSelected
                                        : ImGuiTabItemFlags_None)) {
                ImGui::BeginChild("list", ImVec2(0, 0));
                int shown = 0;
                for (Item* it : projectRows)
                    if (it->type == kTypeNames[t] &&
                        activeItemStatus(it->status)) {
                        drawItemRow(a, *it, expanded);
                        ++shown;
                    }
                if (shown == 0 &&
                    (!projectSearch.empty() || a.projectItemPriority != 0))
                    ImGui::TextColored(
                        C_DIM,
                        "No tickets in this tab match the current filters.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        std::snprintf(lbl, sizeof(lbl), "Open (%d)###tabo", openCount);
        if (ImGui::BeginTabItem(lbl)) {
            ImGui::BeginChild("listo", ImVec2(0, 0));
            for (Item* it : projectRows)
                if (activeItemStatus(it->status))
                    drawItemRow(a, *it, expanded);
            if (openCount == 0)
                ImGui::TextColored(
                    C_DIM, (!projectSearch.empty() || a.projectItemPriority != 0)
                        ? "No open tickets match the current filters."
                        : "No open tickets.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        std::snprintf(lbl, sizeof(lbl), "Completed (%d)###tabc", doneCount);
        if (ImGui::BeginTabItem(lbl, nullptr,
                                a.pendingTab == 4 ? ImGuiTabItemFlags_SetSelected
                                                  : ImGuiTabItemFlags_None)) {
            ImGui::BeginChild("listc", ImVec2(0, 0));
            int shown = 0;
            for (Item* it : projectRows)
                if (it->status == "completed") {
                    drawItemRow(a, *it, expanded);
                    ++shown;
                }
            if (shown == 0 &&
                (!projectSearch.empty() || a.projectItemPriority != 0))
                ImGui::TextColored(
                    C_DIM, "No completed tickets match the current filters.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        std::snprintf(lbl, sizeof(lbl), "All (%d)###taba", allCount);
        if (ImGui::BeginTabItem(lbl)) {
            ImGui::BeginChild("lista", ImVec2(0, 0));
            for (Item* it : projectRows) drawItemRow(a, *it, expanded);
            if (allCount == 0 &&
                (!projectSearch.empty() || a.projectItemPriority != 0))
                ImGui::TextColored(
                    C_DIM, "No tickets match the current filters.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        a.pendingTab = -1;
    }
}

static void drawCalendar(App& a) {
    if (a.calYear == 0) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        a.calYear = tm.tm_year + 1900;
        a.calMonth = tm.tm_mon;
    }
    if (ImGui::Button("<")) {
        if (--a.calMonth < 0) { a.calMonth = 11; --a.calYear; }
        a.needCal = true;
    }
    ImGui::SameLine();
    static const char* kMonths[] = {"January","February","March","April","May","June",
                                    "July","August","September","October","November","December"};
    ImGui::PushFont(a.fontBig);
    ImGui::Text("%s %d", kMonths[a.calMonth], a.calYear);
    ImGui::PopFont();
    ImGui::SameLine();
    if (ImGui::Button(">")) {
        if (++a.calMonth > 11) { a.calMonth = 0; ++a.calYear; }
        a.needCal = true;
    }
    ImGui::SameLine(0, 24);
    ImGui::TextColored(C_GREEN, "completions");
    ImGui::SameLine(); ImGui::TextColored(C_ACCENT, "events");
    ImGui::SameLine(); ImGui::TextColored(C_PURPLE, "releases");
    ImGui::SameLine(); ImGui::TextColored(C_ORANGE, "due");
    ImGui::SameLine(); ImGui::TextColored(C_CYAN, "builds");

    // month layout (Monday first)
    std::tm tm{};
    tm.tm_year = a.calYear - 1900;
    tm.tm_mon = a.calMonth;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    std::mktime(&tm);
    int startDow = (tm.tm_wday + 6) % 7;
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int days = mdays[a.calMonth];
    if (a.calMonth == 1 &&
        (a.calYear % 4 == 0 && (a.calYear % 100 != 0 || a.calYear % 400 == 0)))
        days = 29;
    const int weekRows = (startDow + days + 6) / 7;

    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{};
    localtime_s(&nowTm, &nowT);
    char todayStr[16];
    std::strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", &nowTm);

    ImVec4 kindCols[] = {C_GREEN, C_ACCENT, C_PURPLE, C_ORANGE, C_CYAN};
    auto kindColor = [&](const std::string& k) {
        if (k == "completion") return C_GREEN;
        if (k == "release") return C_PURPLE;
        if (k == "deadline") return C_ORANGE;
        if (k == "build") return C_CYAN;
        return C_ACCENT;
    };
    (void)kindCols;
    const float calendarTableHeight = ImGui::GetContentRegionAvail().y;

    if (ImGui::BeginTable("cal", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (const char* d : {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"})
            ImGui::TableSetupColumn(d);
        ImGui::TableHeadersRow();

        // Keep the complete month visible in the page's remaining space. The
        // day popup retains every event's full title and details; cells use
        // concise one-line summaries so dense days cannot expand a week row.
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float headerH = ImGui::GetTextLineHeight() +
                              ImGui::GetStyle().CellPadding.y * 2.0f;
        const float gridH = std::max(1.0f, calendarTableHeight - headerH - 2.0f);
        const float rowH = std::max(1.0f, gridH / static_cast<float>(weekRows));
        const int maxEntries = std::max(1, static_cast<int>(rowH / lineH) - 3);
        constexpr size_t kCalendarSummaryChars = 26;
        int cell = 0, day = 1;
        while (day <= days) {
            if (cell % 7 == 0) ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
            ImGui::TableNextColumn();
            if (cell < startDow) { ++cell; continue; }

            char date[16];
            std::snprintf(date, sizeof(date), "%04d-%02d-%02d", a.calYear,
                          a.calMonth + 1, day);
            ImGui::PushID(cell);
            bool isToday = std::strcmp(date, todayStr) == 0;
            char dayLbl[16];
            std::snprintf(dayLbl, sizeof(dayLbl), "%d", day);
            if (isToday) ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
            if (ImGui::Selectable(dayLbl, isToday,
                                  ImGuiSelectableFlags_AllowOverlap)) {
                a.dayPopupDate = date;
                a.dayExpanded.clear();
                a.ticketCache.clear(); // refetch fresh ticket states
                copyBuf(a.evTitle, sizeof(a.evTitle), "");
                copyBuf(a.evNotes, sizeof(a.evNotes), "");
                a.evKind = 0;
                a.evProject = 0;
            }
            if (isToday) ImGui::PopStyleColor();

            auto it = a.calendar.find(date);
            if (it != a.calendar.end()) {
                int shown = 0;
                for (auto& e : it->second) {
                    if (shown >= maxEntries) {
                        ImGui::TextColored(C_DIM, "+%d more",
                                           (int)it->second.size() - shown);
                        break;
                    }
                    std::string summary = e.title;
                    if (summary.size() > kCalendarSummaryChars) {
                        summary.resize(kCalendarSummaryChars - 3);
                        summary += "...";
                    }
                    ImGui::TextColored(kindColor(e.kind), "%s", summary.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s%s%s", e.project.c_str(),
                                          e.project.empty() ? "" : ": ",
                                          e.title.c_str());
                    ++shown;
                }
            }
            ImGui::PopID();
            ++cell;
            ++day;
        }
        ImGui::EndTable();
    }
}

// Body of the collapsible "Bot" section: status, token, and admin whitelist.
static void drawCredits(App& a) {
    ImGui::TextColored(C_DIM,
        "Promoted Discord suggestions are credited automatically. "
        "Use merge with to correct mistaken or duplicate identities.");
    ImGui::SeparatorText("Community leaderboard");
    ImGui::TextColored(C_DIM,
        "Screenshot-ready top contributors for fixes and implementations. "
        "Ranking is shipped first, then submitted; exclusions affect only this leaderboard.");
    if (!a.leaderboardConfigValid) {
        ImGui::TextColored(C_RED, "Leaderboard hidden: %s",
                           a.leaderboardConfigError.c_str());
        if (ImGui::SmallButton("Reset leaderboard exclusions")) {
            {
                auto lk = a.db->guard();
                a.db->setSetting(lk.token(), "leaderboard_excluded_source_ids", "");
            }
            a.needSources = true;
            toast(a, "leaderboard exclusions reset");
        }
    } else if (a.leaderboard.empty()) {
        ImGui::TextColored(C_DIM, "No public contributors are listed yet.");
    } else if (ImGui::BeginTable("communityLeaderboard", 5,
                                 ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("rank", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("contributor", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("submitted", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("shipped", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("ship rate", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < a.leaderboard.size(); ++i) {
            const auto& entry = a.leaderboard[i];
            const long long rate = entry.submitted > 0
                ? (entry.shipped * 100) / entry.submitted : 0;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("#%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", entry.submitted);
            ImGui::TableNextColumn();
            ImGui::TextColored(C_GREEN, "%lld", entry.shipped);
            ImGui::TableNextColumn();
            ImGui::Text("%lld%%", rate);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Contributor credit history");
    bool openMergePopup = false;
    if (ImGui::BeginTable("sources", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("suggested", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("shipped", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("manual/legacy owed", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("leaderboard", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableHeadersRow();
        for (auto& s : a.sources) {
            ImGui::PushID((int)s.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(C_DIM, "%s", s.platform.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", s.items);
            ImGui::TableNextColumn();
            ImGui::TextColored(C_GREEN, "%lld", s.completed);
            ImGui::TableNextColumn();
            if (s.uncredited > 0) ImGui::TextColored(C_ORANGE, "%lld", s.uncredited);
            else ImGui::TextColored(C_DIM, "0");
            ImGui::TableNextColumn();
            bool visible = s.leaderboardVisible;
            if (!a.leaderboardConfigValid) ImGui::BeginDisabled();
            if (ImGui::Checkbox("show", &visible)) {
                std::string error;
                if (setLeaderboardSourceExcluded(a.db, s.id, !visible, &error)) {
                    s.leaderboardVisible = visible;
                    a.needSources = true;
                    toast(a, visible ? s.name + " added to the leaderboard"
                                     : s.name + " hidden from the leaderboard");
                } else {
                    toast(a, error.empty() ? "leaderboard preference was not saved"
                                           : error,
                          true);
                }
            }
            if (!a.leaderboardConfigValid) ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("items")) {
                if (a.sourceItems.count(s.id)) a.sourceItems.erase(s.id);
                else {
                    std::vector<Item> rows;
                    auto lk = a.db->guard();
                    SQLite::Statement q(a.db->raw(lk.token()),
                        "SELECT i.id, i.project_id, i.title, i.body, i.type, "
                        "i.status, x.credited, i.created_at, i.completed_at, "
                        "p.name "
                        "FROM item_sources x JOIN items i ON i.id=x.item_id "
                        "JOIN projects p ON p.id=i.project_id "
                        "WHERE x.source_id=? ORDER BY i.id DESC");
                    q.bind(1, s.id);
                    while (q.executeStep()) {
                        Item it;
                        int c = 0;
                        it.id = q.getColumn(c++).getInt64();
                        it.projectId = q.getColumn(c++).getInt64();
                        it.title = q.getColumn(c++).getString();
                        it.body = q.getColumn(c++).getString();
                        it.type = q.getColumn(c++).getString();
                        it.status = q.getColumn(c++).getString();
                        it.credited = q.getColumn(c++).getInt();
                        it.created = q.getColumn(c++).getString();
                        it.completedAt = q.getColumn(c++).getString();
                        it.source = q.getColumn(c++).getString(); // project name
                        rows.push_back(std::move(it));
                    }
                    a.sourceItems[s.id] = std::move(rows);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("merge with")) {
                a.mergeSourceId = s.id;
                a.mergeTargetSourceId = 0;
                openMergePopup = true;
            }
            if (s.uncredited > 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton("credit legacy")) {
                    auto lk = a.db->guard();
                    SQLite::Statement up(a.db->raw(lk.token()),
                        "UPDATE item_sources SET credited=1 WHERE source_id=? AND "
                        "item_id IN (SELECT id FROM items WHERE status='completed')");
                    up.bind(1, s.id);
                    up.exec();
                    SQLite::Statement legacy(a.db->raw(lk.token()),
                        "UPDATE items SET credited=1 WHERE status='completed' "
                        "AND EXISTS(SELECT 1 FROM item_sources x WHERE x.item_id=items.id) "
                        "AND NOT EXISTS(SELECT 1 FROM item_sources x WHERE x.item_id=items.id AND x.credited=0)");
                    legacy.exec();
                    lk.unlock();
                    a.needSources = true;
                    a.needProjects = true;
                    a.needWork = true;
                    toast(a, "credited legacy items from " + s.name);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (openMergePopup) ImGui::OpenPopup("Merge contributor");
    if (ImGui::BeginPopupModal("Merge contributor", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        auto findSource = [&](long long id) -> const Source* {
            auto it = std::find_if(
                a.sources.begin(), a.sources.end(),
                [id](const Source& source) { return source.id == id; });
            return it == a.sources.end() ? nullptr : &*it;
        };
        const Source* source = findSource(a.mergeSourceId);
        const Source* target = findSource(a.mergeTargetSourceId);
        if (!source) {
            ImGui::TextColored(C_RED,
                "The contributor is no longer available. Refresh Credits and try again.");
        } else {
            ImGui::TextWrapped(
                "Merge every ticket attributed to %s into another contributor.",
                source->name.c_str());
            ImGui::Spacing();
            const char* preview = target ? target->name.c_str() : "select survivor";
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::BeginCombo("Merge with", preview)) {
                for (const auto& candidate : a.sources) {
                    if (candidate.id == source->id) continue;
                    const std::string label = candidate.name + " (" +
                        std::to_string(candidate.items) + " item" +
                        (candidate.items == 1 ? ")" : "s)");
                    if (ImGui::Selectable(label.c_str(),
                                          candidate.id == a.mergeTargetSourceId))
                        a.mergeTargetSourceId = candidate.id;
                }
                ImGui::EndCombo();
            }
            target = findSource(a.mergeTargetSourceId);
            ImGui::Spacing();
            ImGui::PushTextWrapPos(520.0f);
            ImGui::TextColored(C_ORANGE,
                "The old contributor remains as a hidden identity alias. Future Discord "
                "messages from that account will credit the survivor. If both people are "
                "already on one ticket, it becomes one attribution and remains credited "
                "when either prior attribution was credited. If either person is hidden "
                "from the public leaderboard, the combined identity stays hidden until "
                "you explicitly enable it.");
            ImGui::PopTextWrapPos();
            ImGui::TextColored(C_DIM,
                "Ticket text, Discord author history, and lifecycle state are not rewritten.");
            ImGui::TextColored(C_RED,
                "This cannot be undone in DevHub; the old identity remains as an audit alias.");

            if (!target) ImGui::BeginDisabled();
            if (ImGui::Button("Merge contributor")) {
                const std::string sourceName = source->name;
                const std::string targetName = target ? target->name : std::string();
                std::string error;
                bool merged = false;
                try {
                    auto lk = a.db->guard();
                    merged = mergeSourcesLocked(lk.token(),
                        a.db, a.mergeSourceId, a.mergeTargetSourceId, error);
                } catch (const std::exception& e) {
                    error = e.what();
                }
                if (merged) {
                    a.sourceItems.clear();
                    a.needSources = a.needItems = a.needProjects = a.needWork = true;
                    a.mergeSourceId = a.mergeTargetSourceId = 0;
                    ImGui::CloseCurrentPopup();
                    toast(a, sourceName + " merged into " + targetName);
                } else {
                    toast(a, error.empty() ? "contributor merge failed" : error, true);
                }
            }
            if (!target) ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.mergeSourceId = a.mergeTargetSourceId = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Full-width drill-down: every suggestion from the selected people.
    // Click an entry to read the complete original message (promoted Discord
    // messages keep the full text + author/channel/timestamp footer in the
    // item body - nothing is cut off here, unlike the short title).
    for (auto& s : a.sources) {
        auto itn = a.sourceItems.find(s.id);
        if (itn == a.sourceItems.end()) continue;
        ImGui::PushID((int)(s.id + 500000));
        ImGui::SeparatorText(("Suggestions from " + s.name).c_str());
        if (itn->second.empty())
            ImGui::TextColored(C_DIM, "nothing recorded yet");
        for (auto& it : itn->second) {
            ImGui::PushID((int)it.id);
            bool open = a.creditExpanded.count(it.id) > 0;
            if (ImGui::Selectable((it.title + "###ci").c_str(), open)) {
                if (open) a.creditExpanded.erase(it.id);
                else a.creditExpanded.insert(it.id);
                open = !open;
            }
            chip(it.type.c_str(), typeColor(it.type));
            ImGui::SameLine();
            chip(it.status.c_str(), statusColor(it.status));
            ImGui::SameLine();
            if (it.status == "completed")
                chip(it.credited ? "credited" : "credit owed",
                     it.credited ? C_GREEN : C_ORANGE);
            if (it.status == "completed") ImGui::SameLine();
            ImGui::TextColored(C_DIM, "%s", it.source.c_str()); // project
            ImGui::SameLine();
            ImGui::TextColored(C_DIM, "- %s", shortDate(it.created).c_str());
            if (open) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG2);
                ImGui::BeginChild("cbody", ImVec2(0, 0),
                                  ImGuiChildFlags_AutoResizeY |
                                      ImGuiChildFlags_Borders);
                ImGui::TextColored(C_DIM, "suggested:");
                ImGui::SameLine();
                ImGui::TextUnformatted(shortTs(it.created).c_str());
                if (!it.completedAt.empty()) {
                    ImGui::SameLine(0, 18);
                    ImGui::TextColored(C_DIM, "shipped:");
                    ImGui::SameLine();
                    ImGui::TextColored(C_GREEN, "%s",
                                       shortTs(it.completedAt).c_str());
                }
                ImGui::Separator();
                if (it.body.empty())
                    ImGui::TextUnformatted("(no message body - added manually)");
                else
                    drawBodyWithLinks(it.body, C_TEXT);
                if (ImGui::SmallButton("open ticket ->")) {
                    jumpToItem(a, it.projectId, it.id);
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    ImGui::PopID();
                    return;
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

static void drawSettings(App& a) {
    ImGui::SeparatorText("App");
    ImGui::Text("Internal application: %s", DEVHUB_APP_NAME);
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("Visual display name", "My Devhub",
                             a.appDisplayNameInput,
                             sizeof(a.appDisplayNameInput));
    if (ImGui::Button("Save display name"))
        saveAppDisplayName(a, a.appDisplayNameInput);
    ImGui::SameLine();
    if (ImGui::Button("Reset to XA DevHub")) {
        a.appDisplayNameInput[0] = 0;
        saveAppDisplayName(a, "");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(C_DIM,
        "Optional visual name for the sidebar and Windows title only. Leave "
        "blank to use XA DevHub. Up to 48 letters, numbers, spaces, and "
        "standard punctuation; the XA logo, product identity, API, bot, "
        "packets, and automation names do not change.");
    ImGui::PopTextWrapPos();
    if (a.appDisplayNameInvalid) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_ORANGE,
            "The saved visual name is invalid, so XA DevHub is being used. "
            "Save a valid name or reset it.");
        ImGui::PopTextWrapPos();
    }
    ImGui::Text("Local automation API: http://127.0.0.1:%u", a.apiPort);
    ImGui::TextColored(C_DIM,
        "The Discord monitor runs inside this app (Discord page). The local "
        "API is only for scripts/automation. Webhook notifications for new "
        "detections are configured on the Discord page. The bottom debug log "
        "toggle lives at the foot of the sidebar.");
    std::string dataDir = a.db->path();
    auto slash = dataDir.find_last_of("/\\");
    if (slash != std::string::npos) dataDir = dataDir.substr(0, slash);
    if (ImGui::Button(ICON_FOLDER " open data folder")) openFolder(dataDir);

    ImGui::SeparatorText("Work management");
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("stale after days", &a.staleDays);
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("release draft window (days)", &a.releaseDraftDays);
    int completedPreset =
        dashboardCompletedPresetIndex(a.dashboardCompletedDays);
    ImGui::SetNextItemWidth(180);
    if (ImGui::Combo("dashboard completion window", &completedPreset,
                     kDashboardCompletedPresetLabels,
                     kDashboardCompletedPresetCount)) {
        a.dashboardCompletedDays =
            kDashboardCompletedPresetDays[completedPreset];
        a.needProjects = true;
    }
    ImGui::TextColored(C_DIM,
        "Stale items are highlighted in Work Queue. Release drafts use their "
        "own window. The Dashboard completion tile uses the selected preset.");

    ImGui::SeparatorText("Backups and portable exports");
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("versions to retain", &a.backupRetention);
    if (ImGui::Button("Save maintenance settings")) {
        a.staleDays = std::max(1, std::min(a.staleDays, 3650));
        a.releaseDraftDays = std::max(1, std::min(a.releaseDraftDays, 3650));
        a.backupRetention = std::max(3, std::min(a.backupRetention, 90));
        a.dashboardCompletedDays =
            normalizeDashboardCompletedDays(a.dashboardCompletedDays);
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), "stale_days", std::to_string(a.staleDays));
        a.db->setSetting(lk.token(), "release_draft_days", std::to_string(a.releaseDraftDays));
        a.db->setSetting(lk.token(), "backup_retention", std::to_string(a.backupRetention));
        a.db->setSetting(lk.token(), "dashboard_completed_days",
                         std::to_string(a.dashboardCompletedDays));
        a.needProjects = true;
        a.needWork = true;
        toast(a, "maintenance settings saved");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(a.maintenance && a.maintenance->busy());
    if (ImGui::Button("Back up now") && a.maintenance)
        a.maintenance->requestBackup(a.backupRetention);
    ImGui::SameLine();
    if (ImGui::Button("Export Markdown + JSON") && a.maintenance)
        a.maintenance->requestExport(a.backupRetention);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER " backups")) openFolder(dataDir + "\\backups");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FOLDER " exports")) openFolder(dataDir + "\\exports");
    ImGui::TextColored(C_DIM,
        "A coherent SQLite backup plus Markdown/JSON export is created automatically once per day.");
    if (a.maintenance && a.maintenance->busy())
        ImGui::TextColored(C_DIM, "maintenance running...");
    if (!a.maintenanceStatus.empty())
        ImGui::TextColored(C_DIM, "%s", a.maintenanceStatus.c_str());

    ImGui::SeparatorText("AI integration");
    ImGui::TextColored(C_DIM,
        "Review, release, knowledge, report, and resume copies use one provider-neutral "
        "packet contract with Codex skill routes, context-only authority, stable SHA-256 "
        "identity, active workflow direction, and preservation safeguards. Raw data copy "
        "buttons are labeled separately. Automation can GET /api/export/review?project_id=N.");
}

// ---------------------------------------------------------------------------
// modals + console + toast
// ---------------------------------------------------------------------------
static void drawConsole(App& a) {
    if (!a.console.open) return;
    ImGui::Separator();
    ImGui::TextUnformatted(a.console.title.c_str());
    ImGui::SameLine();
    std::string st = a.console.status;
    if (a.console.done && a.console.exitCode >= 0)
        st += " (exit " + std::to_string(a.console.exitCode) + ")";
    ImGui::TextColored(statusColor(a.console.status), "%s", st.c_str());
    if (a.console.truncated) {
        ImGui::SameLine();
        ImGui::TextColored(C_ORANGE, "visible tail capped at 512 KiB");
    }
    ImGui::SameLine();
    ImGui::Checkbox("autoscroll", &a.console.autoScroll);
    ImGui::SameLine();
    if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(a.console.text.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("close")) { a.console.open = false; return; }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0x0a0d12));
    ImGui::BeginChild("console", ImVec2(0, 230), ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(a.console.text.c_str());
    ImGui::PopTextWrapPos();
    if (a.console.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Optional bottom debug-log panel: everything the app does (ui actions,
// builds, discord bot, detections, webhook deliveries) in one strip.
static void drawLogPanel(App& a) {
    uint64_t rev = appLogRevision();
    if (rev != a.logRevSeen) {
        a.logLines = appLogSnapshot();
        a.logRevSeen = rev;
    }
    ImGui::Separator();
    ImGui::TextColored(C_DIM, "Debug log (%d)", (int)a.logLines.size());
    ImGui::SameLine();
    ImGui::Checkbox("autoscroll##log", &a.logAutoScroll);
    ImGui::SameLine();
    if (ImGui::SmallButton("copy##log")) {
        std::string all;
        for (auto& l : a.logLines) { all += l; all += '\n'; }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("clear##log")) appLogClear();
    ImGui::SameLine();
    if (ImGui::SmallButton("hide##log")) {
        a.showLog = false;
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), "show_log_panel", "0");
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgba(0x0a0d12));
    // Height 0 = fill whatever remains of the bottom strip exactly - a
    // fixed height here overflowed the strip by a few pixels and forced
    // the main area itself to scroll. Long logs scroll INSIDE this box.
    ImGui::BeginChild("applog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (auto& l : a.logLines) {
        ImVec4 col = C_DIM;
        if (l.find("ERROR") != std::string::npos ||
            l.find("FAILED") != std::string::npos)
            col = C_RED;
        else if (l.find("[detect]") != std::string::npos)
            col = C_ORANGE;
        ImGui::TextColored(col, "%s", l.c_str());
    }
    if (a.logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 30)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void drawToast(App& a) {
    if (ImGui::GetTime() >= a.toastUntil) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16,
                                   vp->WorkPos.y + vp->WorkSize.y - 16),
                            ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleColor(ImGuiCol_Border, a.toastErr ? C_RED : C_ACCENT);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(a.toastErr ? C_RED : C_TEXT, "%s", a.toastMsg.c_str());
    ImGui::End();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------
static void refreshFrameCaches(App& a) {
    // The user is mid-interaction (open dropdown, held widget, active text
    // field, modal...) - do NOT touch the caches under them: a reload would
    // rebuild the rows and reset whatever they are picking or typing.
    bool interacting =
        ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                        ImGuiPopupFlags_AnyPopupLevel);

    // Live refresh: the embedded bot writes to the DB from its own threads,
    // so poll the lists every few seconds while relevant pages are visible.
    double now = ImGui::GetTime();
    if (!interacting && now - a.lastAutoRefresh > 5.0) {
        a.lastAutoRefresh = now;
        a.needProjects = true;
        if (a.page == Page::Project) a.needItems = true;
        if (a.page == Page::Work) a.needWork = true;
        if (a.page == Page::Knowledge) a.needKnowledge = true;
        if (a.page == Page::Discord) {
            a.needInbox = a.needChannels = a.needGuilds = true;
            a.needNotifyCounters = true;
        }
    }
    // reloads (deferred while interacting; the need* flags keep them queued)
    if (!interacting) {
        if (a.needProjects) loadProjects(a);
        if (a.needWork && a.page == Page::Work) loadWorkItems(a);
        if (a.needKnowledge && a.page == Page::Knowledge) loadKnowledgeWorkspace(a);
        if (a.needItems && a.page == Page::Project) loadItems(a);
        if (a.needSources && a.page == Page::Credits) loadSources(a);
        if (a.needChannels && a.page == Page::Discord) loadChannels(a);
        if (a.needGuilds && a.page == Page::Discord) loadGuilds(a);
        if (a.needInbox && a.page == Page::Discord) loadInbox(a);
        if (a.needCal && a.page == Page::Calendar) loadCalendar(a);
    }
}

static void recordFrameFailure(App& a, const std::string& error) {
    const std::string safe = normalizeUtf8(error);
    appLog("[ui] frame failed: " + safe);
    a.toastMsg = "frame error: " + safe;
    if (a.toastMsg.size() > 500) a.toastMsg.resize(500);
    a.toastErr = true;
    a.toastUntil = ImGui::GetTime() + 6.0;
    a.needProjects = true;
    a.needWork = true;
    a.needItems = true;
}

static void drawFrame(App& a) {
    pollConsole(a);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    // Zero window padding so the sidebar sits flush against the window
    // edges - none of the dark blue background peeks through around it.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    // sidebar: full-height light-gray panel, its own color family
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_SIDEBAR);
    ImGui::PushStyleColor(ImGuiCol_Header, C_SIDESEL);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, C_SIDEHOV);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, C_SIDESEL);
    // Keep the default layout compact, widening only as needed for a custom
    // presentation name. Very long valid names wrap at the bounded maximum.
    ImGui::PushFont(a.fontBig);
    const float displayNameWidth = ImGui::CalcTextSize(a.appDisplayName.c_str()).x;
    ImGui::PopFont();
    const float sidebarWidth = std::clamp(displayNameWidth + 28.0f,
                                          154.0f, 260.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
    ImGui::BeginChild("sidebar", ImVec2(sidebarWidth, 0),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(a.fontBig);
    if (a.appDisplayName == DEVHUB_APP_NAME) {
        ImGui::TextUnformatted("XA");
        ImGui::SameLine(0, 6);
        ImGui::TextColored(C_ACCENT, "DevHub");
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_ACCENT, "%s", a.appDisplayName.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::TextColored(C_DIM, "v%s", DEVHUB_VERSION);
    ImGui::Spacing();
    ImGui::Spacing();

    struct Nav { const char* label; Page page; };
    Nav navs[] = {
        {"Dashboard", Page::Dashboard}, {"Work Queue", Page::Work},
        {"Knowledge", Page::Knowledge},
        {"Calendar", Page::Calendar},
        {"Discord", Page::Discord},     {"Credits", Page::Credits},
        {"Settings", Page::Settings},
    };
    // Center the label vertically in the taller row so the active highlight
    // has equal breathing room above and below the text.
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    for (auto& n : navs) {
        bool active = a.page == n.page ||
                      (n.page == Page::Dashboard && a.page == Page::Project);
        std::string label = std::string("  ") + n.label;
        if (n.page == Page::Discord && a.statPending > 0)
            label += " (" + std::to_string(a.statPending) + ")";
        if (ImGui::Selectable(label.c_str(), active, 0,
                              ImVec2(0, ImGui::GetTextLineHeight() * 1.7f))) {
            a.page = n.page;
            if (n.page == Page::Discord)
                a.needChannels = a.needGuilds = a.needInbox = true;
            if (n.page == Page::Work) a.needWork = true;
            if (n.page == Page::Knowledge) a.needKnowledge = true;
            if (n.page == Page::Credits) a.needSources = true;
            if (n.page == Page::Calendar) a.needCal = true;
        }
    }
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, ImGui::GetContentRegionAvail().y -
                               ImGui::GetTextLineHeightWithSpacing() * 3));
    bool sl = a.showLog;
    if (ImGui::Checkbox("debug log", &sl)) {
        a.showLog = sl;
        auto lk = a.db->guard();
        a.db->setSetting(lk.token(), "show_log_panel", sl ? "1" : "0");
    }
    ImGui::TextColored(C_DIM, "api :%u", a.apiPort);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0, 0);

    // right column: page content on top, then (optional) build console and
    // debug log strips pinned to the bottom
    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_None);
    float reserve = 0.0f;
    if (a.console.open) reserve += 276.0f;
    if (a.showLog) reserve += 192.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::BeginChild("content", ImVec2(0, reserve > 0 ? -reserve : 0.0f),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (a.page) {
        case Page::Dashboard: drawDashboard(a); break;
        case Page::Work:      drawWork(a); break;
        case Page::Project:   drawProject(a); break;
        case Page::Knowledge: drawKnowledge(a); break;
        case Page::Calendar:  drawCalendar(a); break;
        case Page::Discord:   drawDiscord(a); break;
        case Page::Credits:   drawCredits(a); break;
        case Page::Settings:  drawSettings(a); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (a.console.open || a.showLog) {
        // The strip itself must never scroll - only the page content above
        // and the log/console boxes inside it do.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 4));
        ImGui::BeginChild("bottombars", ImVec2(0, 0),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        if (a.console.open) drawConsole(a);
        if (a.showLog) drawLogPanel(a);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild(); // right

    drawModals(a);
    ImGui::End();
    drawToast(a);
}

// ---------------------------------------------------------------------------
// theme
// ---------------------------------------------------------------------------
static void setupTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.ChildRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.CellPadding = ImVec2(8, 5);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = C_BG;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = C_BG2;
    c[ImGuiCol_Border] = C_BORDER;
    c[ImGuiCol_Text] = C_TEXT;
    c[ImGuiCol_TextDisabled] = C_DIM;
    c[ImGuiCol_FrameBg] = C_BG3;
    c[ImGuiCol_FrameBgHovered] = rgba(0x2a3442);
    c[ImGuiCol_FrameBgActive] = rgba(0x2a3442);
    c[ImGuiCol_TitleBg] = C_BG2;
    c[ImGuiCol_TitleBgActive] = C_BG2;
    c[ImGuiCol_Button] = C_BG3;
    c[ImGuiCol_ButtonHovered] = rgba(0x2a3442);
    c[ImGuiCol_ButtonActive] = rgba(0x33415a);
    c[ImGuiCol_Header] = rgba(0x1f2937);
    c[ImGuiCol_HeaderHovered] = rgba(0x2a3442);
    c[ImGuiCol_HeaderActive] = rgba(0x33415a);
    c[ImGuiCol_CheckMark] = C_ACCENT;
    c[ImGuiCol_SliderGrab] = C_ACCENT;
    c[ImGuiCol_SliderGrabActive] = C_ACCENT;
    c[ImGuiCol_Tab] = C_BG;
    c[ImGuiCol_TabHovered] = rgba(0x2a3442);
    c[ImGuiCol_TabSelected] = C_BG3;
    c[ImGuiCol_TableHeaderBg] = C_BG2;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgba(0x161b22, 0.55f);
    c[ImGuiCol_TableBorderLight] = C_BORDER;
    c[ImGuiCol_TableBorderStrong] = C_BORDER;
    c[ImGuiCol_Separator] = C_BORDER;
    c[ImGuiCol_ScrollbarBg] = C_BG;
    c[ImGuiCol_ScrollbarGrab] = C_BG3;
    c[ImGuiCol_ScrollbarGrabHovered] = rgba(0x2a3442);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    c[ImGuiCol_NavCursor] = C_ACCENT;
}

// ---------------------------------------------------------------------------
// DX11 + Win32 host
// ---------------------------------------------------------------------------
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
static void cleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}
static bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &level, &g_context);
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}
static void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

static LRESULT WINAPI wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                g_resizeW = LOWORD(lp);
                g_resizeH = HIWORD(lp);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0; // no ALT menu beep
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int runGui(Db* db, BuildRunner* builds, VersionChecker* versions,
           DiscordBot* discord, MaintenanceRunner* maintenance,
           uint16_t apiPort) {
    ImGui_ImplWin32_EnableDpiAwareness();

    std::string configuredDisplayName;
    bool configuredDisplayNameValid = false;
    {
        auto lk = db->guard();
        configuredDisplayNameValid = normalizeAppDisplayName(
            db->getSetting(lk.token(), kAppDisplayNameSetting), configuredDisplayName);
    }
    const std::string initialDisplayName = configuredDisplayNameValid &&
            !configuredDisplayName.empty()
        ? configuredDisplayName : DEVHUB_APP_NAME;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"XADevHubNative";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_DEVHUB),
                                 IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_DEVHUB),
                                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);

    std::wstring title = widen(initialDisplayName);
    // The compact Dashboard table reserves room for the full Pipeline action
    // strip, so the normal default size remains practical. Clamp it to the
    // usable primary work area so taskbars and smaller screens remain
    // reachable without an off-screen first launch.
    RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int workW = std::max(1L, workArea.right - workArea.left);
    const int workH = std::max(1L, workArea.bottom - workArea.top);
    constexpr int kDefaultWindowWidth = 1360;
    constexpr int kDefaultWindowHeight = 860;
    constexpr int kWorkAreaMargin = 32;
    const int W = std::min(kDefaultWindowWidth, std::max(1, workW - kWorkAreaMargin));
    const int H = std::min(kDefaultWindowHeight, std::max(1, workH - kWorkAreaMargin));
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                WS_OVERLAPPEDWINDOW,
                                workArea.left + (workW - W) / 2,
                                workArea.top + (workH - H) / 2,
                                W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd || !createDevice(hwnd)) {
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Failed to initialise Direct3D 11.", L"XA DevHub",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    // Dark titlebar matching the app theme, set before the window shows so
    // it never flashes white. Attributes are numeric so older SDKs still
    // compile; unsupported Windows builds just ignore them.
    {
        BOOL dark = TRUE; // Windows 10 20H1+
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                              &dark, sizeof(dark));
        COLORREF caption = RGB(0x0d, 0x11, 0x17); // C_BG - Windows 11+
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption,
                              sizeof(caption));
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // fixed layout; nothing worth persisting

    float dpi = (float)GetDpiForWindow(hwnd) / 96.0f;
    if (dpi <= 0) dpi = 1.0f;

    char winDir[MAX_PATH]{};
    GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string fonts = std::string(winDir) + "\\Fonts\\";
    ImFont* base = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(),
                                                17.0f * dpi);
    if (!base) base = io.Fonts->AddFontDefault();
    static const ImWchar iconRange[] = {0xE700, 0xE8FF, 0};
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.GlyphOffset.y = 2.0f * dpi;
    io.Fonts->AddFontFromFileTTF((fonts + "segmdl2.ttf").c_str(), 15.0f * dpi,
                                 &merge, iconRange);

    App app;
    app.fontBig = io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(),
                                               26.0f * dpi);
    if (!app.fontBig) app.fontBig = base;

    setupTheme();
    ImGui::GetStyle().ScaleAllSizes(dpi);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    app.db = db;
    app.builds = builds;
    app.versions = versions;
    app.discord = discord;
    app.maintenance = maintenance;
    app.hwnd = hwnd;
    app.apiPort = apiPort;
    app.appDisplayName = initialDisplayName;
    app.appDisplayNameInvalid = !configuredDisplayNameValid;
    copyBuf(app.appDisplayNameInput, sizeof(app.appDisplayNameInput),
            configuredDisplayNameValid ? configuredDisplayName : "");
    {
        auto lk = db->guard();
        app.showLog = db->getSetting(lk.token(), "show_log_panel") == "1";
        auto intSetting = [&](const char* key, int fallback) {
            try { return std::stoi(db->getSetting(lk.token(), key, std::to_string(fallback))); }
            catch (...) { return fallback; }
        };
        app.staleDays = std::max(1, std::min(intSetting("stale_days", 30), 3650));
        app.releaseDraftDays = std::max(1, std::min(
            intSetting("release_draft_days", 30), 3650));
        app.backupRetention = std::max(3, std::min(
            intSetting("backup_retention", 14), 90));
        app.dashboardCompletedDays = normalizeDashboardCompletedDays(
            intSetting("dashboard_completed_days", 30));
    }
    appLog(std::string("[app] ") + DEVHUB_APP_NAME + " v" + DEVHUB_VERSION +
           " started - api :" + std::to_string(apiPort));
    versions->refreshAsync();

    bool running = true;
    bool occluded = false;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (occluded) {
            const HRESULT visible = g_swap->Present(0, DXGI_PRESENT_TEST);
            if (visible == DXGI_STATUS_OCCLUDED) {
                Sleep(50);
                continue;
            }
            if (FAILED(visible)) {
                appLog("[ui] swap-chain visibility test failed (hr=" +
                       std::to_string(static_cast<long long>(visible)) + ")");
                Sleep(50);
                continue;
            }
            occluded = false;
        }

        if (g_resizeW) {
            cleanupRenderTarget();
            const HRESULT resized = g_swap->ResizeBuffers(
                0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(resized)) {
                appLog("[ui] swap-chain resize failed (hr=" +
                       std::to_string(static_cast<long long>(resized)) + ")");
            }
            g_resizeW = g_resizeH = 0;
            createRenderTarget();
        }

        // Database cache refreshes run between frames so an exception cannot
        // unwind through an active ImGui frame.
        try {
            MaintenanceResult completed;
            if (app.maintenance && app.maintenance->takeCompleted(completed)) {
                app.maintenanceStatus = completed.message;
                toast(app, completed.message, !completed.ok);
            }
            refreshFrameCaches(app);
        } catch (const std::exception& e) {
            recordFrameFailure(app, e.what());
        } catch (...) {
            recordFrameFailure(app, "unknown cache refresh exception");
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        try {
            drawFrame(app);
        } catch (const std::exception& e) {
            recordFrameFailure(app, e.what());
        } catch (...) {
            recordFrameFailure(app, "unknown draw exception");
        }
        ImGui::Render();

        const float clear[4] = {C_BG.x, C_BG.y, C_BG.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT presented = g_swap->Present(1, 0); // vsync
        occluded = presented == DXGI_STATUS_OCCLUDED;
        if (FAILED(presented) && !occluded) {
            appLog("[ui] swap-chain present failed (hr=" +
                   std::to_string(static_cast<long long>(presented)) + ")");
            Sleep(50);
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace devhub
