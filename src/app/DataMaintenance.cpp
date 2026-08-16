#include "DataMaintenance.h"
#include "Db.h"
#include "Version.h"
#include "devhub/Util.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <windows.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace devhub {

static std::string localStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
    return buf;
}

static std::string sqlQuote(std::string value) {
    return replaceAll(std::move(value), "'", "''");
}

static fs::path uniquePath(const fs::path& dir, const std::string& stem,
                           const std::string& extension) {
    fs::path candidate = dir / (stem + extension);
    for (int n = 2; fs::exists(candidate); ++n)
        candidate = dir / (stem + "-" + std::to_string(n) + extension);
    return candidate;
}

static void retainNewest(const fs::path& dir, const std::string& prefix,
                         const std::vector<std::string>& extensions, int keep) {
    keep = std::max(3, std::min(keep, 90));
    // Snapshot timestamps once. A file removed between enumeration and sort is
    // skipped, and the comparator stays total and non-throwing.
    struct Candidate {
        fs::path path;
        fs::file_time_type when;
    };
    std::vector<Candidate> files;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        if (std::find(extensions.begin(), extensions.end(),
                      entry.path().extension().string()) == extensions.end()) continue;
        std::error_code timeEc;
        const auto when = fs::last_write_time(entry.path(), timeEc);
        if (timeEc) continue;
        files.push_back({entry.path(), when});
    }
    std::sort(files.begin(), files.end(), [](const Candidate& a, const Candidate& b) {
        return a.when > b.when;
    });
    const size_t maxFiles = static_cast<size_t>(keep) * extensions.size();
    for (size_t i = maxFiles; i < files.size(); ++i)
        fs::remove(files[i].path, ec);
}

MaintenanceResult createDatabaseBackup(Db* db, int retentionCount) {
    MaintenanceResult result;
    try {
        fs::path dir = fs::path(db->path()).parent_path() / "backups";
        fs::create_directories(dir);
        fs::path out = uniquePath(dir, "devhub-" + localStamp(), ".db");
        {
            auto lk = db->guard();
            db->raw(lk.token()).exec("PRAGMA wal_checkpoint(PASSIVE)");
            db->raw(lk.token()).exec("VACUUM INTO '" + sqlQuote(out.string()) + "'");
        }
        retainNewest(dir, "devhub-", {".db"}, retentionCount);
        result.ok = true;
        result.primaryPath = out.string();
        result.message = "Database backup created: " + out.filename().string();
        appLog("[maintenance] " + result.message);
    } catch (const std::exception& e) {
        result.message = std::string("Database backup failed: ") + e.what();
        appLog("[maintenance] " + result.message);
    }
    return result;
}

MaintenanceResult exportPortableData(Db* db, int retentionCount) {
    MaintenanceResult result;
    try {
        fs::path dir = fs::path(db->path()).parent_path() / "exports";
        fs::create_directories(dir);
        const std::string stem = "devhub-export-" + localStamp();
        fs::path jsonPath = uniquePath(dir, stem, ".json");
        fs::path mdPath = jsonPath;
        mdPath.replace_extension(".md");
        json root;
        std::ostringstream md;
        {
            auto lk = db->guard();
            root["format"] = "xa-devhub-portable-export";
            root["format_version"] = 3;
            root["app_version"] = DEVHUB_VERSION;
            root["exported_at"] = nowIsoUtc();
            auto table = [&](const char* name) {
                SQLite::Statement q(db->raw(lk.token()), std::string("SELECT * FROM ") + name);
                root[name] = Db::rowsToJson(q);
            };
            table("projects"); table("sources"); table("source_merges");
            table("items");
            table("item_sources"); table("item_merges"); table("events");
            table("builds"); table("discord_guilds"); table("discord_channels");
            table("discord_messages"); table("activity_log");
            // Portable exports never copy arbitrary settings: the settings
            // table may contain Discord tokens or future credentials. Export
            // only explicit non-secret operator/UI preferences.
            {
                SQLite::Statement settings(db->raw(lk.token()), R"sql(
SELECT key,value FROM settings
WHERE key IN ('stale_days','release_draft_days','backup_retention',
              'dashboard_completed_days',
              'last_auto_maintenance','show_log_panel',
              'leaderboard_excluded_source_ids')
   OR key LIKE 'ui_sec_%'
ORDER BY key)sql");
                root["settings"] = Db::rowsToJson(settings);
            }
            root["privacy_notice"] =
                "Secret settings are excluded. User-authored knowledge, work, Discord, build, and evidence text may still be sensitive; review before sharing.";
            table("knowledge_sources"); table("knowledge_nodes");
            table("knowledge_node_sources"); table("knowledge_claims");
            table("knowledge_links"); table("knowledge_conflicts");
            table("reports"); table("packet_snapshots"); table("workflow_runs");
            table("workflow_artifacts");
            table("verification_evidence"); table("learnings");
            table("retrieval_evaluations"); table("processing_runs");

            md << "# XA DevHub portable project export\n\n"
               << "Generated by XA DevHub v" << DEVHUB_VERSION << " on "
               << root["exported_at"].get<std::string>() << ".\n\n"
               << "> Secret settings are excluded. User-authored knowledge, work, "
                  "Discord, build, and evidence text may still be sensitive; review "
                  "this export before sharing.\n\n";
            SQLite::Statement projects(db->raw(lk.token()),
                "SELECT id,name,description,path,archived FROM projects "
                "ORDER BY archived,sort_order,name COLLATE NOCASE");
            while (projects.executeStep()) {
                const long long projectId = projects.getColumn(0).getInt64();
                md << "## " << projects.getColumn(1).getString();
                if (projects.getColumn(4).getInt()) md << " (archived)";
                md << "\n\n";
                std::string description = projects.getColumn(2).getString();
                std::string path = projects.getColumn(3).getString();
                if (!description.empty()) md << description << "\n\n";
                if (!path.empty()) md << "Path: `" << path << "`\n\n";
                SQLite::Statement items(db->raw(lk.token()), R"sql(
SELECT i.type,i.status,i.priority,i.title,i.body,i.due_date,i.review_date,
       i.blocked_reason,i.created_at,i.updated_at,i.completed_at,
       IFNULL(GROUP_CONCAT(s.name, ', '),'')
FROM items i LEFT JOIN item_sources x ON x.item_id=i.id
LEFT JOIN sources s ON s.id=x.source_id
WHERE i.project_id=? GROUP BY i.id
ORDER BY CASE i.status WHEN 'open' THEN 0 WHEN 'in_progress' THEN 1
 WHEN 'blocked' THEN 2 WHEN 'completed' THEN 3 ELSE 4 END,
 i.priority DESC,i.id)sql");
                items.bind(1, projectId);
                while (items.executeStep()) {
                    md << "### [" << items.getColumn(1).getString() << "] "
                       << items.getColumn(3).getString() << "\n\n"
                       << "- Type: " << items.getColumn(0).getString()
                       << "; priority: P" << items.getColumn(2).getInt() << "\n";
                    const std::string contributors = items.getColumn(11).getString();
                    if (!contributors.empty()) md << "- Contributors: " << contributors << "\n";
                    const std::string due = items.getColumn(5).getString();
                    const std::string review = items.getColumn(6).getString();
                    if (!due.empty()) md << "- Due: " << due << "\n";
                    if (!review.empty()) md << "- Review again: " << review << "\n";
                    const std::string blocked = items.getColumn(7).getString();
                    if (!blocked.empty()) md << "- Blocked: " << blocked << "\n";
                    const std::string body = items.getColumn(4).getString();
                    if (!body.empty()) md << "\n" << body << "\n";
                    md << "\n";
                }

                md << "### Current project knowledge\n\n";
                SQLite::Statement knowledge(db->raw(lk.token()), R"sql(
SELECT id,kind,title,preamble,summary,confidence,freshness,volatility,
 observed_at,review_after,tags,updated_at
FROM knowledge_nodes WHERE project_id=? AND status='active'
ORDER BY pinned DESC,updated_at DESC,title COLLATE NOCASE)sql");
                knowledge.bind(1, projectId);
                bool hasKnowledge = false;
                while (knowledge.executeStep()) {
                    hasKnowledge = true;
                    md << "#### K" << knowledge.getColumn(0).getInt64() << " ["
                       << knowledge.getColumn(1).getString() << "] "
                       << knowledge.getColumn(2).getString() << "\n\n"
                       << "- Confidence: " << knowledge.getColumn(5).getString()
                       << "; freshness: " << knowledge.getColumn(6).getString()
                       << "; volatility: " << knowledge.getColumn(7).getString() << "\n";
                    const std::string observed = knowledge.getColumn(8).getString();
                    const std::string review = knowledge.getColumn(9).getString();
                    if (!observed.empty()) md << "- Observed: " << observed << "\n";
                    if (!review.empty()) md << "- Review after: " << review << "\n";
                    const std::string preamble = knowledge.getColumn(3).getString();
                    const std::string summary = knowledge.getColumn(4).getString();
                    if (!preamble.empty()) md << "\n" << preamble << "\n";
                    if (!summary.empty()) md << "\n" << summary << "\n";
                    md << "\n";
                }
                if (!hasKnowledge) md << "_No active knowledge nodes._\n\n";

                md << "### Development workflows\n\n";
                SQLite::Statement workflows(db->raw(lk.token()), R"sql(
SELECT id,title,context,phase,status,current_step,next_action,blockers_json,updated_at
FROM workflow_runs WHERE project_id=? AND status IN ('active','blocked','verification')
ORDER BY updated_at DESC)sql");
                workflows.bind(1, projectId);
                bool hasWorkflows = false;
                while (workflows.executeStep()) {
                    hasWorkflows = true;
                    md << "- W" << workflows.getColumn(0).getInt64() << " ["
                       << workflows.getColumn(2).getString() << "/"
                       << workflows.getColumn(3).getString() << "/"
                       << workflows.getColumn(4).getString() << "] "
                       << workflows.getColumn(1).getString() << "; current: "
                       << workflows.getColumn(5).getString() << "; next: "
                       << workflows.getColumn(6).getString() << "\n";
                }
                if (!hasWorkflows) md << "_No open development workflows._\n";
                md << "\n### Reports\n\n";
                SQLite::Statement reports(db->raw(lk.token()), R"sql(
SELECT id,report_type,title,status,overall_status,evidence_at,input_hash,updated_at
FROM reports WHERE project_id=? ORDER BY updated_at DESC LIMIT 50)sql");
                reports.bind(1, projectId);
                bool hasReports = false;
                while (reports.executeStep()) {
                    hasReports = true;
                    md << "- R" << reports.getColumn(0).getInt64() << " ["
                       << reports.getColumn(1).getString() << "/"
                       << reports.getColumn(3).getString() << "] "
                       << reports.getColumn(2).getString();
                    const std::string overall = reports.getColumn(4).getString();
                    if (!overall.empty()) md << " - " << overall;
                    md << "; evidence " << reports.getColumn(5).getString()
                       << "; input " << reports.getColumn(6).getString() << "\n";
                }
                if (!hasReports) md << "_No saved reports._\n";
                md << "\n";
            }
            md << "## Cross-project knowledge\n\n";
            SQLite::Statement globalKnowledge(db->raw(lk.token()), R"sql(
SELECT id,kind,title,summary,confidence,freshness,updated_at
FROM knowledge_nodes WHERE project_id IS NULL AND status='active'
ORDER BY pinned DESC,updated_at DESC)sql");
            bool hasGlobalKnowledge = false;
            while (globalKnowledge.executeStep()) {
                hasGlobalKnowledge = true;
                md << "### K" << globalKnowledge.getColumn(0).getInt64() << " ["
                   << globalKnowledge.getColumn(1).getString() << "] "
                   << globalKnowledge.getColumn(2).getString() << "\n\n"
                   << "- Confidence: " << globalKnowledge.getColumn(4).getString()
                   << "; freshness: " << globalKnowledge.getColumn(5).getString() << "\n\n"
                   << globalKnowledge.getColumn(3).getString() << "\n\n";
            }
            if (!hasGlobalKnowledge) md << "_No active cross-project knowledge._\n\n";

            md << "## Unresolved knowledge conflicts\n\n";
            SQLite::Statement conflicts(db->raw(lk.token()), R"sql(
SELECT x.id,l.title,r.title,x.classification,x.reason,x.updated_at
FROM knowledge_conflicts x JOIN knowledge_nodes l ON l.id=x.left_node_id
JOIN knowledge_nodes r ON r.id=x.right_node_id
WHERE x.status='open' ORDER BY x.updated_at DESC)sql");
            bool hasConflicts = false;
            while (conflicts.executeStep()) {
                hasConflicts = true;
                md << "- X" << conflicts.getColumn(0).getInt64() << " ["
                   << conflicts.getColumn(3).getString() << "] "
                   << conflicts.getColumn(1).getString() << " <> "
                   << conflicts.getColumn(2).getString() << ": "
                   << conflicts.getColumn(4).getString() << "\n";
            }
            if (!hasConflicts) md << "_No unresolved knowledge conflicts._\n";
            md << "\n";
            md << "## Pending Discord feedback\n\n";
            SQLite::Statement pending(db->raw(lk.token()), R"sql(
SELECT IFNULL(p.name,'unassigned'),m.kind,m.author,m.content,m.posted_at,
       IFNULL(NULLIF(c.channel_name,''),c.channel_id)
FROM discord_messages m JOIN discord_channels c ON c.id=m.channel_row_id
LEFT JOIN projects p ON p.id=COALESCE(c.project_id,m.inferred_project_id)
WHERE m.state='new' AND m.kind!='none' ORDER BY m.posted_at)sql");
            bool hasPending = false;
            while (pending.executeStep()) {
                hasPending = true;
                md << "### [" << pending.getColumn(1).getString() << "] "
                   << pending.getColumn(0).getString() << "\n\n"
                   << "- From: " << pending.getColumn(2).getString()
                   << " in #" << pending.getColumn(5).getString()
                   << " at " << pending.getColumn(4).getString() << "\n\n"
                   << pending.getColumn(3).getString() << "\n\n";
            }
            if (!hasPending) md << "_No pending captured feedback._\n\n";
        }
        const std::string portableJson = root.dump(2);
        const std::string portableMarkdown = normalizeUtf8(md.str());
        if (!writeFileUtf8(jsonPath.string(), portableJson) ||
            !writeFileUtf8(mdPath.string(), portableMarkdown))
            throw std::runtime_error("could not write export files");
        auto publishLatest = [&](const char* name,
                                 const std::string& text) -> bool {
            const fs::path finalPath = dir / name;
            fs::path stagedPath = uniquePath(
                dir, std::string(name) + ".tmp-" + localStamp(), "");
            if (!writeFileUtf8(stagedPath.string(), text)) {
                std::error_code cleanup;
                fs::remove(stagedPath, cleanup);
                return false;
            }
            // The staging file is in the destination directory, so this is a
            // same-volume atomic replacement. A failure leaves the previous
            // latest file intact instead of truncating it first.
            if (!MoveFileExW(stagedPath.c_str(), finalPath.c_str(),
                             MOVEFILE_REPLACE_EXISTING |
                                 MOVEFILE_WRITE_THROUGH)) {
                std::error_code cleanup;
                fs::remove(stagedPath, cleanup);
                return false;
            }
            return true;
        };
        if (!publishLatest("devhub-latest.json", portableJson) ||
            !publishLatest("devhub-latest.md", portableMarkdown))
            throw std::runtime_error(
                "timestamped exports were created, but devhub-latest.* "
                "could not be updated; the previous latest files remain intact");
        retainNewest(dir, "devhub-export-", {".json", ".md"}, retentionCount);
        result.ok = true;
        result.primaryPath = mdPath.string();
        result.secondaryPath = jsonPath.string();
        result.message = "Portable Markdown and JSON exports created";
        appLog("[maintenance] " + result.message);
    } catch (const std::exception& e) {
        result.message = std::string("Portable export failed: ") + e.what();
        appLog("[maintenance] " + result.message);
    }
    return result;
}

MaintenanceResult runAutomaticDailyMaintenance(Db* db) {
    int retention = 14;
    const std::string today = todayLocal();
    bool needBackup = true;
    bool needExport = true;
    {
        auto lk = db->guard();
        const std::string legacy = db->getSetting(lk.token(), "last_auto_maintenance");
        needBackup = db->getSetting(lk.token(), "last_auto_backup", legacy) != today;
        needExport = db->getSetting(lk.token(), "last_auto_export", legacy) != today;
        if (!needBackup && !needExport)
            return {true, "Daily maintenance already completed", {}, {}};
        try { retention = std::stoi(db->getSetting(lk.token(), "backup_retention", "14")); }
        catch (...) { retention = 14; }
    }
    MaintenanceResult backup;
    if (needBackup) {
        backup = createDatabaseBackup(db, retention);
        if (!backup.ok) return backup;
        {
            auto lk = db->guard();
            db->setSetting(lk.token(), "last_auto_backup", today);
            // Captured report packets are approval evidence. Preserve every
            // snapshot referenced by a saved report; age out only unreferenced
            // captures so repeated abandoned drafts cannot grow exports forever.
            db->raw(lk.token()).exec(R"sql(
DELETE FROM packet_snapshots
WHERE datetime(created_at)<datetime('now','-90 days')
 AND NOT EXISTS(
   SELECT 1 FROM reports r
   WHERE r.input_hash='sha256:'||packet_snapshots.source_hash
    AND r.evidence_at=packet_snapshots.generated_at
 );
)sql");
        }
    }
    MaintenanceResult portable;
    if (needExport) {
        portable = exportPortableData(db, retention);
        if (!portable.ok) return portable;
        auto lk = db->guard();
        db->setSetting(lk.token(), "last_auto_export", today);
    }
    {
        auto lk = db->guard();
        db->setSetting(lk.token(), "last_auto_maintenance", today);
        db->logActivity(lk.token(), "daily_maintenance", 0,
                        backup.primaryPath.empty() ? portable.primaryPath
                                                   : backup.primaryPath);
    }
    MaintenanceResult result = needExport ? portable : backup;
    result.ok = true;
    result.message = needBackup && needExport
        ? "Daily backup and portable export completed"
        : (needBackup ? "Daily backup completed" : "Daily portable export completed");
    return result;
}

bool MaintenanceRunner::requestBackup(int retentionCount) {
    return request(Kind::Backup, retentionCount);
}

bool MaintenanceRunner::requestExport(int retentionCount) {
    return request(Kind::Export, retentionCount);
}

bool MaintenanceRunner::requestAutomatic() {
    return request(Kind::Automatic, 14);
}

bool MaintenanceRunner::request(Kind kind, int retentionCount) {
    if (stopping_.load()) return false;
    bool idle = false;
    if (!busy_.compare_exchange_strong(idle, true)) return false;

    std::thread finished;
    {
        std::lock_guard<std::mutex> lock(workerMu_);
        if (worker_.joinable()) finished = std::move(worker_);
    }
    if (finished.joinable()) finished.join();

    try {
        std::lock_guard<std::mutex> lock(workerMu_);
        if (stopping_.load()) {
            busy_ = false;
            return false;
        }
        worker_ = std::thread([this, kind, retentionCount]() {
            MaintenanceResult result;
            try {
                if (kind == Kind::Backup)
                    result = createDatabaseBackup(db_, retentionCount);
                else if (kind == Kind::Export)
                    result = exportPortableData(db_, retentionCount);
                else
                    result = runAutomaticDailyMaintenance(db_);
            } catch (const std::exception& e) {
                result.message = std::string("Maintenance failed: ") + e.what();
                appLog("[maintenance] " + result.message);
            } catch (...) {
                result.message = "Maintenance failed with an unknown exception";
                appLog("[maintenance] " + result.message);
            }
            {
                std::lock_guard<std::mutex> resultLock(resultMu_);
                completed_ = std::move(result);
                resultReady_ = true;
            }
            busy_ = false;
        });
    } catch (...) {
        busy_ = false;
        throw;
    }
    return true;
}

bool MaintenanceRunner::takeCompleted(MaintenanceResult& result) {
    std::lock_guard<std::mutex> lock(resultMu_);
    if (!resultReady_) return false;
    result = completed_;
    resultReady_ = false;
    return true;
}

void MaintenanceRunner::stop() {
    stopping_ = true;
    std::thread owned;
    {
        std::lock_guard<std::mutex> lock(workerMu_);
        if (worker_.joinable()) owned = std::move(worker_);
    }
    if (owned.joinable()) owned.join();
    busy_ = false;
}

} // namespace devhub
