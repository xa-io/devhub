#include "Db.h"
#include "devhub/Util.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <vector>

using json = nlohmann::json;

namespace devhub {

namespace {

constexpr const char* kDpapiPrefix = "dpapi:v1:";

bool sensitiveSettingKey(std::string key) {
    // A setting exposed by the API is intentionally non-secret. Check the
    // canonical public policy first so UI section names such as
    // ui_sec_discord_webhook cannot be over-sealed.
    if (isPublicSettingKey(key)) return false;
    for (char& ch : key)
        ch = std::isalnum(static_cast<unsigned char>(ch))
            ? static_cast<char>(
                  std::tolower(static_cast<unsigned char>(ch)))
            : '_';
    // Setting names are identifiers, not prose. Match complete identifier
    // components so an innocent preference such as detection_patterns is
    // never classified as a PAT merely because it contains the bytes "pat".
    static const std::set<std::string> credentialComponents = {
        "token", "secret", "password", "passwd", "credential",
        "authorization", "webhook", "cookie", "bearer", "connection",
        "session", "refresh", "hmac", "salt", "pat", "key"
    };
    size_t start = 0;
    while (start < key.size()) {
        const size_t end = key.find('_', start);
        const std::string component = key.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (credentialComponents.find(component) != credentialComponents.end())
            return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

std::string base64Encode(const BYTE* bytes, DWORD size) {
    DWORD chars = 0;
    if (!CryptBinaryToStringA(bytes, size,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &chars))
        throw std::runtime_error("could not size DPAPI ciphertext encoding");
    std::string encoded(chars, '\0');
    if (!CryptBinaryToStringA(bytes, size,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              encoded.data(), &chars))
        throw std::runtime_error("could not encode DPAPI ciphertext");
    while (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
    return encoded;
}

bool base64Decode(const std::string& encoded, std::vector<BYTE>& bytes) {
    DWORD size = 0;
    if (!CryptStringToBinaryA(encoded.c_str(),
                              static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, nullptr, &size, nullptr,
                              nullptr))
        return false;
    bytes.resize(size);
    return CryptStringToBinaryA(encoded.c_str(),
                                static_cast<DWORD>(encoded.size()),
                                CRYPT_STRING_BASE64, bytes.data(), &size,
                                nullptr, nullptr) != FALSE;
}

std::string sealSettingValue(const std::string& value) {
    DATA_BLOB plain{
        static_cast<DWORD>(value.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    DATA_BLOB sealed{};
    if (!CryptProtectData(&plain, L"XA DevHub setting", nullptr, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &sealed))
        throw std::runtime_error("could not protect a credential setting");
    try {
        const std::string encoded = base64Encode(sealed.pbData, sealed.cbData);
        LocalFree(sealed.pbData);
        return std::string(kDpapiPrefix) + encoded;
    } catch (...) {
        LocalFree(sealed.pbData);
        throw;
    }
}

bool unsealSettingValue(const std::string& stored, std::string& value) {
    if (stored.rfind(kDpapiPrefix, 0) != 0) return false;
    std::vector<BYTE> ciphertext;
    if (!base64Decode(stored.substr(std::char_traits<char>::length(kDpapiPrefix)),
                      ciphertext))
        return false;
    DATA_BLOB sealed{static_cast<DWORD>(ciphertext.size()),
                     ciphertext.data()};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&sealed, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &plain))
        return false;
    value.assign(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    LocalFree(plain.pbData);
    return true;
}

bool tableHasColumn(SQLite::Database& db, const std::string& table,
                    const std::string& column) {
    SQLite::Statement q(db, "PRAGMA table_info(" + table + ")");
    while (q.executeStep()) {
        if (q.getColumn(1).getString() == column) return true;
    }
    return false;
}

bool schemaObjectExists(SQLite::Database& db, const std::string& type,
                        const std::string& name) {
    SQLite::Statement q(db,
        "SELECT 1 FROM sqlite_master WHERE type=? AND name=?");
    q.bind(1, type);
    q.bind(2, name);
    return q.executeStep();
}

std::string compactSchemaSql(std::string sql) {
    std::string compact;
    compact.reserve(sql.size());
    for (const unsigned char ch : sql) {
        if (!std::isspace(ch))
            compact.push_back(static_cast<char>(std::tolower(ch)));
    }
    return compact;
}

struct RequiredColumn {
    const char* table;
    const char* name;
    const char* declaration;
};

void ensureColumns(SQLite::Database& db, int schemaVersion,
                   std::initializer_list<RequiredColumn> columns) {
    for (const auto& column : columns) {
        if (!tableHasColumn(db, column.table, column.name)) {
            db.exec(std::string("ALTER TABLE ") + column.table +
                    " ADD COLUMN " + column.name + " " +
                    column.declaration);
        }
    }
    for (const auto& column : columns) {
        if (!tableHasColumn(db, column.table, column.name)) {
            throw std::runtime_error(
                "schema v" + std::to_string(schemaVersion) +
                " migration did not create " + column.table + "." +
                column.name);
        }
    }
}

void ensurePacketSnapshotSchema(SQLite::Database& db) {
    SQLite::Transaction tx(db);
    db.exec(R"sql(
CREATE TABLE IF NOT EXISTS packet_snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    packet_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    scope_project_id INTEGER NOT NULL DEFAULT 0,
    workflow_id INTEGER REFERENCES workflow_runs(id) ON DELETE SET NULL,
    scope_type TEXT NOT NULL DEFAULT '',
    scope_start TEXT NOT NULL DEFAULT '',
    scope_end TEXT NOT NULL DEFAULT '',
    source_hash TEXT NOT NULL,
    packet_hash TEXT NOT NULL,
    generated_at TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_packet_snapshot_source
 ON packet_snapshots(kind,source_hash,generated_at);
)sql");
    ensureColumns(db, 9, {
        {"packet_snapshots", "scope_project_id", "INTEGER NOT NULL DEFAULT 0"},
        {"packet_snapshots", "scope_type", "TEXT NOT NULL DEFAULT ''"},
        {"packet_snapshots", "scope_start", "TEXT NOT NULL DEFAULT ''"},
        {"packet_snapshots", "scope_end", "TEXT NOT NULL DEFAULT ''"},
    });
    db.exec("UPDATE packet_snapshots "
            "SET scope_project_id=IFNULL(project_id,0) "
            "WHERE scope_project_id=0 AND project_id IS NOT NULL");
    db.exec(R"sql(
DELETE FROM packet_snapshots WHERE id NOT IN (
 SELECT MAX(id) FROM packet_snapshots
 GROUP BY kind,source_hash,generated_at,scope_type,scope_start,scope_end
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_packet_snapshot_identity
 ON packet_snapshots(kind,source_hash,generated_at,scope_type,scope_start,scope_end);
CREATE INDEX IF NOT EXISTS idx_packet_snapshot_scope_project
 ON packet_snapshots(scope_project_id,kind,generated_at);
)sql");
    tx.commit();
}

} // namespace

bool isPublicSettingKey(const std::string& key) {
    static const std::set<std::string> publicKeys = {
        "stale_days", "release_draft_days", "backup_retention",
        "dashboard_completed_days", "show_log_panel",
        "last_auto_maintenance", "leaderboard_excluded_source_ids",
        "app_display_name", "detection_patterns", "notify_guild_id",
        "notify_channel_id"
    };
    return publicKeys.find(key) != publicKeys.end() ||
           (key.size() > 7 && key.size() <= 64 &&
            key.rfind("ui_sec_", 0) == 0 &&
            std::all_of(key.begin() + 7, key.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '_';
            }));
}

Db::Db(const std::string& path) : path_(path) {
    db_ = std::make_unique<SQLite::Database>(
        path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db_->exec("PRAGMA journal_mode=WAL");
    db_->exec("PRAGMA foreign_keys=ON");
    db_->exec("PRAGMA busy_timeout=5000");
    migrate();
}

void Db::migrate() {
    static constexpr int kSchemaVersion = 14;
    int userVersion = 0;
    {
        SQLite::Statement version(*db_, "PRAGMA user_version");
        if (!version.executeStep())
            throw std::runtime_error("Could not read the database schema version");
        userVersion = version.getColumn(0).getInt();
    }
    if (userVersion > kSchemaVersion) {
        throw std::runtime_error(
            "This database was created by a newer XA DevHub schema (v" +
            std::to_string(userVersion) + "; this build supports v" +
            std::to_string(kSchemaVersion) +
            "). Opening it here could overwrite newer trigger semantics. "
            "Install the newer build again or restore a database backup from "
            "data\\backups\\.");
    }
    db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS projects (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    slug TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '',
    path TEXT NOT NULL DEFAULT '',
    rules_path TEXT NOT NULL DEFAULT '',
    codex_skills TEXT NOT NULL DEFAULT '',
    build_command TEXT NOT NULL DEFAULT '',
    build_cwd TEXT NOT NULL DEFAULT '',
    run_command TEXT NOT NULL DEFAULT '',
    repo_url TEXT NOT NULL DEFAULT '',
    color TEXT NOT NULL DEFAULT '',
    release_notes TEXT NOT NULL DEFAULT '',
    archived INTEGER NOT NULL DEFAULT 0,
    sort_order INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sources (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    platform TEXT NOT NULL DEFAULT '',
    handle TEXT NOT NULL DEFAULT '',
    notes TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    type TEXT NOT NULL DEFAULT 'note',
    title TEXT NOT NULL,
    body TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'open',
    priority INTEGER NOT NULL DEFAULT 2,
    source_id INTEGER REFERENCES sources(id) ON DELETE SET NULL,
    credited INTEGER NOT NULL DEFAULT 0,
    origin TEXT NOT NULL DEFAULT 'manual',
    due_date TEXT NOT NULL DEFAULT '',
    tags TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    completed_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_items_project ON items(project_id, status);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER REFERENCES projects(id) ON DELETE CASCADE,
    item_id INTEGER REFERENCES items(id) ON DELETE SET NULL,
    title TEXT NOT NULL,
    kind TEXT NOT NULL DEFAULT 'event',
    date TEXT NOT NULL,
    notes TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_date ON events(date);
CREATE TABLE IF NOT EXISTS builds (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    command TEXT NOT NULL,
    cwd TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'running',
    exit_code INTEGER,
    output TEXT NOT NULL DEFAULT '',
    started_at TEXT NOT NULL,
    finished_at TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS discord_channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    channel_id TEXT NOT NULL UNIQUE,
    guild_name TEXT NOT NULL DEFAULT '',
    channel_name TEXT NOT NULL DEFAULT '',
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    enabled INTEGER NOT NULL DEFAULT 1,
    last_read_ts TEXT NOT NULL DEFAULT '',
    last_scan_at TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS discord_messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    channel_row_id INTEGER NOT NULL REFERENCES discord_channels(id) ON DELETE CASCADE,
    message_id TEXT NOT NULL UNIQUE,
    author TEXT NOT NULL DEFAULT '',
    author_id TEXT NOT NULL DEFAULT '',
    content TEXT NOT NULL DEFAULT '',
    posted_at TEXT NOT NULL DEFAULT '',
    ingested_at TEXT NOT NULL,
    kind TEXT NOT NULL DEFAULT 'none',
    score REAL NOT NULL DEFAULT 0,
    matched TEXT NOT NULL DEFAULT '[]',
    state TEXT NOT NULL DEFAULT 'new',
    item_id INTEGER REFERENCES items(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS idx_dmsg_state ON discord_messages(state, kind);
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS activity_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,
    kind TEXT NOT NULL,
    project_id INTEGER,
    detail TEXT NOT NULL DEFAULT ''
);
)sql");

    // ---- v2: release pipeline + version tracking columns ----
    // Replace the old single-admin override with an editable plural whitelist
    // before advancing any schema version. This closes the first-start crash
    // window: a genuinely new database durably records an empty whitelist and
    // can never be mistaken for an existing install on restart. Existing
    // installs keep an explicitly configured legacy owner; an existing plural
    // row always wins, including an intentionally empty row. No source-owned
    // account ID is ever granted administrator access implicitly.
    {
        SQLite::Transaction tx(*db_);
        bool hasCurrent = false;
        {
            SQLite::Statement current(*db_,
                "SELECT 1 FROM settings WHERE key='discord_admin_user_ids'");
            hasCurrent = current.executeStep();
        }
        if (!hasCurrent) {
            std::string initial;
            {
                SQLite::Statement legacy(*db_,
                    "SELECT value FROM settings WHERE key='discord_admin_user_id'");
                if (legacy.executeStep()) {
                    const std::string configured = trim(
                        legacy.getColumn(0).getString());
                    if (!configured.empty()) initial = configured;
                }
            }
            SQLite::Statement insert(*db_,
                "INSERT INTO settings(key,value) VALUES(?,?)");
            insert.bind(1, "discord_admin_user_ids");
            insert.bind(2, initial);
            insert.exec();
        }
        db_->exec("DELETE FROM settings WHERE key='discord_admin_user_id'");
        tx.commit();
    }
    if (userVersion < 2) {
        SQLite::Transaction tx(*db_);
        ensureColumns(*db_, 2, {
            {"projects", "prep_command", "TEXT NOT NULL DEFAULT ''"},
            {"projects", "release_command", "TEXT NOT NULL DEFAULT ''"},
            {"projects", "version_command", "TEXT NOT NULL DEFAULT ''"},
            {"projects", "local_version_file", "TEXT NOT NULL DEFAULT ''"},
            {"projects", "remote_version_url", "TEXT NOT NULL DEFAULT ''"},
            {"projects", "remote_version_key", "TEXT NOT NULL DEFAULT ''"},
            {"projects", "github_url", "TEXT NOT NULL DEFAULT ''"},
            {"builds", "kind", "TEXT NOT NULL DEFAULT 'build'"},
        });
        db_->exec("PRAGMA user_version = 2");
        tx.commit();
    }

    // ---- v3: server-wide Discord monitoring + project inference ----
    if (userVersion < 3) {
        SQLite::Transaction tx(*db_);
        ensureColumns(*db_, 3, {
            {"projects", "aliases", "TEXT NOT NULL DEFAULT ''"},
            {"discord_messages", "inferred_project_id", "INTEGER"},
        });
        db_->exec(
            "CREATE TABLE IF NOT EXISTS discord_guilds ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " guild_id TEXT NOT NULL UNIQUE,"
            " guild_name TEXT NOT NULL DEFAULT '',"
            " enabled INTEGER NOT NULL DEFAULT 1,"
            " created_at TEXT NOT NULL)");
        db_->exec("PRAGMA user_version = 3");
        tx.commit();
    }
    // ---- v4: bot-notification cards + guild ids on channels ----
    if (userVersion < 4) {
        SQLite::Transaction tx(*db_);
        ensureColumns(*db_, 4, {
            {"discord_channels", "guild_id", "TEXT NOT NULL DEFAULT ''"},
            {"discord_messages", "notify_channel_id", "TEXT NOT NULL DEFAULT ''"},
            {"discord_messages", "notify_message_id", "TEXT NOT NULL DEFAULT ''"},
        });
        db_->exec("PRAGMA user_version = 4");
        tx.commit();
    }
    // ---- v5: admin note on manual reply-mention captures ----
    if (userVersion < 5) {
        SQLite::Transaction tx(*db_);
        ensureColumns(*db_, 5, {
            {"discord_messages", "admin_note", "TEXT NOT NULL DEFAULT ''"},
        });
        db_->exec("PRAGMA user_version = 5");
        tx.commit();
    }
    // ---- v6: which projects the Discord !tickets menu lists ----
    if (userVersion < 6) {
        SQLite::Transaction tx(*db_);
        ensureColumns(*db_, 6, {
            {"projects", "discord_tickets", "INTEGER NOT NULL DEFAULT 1"},
        });
        // Seed the non-Discord-facing apps off; one-time so re-enabling a
        // project later sticks. New projects default to listed.
        db_->exec("UPDATE projects SET discord_tickets=0 WHERE name IN "
                  "('CB XA TT','XA TT','Discord Omnibot',"
                  "'Teamspeak WP-Bridge','TS WP-Bridge')");
        db_->exec("PRAGMA user_version = 6");
        tx.commit();
    }
    // ---- v7: review workflow, merged feedback and multi-contributor credit ----
    if (userVersion < 7) {
        SQLite::Transaction tx(*db_);
        ensureColumns(*db_, 7, {
            {"items", "review_date", "TEXT NOT NULL DEFAULT ''"},
            {"items", "blocked_reason", "TEXT NOT NULL DEFAULT ''"},
        });
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS item_sources (
    item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
    credited INTEGER NOT NULL DEFAULT 0,
    added_at TEXT NOT NULL,
    PRIMARY KEY(item_id, source_id)
);
CREATE INDEX IF NOT EXISTS idx_item_sources_source ON item_sources(source_id, credited);
CREATE TABLE IF NOT EXISTS item_merges (
    source_item_id INTEGER PRIMARY KEY REFERENCES items(id) ON DELETE CASCADE,
    target_item_id INTEGER NOT NULL REFERENCES items(id) ON DELETE CASCADE,
    merged_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_item_merges_target ON item_merges(target_item_id);
)sql");
        db_->exec(
            "INSERT OR IGNORE INTO item_sources(item_id,source_id,credited,added_at) "
            "SELECT id,source_id,credited,created_at FROM items WHERE source_id IS NOT NULL");
        db_->exec("PRAGMA user_version = 7");
        tx.commit();
    }
    // ---- v8: advanced knowledge metabolism + resumable development control ----
    if (userVersion < 8) {
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS knowledge_sources (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    uri TEXT NOT NULL DEFAULT '',
    title TEXT NOT NULL DEFAULT '',
    source_type TEXT NOT NULL DEFAULT 'document',
    captured_at TEXT NOT NULL,
    published_at TEXT NOT NULL DEFAULT '',
    content_hash TEXT NOT NULL DEFAULT '',
    raw_text TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ksource_project ON knowledge_sources(project_id, captured_at);
CREATE INDEX IF NOT EXISTS idx_ksource_hash ON knowledge_sources(content_hash);
CREATE UNIQUE INDEX IF NOT EXISTS idx_ksource_uri_hash
 ON knowledge_sources(uri, content_hash) WHERE uri!='' AND content_hash!='';

CREATE TABLE IF NOT EXISTS knowledge_nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    kind TEXT NOT NULL DEFAULT 'reference',
    title TEXT NOT NULL,
    preamble TEXT NOT NULL DEFAULT '',
    summary TEXT NOT NULL DEFAULT '',
    body TEXT NOT NULL DEFAULT '',
    tags TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'active',
    confidence TEXT NOT NULL DEFAULT 'stated',
    freshness TEXT NOT NULL DEFAULT 'timeless',
    volatility TEXT NOT NULL DEFAULT 'stable',
    context_level INTEGER NOT NULL DEFAULT 2,
    pinned INTEGER NOT NULL DEFAULT 0,
    observed_at TEXT NOT NULL DEFAULT '',
    review_after TEXT NOT NULL DEFAULT '',
    pointer_uri TEXT NOT NULL DEFAULT '',
    content_hash TEXT NOT NULL DEFAULT '',
    dedupe_key TEXT NOT NULL DEFAULT '',
    superseded_by INTEGER REFERENCES knowledge_nodes(id) ON DELETE SET NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_knode_project ON knowledge_nodes(project_id, status, kind);
CREATE INDEX IF NOT EXISTS idx_knode_review ON knowledge_nodes(review_after, status);
CREATE INDEX IF NOT EXISTS idx_knode_dedupe ON knowledge_nodes(dedupe_key, status);
CREATE INDEX IF NOT EXISTS idx_knode_hash ON knowledge_nodes(content_hash);

CREATE TABLE IF NOT EXISTS knowledge_node_sources (
    node_id INTEGER NOT NULL REFERENCES knowledge_nodes(id) ON DELETE CASCADE,
    source_id INTEGER NOT NULL REFERENCES knowledge_sources(id) ON DELETE CASCADE,
    locator TEXT NOT NULL DEFAULT '',
    relationship TEXT NOT NULL DEFAULT 'derived_from',
    note TEXT NOT NULL DEFAULT '',
    added_at TEXT NOT NULL,
    PRIMARY KEY(node_id, source_id, locator, relationship)
);
CREATE INDEX IF NOT EXISTS idx_knode_sources_source ON knowledge_node_sources(source_id, node_id);

CREATE TABLE IF NOT EXISTS knowledge_claims (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id INTEGER NOT NULL REFERENCES knowledge_nodes(id) ON DELETE CASCADE,
    claim_text TEXT NOT NULL,
    confidence TEXT NOT NULL DEFAULT 'stated',
    freshness TEXT NOT NULL DEFAULT 'timeless',
    volatility TEXT NOT NULL DEFAULT 'stable',
    observed_at TEXT NOT NULL DEFAULT '',
    review_after TEXT NOT NULL DEFAULT '',
    source_id INTEGER REFERENCES knowledge_sources(id) ON DELETE SET NULL,
    source_locator TEXT NOT NULL DEFAULT '',
    inference INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'active',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_kclaim_node ON knowledge_claims(node_id, status);
CREATE INDEX IF NOT EXISTS idx_kclaim_review ON knowledge_claims(review_after, status);
CREATE INDEX IF NOT EXISTS idx_kclaim_source ON knowledge_claims(source_id);

CREATE TABLE IF NOT EXISTS knowledge_links (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    from_node_id INTEGER NOT NULL REFERENCES knowledge_nodes(id) ON DELETE CASCADE,
    to_node_id INTEGER NOT NULL REFERENCES knowledge_nodes(id) ON DELETE CASCADE,
    relation TEXT NOT NULL DEFAULT 'related',
    strength REAL NOT NULL DEFAULT 1.0,
    note TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    UNIQUE(from_node_id, to_node_id, relation)
);
CREATE INDEX IF NOT EXISTS idx_klink_from ON knowledge_links(from_node_id, relation);
CREATE INDEX IF NOT EXISTS idx_klink_to ON knowledge_links(to_node_id, relation);

CREATE TABLE IF NOT EXISTS knowledge_conflicts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    left_node_id INTEGER NOT NULL REFERENCES knowledge_nodes(id) ON DELETE CASCADE,
    right_node_id INTEGER NOT NULL REFERENCES knowledge_nodes(id) ON DELETE CASCADE,
    classification TEXT NOT NULL DEFAULT 'ambiguous',
    status TEXT NOT NULL DEFAULT 'open',
    reason TEXT NOT NULL DEFAULT '',
    resolution TEXT NOT NULL DEFAULT '',
    winner_node_id INTEGER REFERENCES knowledge_nodes(id) ON DELETE SET NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    resolved_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_kconflict_status ON knowledge_conflicts(status, updated_at);

CREATE TABLE IF NOT EXISTS reports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    report_type TEXT NOT NULL DEFAULT 'project',
    title TEXT NOT NULL,
    period_start TEXT NOT NULL DEFAULT '',
    period_end TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'draft',
    overall_status TEXT NOT NULL DEFAULT '',
    content TEXT NOT NULL DEFAULT '',
    findings_json TEXT NOT NULL DEFAULT '[]',
    evidence_at TEXT NOT NULL DEFAULT '',
    input_hash TEXT NOT NULL DEFAULT '',
    generated_by TEXT NOT NULL DEFAULT 'codex',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    approved_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_report_project ON reports(project_id, report_type, status, updated_at);
CREATE INDEX IF NOT EXISTS idx_report_hash ON reports(input_hash);

CREATE TABLE IF NOT EXISTS workflow_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    title TEXT NOT NULL,
    context TEXT NOT NULL DEFAULT 'dev',
    phase TEXT NOT NULL DEFAULT 'discover',
    status TEXT NOT NULL DEFAULT 'active',
    objective TEXT NOT NULL DEFAULT '',
    minimum_success TEXT NOT NULL DEFAULT '',
    exceptional_success TEXT NOT NULL DEFAULT '',
    boundaries_json TEXT NOT NULL DEFAULT '[]',
    stakeholders_json TEXT NOT NULL DEFAULT '[]',
    existing_assets_json TEXT NOT NULL DEFAULT '[]',
    constraints_json TEXT NOT NULL DEFAULT '[]',
    validation_json TEXT NOT NULL DEFAULT '[]',
    current_step TEXT NOT NULL DEFAULT '',
    next_action TEXT NOT NULL DEFAULT '',
    blockers_json TEXT NOT NULL DEFAULT '[]',
    changed_files_json TEXT NOT NULL DEFAULT '[]',
    workspace TEXT NOT NULL DEFAULT '',
    branch TEXT NOT NULL DEFAULT '',
    commit_hash TEXT NOT NULL DEFAULT '',
    contract_revision INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    completed_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_workflow_project ON workflow_runs(project_id, status, updated_at);
CREATE INDEX IF NOT EXISTS idx_workflow_phase ON workflow_runs(phase, status);

CREATE TABLE IF NOT EXISTS workflow_artifacts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    workflow_id INTEGER NOT NULL REFERENCES workflow_runs(id) ON DELETE CASCADE,
    phase TEXT NOT NULL DEFAULT '',
    artifact_type TEXT NOT NULL DEFAULT 'checkpoint',
    title TEXT NOT NULL DEFAULT '',
    content TEXT NOT NULL DEFAULT '',
    data_json TEXT NOT NULL DEFAULT '{}',
    knowledge_node_id INTEGER REFERENCES knowledge_nodes(id) ON DELETE SET NULL,
    supersedes_artifact_id INTEGER REFERENCES workflow_artifacts(id) ON DELETE SET NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_wartifact_workflow ON workflow_artifacts(workflow_id, created_at);

CREATE TABLE IF NOT EXISTS verification_evidence (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    workflow_id INTEGER NOT NULL REFERENCES workflow_runs(id) ON DELETE CASCADE,
    criterion TEXT NOT NULL DEFAULT '',
    claim TEXT NOT NULL DEFAULT '',
    check_command TEXT NOT NULL DEFAULT '',
    environment TEXT NOT NULL DEFAULT '',
    started_at TEXT NOT NULL DEFAULT '',
    finished_at TEXT NOT NULL DEFAULT '',
    exit_code INTEGER,
    failure_count INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'not_run',
    output_excerpt TEXT NOT NULL DEFAULT '',
    artifact_uri TEXT NOT NULL DEFAULT '',
    commit_hash TEXT NOT NULL DEFAULT '',
    contract_revision INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_vevidence_workflow ON verification_evidence(workflow_id, created_at);
CREATE INDEX IF NOT EXISTS idx_vevidence_status ON verification_evidence(status, created_at);

CREATE TABLE IF NOT EXISTS learnings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,
    task_type TEXT NOT NULL DEFAULT '',
    approach TEXT NOT NULL DEFAULT '',
    outcome TEXT NOT NULL DEFAULT '',
    lesson TEXT NOT NULL,
    file_patterns TEXT NOT NULL DEFAULT '',
    tags TEXT NOT NULL DEFAULT '',
    relevance REAL NOT NULL DEFAULT 1.0,
    status TEXT NOT NULL DEFAULT 'active',
    review_after TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_learning_project ON learnings(project_id, status, created_at);

CREATE TABLE IF NOT EXISTS retrieval_evaluations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL DEFAULT '',
    query TEXT NOT NULL,
    engine TEXT NOT NULL DEFAULT 'lexical',
    k INTEGER NOT NULL DEFAULT 10,
    expected_ids_json TEXT NOT NULL DEFAULT '[]',
    actual_ids_json TEXT NOT NULL DEFAULT '[]',
    recall REAL NOT NULL DEFAULT 0,
    reciprocal_rank REAL NOT NULL DEFAULT 0,
    notes TEXT NOT NULL DEFAULT '',
    run_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_retrieval_eval ON retrieval_evaluations(engine, run_at);

CREATE TABLE IF NOT EXISTS processing_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'running',
    input_count INTEGER NOT NULL DEFAULT 0,
    output_count INTEGER NOT NULL DEFAULT 0,
    details_json TEXT NOT NULL DEFAULT '{}',
    error TEXT NOT NULL DEFAULT '',
    started_at TEXT NOT NULL,
    finished_at TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_processing_status ON processing_runs(status, started_at);
)sql");
        db_->exec("PRAGMA user_version = 8");
    }
    // ---- v9: explicit Codex skill routing for portable context packets ----
    if (userVersion < 9) {
        SQLite::Transaction tx(*db_);
        if (!tableHasColumn(*db_, "projects", "codex_skills")) {
            db_->exec("ALTER TABLE projects ADD COLUMN "
                      "codex_skills TEXT NOT NULL DEFAULT ''");
        }
        if (!tableHasColumn(*db_, "workflow_runs", "contract_revision")) {
            db_->exec("ALTER TABLE workflow_runs ADD COLUMN "
                      "contract_revision INTEGER NOT NULL DEFAULT 1");
        }
        if (!tableHasColumn(*db_, "verification_evidence", "contract_revision")) {
            db_->exec("ALTER TABLE verification_evidence ADD COLUMN "
                      "contract_revision INTEGER NOT NULL DEFAULT 1");
        }
        if (!tableHasColumn(*db_, "projects", "codex_skills"))
            throw std::runtime_error("schema v9 migration did not create projects.codex_skills");
        if (!tableHasColumn(*db_, "workflow_runs", "contract_revision") ||
            !tableHasColumn(*db_, "verification_evidence", "contract_revision"))
            throw std::runtime_error("schema v9 migration did not create workflow contract revision columns");
        db_->exec("PRAGMA user_version = 9");
        tx.commit();
    }
    // Early development builds could already have user_version=9 before the
    // packet registry/revision binding was added. Make that state safe without
    // a destructive or ambiguous migration retry.
    if (!tableHasColumn(*db_, "workflow_runs", "contract_revision"))
        db_->exec("ALTER TABLE workflow_runs ADD COLUMN contract_revision INTEGER NOT NULL DEFAULT 1");
    if (!tableHasColumn(*db_, "verification_evidence", "contract_revision"))
        db_->exec("ALTER TABLE verification_evidence ADD COLUMN contract_revision INTEGER NOT NULL DEFAULT 1");
    ensurePacketSnapshotSchema(*db_);
    // ---- v10: approval-gated Discord ticket image lifecycle ----
    if (userVersion < 10) {
        SQLite::Transaction tx(*db_);
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS ticket_attachments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    discord_message_row_id INTEGER REFERENCES discord_messages(id) ON DELETE SET NULL,
    item_id INTEGER REFERENCES items(id) ON DELETE CASCADE,
    source_channel_id TEXT NOT NULL,
    source_message_id TEXT NOT NULL,
    attachment_id TEXT NOT NULL,
    source_role TEXT NOT NULL DEFAULT 'suggestion'
      CHECK(source_role IN ('suggestion','admin_reply','direct_ping')),
    original_filename TEXT NOT NULL DEFAULT '',
    content_type TEXT NOT NULL DEFAULT '',
    declared_size INTEGER NOT NULL DEFAULT 0 CHECK(declared_size>=0),
    width INTEGER NOT NULL DEFAULT 0 CHECK(width>=0),
    height INTEGER NOT NULL DEFAULT 0 CHECK(height>=0),
    state TEXT NOT NULL DEFAULT 'captured'
      CHECK(state IN ('captured','queued','saved','failed')),
    relative_path TEXT NOT NULL DEFAULT '',
    actual_size INTEGER NOT NULL DEFAULT 0 CHECK(actual_size>=0),
    sha256 TEXT NOT NULL DEFAULT '',
    error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    saved_at TEXT NOT NULL DEFAULT '',
    UNIQUE(source_message_id, attachment_id),
    CHECK(
      (state='captured' AND discord_message_row_id IS NOT NULL AND item_id IS NULL
       AND relative_path='' AND actual_size=0 AND sha256='' AND saved_at='') OR
      (state IN ('queued','failed') AND item_id IS NOT NULL
       AND relative_path='' AND actual_size=0 AND sha256='' AND saved_at='') OR
      (state='saved' AND item_id IS NOT NULL AND relative_path!=''
       AND actual_size>0 AND actual_size<=10485760 AND length(sha256)=64
       AND saved_at!='')
    )
);
CREATE INDEX IF NOT EXISTS idx_ticket_attachment_capture
 ON ticket_attachments(discord_message_row_id,state);
CREATE INDEX IF NOT EXISTS idx_ticket_attachment_item
 ON ticket_attachments(item_id,state,id);
CREATE TRIGGER IF NOT EXISTS trg_discord_message_ticket_attachments
 BEFORE DELETE ON discord_messages
 BEGIN
   DELETE FROM ticket_attachments
    WHERE discord_message_row_id=OLD.id AND item_id IS NULL;
 END;
)sql");
        bool created = false;
        {
            SQLite::Statement createdQuery(*db_,
                "SELECT 1 FROM sqlite_master WHERE type='table' "
                "AND name='ticket_attachments'");
            created = createdQuery.executeStep();
        }
        if (!created)
            throw std::runtime_error("schema v10 migration did not create ticket_attachments");
        db_->exec("PRAGMA user_version = 10");
        tx.commit();
    }
    // Repair early v10 development databases that may have created the table
    // before the delete trigger existed. Captured metadata must disappear with
    // its inbox row, while promoted records survive via the SET NULL FK.
    db_->exec(R"sql(
CREATE TRIGGER IF NOT EXISTS trg_discord_message_ticket_attachments
 BEFORE DELETE ON discord_messages
 BEGIN
   DELETE FROM ticket_attachments
    WHERE discord_message_row_id=OLD.id AND item_id IS NULL;
 END;
)sql");
    // ---- v11: durable Discord card lineage + retryable manual commands ----
    if (userVersion < 11) {
        SQLite::Transaction tx(*db_);
        if (!tableHasColumn(*db_, "discord_messages", "manual_target_message_id"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_target_message_id TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_command_state"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_command_state TEXT NOT NULL DEFAULT '' "
                      "CHECK(manual_command_state IN ('','pending','done','failed'))");
        if (!tableHasColumn(*db_, "discord_messages", "manual_attempts"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_attempts INTEGER NOT NULL DEFAULT 0 "
                      "CHECK(manual_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_next_retry_at"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_last_error"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_last_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_kind"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_result_kind TEXT NOT NULL DEFAULT '' "
                      "CHECK(manual_result_kind IN ('','captured','appended','repeat'))");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_message_row_id"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_result_message_row_id INTEGER NOT NULL DEFAULT 0 "
                      "CHECK(manual_result_message_row_id>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_item_id"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_result_item_id INTEGER NOT NULL DEFAULT 0 "
                      "CHECK(manual_result_item_id>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_images_queued"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_result_images_queued INTEGER NOT NULL DEFAULT 0 "
                      "CHECK(manual_result_images_queued>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_state"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_effect_state TEXT NOT NULL DEFAULT '' "
                      "CHECK(manual_effect_state IN ('','pending','posting','done','failed'))");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_attempts"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_effect_attempts INTEGER NOT NULL DEFAULT 0 "
                      "CHECK(manual_effect_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_next_retry_at"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_effect_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_error"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_effect_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_updated_at"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN "
                      "manual_effect_updated_at TEXT NOT NULL DEFAULT ''");
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS discord_notify_cards (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    discord_message_row_id INTEGER REFERENCES discord_messages(id) ON DELETE SET NULL,
    item_id INTEGER REFERENCES items(id) ON DELETE CASCADE,
    notify_channel_id TEXT NOT NULL DEFAULT '',
    notify_message_id TEXT NOT NULL DEFAULT '',
    post_state TEXT NOT NULL DEFAULT 'pending'
      CHECK(post_state IN ('pending','posting','posted','failed')),
    post_attempts INTEGER NOT NULL DEFAULT 0 CHECK(post_attempts>=0),
    post_revision INTEGER NOT NULL DEFAULT 0 CHECK(post_revision>=0),
    post_next_retry_at TEXT NOT NULL DEFAULT '',
    post_last_error TEXT NOT NULL DEFAULT '',
    edit_state TEXT NOT NULL DEFAULT 'idle'
      CHECK(edit_state IN ('idle','pending','posting','failed')),
    edit_attempts INTEGER NOT NULL DEFAULT 0 CHECK(edit_attempts>=0),
    edit_revision INTEGER NOT NULL DEFAULT 0 CHECK(edit_revision>=0),
    edit_next_retry_at TEXT NOT NULL DEFAULT '',
    edit_last_error TEXT NOT NULL DEFAULT '',
    edit_updated_at TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    CHECK(discord_message_row_id IS NOT NULL OR item_id IS NOT NULL)
);
CREATE TABLE IF NOT EXISTS discord_notify_orphans (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    card_row_id INTEGER NOT NULL DEFAULT 0 CHECK(card_row_id>=0),
    notify_channel_id TEXT NOT NULL,
    notify_message_id TEXT NOT NULL,
    cleanup_state TEXT NOT NULL DEFAULT 'pending'
      CHECK(cleanup_state IN ('pending','deleting','failed')),
    cleanup_attempts INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_attempts>=0),
    cleanup_revision INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_revision>=0),
    cleanup_next_retry_at TEXT NOT NULL DEFAULT '',
    cleanup_last_error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(notify_channel_id,notify_message_id)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_discord_notify_source
 ON discord_notify_cards(discord_message_row_id)
 WHERE discord_message_row_id IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS idx_discord_notify_message
 ON discord_notify_cards(notify_channel_id,notify_message_id)
 WHERE notify_channel_id!='' AND notify_message_id!='';
CREATE INDEX IF NOT EXISTS idx_discord_notify_item
 ON discord_notify_cards(item_id,notify_message_id);
CREATE INDEX IF NOT EXISTS idx_discord_notify_orphan_cleanup
 ON discord_notify_orphans(cleanup_state,cleanup_next_retry_at,updated_at,id);
CREATE TRIGGER IF NOT EXISTS trg_discord_message_notify_cards
 BEFORE DELETE ON discord_messages
 BEGIN
   DELETE FROM discord_notify_cards
    WHERE discord_message_row_id=OLD.id AND item_id IS NULL;
 END;
)sql");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_state"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_state TEXT NOT NULL DEFAULT 'pending' CHECK(post_state IN ('pending','posting','posted','failed'))");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_attempts"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_attempts INTEGER NOT NULL DEFAULT 0 CHECK(post_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_revision"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_revision INTEGER NOT NULL DEFAULT 0 CHECK(post_revision>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_next_retry_at"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_last_error"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_last_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_state"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_state TEXT NOT NULL DEFAULT 'idle' CHECK(edit_state IN ('idle','pending','posting','failed'))");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_attempts"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_attempts INTEGER NOT NULL DEFAULT 0 CHECK(edit_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_revision"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_revision INTEGER NOT NULL DEFAULT 0 CHECK(edit_revision>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_next_retry_at"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_last_error"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_last_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_updated_at"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_updated_at TEXT NOT NULL DEFAULT ''");
        db_->exec("CREATE INDEX IF NOT EXISTS idx_discord_notify_post ON discord_notify_cards(post_state,post_next_retry_at,updated_at,id)");
        db_->exec("CREATE INDEX IF NOT EXISTS idx_discord_notify_edit ON discord_notify_cards(edit_state,edit_next_retry_at,edit_updated_at,id)");
        // Preserve cards created by schema-v4 builds. New code treats this
        // table as canonical; the legacy message columns remain synchronized
        // for backward-compatible diagnostics during the transition.
        db_->exec(R"sql(
INSERT OR IGNORE INTO discord_notify_cards(
 discord_message_row_id,item_id,notify_channel_id,notify_message_id,
 post_state,created_at,updated_at)
SELECT id,item_id,notify_channel_id,notify_message_id,'posted',ingested_at,
       strftime('%Y-%m-%dT%H:%M:%SZ','now')
  FROM discord_messages
 WHERE notify_channel_id!='' AND notify_message_id!='';
)sql");
        bool cardsCreated = false;
        {
            SQLite::Statement cards(*db_,
                "SELECT 1 FROM sqlite_master WHERE type='table' "
                "AND name='discord_notify_cards'");
            cardsCreated = cards.executeStep();
        }
        if (!cardsCreated ||
            !tableHasColumn(*db_, "discord_messages", "manual_command_state"))
            throw std::runtime_error(
                "schema v11 migration did not create Discord durability state");
        db_->exec("PRAGMA user_version = 11");
        tx.commit();
    }
    // Repair early v11 development databases whose version was advanced before
    // the durable card outbox/manual-effect columns or cleanup trigger landed.
    {
        SQLite::Transaction tx(*db_);
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_kind"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_result_kind TEXT NOT NULL DEFAULT '' CHECK(manual_result_kind IN ('','captured','appended','repeat'))");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_message_row_id"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_result_message_row_id INTEGER NOT NULL DEFAULT 0 CHECK(manual_result_message_row_id>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_item_id"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_result_item_id INTEGER NOT NULL DEFAULT 0 CHECK(manual_result_item_id>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_result_images_queued"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_result_images_queued INTEGER NOT NULL DEFAULT 0 CHECK(manual_result_images_queued>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_state"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_effect_state TEXT NOT NULL DEFAULT '' CHECK(manual_effect_state IN ('','pending','posting','done','failed'))");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_attempts"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_effect_attempts INTEGER NOT NULL DEFAULT 0 CHECK(manual_effect_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_next_retry_at"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_effect_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_error"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_effect_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_messages", "manual_effect_updated_at"))
            db_->exec("ALTER TABLE discord_messages ADD COLUMN manual_effect_updated_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_state"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_state TEXT NOT NULL DEFAULT 'pending' CHECK(post_state IN ('pending','posting','posted','failed'))");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_attempts"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_attempts INTEGER NOT NULL DEFAULT 0 CHECK(post_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_revision"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_revision INTEGER NOT NULL DEFAULT 0 CHECK(post_revision>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_next_retry_at"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "post_last_error"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN post_last_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_state"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_state TEXT NOT NULL DEFAULT 'idle' CHECK(edit_state IN ('idle','pending','posting','failed'))");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_attempts"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_attempts INTEGER NOT NULL DEFAULT 0 CHECK(edit_attempts>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_revision"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_revision INTEGER NOT NULL DEFAULT 0 CHECK(edit_revision>=0)");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_next_retry_at"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_next_retry_at TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_last_error"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_last_error TEXT NOT NULL DEFAULT ''");
        if (!tableHasColumn(*db_, "discord_notify_cards", "edit_updated_at"))
            db_->exec("ALTER TABLE discord_notify_cards ADD COLUMN edit_updated_at TEXT NOT NULL DEFAULT ''");
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS discord_notify_orphans (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    card_row_id INTEGER NOT NULL DEFAULT 0 CHECK(card_row_id>=0),
    notify_channel_id TEXT NOT NULL,
    notify_message_id TEXT NOT NULL,
    cleanup_state TEXT NOT NULL DEFAULT 'pending'
      CHECK(cleanup_state IN ('pending','deleting','failed')),
    cleanup_attempts INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_attempts>=0),
    cleanup_revision INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_revision>=0),
    cleanup_next_retry_at TEXT NOT NULL DEFAULT '',
    cleanup_last_error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE(notify_channel_id,notify_message_id)
);
CREATE INDEX IF NOT EXISTS idx_discord_notify_orphan_cleanup
 ON discord_notify_orphans(cleanup_state,cleanup_next_retry_at,updated_at,id);
)sql");
        db_->exec("UPDATE discord_notify_cards SET post_state='posted' WHERE notify_message_id!='' AND post_state!='posted'");
        // Every pre-v11 bot-owned card gets one current-state reconciliation.
        // Cards created by the v11 outbox reach revision 1 through the
        // notify-message lineage trigger when their POST receipt commits.
        db_->exec(R"sql(
UPDATE discord_notify_cards
   SET edit_state='pending',edit_attempts=0,edit_revision=1,
       edit_next_retry_at='',edit_last_error='',
       edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
       updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE notify_message_id!='' AND edit_state='idle' AND edit_revision=0;
)sql");
        db_->exec("CREATE INDEX IF NOT EXISTS idx_discord_notify_post ON discord_notify_cards(post_state,post_next_retry_at,updated_at,id)");
        db_->exec("CREATE INDEX IF NOT EXISTS idx_discord_notify_edit ON discord_notify_cards(edit_state,edit_next_retry_at,edit_updated_at,id)");
        db_->exec("CREATE INDEX IF NOT EXISTS idx_discord_manual_pending ON discord_messages(manual_command_state,manual_next_retry_at,ingested_at,id)");
        db_->exec("CREATE INDEX IF NOT EXISTS idx_discord_manual_effect ON discord_messages(manual_command_state,manual_effect_state,manual_effect_next_retry_at,manual_effect_updated_at,ingested_at,id)");
        // Pending manual captures already appear through discord_messages in
        // Recent activity.  Older builds also copied author/body text into an
        // unlinked permanent activity row, so retain the event while removing
        // that redundant personal/content payload.
        db_->exec("UPDATE activity_log SET detail='Manual Discord capture' "
                  "WHERE kind='discord_manual_capture' "
                  "AND detail!='Manual Discord capture'");
        tx.commit();
    }
    // Card content is derived from item/source/image state, so mark the
    // bot-owned card dirty in the same SQLite transaction as every state
    // mutation that can change the rendered embed.  Replacing the original
    // cleanup-only trigger also repairs early schema-v11 development DBs.
    {
        SQLite::Transaction tx(*db_);
        db_->exec(R"sql(
DROP TRIGGER IF EXISTS trg_discord_message_notify_cards;
DROP TRIGGER IF EXISTS trg_discord_message_notify_card_update;
DROP TRIGGER IF EXISTS trg_discord_channel_notify_cards;
DROP TRIGGER IF EXISTS trg_item_notify_cards;
DROP TRIGGER IF EXISTS trg_project_notify_cards;
DROP TRIGGER IF EXISTS trg_ticket_attachment_notify_card_insert;
DROP TRIGGER IF EXISTS trg_ticket_attachment_notify_card_update;
DROP TRIGGER IF EXISTS trg_ticket_attachment_notify_card_delete;
DROP TRIGGER IF EXISTS trg_discord_notify_card_lineage;

CREATE TRIGGER trg_discord_message_notify_cards
 BEFORE DELETE ON discord_messages
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE discord_message_row_id=OLD.id AND item_id IS NOT NULL
      AND notify_message_id!='';
   UPDATE discord_notify_cards
      SET post_state='pending',post_attempts=0,
          post_revision=post_revision+1,post_next_retry_at='',
          post_last_error='',
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE discord_message_row_id=OLD.id AND item_id IS NOT NULL
      AND notify_message_id='' AND post_state='posting';
   DELETE FROM discord_notify_cards
    WHERE discord_message_row_id=OLD.id AND item_id IS NULL;
 END;

CREATE TRIGGER trg_discord_message_notify_card_update
 AFTER UPDATE OF state,content,admin_note,inferred_project_id,kind,score,author,item_id
 ON discord_messages
 WHEN OLD.state IS NOT NEW.state OR OLD.content IS NOT NEW.content
   OR OLD.admin_note IS NOT NEW.admin_note
   OR OLD.inferred_project_id IS NOT NEW.inferred_project_id
   OR OLD.kind IS NOT NEW.kind OR OLD.score IS NOT NEW.score
   OR OLD.author IS NOT NEW.author OR OLD.item_id IS NOT NEW.item_id
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE discord_message_row_id=NEW.id AND notify_message_id!='';
   UPDATE discord_notify_cards
      SET post_state='pending',post_attempts=0,
          post_revision=post_revision+1,post_next_retry_at='',
          post_last_error='',
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE discord_message_row_id=NEW.id
      AND notify_message_id='' AND post_state='posting';
 END;

CREATE TRIGGER trg_discord_channel_notify_cards
 AFTER UPDATE OF channel_id,guild_id,channel_name ON discord_channels
 WHEN OLD.channel_id IS NOT NEW.channel_id OR OLD.guild_id IS NOT NEW.guild_id
   OR OLD.channel_name IS NOT NEW.channel_name
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE discord_message_row_id IN
          (SELECT id FROM discord_messages WHERE channel_row_id=NEW.id)
      AND notify_message_id!='';
 END;

CREATE TRIGGER trg_item_notify_cards
 AFTER UPDATE OF project_id,type,title,status ON items
 WHEN OLD.project_id IS NOT NEW.project_id OR OLD.type IS NOT NEW.type
   OR OLD.title IS NOT NEW.title OR OLD.status IS NOT NEW.status
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE item_id=NEW.id AND notify_message_id!='';
 END;

CREATE TRIGGER trg_project_notify_cards
 AFTER UPDATE OF name ON projects
 WHEN OLD.name IS NOT NEW.name
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE item_id IN (SELECT id FROM items WHERE project_id=NEW.id)
      AND notify_message_id!='';
 END;

CREATE TRIGGER trg_ticket_attachment_notify_card_insert
 AFTER INSERT ON ticket_attachments
 WHEN NEW.item_id IS NOT NULL
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE item_id=NEW.item_id AND notify_message_id!='';
 END;

CREATE TRIGGER trg_ticket_attachment_notify_card_update
 AFTER UPDATE OF item_id,state ON ticket_attachments
 WHEN OLD.item_id IS NOT NEW.item_id OR OLD.state IS NOT NEW.state
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE item_id=OLD.item_id AND OLD.item_id IS NOT NULL
      AND notify_message_id!='';
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE item_id=NEW.item_id AND NEW.item_id IS NOT NULL
      AND NEW.item_id IS NOT OLD.item_id AND notify_message_id!='';
 END;

CREATE TRIGGER trg_ticket_attachment_notify_card_delete
 AFTER DELETE ON ticket_attachments
 WHEN OLD.item_id IS NOT NULL
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE item_id=OLD.item_id AND notify_message_id!='';
 END;

CREATE TRIGGER trg_discord_notify_card_lineage
 AFTER UPDATE OF item_id,notify_message_id ON discord_notify_cards
 WHEN NEW.notify_message_id!='' AND
      (OLD.item_id IS NOT NEW.item_id OR
       OLD.notify_message_id IS NOT NEW.notify_message_id)
 BEGIN
   UPDATE discord_notify_cards
      SET edit_state='pending',edit_attempts=0,
          edit_revision=edit_revision+1,edit_next_retry_at='',
          edit_last_error='',
          edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
          updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
    WHERE id=NEW.id;
 END;
)sql");
        tx.commit();
    }
    // ---- v12: durable contributor identity aliases ----
    // A contributor merge must not discard the stable Discord platform/user
    // identity.  Retain the old source row as an audit alias and resolve it to
    // the surviving canonical source whenever future feedback is captured.
    if (userVersion < 12) {
        SQLite::Transaction tx(*db_);
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS source_merges (
    source_id INTEGER PRIMARY KEY REFERENCES sources(id) ON DELETE RESTRICT,
    target_source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE RESTRICT,
    merged_at TEXT NOT NULL,
    CHECK(source_id<>target_source_id)
);
CREATE INDEX IF NOT EXISTS idx_source_merges_target
 ON source_merges(target_source_id);
-- Deliberately non-unique: source_merges retains immutable source identities
-- as aliases so future events from a merged Discord user resolve to the chosen
-- canonical contributor. ensureSourceLocked performs deterministic canonical
-- lookup; making this index UNIQUE would break that durable alias contract.
CREATE INDEX IF NOT EXISTS idx_sources_stable_identity
 ON sources(platform COLLATE NOCASE,handle) WHERE handle!='';
)sql");
        bool aliasesCreated = false;
        {
            SQLite::Statement aliases(*db_,
                "SELECT 1 FROM sqlite_master WHERE type='table' "
                "AND name='source_merges'");
            aliasesCreated = aliases.executeStep();
        }
        if (!aliasesCreated)
            throw std::runtime_error(
                "schema v12 migration did not create source_merges");
        db_->exec("PRAGMA user_version = 12");
        tx.commit();
    }
    // user_version is only a claim. Refuse a damaged/current database whose
    // v12 objects are absent instead of allowing startup or --check-db to pass
    // and failing later when Credits or attribution ingest first queries them.
    if (!schemaObjectExists(*db_, "table", "source_merges") ||
        !tableHasColumn(*db_, "source_merges", "source_id") ||
        !tableHasColumn(*db_, "source_merges", "target_source_id") ||
        !tableHasColumn(*db_, "source_merges", "merged_at")) {
        throw std::runtime_error(
            "schema v12 source_merges is incomplete. Back up data\\devhub.db "
            "and restore a known-good database backup from data\\backups\\.");
    }
    {
        // These indexes are owned derived objects. CREATE IF NOT EXISTS cannot
        // repair a same-name malformed definition, so rebuild them before
        // validating their columns.
        SQLite::Transaction tx(*db_);
        db_->exec("DROP INDEX IF EXISTS idx_source_merges_target");
        db_->exec("DROP INDEX IF EXISTS idx_sources_stable_identity");
        db_->exec("CREATE INDEX idx_source_merges_target "
                  "ON source_merges(target_source_id)");
        db_->exec("CREATE INDEX idx_sources_stable_identity "
                  "ON sources(platform COLLATE NOCASE,handle) "
                  "WHERE handle!=''");
        tx.commit();
    }
    {
        SQLite::Statement sourceMergeIndexColumns(*db_, R"sql(
SELECT
 (SELECT GROUP_CONCAT(name, ',') FROM
   (SELECT name FROM pragma_index_info('idx_source_merges_target')
     ORDER BY seqno)),
 (SELECT GROUP_CONCAT(name, ',') FROM
   (SELECT name FROM pragma_index_info('idx_sources_stable_identity')
     ORDER BY seqno)),
 (SELECT "unique" FROM pragma_index_list('sources')
   WHERE name='idx_sources_stable_identity'))sql");
        sourceMergeIndexColumns.executeStep();
        if (sourceMergeIndexColumns.getColumn(0).getString() !=
                "target_source_id" ||
            sourceMergeIndexColumns.getColumn(1).getString() !=
                "platform,handle" ||
            sourceMergeIndexColumns.getColumn(2).getInt() != 0) {
            throw std::runtime_error(
                "schema v12 contributor identity indexes could not be rebuilt. "
                "Back up data\\devhub.db and restore data from data\\backups\\.");
        }
    }
    {
        SQLite::Statement sourceMergeForeignKeys(*db_, R"sql(
SELECT
 SUM(CASE WHEN "from"='source_id' THEN 1 ELSE 0 END),
 SUM(CASE WHEN "from"='target_source_id' THEN 1 ELSE 0 END)
 FROM pragma_foreign_key_list('source_merges')
 WHERE "table"='sources' AND "to"='id' AND "on_delete"='RESTRICT')sql");
        sourceMergeForeignKeys.executeStep();
        if (sourceMergeForeignKeys.getColumn(0).getInt() != 1 ||
            sourceMergeForeignKeys.getColumn(1).getInt() != 1)
            throw std::runtime_error(
                "schema v12 source_merges foreign keys are incomplete. Back up "
                "data\\devhub.db and restore a known-good database backup from "
                "data\\backups\\.");
    }
    // ---- v13: durable terminal review-reaction cleanup -------------------
    // Terminal reaction removal is independent from notification message
    // edits.  Keeping it in its own outbox prevents reconnect recovery from
    // PATCHing every historical card merely to clear terminal reactions.
    if (userVersion < 13) {
        SQLite::Transaction tx(*db_);
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS discord_review_control_cleanups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    card_row_id INTEGER NOT NULL DEFAULT 0 CHECK(card_row_id>=0),
    notify_channel_id TEXT NOT NULL,
    notify_message_id TEXT NOT NULL,
    cleanup_state TEXT NOT NULL DEFAULT 'pending'
      CHECK(cleanup_state IN ('pending','deleting','failed')),
    cleanup_attempts INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_attempts>=0),
    cleanup_revision INTEGER NOT NULL DEFAULT 0 CHECK(cleanup_revision>=0),
    cleanup_next_retry_at TEXT NOT NULL DEFAULT '',
    cleanup_last_error TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_discord_review_cleanup_target
 ON discord_review_control_cleanups(notify_channel_id,notify_message_id);
CREATE INDEX IF NOT EXISTS idx_discord_review_cleanup_due
 ON discord_review_control_cleanups(
    cleanup_state,cleanup_next_retry_at,updated_at,id);
)sql");
        // v1 mistakenly treated a terminal reaction cleanup as a card edit.
        // Stop only exact Discord old-message-limit retries already identified
        // by their persisted error; all other ambiguous edit rows stay visible
        // and retain their normal operator recovery path.
        db_->exec(R"sql(
UPDATE discord_notify_cards
   SET edit_state='failed',edit_next_retry_at='',
       edit_updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now'),
       updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now')
 WHERE edit_state='pending'
   AND edit_last_error LIKE
       '%Maximum number of edits to messages older than 1 hour reached%';
)sql");
        if (!schemaObjectExists(
                *db_, "table", "discord_review_control_cleanups") ||
            !schemaObjectExists(
                *db_, "index", "idx_discord_review_cleanup_target") ||
            !schemaObjectExists(
                *db_, "index", "idx_discord_review_cleanup_due"))
            throw std::runtime_error(
                "schema v13 migration did not create review cleanup state");
        db_->exec("PRAGMA user_version = 13");
        tx.commit();
    }
    if (!schemaObjectExists(
            *db_, "table", "discord_review_control_cleanups") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "card_row_id") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "notify_channel_id") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "notify_message_id") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "cleanup_state") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "cleanup_next_retry_at") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "cleanup_revision") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "cleanup_last_error") ||
        !tableHasColumn(*db_, "discord_review_control_cleanups", "updated_at"))
        throw std::runtime_error(
            "schema v13 review-control cleanup table is incomplete. Back up "
            "data\\devhub.db and restore a known-good database backup from "
            "data\\backups\\.");
    {
        SQLite::Transaction tx(*db_);
        db_->exec("DROP INDEX IF EXISTS idx_discord_review_cleanup_target");
        db_->exec("DROP INDEX IF EXISTS idx_discord_review_cleanup_due");
        db_->exec(R"sql(
DELETE FROM discord_review_control_cleanups WHERE id NOT IN (
 SELECT MAX(id) FROM discord_review_control_cleanups
 GROUP BY notify_channel_id,notify_message_id
);
CREATE UNIQUE INDEX idx_discord_review_cleanup_target
 ON discord_review_control_cleanups(notify_channel_id,notify_message_id);
CREATE INDEX idx_discord_review_cleanup_due
 ON discord_review_control_cleanups(
    cleanup_state,cleanup_next_retry_at,updated_at,id);
)sql");
        tx.commit();
    }
    {
        SQLite::Statement reviewCleanupIndexColumns(*db_, R"sql(
SELECT GROUP_CONCAT(name, ',') FROM
 (SELECT name FROM pragma_index_info('idx_discord_review_cleanup_target')
   ORDER BY seqno))sql");
        reviewCleanupIndexColumns.executeStep();
        if (reviewCleanupIndexColumns.getColumn(0).getString() !=
            "notify_channel_id,notify_message_id")
            throw std::runtime_error(
                "schema v13 review-control cleanup identity index could not be "
                "rebuilt; restore data\\devhub.db from data\\backups\\.");
    }
    {
        SQLite::Statement reviewCleanupIndexUnique(*db_, R"sql(
SELECT "unique" FROM pragma_index_list('discord_review_control_cleanups')
 WHERE name='idx_discord_review_cleanup_target')sql");
        if (!reviewCleanupIndexUnique.executeStep() ||
            reviewCleanupIndexUnique.getColumn(0).getInt() != 1)
            throw std::runtime_error(
                "schema v13 review-control cleanup identity is not unique after "
                "repair; restore data\\devhub.db from data\\backups\\.");
    }
    // ---- v14: revision-scoped notification failure acknowledgements ------
    // Transport state remains authoritative.  An operator dismissal hides
    // only one exact failed create/edit generation and never deletes the
    // canonical item/source/channel/message lineage or pretends delivery
    // succeeded.  Any later revision is therefore visible again.
    auto validateNotifyFailureDismissals = [&]() {
        if (!schemaObjectExists(
                *db_, "table", "discord_notify_failure_dismissals") ||
            !tableHasColumn(
                *db_, "discord_notify_failure_dismissals", "card_row_id") ||
            !tableHasColumn(
                *db_, "discord_notify_failure_dismissals", "operation") ||
            !tableHasColumn(
                *db_, "discord_notify_failure_dismissals", "revision") ||
            !tableHasColumn(
                *db_, "discord_notify_failure_dismissals", "dismissed_at") ||
            !tableHasColumn(
                *db_, "discord_notify_failure_dismissals", "reason"))
            throw std::runtime_error(
                "schema v14 notification failure acknowledgement table is "
                "incomplete. Back up data\\devhub.db and restore a known-good "
                "copy from data\\backups\\.");

        int requiredNotNull = 0;
        bool reasonDefault = false;
        SQLite::Statement columns(*db_, R"sql(
PRAGMA table_info('discord_notify_failure_dismissals'))sql");
        while (columns.executeStep()) {
            const std::string name = columns.getColumn(1).getString();
            if ((name == "card_row_id" || name == "operation" ||
                 name == "revision" || name == "dismissed_at" ||
                 name == "reason") &&
                columns.getColumn(3).getInt() == 1)
                ++requiredNotNull;
            if (name == "reason" && !columns.getColumn(4).isNull() &&
                columns.getColumn(4).getString() == "'operator'")
                reasonDefault = true;
        }
        if (requiredNotNull != 5 || !reasonDefault)
            throw std::runtime_error(
                "schema v14 notification failure columns are malformed. Back "
                "up data\\devhub.db and restore data from data\\backups\\.");

        SQLite::Statement identity(*db_, R"sql(
SELECT GROUP_CONCAT(name, ',') FROM
 (SELECT name FROM pragma_table_info(
      'discord_notify_failure_dismissals')
   WHERE pk>0 ORDER BY pk))sql");
        identity.executeStep();
        if (identity.getColumn(0).getString() !=
            "card_row_id,operation,revision")
            throw std::runtime_error(
                "schema v14 notification failure identity is malformed. Back "
                "up data\\devhub.db and restore data from data\\backups\\.");

        SQLite::Statement foreignKey(*db_, R"sql(
SELECT COUNT(*) FROM pragma_foreign_key_list(
    'discord_notify_failure_dismissals')
 WHERE "table"='discord_notify_cards' AND "from"='card_row_id'
   AND "to"='id' AND "on_delete"='CASCADE')sql");
        foreignKey.executeStep();
        if (foreignKey.getColumn(0).getInt() != 1)
            throw std::runtime_error(
                "schema v14 notification failure lineage is malformed. Back "
                "up data\\devhub.db and restore data from data\\backups\\.");

        SQLite::Statement declaration(*db_, R"sql(
SELECT sql FROM sqlite_master
 WHERE type='table' AND name='discord_notify_failure_dismissals')sql");
        if (!declaration.executeStep())
            throw std::runtime_error(
                "schema v14 notification failure declaration is missing. Back "
                "up data\\devhub.db and restore data from data\\backups\\.");
        const std::string compact =
            compactSchemaSql(declaration.getColumn(0).getString());
        if (compact.find(
                "check(operationin('create','edit'))") ==
                std::string::npos ||
            compact.find("check(revision>=0)") == std::string::npos)
            throw std::runtime_error(
                "schema v14 notification failure constraints are malformed. "
                "Back up data\\devhub.db and restore data from data\\backups\\.");
    };
    if (userVersion < 14) {
        SQLite::Transaction tx(*db_);
        db_->exec(R"sql(
CREATE TABLE IF NOT EXISTS discord_notify_failure_dismissals (
    card_row_id INTEGER NOT NULL
      REFERENCES discord_notify_cards(id) ON DELETE CASCADE,
    operation TEXT NOT NULL CHECK(operation IN ('create','edit')),
    revision INTEGER NOT NULL CHECK(revision>=0),
    dismissed_at TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT 'operator',
    PRIMARY KEY(card_row_id,operation,revision)
);
)sql");
        // Validate before advancing user_version so a pre-existing malformed
        // same-name table cannot turn a failed v13 migration into a v14 claim.
        validateNotifyFailureDismissals();
        // v13 intentionally stopped these impossible historical retries and
        // exposed them for review.  Acknowledge only those exact pre-v14 edit
        // generations now that the operator has reviewed them.  New failures,
        // including a later revision with the same Discord error, remain
        // visible until explicitly dismissed.
        db_->exec(R"sql(
INSERT OR IGNORE INTO discord_notify_failure_dismissals(
    card_row_id,operation,revision,dismissed_at,reason)
SELECT id,'edit',edit_revision,
       strftime('%Y-%m-%dT%H:%M:%SZ','now'),
       'v14-legacy-old-edit-limit'
 FROM discord_notify_cards
 WHERE edit_state='failed'
   AND (edit_last_error LIKE
       '%Maximum number of edits to messages older than 1 hour reached%'
     OR instr(edit_last_error,'30046')>0);
)sql");
        db_->exec("PRAGMA user_version = 14");
        tx.commit();
    }
    validateNotifyFailureDismissals();
    // A deleted Discord source is intentionally retained as a scrubbed
    // message-id tombstone.  Do not let channel cleanup cascade that marker
    // away: a late gateway/history/manual callback could otherwise recreate
    // the source under the same Discord message id.
    db_->exec(R"sql(
CREATE TRIGGER IF NOT EXISTS trg_preserve_deleted_discord_sources
 BEFORE DELETE ON discord_channels
 WHEN EXISTS(
   SELECT 1 FROM discord_messages
    WHERE channel_row_id=OLD.id AND state='deleted'
 )
 BEGIN
   SELECT RAISE(ABORT,
     'channel contains retained deleted Discord source markers');
 END;
)sql");
    // PTD importing was removed; drop its leftover setting. Webhook relay was
    // replaced by bot-posted notification cards (v4); drop its settings too.
    // dismiss_delete_original existed briefly - dismissing must NEVER delete
    // users' messages, so the setting is gone entirely.
    db_->exec("DELETE FROM settings WHERE key IN "
              "('ptd_path','notify_webhook_url','ui_sec_discord_webhook',"
              "'dismiss_delete_original')");
    std::vector<std::pair<std::string, std::string>> plaintextCredentials;
    std::vector<std::pair<std::string, std::string>> sealedPublicSettings;
    {
        SQLite::Statement settings(*db_, "SELECT key,value FROM settings");
        while (settings.executeStep()) {
            const std::string key = settings.getColumn(0).getString();
            const std::string value = settings.getColumn(1).getString();
            if (isPublicSettingKey(key) &&
                value.rfind(kDpapiPrefix, 0) == 0) {
                std::string plaintext;
                if (unsealSettingValue(value, plaintext))
                    sealedPublicSettings.emplace_back(key, std::move(plaintext));
                else
                    appLog("[settings] public setting '" + key +
                           "' cannot be decrypted by the current Windows profile");
            } else if (sensitiveSettingKey(key) && !value.empty() &&
                value.rfind(kDpapiPrefix, 0) != 0)
                plaintextCredentials.emplace_back(key, value);
        }
    }
    for (const auto& [key, value] : sealedPublicSettings)
        setSettingUnlocked(key, value);
    for (const auto& [key, value] : plaintextCredentials)
        setSettingUnlocked(key, value);
    seedKnownProjectDefaults();
}

// Fill release-pipeline commands and version sources for the known XA
// projects wherever the field is still blank (idempotent; user edits win).
// The old v1 seed used a guessed dotnet build command for plugins and a bare
// Dashboard updater command. Replace only those exact generated values.
void Db::seedKnownProjectDefaults() {
    struct Known {
        const char* name;
        const char* internalName; // csproj/x.json name; null for non-plugins
        const char* versionCommand;
        const char* legacyVersionCommand; // exact generated value; null otherwise
        const char* codexSkills;
    };
    const Known known[] = {
        {"XA Database",      "XADatabase",     "python \"6. Update_test_version.py\"", nullptr, "$ffxiv-dalamud-plugin-builder"},
        {"XA Slave",         "XASlave",        "python \"6. Update_test_version.py\"", nullptr, "$ffxiv-dalamud-plugin-builder"},
        {"XA HUD Navigator", "XAHudNavigator", "python \"6. Update_test_version.py\"", nullptr, "$ffxiv-dalamud-plugin-builder"},
        {"XA Zod",           "XAZod",          "python \"6. Update_test_version.py\"", nullptr, "$ffxiv-dalamud-plugin-builder"},
        {"XA Sub Manager",   nullptr,           "python \"Update_Version.py\" --devhub", nullptr, "$ffxiv-dalamud-plugin-builder"},
        {"XA Dashboard",     nullptr,           "python \"Update_Version.py\" --devhub", "python \"Update_Version.py\"", "$ffxiv-dalamud-plugin-builder"},
    };
    const char* oldGuessedBuild = "dotnet build -c Debug -p:Platform=x64";

    for (const auto& k : known) {
        long long id = 0;
        std::string path;
        std::string build;
        {
            SQLite::Statement q(*db_,
                "SELECT id, path, build_command FROM projects WHERE name=? COLLATE NOCASE");
            q.bind(1, k.name);
            if (!q.executeStep()) continue;
            id = q.getColumn(0).getInt64();
            path = q.getColumn(1).getString();
            build = q.getColumn(2).getString();
        }

        auto fillIfEmpty = [&](const char* col, const std::string& value) {
            if (value.empty()) return;
            SQLite::Statement up(*db_,
                std::string("UPDATE projects SET ") + col + "=? WHERE id=? AND " +
                col + "=''");
            up.bind(1, value);
            up.bind(2, id);
            up.exec();
        };

        fillIfEmpty("codex_skills", k.codexSkills);
        if (path.empty()) continue;

        if (build.empty() || build == oldGuessedBuild) {
            SQLite::Statement up(*db_,
                "UPDATE projects SET build_command=? WHERE id=?");
            up.bind(1, "python \"1. Build.py\"");
            up.bind(2, id);
            up.exec();
        }
        fillIfEmpty("prep_command", "python \"2. Prepare_release.py\"");
        fillIfEmpty("release_command", "python \"3. Push_release.py\"");
        if (k.legacyVersionCommand) {
            SQLite::Statement up(*db_,
                "UPDATE projects SET version_command=? WHERE id=? "
                "AND version_command=?");
            up.bind(1, k.versionCommand);
            up.bind(2, id);
            up.bind(3, k.legacyVersionCommand);
            up.exec();
        }
        if (k.versionCommand)
            fillIfEmpty("version_command", k.versionCommand);
        if (k.internalName) {
            fillIfEmpty("local_version_file",
                        path + "\\" + k.internalName + "\\" + k.internalName + ".csproj");
            fillIfEmpty("remote_version_url", "https://aethertek.io/x.json");
            fillIfEmpty("remote_version_key", k.internalName);
        }
    }

    // Version sources and suggestion-matching aliases for legacy XA project
    // names. Derive every local source from the operator's saved project path;
    // never inject a developer-machine path into another installation.
    struct Extra {
        const char* name;
        const char* relativeVersionFile;
        const char* aliases;
        const char* codexSkills;
    };
    const Extra extras[] = {
        {"CB XA TT", "src\\main.cpp",
         "trading terminal, xa tt, coinbase", "$tradingterminal"},
        {"XA TT", "src\\main.cpp",
         "trading terminal, xa tt, coinbase", "$tradingterminal"},
        {"Discord Omnibot", "bot.py", "omnibot", "$python-script-maintainer"},
        {"Omnibot X", "bot.py", "omnibot", "$python-script-maintainer"},
        {"Teamspeak WP-Bridge", "SERVER FILES\\Bridge.py",
         "wp-bridge, ts bridge, teamspeak bridge", "$teamspeak-discord-bridge"},
        {"WP-Bridge", "SERVER FILES\\Bridge.py",
         "wp-bridge, ts bridge, teamspeak bridge", "$teamspeak-discord-bridge"},
        {"XA Dashboard", "CMakeLists.txt", "ar dashboard", "$ffxiv-dalamud-plugin-builder"},
        {"XA Sub Manager", "CMakeLists.txt",
         "sub manager, xa sub", "$ffxiv-dalamud-plugin-builder"},
        {"XA Database", nullptr, "xadb", "$ffxiv-dalamud-plugin-builder"},
        {"XA Slave", nullptr, "slave, xagman", "$ffxiv-dalamud-plugin-builder"},
        {"XA HUD Navigator", nullptr, "hud nav, hud navigator", "$ffxiv-dalamud-plugin-builder"},
        {"XA Zod", nullptr, "zod, testbench", "$ffxiv-dalamud-plugin-builder"},
        {"XA DevHub", "src\\app\\Version.h", "devhub, dev hub",
         "$ffxiv-dalamud-plugin-builder"},
    };
    for (const auto& e : extras) {
        long long id = 0;
        std::string path;
        {
            SQLite::Statement q(*db_,
                "SELECT id,path FROM projects WHERE name=? COLLATE NOCASE");
            q.bind(1, e.name);
            if (!q.executeStep()) continue;
            id = q.getColumn(0).getInt64();
            path = q.getColumn(1).getString();
        }
        auto fill = [&](const char* col, const std::string& value) {
            if (value.empty()) return;
            SQLite::Statement up(*db_,
                std::string("UPDATE projects SET ") + col + "=? WHERE id=? AND " +
                col + "=''");
            up.bind(1, value);
            up.bind(2, id);
            up.exec();
        };
        if (e.relativeVersionFile && !path.empty())
            fill("local_version_file", path + "\\" + e.relativeVersionFile);
        fill("aliases", e.aliases ? e.aliases : "");
        fill("codex_skills", e.codexSkills ? e.codexSkills : "");
    }

    // Known published-version sources and compatible updater commands. Empty
    // values receive the canonical default, while non-empty user-authored
    // values remain untouched. OmniBot's exact old generated changelog source
    // is the one exception: it was never the application's v1.x authority and
    // is migrated to bot.py once.
    struct VersionReference {
        const char* name;
        const char* relativeVersionFile;
        const char* legacyRelativeVersionFile;
        const char* remoteUrl;
        const char* githubUrl;
        const char* versionCommand;
    };
    const VersionReference references[] = {
        {"XA Dashboard", "CMakeLists.txt", nullptr,
         "https://aethertek.io/downloads/xa-dashboard/latest.json", nullptr,
         nullptr},
        {"XA Sub Manager", "CMakeLists.txt", nullptr,
         "https://aethertek.io/downloads/xa-sub-manager/latest.json", nullptr,
         nullptr},
        {"Discord Omnibot", "bot.py", "changelog.txt",
         "https://raw.githubusercontent.com/xa-io/discord-omnibot/main/bot.py",
         "https://github.com/xa-io/discord-omnibot/blob/main/bot.py", nullptr},
        {"Omnibot X", "bot.py", "changelog.txt",
         "https://raw.githubusercontent.com/xa-io/discord-omnibot/main/bot.py",
         "https://github.com/xa-io/discord-omnibot/blob/main/bot.py", nullptr},
        {"XA DevHub", "src\\app\\Version.h", nullptr,
         nullptr, nullptr, "python \"Update_Version.py\" --devhub"},
    };
    for (const auto& r : references) {
        long long id = 0;
        std::string path;
        {
            SQLite::Statement q(*db_,
                "SELECT id,path FROM projects WHERE name=? COLLATE NOCASE");
            q.bind(1, r.name);
            if (!q.executeStep()) continue;
            id = q.getColumn(0).getInt64();
            path = q.getColumn(1).getString();
        }

        auto fillIfEmpty = [&](const char* col, const char* value) {
            if (!value || !*value) return;
            SQLite::Statement up(*db_,
                std::string("UPDATE projects SET ") + col + "=? WHERE id=? AND " +
                col + "=''");
            up.bind(1, value);
            up.bind(2, id);
            up.exec();
        };

        const std::string versionFile =
            r.relativeVersionFile && !path.empty()
                ? path + "\\" + r.relativeVersionFile : "";
        const std::string legacyVersionFile =
            r.legacyRelativeVersionFile && !path.empty()
                ? path + "\\" + r.legacyRelativeVersionFile : "";
        if (!versionFile.empty() && !legacyVersionFile.empty()) {
            SQLite::Statement up(*db_,
                "UPDATE projects SET local_version_file=? WHERE id=? "
                "AND (local_version_file='' OR local_version_file=?)");
            up.bind(1, versionFile);
            up.bind(2, id);
            up.bind(3, legacyVersionFile);
            up.exec();
        } else {
            fillIfEmpty("local_version_file", versionFile.c_str());
        }
        fillIfEmpty("remote_version_url", r.remoteUrl);
        fillIfEmpty("github_url", r.githubUrl);
        fillIfEmpty("version_command", r.versionCommand);
    }
}

std::string Db::getSettingUnlocked(const std::string& key,
                                   const std::string& def) {
    SQLite::Statement q(*db_, "SELECT value FROM settings WHERE key=?");
    q.bind(1, key);
    if (!q.executeStep()) return def;
    const std::string stored = q.getColumn(0).getString();
    if (stored.rfind(kDpapiPrefix, 0) != 0) return stored;
    std::string plain;
    if (unsealSettingValue(stored, plain)) return plain;
    appLog("[settings] '" + key +
           "' cannot be decrypted by the current Windows profile");
    return def;
}

void Db::setSettingUnlocked(const std::string& key,
                            const std::string& value) {
    const std::string stored =
        sensitiveSettingKey(key) && !value.empty()
            ? sealSettingValue(value) : value;
    SQLite::Statement q(*db_,
        "INSERT INTO settings(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.bind(1, key);
    q.bind(2, stored);
    q.exec();
}

void Db::logActivityUnlocked(const std::string& kind, long long projectId,
                             const std::string& detail) {
    SQLite::Statement q(*db_,
        "INSERT INTO activity_log(ts,kind,project_id,detail) VALUES(?,?,?,?)");
    q.bind(1, nowIsoUtc());
    q.bind(2, kind);
    if (projectId > 0) q.bind(3, (long long)projectId); else q.bind(3);
    q.bind(4, detail);
    q.exec();
}

std::string Db::getSetting(Held held, const std::string& key,
                           const std::string& def) {
    (void)raw(held);
    return getSettingUnlocked(key, def);
}

void Db::setSetting(Held held, const std::string& key,
                    const std::string& value) {
    (void)raw(held);
    setSettingUnlocked(key, value);
}

void Db::logActivity(Held held, const std::string& kind, long long projectId,
                     const std::string& detail) {
    (void)raw(held);
    logActivityUnlocked(kind, projectId, detail);
}

json Db::rowToJson(SQLite::Statement& q) {
    json obj = json::object();
    for (int i = 0; i < q.getColumnCount(); ++i) {
        SQLite::Column col = q.getColumn(i);
        const char* name = col.getName();
        const int type = col.getType();
        if (type == SQLite::INTEGER)    obj[name] = col.getInt64();
        else if (type == SQLite::FLOAT) obj[name] = col.getDouble();
        else if (type == SQLite::Null)  obj[name] = nullptr;
        else                            obj[name] = normalizeUtf8(col.getString());
    }
    return obj;
}

json Db::rowsToJson(SQLite::Statement& q) {
    json arr = json::array();
    while (q.executeStep()) arr.push_back(rowToJson(q));
    return arr;
}

} // namespace devhub
