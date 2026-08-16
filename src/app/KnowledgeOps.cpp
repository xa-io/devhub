#include "KnowledgeOps.h"
#include "Db.h"
#include "PacketData.h"
#include "PacketOps.h"
#include "devhub/Util.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace devhub {
namespace {

json fail(const std::string& message,
          const std::string& code = "invalid_request") {
    return json{{"ok", false}, {"code", code}, {"error", message}};
}

std::string textField(const json& value, const char* key,
                      const std::string& fallback = "") {
    if (!value.is_object() || !value.contains(key) || value[key].is_null())
        return fallback;
    if (value[key].is_string()) return value[key].get<std::string>();
    return value[key].dump();
}

std::string nullableText(const json& value, const char* key,
                         const std::string& fallback = "") {
    if (!value.is_object() || !value.contains(key) ||
        !value[key].is_string())
        return fallback;
    return value[key].get<std::string>();
}

long long intField(const json& value, const char* key, long long fallback = 0) {
    if (!value.is_object() || !value.contains(key) || value[key].is_null())
        return fallback;
    if (value[key].is_number_integer()) return value[key].get<long long>();
    if (value[key].is_boolean()) return value[key].get<bool>() ? 1 : 0;
    try { return std::stoll(value[key].get<std::string>()); } catch (...) {}
    return fallback;
}

double doubleField(const json& value, const char* key, double fallback = 0) {
    if (!value.is_object() || !value.contains(key) || value[key].is_null())
        return fallback;
    if (value[key].is_number()) return value[key].get<double>();
    try { return std::stod(value[key].get<std::string>()); } catch (...) {}
    return fallback;
}

bool boolField(const json& value, const char* key, bool fallback = false) {
    if (!value.is_object() || !value.contains(key) || value[key].is_null())
        return fallback;
    if (value[key].is_boolean()) return value[key].get<bool>();
    if (value[key].is_number_integer()) return value[key].get<int>() != 0;
    std::string s = toLower(textField(value, key));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string jsonField(const json& value, const char* key,
                      const std::string& fallback) {
    if (!value.is_object() || !value.contains(key) || value[key].is_null())
        return fallback;
    if (value[key].is_string()) {
        std::string raw = value[key].get<std::string>();
        json parsed = json::parse(raw, nullptr, false);
        return parsed.is_discarded() ? json(raw).dump() : parsed.dump();
    }
    return value[key].dump();
}

bool choice(const std::string& value,
            std::initializer_list<const char*> allowed) {
    for (const char* item : allowed) if (value == item) return true;
    return false;
}

int boundLimit(int limit, int fallback = 50) {
    if (limit <= 0) return fallback;
    return std::max(1, std::min(limit, 200));
}

std::string normalizeKey(const std::string& value) {
    std::string out;
    bool gap = false;
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) {
            if (gap && !out.empty()) out.push_back(' ');
            out.push_back(static_cast<char>(std::tolower(ch)));
            gap = false;
        } else {
            gap = true;
        }
    }
    return trim(out);
}

std::string stableHash(const std::string& value) {
    // FNV-1a is deterministic across processes and sufficient for cache/
    // dedupe identity. It is not used for security decisions.
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char ch : value) {
        h ^= static_cast<unsigned long long>(ch);
        h *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << h;
    return out.str();
}

bool validSha256Ref(const std::string& value) {
    if (value.size() != 71 || value.rfind("sha256:", 0) != 0) return false;
    return std::all_of(value.begin() + 7, value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

bool reportPacketRegisteredLocked(Db::Held held, Db* db, long long projectId,
                                  const std::string& reportType,
                                  const std::string& periodStart,
                                  const std::string& periodEnd,
                                  const std::string& evidenceAt,
                                  const std::string& inputHash) {
    if (!isValidReportType(reportType) || !validSha256Ref(inputHash) ||
        evidenceAt.empty() ||
        periodStart.empty() || periodEnd.empty())
        return false;
    SQLite::Statement q(db->raw(held), R"sql(
SELECT 1 FROM packet_snapshots
WHERE kind='report' AND source_hash=? AND generated_at=? AND scope_type=?
 AND scope_start=? AND scope_end=?
 AND scope_project_id=?
ORDER BY id DESC LIMIT 1)sql");
    q.bind(1, inputHash.substr(7));
    q.bind(2, evidenceAt);
    q.bind(3, reportType);
    q.bind(4, periodStart);
    q.bind(5, periodEnd);
    q.bind(6, projectId);
    return q.executeStep();
}

bool recordReportPacket(Db* db, const PacketRenderResult& packet,
                        const PacketTarget& target,
                        const std::string& reportType,
                        const std::string& periodStart,
                        const std::string& periodEnd,
                        const std::string& generatedAt) {
    if (!isValidReportType(reportType)) return false;
    auto lk = db->guard();
    try {
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT OR IGNORE INTO packet_snapshots(
 packet_id,kind,project_id,scope_project_id,workflow_id,scope_type,scope_start,scope_end,
 source_hash,packet_hash,generated_at,created_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?))sql");
        ins.bind(1, packet.packetId);
        ins.bind(2, "report");
        if (target.projectId > 0) ins.bind(3, target.projectId); else ins.bind(3);
        ins.bind(4, target.projectId);
        if (target.workflowId > 0) ins.bind(5, target.workflowId); else ins.bind(5);
        ins.bind(6, reportType);
        ins.bind(7, periodStart);
        ins.bind(8, periodEnd);
        ins.bind(9, packet.sourceHash);
        ins.bind(10, packet.packetHash);
        ins.bind(11, generatedAt);
        ins.bind(12, nowIsoUtc());
        ins.exec();
        return reportPacketRegisteredLocked(lk.token(),
            db, target.projectId, reportType, periodStart, periodEnd,
            generatedAt, "sha256:" + packet.sourceHash);
    } catch (const std::exception&) {
        return false;
    }
}

std::string dateAfterDays(int days) {
    std::time_t t = std::time(nullptr) + static_cast<std::time_t>(days) * 86400;
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[16]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string clip(const std::string& value, size_t size) {
    if (value.size() <= size) return value;
    size_t cut = size;
    while (cut > 0 && cut < value.size() &&
           (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80)
        --cut;
    return value.substr(0, cut) + "\n...[truncated by DevHub; omitted " +
           std::to_string(value.size() - cut) + " UTF-8 bytes]";
}

bool rowExists(SQLite::Database& db, const char* table, long long id) {
    SQLite::Statement q(db, std::string("SELECT 1 FROM ") + table + " WHERE id=?");
    q.bind(1, id);
    return q.executeStep();
}

std::string workflowCompletionErrorLocked(Db::Held held, Db* db, long long workflowId,
                                          const std::string* minimumOverride = nullptr,
                                          const std::string* validationOverride = nullptr,
                                          const long long* revisionOverride = nullptr) {
    SQLite::Statement workflow(db->raw(held),
        "SELECT minimum_success,validation_json,contract_revision "
        "FROM workflow_runs WHERE id=?");
    workflow.bind(1, workflowId);
    if (!workflow.executeStep()) return "workflow not found";
    std::string minimum = minimumOverride
        ? trim(*minimumOverride) : trim(workflow.getColumn(0).getString());
    std::string validation = validationOverride
        ? *validationOverride : workflow.getColumn(1).getString();
    const long long contractRevision = revisionOverride
        ? *revisionOverride : workflow.getColumn(2).getInt64();
    if (minimum.empty()) return "workflow completion requires minimum_success";

    json parsed = json::parse(validation, nullptr, false);
    if (!parsed.is_array() || parsed.empty())
        return "workflow completion requires a non-empty validation array";

    std::set<std::string> seen;
    std::vector<std::string> criteria;
    for (const auto& entry : parsed) {
        std::string criterion;
        if (entry.is_string()) criterion = trim(entry.get<std::string>());
        else if (entry.is_object()) {
            criterion = trim(textField(entry, "criterion",
                             textField(entry, "name", textField(entry, "title"))));
        }
        if (criterion.empty())
            return "every validation entry must name a criterion";
        std::string key = toLower(criterion);
        if (!seen.insert(key).second)
            return "validation contains a duplicate criterion: " + criterion;
        criteria.push_back(criterion);
    }

    for (const auto& criterion : criteria) {
        SQLite::Statement evidence(db->raw(held), R"sql(
SELECT status,check_command,started_at,finished_at,exit_code,failure_count,
 CASE WHEN datetime(started_at) IS NOT NULL AND datetime(finished_at) IS NOT NULL
  AND datetime(started_at)<=datetime(finished_at)
  AND datetime(finished_at)>=datetime('now','-7 days')
  AND datetime(finished_at)<=datetime('now','+5 minutes')
 THEN 1 ELSE 0 END AS is_fresh
FROM verification_evidence
WHERE workflow_id=? AND lower(trim(criterion))=lower(trim(?))
 AND contract_revision=?
ORDER BY datetime(created_at) DESC,id DESC LIMIT 1)sql");
        evidence.bind(1, workflowId);
        evidence.bind(2, criterion);
        evidence.bind(3, contractRevision);
        if (!evidence.executeStep())
            return "missing verification evidence for criterion: " + criterion;
        if (evidence.getColumn(0).getString() != "pass")
            return "latest evidence is not passing for criterion: " + criterion;
        if (trim(evidence.getColumn(1).getString()).empty() ||
            trim(evidence.getColumn(2).getString()).empty() ||
            trim(evidence.getColumn(3).getString()).empty() ||
            evidence.getColumn(4).isNull() || evidence.getColumn(4).getInt() != 0 ||
            evidence.getColumn(5).getInt() != 0)
            return "passing evidence is incomplete for criterion: " + criterion;
        if (evidence.getColumn(6).getInt() != 1)
            return "passing evidence is older than 7 days for criterion: " + criterion;
    }
    return "";
}

long long scalar(SQLite::Database& db, const std::string& sql) {
    SQLite::Statement q(db, sql);
    if (!q.executeStep()) return 0;
    return q.getColumn(0).getInt64();
}

json saveSourceLocked(Db::Held held, Db* db, const json& body) {
    const long long projectId = intField(body, "project_id");
    std::string uri = trim(textField(body, "uri"));
    std::string title = trim(textField(body, "title"));
    std::string raw = textField(body, "raw_text");
    std::string sourceType = trim(textField(body, "source_type", "document"));
    std::string captured = trim(textField(body, "captured_at", nowIsoUtc()));
    std::string published = trim(textField(body, "published_at"));
    std::string hash = trim(textField(body, "content_hash"));
    std::string metadata = jsonField(body, "metadata", "{}");
    if (uri.empty() && title.empty() && raw.empty())
        return fail("source requires uri, title, or raw_text");
    if (hash.empty()) hash = stableHash(raw.empty() ? uri + "\n" + title + "\n" + published : raw);

    if (!uri.empty() && !hash.empty()) {
        SQLite::Statement existing(db->raw(held),
            "SELECT id FROM knowledge_sources WHERE uri=? AND content_hash=?");
        existing.bind(1, uri);
        existing.bind(2, hash);
        if (existing.executeStep())
            return json{{"ok", true}, {"id", existing.getColumn(0).getInt64()},
                        {"duplicate", true}, {"content_hash", hash}};
    }

    SQLite::Statement ins(db->raw(held), R"sql(
INSERT INTO knowledge_sources(project_id,uri,title,source_type,captured_at,
 published_at,content_hash,raw_text,metadata_json,created_at)
VALUES(?,?,?,?,?,?,?,?,?,?))sql");
    if (projectId > 0) ins.bind(1, projectId); else ins.bind(1);
    ins.bind(2, uri); ins.bind(3, title); ins.bind(4, sourceType);
    ins.bind(5, captured); ins.bind(6, published); ins.bind(7, hash);
    ins.bind(8, raw); ins.bind(9, metadata); ins.bind(10, nowIsoUtc());
    ins.exec();
    return json{{"ok", true}, {"id", db->raw(held).getLastInsertRowid()},
                {"duplicate", false}, {"content_hash", hash}};
}

json attachSourceLocked(Db::Held held, Db* db, long long nodeId, long long sourceId,
                        const std::string& locator,
                        const std::string& relationship,
                        const std::string& note) {
    if (!rowExists(db->raw(held), "knowledge_nodes", nodeId))
        return fail("knowledge node not found", "not_found");
    if (!rowExists(db->raw(held), "knowledge_sources", sourceId))
        return fail("knowledge source not found", "not_found");
    SQLite::Statement ins(db->raw(held), R"sql(
INSERT INTO knowledge_node_sources(node_id,source_id,locator,relationship,note,added_at)
VALUES(?,?,?,?,?,?) ON CONFLICT(node_id,source_id,locator,relationship)
DO UPDATE SET note=excluded.note)sql");
    ins.bind(1, nodeId); ins.bind(2, sourceId); ins.bind(3, locator);
    ins.bind(4, relationship.empty() ? "derived_from" : relationship);
    ins.bind(5, note); ins.bind(6, nowIsoUtc());
    ins.exec();
    return json{{"ok", true}, {"node_id", nodeId}, {"source_id", sourceId}};
}

json addClaimLocked(Db::Held held, Db* db, long long nodeId, const json& body) {
    if (!rowExists(db->raw(held), "knowledge_nodes", nodeId))
        return fail("knowledge node not found", "not_found");
    std::string claimText = trim(textField(body, "claim_text", textField(body, "claim")));
    if (claimText.empty()) return fail("claim_text is required");
    std::string confidence = toLower(trim(textField(body, "confidence", "stated")));
    std::string freshness = toLower(trim(textField(body, "freshness", "timeless")));
    std::string volatility = toLower(trim(textField(body, "volatility", "stable")));
    std::string observed = trim(textField(body, "observed_at"));
    std::string review = trim(textField(body, "review_after"));
    long long sourceId = intField(body, "source_id");
    std::string locator = trim(textField(body, "source_locator", textField(body, "locator")));
    const bool inference = boolField(body, "inference");
    std::string status = toLower(trim(textField(body, "status", "active")));

    if (!choice(confidence, {"stated", "high", "medium", "speculation"}))
        return fail("invalid claim confidence");
    if (!choice(freshness, {"timeless", "snapshot", "pointer"}))
        return fail("invalid claim freshness");
    if (!choice(volatility, {"stable", "slow", "fast"}))
        return fail("invalid claim volatility");
    if (freshness == "snapshot" && observed.empty()) observed = nowIsoUtc();
    if (volatility == "fast") {
        if (observed.empty()) observed = nowIsoUtc();
        if (review.empty()) review = dateAfterDays(7);
    }
    if (freshness == "pointer" && sourceId <= 0)
        return fail("pointer claims require source_id");
    if (sourceId > 0 && !rowExists(db->raw(held), "knowledge_sources", sourceId))
        return fail("claim source not found", "not_found");

    SQLite::Statement duplicate(db->raw(held), R"sql(
SELECT id FROM knowledge_claims WHERE node_id=? AND claim_text=?
 AND IFNULL(source_id,0)=? AND source_locator=? AND status='active')sql");
    duplicate.bind(1, nodeId); duplicate.bind(2, claimText);
    duplicate.bind(3, sourceId); duplicate.bind(4, locator);
    if (duplicate.executeStep())
        return json{{"ok", true}, {"id", duplicate.getColumn(0).getInt64()},
                    {"duplicate", true}};

    SQLite::Statement ins(db->raw(held), R"sql(
INSERT INTO knowledge_claims(node_id,claim_text,confidence,freshness,volatility,
 observed_at,review_after,source_id,source_locator,inference,status,created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?))sql");
    ins.bind(1, nodeId); ins.bind(2, claimText); ins.bind(3, confidence);
    ins.bind(4, freshness); ins.bind(5, volatility); ins.bind(6, observed);
    ins.bind(7, review);
    if (sourceId > 0) ins.bind(8, sourceId); else ins.bind(8);
    ins.bind(9, locator); ins.bind(10, inference ? 1 : 0); ins.bind(11, status);
    ins.bind(12, nowIsoUtc()); ins.bind(13, nowIsoUtc());
    ins.exec();
    return json{{"ok", true}, {"id", db->raw(held).getLastInsertRowid()},
                {"duplicate", false}};
}

json nodeRowLocked(Db::Held held, Db* db, long long nodeId) {
    SQLite::Statement q(db->raw(held), R"sql(
SELECT n.*,IFNULL(p.name,'') AS project_name
FROM knowledge_nodes n LEFT JOIN projects p ON p.id=n.project_id WHERE n.id=?)sql");
    q.bind(1, nodeId);
    if (!q.executeStep())
        return fail("knowledge node not found", "not_found");
    return Db::rowToJson(q);
}

} // namespace

json saveKnowledgeSource(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        SQLite::Transaction tx(db->raw(lk.token()));
        json result = saveSourceLocked(lk.token(), db, body);
        if (result.value("ok", false)) {
            db->logActivity(lk.token(), "knowledge_source", intField(body, "project_id"),
                            textField(body, "uri", textField(body, "title")));
            tx.commit();
        }
        return result;
    } catch (const std::exception& e) { return fail(e.what()); }
}

json listKnowledgeNodes(Db* db, long long projectId, const std::string& query,
                        const std::string& kind, const std::string& status,
                        int limit) {
    auto lk = db->guard();
    std::string needle = "%" + toLower(trim(query)) + "%";
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT n.id,n.project_id,IFNULL(p.name,'') AS project_name,n.kind,n.title,
 n.preamble,n.summary,substr(n.body,1,600) AS body_excerpt,n.tags,n.status,
 n.confidence,n.freshness,n.volatility,n.context_level,n.pinned,n.observed_at,
 n.review_after,n.pointer_uri,n.content_hash,n.dedupe_key,n.superseded_by,
 n.created_at,n.updated_at,
 (SELECT COUNT(*) FROM knowledge_claims c WHERE c.node_id=n.id AND c.status='active') AS claim_count,
 (SELECT COUNT(*) FROM knowledge_node_sources s WHERE s.node_id=n.id) AS source_count,
 (SELECT COUNT(*) FROM knowledge_links l WHERE l.from_node_id=n.id OR l.to_node_id=n.id) AS link_count,
 (SELECT COUNT(*) FROM knowledge_conflicts x WHERE x.status='open' AND
   (x.left_node_id=n.id OR x.right_node_id=n.id)) AS conflict_count
FROM knowledge_nodes n LEFT JOIN projects p ON p.id=n.project_id
WHERE (?=0 OR n.project_id=?) AND (?='' OR n.kind=?) AND (?='' OR n.status=?)
 AND (?='%%' OR lower(n.title||' '||n.preamble||' '||n.summary||' '||n.body||' '||n.tags) LIKE ?
      OR EXISTS(SELECT 1 FROM knowledge_claims c WHERE c.node_id=n.id
                AND lower(c.claim_text) LIKE ?))
ORDER BY n.pinned DESC,
 CASE n.status WHEN 'active' THEN 0 WHEN 'draft' THEN 1 WHEN 'superseded' THEN 2 ELSE 3 END,
 n.updated_at DESC,n.id DESC LIMIT ?)sql");
    int b = 1;
    q.bind(b++, projectId); q.bind(b++, projectId);
    q.bind(b++, kind); q.bind(b++, kind);
    q.bind(b++, status); q.bind(b++, status);
    q.bind(b++, needle); q.bind(b++, needle); q.bind(b++, needle);
    q.bind(b++, boundLimit(limit));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items},
                {"query", query}, {"limit", boundLimit(limit)}};
}

json getKnowledgeNode(Db* db, long long nodeId) {
    auto lk = db->guard();
    json node = nodeRowLocked(lk.token(), db, nodeId);
    if (!node.value("ok", true)) return node;
    {
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT c.*,s.uri AS source_uri,s.title AS source_title
FROM knowledge_claims c LEFT JOIN knowledge_sources s ON s.id=c.source_id
WHERE c.node_id=? ORDER BY c.status,c.id)sql");
        q.bind(1, nodeId); node["claims"] = Db::rowsToJson(q);
    }
    {
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT s.id,s.project_id,s.uri,s.title,s.source_type,s.captured_at,s.published_at,
 s.content_hash,s.metadata_json,substr(s.raw_text,1,2000) AS raw_excerpt,
 x.locator,x.relationship,x.note,x.added_at
FROM knowledge_node_sources x JOIN knowledge_sources s ON s.id=x.source_id
WHERE x.node_id=? ORDER BY x.added_at,s.id)sql");
        q.bind(1, nodeId); node["sources"] = Db::rowsToJson(q);
    }
    {
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT l.id,l.from_node_id,l.to_node_id,l.relation,l.strength,l.note,l.created_at,
 f.title AS from_title,t.title AS to_title
FROM knowledge_links l JOIN knowledge_nodes f ON f.id=l.from_node_id
JOIN knowledge_nodes t ON t.id=l.to_node_id
WHERE l.from_node_id=? OR l.to_node_id=? ORDER BY l.created_at)sql");
        q.bind(1, nodeId); q.bind(2, nodeId); node["links"] = Db::rowsToJson(q);
    }
    {
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT x.*,l.title AS left_title,r.title AS right_title
FROM knowledge_conflicts x JOIN knowledge_nodes l ON l.id=x.left_node_id
JOIN knowledge_nodes r ON r.id=x.right_node_id
WHERE x.left_node_id=? OR x.right_node_id=?
ORDER BY x.status,x.updated_at DESC,x.id DESC)sql");
        q.bind(1, nodeId); q.bind(2, nodeId); node["conflicts"] = Db::rowsToJson(q);
    }
    node["ok"] = true;
    return node;
}

json saveKnowledgeNode(Db* db, long long nodeId, const json& body) {
    auto lk = db->guard();
    // Route ids <= 0 all mean create. Keeping one normalization prevents a
    // negative id from skipping the load/dedupe/insert branches and silently
    // issuing an UPDATE that matches no row.
    if (nodeId < 0) nodeId = 0;
    try {
        SQLite::Transaction tx(db->raw(lk.token()));
        json current = json::object();
        if (nodeId > 0) {
            current = nodeRowLocked(lk.token(), db, nodeId);
            if (!current.value("ok", true)) return current;
        }
        auto chooseText = [&](const char* key, const std::string& fallback) {
            return body.contains(key) ? textField(body, key) : fallback;
        };
        auto chooseInt = [&](const char* key, long long fallback) {
            return body.contains(key) ? intField(body, key, fallback) : fallback;
        };

        const long long projectId = chooseInt("project_id", intField(current, "project_id"));
        std::string kind = toLower(trim(chooseText("kind", textField(current, "kind", "reference"))));
        std::string title = trim(chooseText("title", textField(current, "title")));
        std::string preamble = trim(chooseText("preamble", textField(current, "preamble")));
        std::string summary = trim(chooseText("summary", textField(current, "summary")));
        std::string nodeBody = chooseText("body", textField(current, "body"));
        std::string tags = trim(chooseText("tags", textField(current, "tags")));
        std::string status = toLower(trim(chooseText("status", textField(current, "status", "active"))));
        std::string confidence = toLower(trim(chooseText("confidence", textField(current, "confidence", "stated"))));
        std::string freshness = toLower(trim(chooseText("freshness", textField(current, "freshness", "timeless"))));
        std::string volatility = toLower(trim(chooseText("volatility", textField(current, "volatility", "stable"))));
        int contextLevel = static_cast<int>(chooseInt("context_level", intField(current, "context_level", 2)));
        int pinned = static_cast<int>(chooseInt("pinned", intField(current, "pinned")) != 0);
        std::string observed = trim(chooseText("observed_at", textField(current, "observed_at")));
        std::string review = trim(chooseText("review_after", textField(current, "review_after")));
        std::string pointer = trim(chooseText("pointer_uri", textField(current, "pointer_uri")));
        if (pointer.empty() && body.contains("source") && body["source"].is_object())
            pointer = trim(textField(body["source"], "uri"));

        if (title.empty()) return fail("title is required");
        if (!choice(status, {"draft", "active", "superseded", "archived"}))
            return fail("invalid knowledge status");
        if (!choice(confidence, {"stated", "high", "medium", "speculation"}))
            return fail("invalid knowledge confidence");
        if (!choice(freshness, {"timeless", "snapshot", "pointer"}))
            return fail("invalid knowledge freshness");
        if (!choice(volatility, {"stable", "slow", "fast"}))
            return fail("invalid knowledge volatility");
        contextLevel = std::max(0, std::min(contextLevel, 3));
        if (freshness == "snapshot" && observed.empty()) observed = nowIsoUtc();
        if (volatility == "fast") {
            if (observed.empty()) observed = nowIsoUtc();
            if (review.empty()) review = dateAfterDays(7);
        }
        bool hasSourceInput = body.contains("source") || body.contains("sources") ||
                              body.contains("source_ids");
        if (freshness == "pointer" && pointer.empty() && !hasSourceInput)
            return fail("pointer knowledge requires pointer_uri or an attached source");

        std::string contentHash = stableHash(preamble + "\n" + summary + "\n" + nodeBody);
        std::string dedupeKey = std::to_string(projectId) + ":" + kind + ":" + normalizeKey(title);
        bool duplicate = false;
        long long supersedesId = intField(body, "supersedes_id");

        if (nodeId == 0) {
            SQLite::Statement existing(db->raw(lk.token()),
                "SELECT id,content_hash FROM knowledge_nodes WHERE dedupe_key=? "
                "AND status IN ('active','draft') ORDER BY updated_at DESC LIMIT 1");
            existing.bind(1, dedupeKey);
            if (existing.executeStep()) {
                long long existingId = existing.getColumn(0).getInt64();
                std::string existingHash = existing.getColumn(1).getString();
                if (existingHash == contentHash) {
                    nodeId = existingId;
                    duplicate = true;
                } else if (supersedesId != existingId && !boolField(body, "allow_duplicate")) {
                    json result = fail(
                        "possible duplicate title; update, supersede, or set allow_duplicate",
                        "duplicate");
                    result["existing_id"] = existingId;
                    return result;
                }
            }
        }

        const std::string now = nowIsoUtc();
        if (nodeId == 0) {
            SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO knowledge_nodes(project_id,kind,title,preamble,summary,body,tags,status,
 confidence,freshness,volatility,context_level,pinned,observed_at,review_after,
 pointer_uri,content_hash,dedupe_key,created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql");
            if (projectId > 0) ins.bind(1, projectId); else ins.bind(1);
            ins.bind(2, kind); ins.bind(3, title); ins.bind(4, preamble);
            ins.bind(5, summary); ins.bind(6, nodeBody); ins.bind(7, tags);
            ins.bind(8, status); ins.bind(9, confidence); ins.bind(10, freshness);
            ins.bind(11, volatility); ins.bind(12, contextLevel); ins.bind(13, pinned);
            ins.bind(14, observed); ins.bind(15, review); ins.bind(16, pointer);
            ins.bind(17, contentHash); ins.bind(18, dedupeKey);
            ins.bind(19, now); ins.bind(20, now); ins.exec();
            nodeId = db->raw(lk.token()).getLastInsertRowid();
        } else if (!duplicate) {
            SQLite::Statement up(db->raw(lk.token()), R"sql(
UPDATE knowledge_nodes SET project_id=?,kind=?,title=?,preamble=?,summary=?,body=?,
 tags=?,status=?,confidence=?,freshness=?,volatility=?,context_level=?,pinned=?,
 observed_at=?,review_after=?,pointer_uri=?,content_hash=?,dedupe_key=?,updated_at=?
WHERE id=?)sql");
            if (projectId > 0) up.bind(1, projectId); else up.bind(1);
            up.bind(2, kind); up.bind(3, title); up.bind(4, preamble);
            up.bind(5, summary); up.bind(6, nodeBody); up.bind(7, tags);
            up.bind(8, status); up.bind(9, confidence); up.bind(10, freshness);
            up.bind(11, volatility); up.bind(12, contextLevel); up.bind(13, pinned);
            up.bind(14, observed); up.bind(15, review); up.bind(16, pointer);
            up.bind(17, contentHash); up.bind(18, dedupeKey); up.bind(19, now);
            up.bind(20, nodeId);
            if (up.exec() != 1)
                return fail("knowledge node " + std::to_string(nodeId) +
                            " not found", "not_found");
        }

        if (supersedesId > 0 && supersedesId != nodeId) {
            if (!rowExists(db->raw(lk.token()), "knowledge_nodes", supersedesId))
                return fail("superseded knowledge node not found", "not_found");
            SQLite::Statement old(db->raw(lk.token()),
                "UPDATE knowledge_nodes SET status='superseded',superseded_by=?,updated_at=? WHERE id=?");
            old.bind(1, nodeId); old.bind(2, now); old.bind(3, supersedesId); old.exec();
            SQLite::Statement lineage(db->raw(lk.token()), R"sql(
INSERT INTO knowledge_links(from_node_id,to_node_id,relation,strength,note,created_at)
VALUES(?,?,'supersedes',1.0,'Explicit revision lineage',?)
ON CONFLICT(from_node_id,to_node_id,relation) DO NOTHING)sql");
            lineage.bind(1, nodeId); lineage.bind(2, supersedesId);
            lineage.bind(3, now); lineage.exec();
        }

        long long primarySourceId = 0;
        auto attachObject = [&](const json& sourceSpec) -> json {
            long long sourceId = intField(sourceSpec, "id", intField(sourceSpec, "source_id"));
            if (sourceId <= 0) {
                json spec = sourceSpec;
                if (projectId > 0 && !spec.contains("project_id")) spec["project_id"] = projectId;
                json saved = saveSourceLocked(lk.token(), db, spec);
                if (!saved.value("ok", false)) return saved;
                sourceId = saved["id"].get<long long>();
            }
            if (primarySourceId == 0) primarySourceId = sourceId;
            return attachSourceLocked(lk.token(), db, nodeId, sourceId,
                textField(sourceSpec, "locator"),
                textField(sourceSpec, "relationship", "derived_from"),
                textField(sourceSpec, "note"));
        };
        if (body.contains("source") && body["source"].is_object()) {
            json attached = attachObject(body["source"]);
            if (!attached.value("ok", false)) return attached;
        }
        if (body.contains("sources") && body["sources"].is_array()) {
            for (const auto& source : body["sources"]) {
                json spec = source.is_object() ? source : json{{"id", source}};
                json attached = attachObject(spec);
                if (!attached.value("ok", false)) return attached;
            }
        }
        if (body.contains("source_ids") && body["source_ids"].is_array()) {
            for (const auto& id : body["source_ids"]) {
                long long sourceId = id.is_number_integer() ? id.get<long long>() : 0;
                json attached = attachSourceLocked(lk.token(), db, nodeId, sourceId, "", "derived_from", "");
                if (!attached.value("ok", false)) return attached;
            }
        }
        if (body.contains("claims") && body["claims"].is_array()) {
            for (const auto& claim : body["claims"]) {
                json claimSpec = claim;
                if (!claimSpec.contains("source_id") && primarySourceId > 0 &&
                    !boolField(claimSpec, "inference"))
                    claimSpec["source_id"] = primarySourceId;
                json saved = addClaimLocked(lk.token(), db, nodeId, claimSpec);
                if (!saved.value("ok", false)) return saved;
            }
        }

        db->logActivity(lk.token(), duplicate ? "knowledge_reused" : "knowledge_saved",
                        projectId, title);
        tx.commit();
        return json{{"ok", true}, {"id", nodeId}, {"duplicate", duplicate},
                    {"content_hash", contentHash}, {"dedupe_key", dedupeKey}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json addKnowledgeClaim(Db* db, long long nodeId, const json& body) {
    auto lk = db->guard();
    try {
        SQLite::Transaction tx(db->raw(lk.token()));
        json result = addClaimLocked(lk.token(), db, nodeId, body);
        if (result.value("ok", false)) {
            db->logActivity(lk.token(), "knowledge_claim", 0, textField(body, "claim_text"));
            tx.commit();
        }
        return result;
    } catch (const std::exception& e) { return fail(e.what()); }
}

json linkKnowledgeNodes(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        long long from = intField(body, "from_node_id");
        long long to = intField(body, "to_node_id");
        std::string relation = toLower(trim(textField(body, "relation", "related")));
        if (from <= 0 || to <= 0 || from == to)
            return fail("from_node_id and a different to_node_id are required");
        if (!rowExists(db->raw(lk.token()), "knowledge_nodes", from) ||
            !rowExists(db->raw(lk.token()), "knowledge_nodes", to))
            return fail("linked knowledge node not found", "not_found");
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO knowledge_links(from_node_id,to_node_id,relation,strength,note,created_at)
VALUES(?,?,?,?,?,?) ON CONFLICT(from_node_id,to_node_id,relation)
DO UPDATE SET strength=excluded.strength,note=excluded.note
RETURNING id)sql");
        ins.bind(1, from); ins.bind(2, to); ins.bind(3, relation);
        ins.bind(4, std::max(0.0, std::min(doubleField(body, "strength", 1.0), 1.0)));
        ins.bind(5, textField(body, "note")); ins.bind(6, nowIsoUtc());
        if (!ins.executeStep()) return fail("knowledge link upsert returned no id");
        const long long linkId = ins.getColumn(0).getInt64();
        return json{{"ok", true}, {"id", linkId},
                    {"from_node_id", from}, {"to_node_id", to}, {"relation", relation}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json listKnowledgeConflicts(Db* db, long long projectId,
                            const std::string& status, int limit) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT x.*,l.title AS left_title,r.title AS right_title,
 IFNULL(lp.name,IFNULL(rp.name,'')) AS project_name
FROM knowledge_conflicts x JOIN knowledge_nodes l ON l.id=x.left_node_id
JOIN knowledge_nodes r ON r.id=x.right_node_id
LEFT JOIN projects lp ON lp.id=l.project_id LEFT JOIN projects rp ON rp.id=r.project_id
WHERE (?=0 OR l.project_id=? OR r.project_id=?) AND (?='' OR x.status=?)
ORDER BY CASE x.status WHEN 'open' THEN 0 ELSE 1 END,x.updated_at DESC LIMIT ?)sql");
    q.bind(1, projectId); q.bind(2, projectId); q.bind(3, projectId);
    q.bind(4, status); q.bind(5, status); q.bind(6, boundLimit(limit, 100));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items}};
}

json createKnowledgeConflict(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        long long left = intField(body, "left_node_id");
        long long right = intField(body, "right_node_id");
        std::string classification = toLower(trim(textField(body, "classification", "ambiguous")));
        if (left <= 0 || right <= 0 || left == right)
            return fail("two different knowledge node ids are required");
        if (!choice(classification, {"clear_winner", "ambiguous", "evolution", "duplicate"}))
            return fail("invalid conflict classification");
        if (!rowExists(db->raw(lk.token()), "knowledge_nodes", left) ||
            !rowExists(db->raw(lk.token()), "knowledge_nodes", right))
            return fail("conflict knowledge node not found", "not_found");
        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement duplicate(db->raw(lk.token()), R"sql(
SELECT id FROM knowledge_conflicts WHERE status='open' AND
 ((left_node_id=? AND right_node_id=?) OR (left_node_id=? AND right_node_id=?)))sql");
        duplicate.bind(1, left); duplicate.bind(2, right);
        duplicate.bind(3, right); duplicate.bind(4, left);
        if (duplicate.executeStep())
            return json{{"ok", true}, {"id", duplicate.getColumn(0).getInt64()},
                        {"duplicate", true}};
        std::string now = nowIsoUtc();
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO knowledge_conflicts(left_node_id,right_node_id,classification,status,
 reason,created_at,updated_at) VALUES(?,?,?,'open',?,?,?))sql");
        ins.bind(1, left); ins.bind(2, right); ins.bind(3, classification);
        ins.bind(4, textField(body, "reason")); ins.bind(5, now); ins.bind(6, now);
        ins.exec();
        long long id = db->raw(lk.token()).getLastInsertRowid();
        SQLite::Statement link(db->raw(lk.token()), R"sql(
INSERT INTO knowledge_links(from_node_id,to_node_id,relation,strength,note,created_at)
VALUES(?,?,'contradicts',1.0,?,?)
ON CONFLICT(from_node_id,to_node_id,relation) DO UPDATE SET note=excluded.note)sql");
        link.bind(1, left); link.bind(2, right);
        link.bind(3, textField(body, "reason")); link.bind(4, now); link.exec();
        tx.commit();
        return json{{"ok", true}, {"id", id}, {"duplicate", false}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json resolveKnowledgeConflict(Db* db, long long conflictId, const json& body) {
    auto lk = db->guard();
    try {
        SQLite::Statement current(db->raw(lk.token()),
            "SELECT left_node_id,right_node_id,status FROM knowledge_conflicts WHERE id=?");
        current.bind(1, conflictId);
        if (!current.executeStep())
            return fail("knowledge conflict not found", "not_found");
        long long left = current.getColumn(0).getInt64();
        long long right = current.getColumn(1).getInt64();
        std::string resolution = trim(textField(body, "resolution"));
        long long winner = intField(body, "winner_node_id");
        if (resolution.empty()) return fail("resolution is required");
        if (winner > 0 && winner != left && winner != right)
            return fail("winner_node_id must be one side of the conflict");
        SQLite::Transaction tx(db->raw(lk.token()));
        std::string now = nowIsoUtc();
        SQLite::Statement up(db->raw(lk.token()), R"sql(
UPDATE knowledge_conflicts SET status='resolved',resolution=?,winner_node_id=?,
 updated_at=?,resolved_at=? WHERE id=?)sql");
        up.bind(1, resolution);
        if (winner > 0) up.bind(2, winner); else up.bind(2);
        up.bind(3, now); up.bind(4, now); up.bind(5, conflictId); up.exec();
        if (winner > 0 && boolField(body, "supersede_loser")) {
            long long loser = winner == left ? right : left;
            SQLite::Statement lose(db->raw(lk.token()),
                "UPDATE knowledge_nodes SET status='superseded',superseded_by=?,updated_at=? WHERE id=?");
            lose.bind(1, winner); lose.bind(2, now); lose.bind(3, loser); lose.exec();
        }
        tx.commit();
        return json{{"ok", true}, {"id", conflictId}, {"status", "resolved"},
                    {"winner_node_id", winner}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json knowledgeHealth(Db* db, long long projectId) {
    auto lk = db->guard();
    const std::string p = projectId > 0
        ? " AND n.project_id=" + std::to_string(projectId) : "";
    const std::string today = todayLocal();
    json h;
    h["nodes"] = scalar(db->raw(lk.token()), "SELECT COUNT(*) FROM knowledge_nodes n WHERE 1=1" + p);
    h["active_nodes"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_nodes n WHERE n.status='active'" + p);
    h["sources"] = scalar(db->raw(lk.token()),
        std::string("SELECT COUNT(*) FROM knowledge_sources s WHERE 1=1") +
        (projectId > 0
             ? " AND s.project_id=" + std::to_string(projectId)
             : ""));
    h["claims"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_claims c JOIN knowledge_nodes n ON n.id=c.node_id "
        "WHERE c.status='active'" + p);
    h["stale_nodes"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_nodes n WHERE n.status='active' AND n.review_after!='' "
        "AND n.review_after<'" + today + "'" + p);
    h["stale_claims"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_claims c JOIN knowledge_nodes n ON n.id=c.node_id "
        "WHERE c.status='active' AND c.review_after!='' AND c.review_after<'" + today + "'" + p);
    h["undated_snapshots"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_nodes n WHERE n.status='active' AND n.freshness='snapshot' "
        "AND n.observed_at=''" + p) + scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_claims c JOIN knowledge_nodes n ON n.id=c.node_id "
        "WHERE c.status='active' AND c.freshness='snapshot' AND c.observed_at=''" + p);
    h["undated_fast_claims"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_claims c JOIN knowledge_nodes n ON n.id=c.node_id "
        "WHERE c.status='active' AND c.volatility='fast' "
        "AND (c.observed_at='' OR c.review_after='')" + p);
    h["broken_pointers"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_nodes n WHERE n.status='active' AND n.freshness='pointer' "
        "AND n.pointer_uri='' AND NOT EXISTS(SELECT 1 FROM knowledge_node_sources x WHERE x.node_id=n.id)" + p)
        + scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_claims c JOIN knowledge_nodes n ON n.id=c.node_id "
        "WHERE c.status='active' AND c.freshness='pointer' AND c.source_id IS NULL" + p);
    h["claims_missing_provenance"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_claims c JOIN knowledge_nodes n ON n.id=c.node_id "
        "WHERE c.status='active' AND c.inference=0 AND c.source_id IS NULL" + p);
    h["orphan_nodes"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM knowledge_nodes n WHERE n.status='active' "
        "AND NOT EXISTS(SELECT 1 FROM knowledge_node_sources s WHERE s.node_id=n.id) "
        "AND NOT EXISTS(SELECT 1 FROM knowledge_claims c WHERE c.node_id=n.id) "
        "AND NOT EXISTS(SELECT 1 FROM knowledge_links l WHERE l.from_node_id=n.id OR l.to_node_id=n.id)" + p);
    h["duplicate_groups"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM (SELECT n.dedupe_key FROM knowledge_nodes n WHERE n.status IN ('active','draft')" + p +
        " GROUP BY n.dedupe_key HAVING n.dedupe_key!='' AND COUNT(*)>1)");
    h["open_conflicts"] = scalar(db->raw(lk.token()),
        std::string(
            "SELECT COUNT(*) FROM knowledge_conflicts x "
            "JOIN knowledge_nodes l ON l.id=x.left_node_id "
            "JOIN knowledge_nodes r ON r.id=x.right_node_id "
            "WHERE x.status='open'") +
        (projectId > 0
             ? " AND (l.project_id=" + std::to_string(projectId) +
               " OR r.project_id=" + std::to_string(projectId) + ")"
             : ""));
    h["draft_reports"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM reports r WHERE r.status='draft'" +
        (projectId > 0 ? " AND r.project_id=" + std::to_string(projectId) : ""));
    h["blocked_workflows"] = scalar(db->raw(lk.token()),
        "SELECT COUNT(*) FROM workflow_runs w WHERE w.status='blocked'" +
        (projectId > 0 ? " AND w.project_id=" + std::to_string(projectId) : ""));

    long long critical = h["undated_snapshots"].get<long long>() +
        h["undated_fast_claims"].get<long long>() + h["broken_pointers"].get<long long>();
    long long warning = h["stale_nodes"].get<long long>() + h["stale_claims"].get<long long>() +
        h["claims_missing_provenance"].get<long long>() + h["open_conflicts"].get<long long>() +
        h["duplicate_groups"].get<long long>();
    h["critical"] = critical;
    h["warnings"] = warning;
    h["hygiene_score"] = std::max(0LL, 100LL - critical * 10LL - warning * 2LL);
    h["status"] = critical > 0 ? "critical" : (warning > 0 ? "warning" : "healthy");
    h["checked_at"] = nowIsoUtc();
    h["project_id"] = projectId;
    h["recommendations"] = json::array();
    if (h["broken_pointers"].get<long long>() > 0)
        h["recommendations"].push_back("Repair or retire unresolved pointer records.");
    if (h["claims_missing_provenance"].get<long long>() > 0)
        h["recommendations"].push_back("Attach exact sources to non-inference claims.");
    if (h["stale_nodes"].get<long long>() + h["stale_claims"].get<long long>() > 0)
        h["recommendations"].push_back("Re-observe stale facts, convert them to pointers, or retain them as dated snapshots.");
    if (h["open_conflicts"].get<long long>() > 0)
        h["recommendations"].push_back("Reconcile open contradictions without deleting either source history.");
    h["ok"] = true;
    return h;
}

static std::string buildKnowledgePayload(Db* db, long long projectId,
                                         const std::string& query, int level,
                                         int limit) {
    level = std::max(0, std::min(level, 3));
    std::string projectName = "All projects";
    if (projectId > 0) {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()), "SELECT name FROM projects WHERE id=?");
        q.bind(1, projectId);
        if (!q.executeStep()) return {};
        projectName = q.getColumn(0).getString();
    }
    json listed = listKnowledgeNodes(db, projectId, query, "", "active", limit);
    json health = knowledgeHealth(db, projectId);

    std::ostringstream out;
    out << "## L0 - Identity and scope\n\n"
        << "- Project: " << projectName << " (id " << projectId << ")\n"
        << "- Query: " << (query.empty() ? "current project knowledge" : query) << "\n"
        << "- Requested depth: L" << level << "\n"
        << "- Knowledge health: " << health.value("status", "unknown")
        << "; critical " << health.value("critical", 0LL)
        << "; warnings " << health.value("warnings", 0LL) << "\n"
        << "- Matching active nodes: " << listed.value("count", 0) << "\n\n";
    if (level == 0) return out.str();

    out << "## L1 - Navigation\n\n";
    const json& items = listed["items"];
    for (const auto& item : items) {
        std::string itemProject = item.value("project_name", "");
        if (itemProject.empty()) itemProject = "cross-project";
        out << "- K" << item["id"] << " [" << item.value("kind", "reference")
            << "] " << item.value("title", "") << " - "
            << itemProject
            << "; claims " << item.value("claim_count", 0)
            << "; sources " << item.value("source_count", 0)
            << "; links " << item.value("link_count", 0)
            << "; open conflicts " << item.value("conflict_count", 0) << "\n";
    }
    out << "\n";
    if (level == 1) return out.str();

    out << "## L2 - Current state\n\n";
    for (const auto& item : items) {
        out << "### K" << item["id"] << " - " << item.value("title", "") << "\n\n"
            << "- Kind/confidence: " << item.value("kind", "") << " / "
            << item.value("confidence", "") << "\n"
            << "- Freshness: " << item.value("freshness", "") << " / "
            << item.value("volatility", "") << "; observed "
            << item.value("observed_at", "") << "; review "
            << item.value("review_after", "") << "\n"
            << "- Relationships/conflicts: " << item.value("link_count", 0)
            << " links / " << item.value("conflict_count", 0)
            << " open conflicts\n";
        if (!item.value("preamble", "").empty())
            out << "- Future-reader context: " << item.value("preamble", "") << "\n";
        if (!item.value("summary", "").empty()) out << "\n" << item.value("summary", "") << "\n";
        out << "\n";
    }
    if (level == 2) return out.str();

    out << "## L3 - Evidence and deep detail\n\n";
    size_t deepCount = std::min<size_t>(items.size(), 8);
    for (size_t i = 0; i < deepCount; ++i) {
        long long id = items[i]["id"].get<long long>();
        json detail = getKnowledgeNode(db, id);
        out << "### K" << id << " - " << detail.value("title", "") << "\n\n";
        std::string body = detail.value("body", "");
        if (!body.empty()) out << clip(body, 5000) << "\n\n";
        if (detail.contains("claims") && !detail["claims"].empty()) {
            out << "Claims:\n";
            for (const auto& claim : detail["claims"]) {
                out << "- C" << claim["id"] << ": "
                    << nullableText(claim, "claim_text")
                    << " [" << nullableText(claim, "confidence") << ", "
                    << nullableText(claim, "freshness") << "]";
                const std::string sourceUri = nullableText(claim, "source_uri");
                if (!sourceUri.empty())
                    out << " source " << sourceUri << " "
                        << nullableText(claim, "source_locator");
                if (claim.value("inference", 0) != 0) out << " (inference)";
                out << "\n";
            }
            out << "\n";
        }
        if (detail.contains("sources") && !detail["sources"].empty()) {
            out << "Source manifest:\n";
            for (const auto& source : detail["sources"])
                out << "- S" << source["id"] << ": "
                    << nullableText(source, "uri") << " "
                    << nullableText(source, "locator") << "; captured "
                    << nullableText(source, "captured_at") << "; hash "
                    << nullableText(source, "content_hash") << "\n";
            out << "\n";
        }
        if (detail.contains("links") && !detail["links"].empty()) {
            out << "Typed links:\n";
            const size_t count = std::min<size_t>(detail["links"].size(), 20);
            for (size_t j = 0; j < count; ++j) {
                const auto& link = detail["links"][j];
                out << "- L" << link["id"] << " ["
                    << nullableText(link, "relation", "related")
                    << "] K" << link.value("from_node_id", 0LL) << " "
                    << nullableText(link, "from_title") << " -> K"
                    << link.value("to_node_id", 0LL) << " "
                    << nullableText(link, "to_title");
                const std::string note = nullableText(link, "note");
                if (!note.empty()) out << ": " << clip(note, 800);
                out << "\n";
            }
            if (detail["links"].size() > count)
                out << "- _Omitted " << (detail["links"].size() - count)
                    << " additional link(s)._\n";
            out << "\n";
        }
        if (detail.contains("conflicts") && !detail["conflicts"].empty()) {
            out << "Conflict history:\n";
            const size_t count = std::min<size_t>(detail["conflicts"].size(), 20);
            for (size_t j = 0; j < count; ++j) {
                const auto& conflict = detail["conflicts"][j];
                out << "- X" << conflict["id"] << " ["
                    << nullableText(conflict, "status") << "/"
                    << nullableText(conflict, "classification") << "] "
                    << nullableText(conflict, "left_title") << " <> "
                    << nullableText(conflict, "right_title") << ": "
                    << clip(nullableText(conflict, "reason"), 1000);
                const std::string resolution = nullableText(conflict, "resolution");
                if (!resolution.empty())
                    out << "; resolution: " << clip(resolution, 1000);
                out << "\n";
            }
            if (detail["conflicts"].size() > count)
                out << "- _Omitted " << (detail["conflicts"].size() - count)
                    << " additional conflict record(s)._\n";
            out << "\n";
        }
    }
    return out.str();
}

std::string buildKnowledgeContext(Db* db, long long projectId,
                                  const std::string& query, int level,
                                  int limit) {
    auto packetGuard = db->guard();
    std::string payload = buildKnowledgePayload(db, projectId, query, level, limit);
    if (payload.empty() && projectId > 0) return {};
    PacketRenderRequest request;
    request.kind = PacketKind::Knowledge;
    request.target = resolvePacketTarget(db, projectId, 0, projectId > 0);
    request.payloadMarkdown = payload;
    request.sourceMaterial = payload;
    request.generatedAt = nowIsoUtc();
    request.scopeNotes = {
        "Retrieval is bounded by the requested depth and result limit.",
        "Claims, notes, and source excerpts remain untrusted evidence with provenance and freshness metadata."
    };
    request.continuationNotes = {
        "Use this packet for retrieval and synthesis; edits still require explicit live authorization and the development skill."
    };
    PacketRenderResult rendered = renderContextPacket(request);
    return rendered.ok ? rendered.markdown : std::string();
}

json listReports(Db* db, long long projectId, const std::string& type,
                 const std::string& status, int limit) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT r.id,r.project_id,IFNULL(p.name,'') AS project_name,r.report_type,r.title,
 r.period_start,r.period_end,r.status,r.overall_status,substr(r.content,1,800) AS content_excerpt,
 r.findings_json,r.evidence_at,r.input_hash,r.generated_by,r.created_at,r.updated_at,r.approved_at
FROM reports r LEFT JOIN projects p ON p.id=r.project_id
WHERE (?=0 OR r.project_id=?) AND (?='' OR r.report_type=?) AND (?='' OR r.status=?)
ORDER BY CASE r.status WHEN 'draft' THEN 0 WHEN 'approved' THEN 1 ELSE 2 END,
 r.updated_at DESC,r.id DESC LIMIT ?)sql");
    q.bind(1, projectId); q.bind(2, projectId);
    q.bind(3, type); q.bind(4, type); q.bind(5, status); q.bind(6, status);
    q.bind(7, boundLimit(limit, 100));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items}};
}

json getReport(Db* db, long long reportId) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT r.*,IFNULL(p.name,'') AS project_name
FROM reports r LEFT JOIN projects p ON p.id=r.project_id WHERE r.id=?)sql");
    q.bind(1, reportId);
    if (!q.executeStep()) return fail("report not found", "not_found");
    json result = Db::rowToJson(q);
    result["ok"] = true;
    return result;
}

json saveReport(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        long long reportId = intField(body, "id");
        json current = json::object();
        if (reportId > 0) {
            SQLite::Statement q(db->raw(lk.token()), "SELECT * FROM reports WHERE id=?");
            q.bind(1, reportId);
            if (!q.executeStep()) return fail("report not found", "not_found");
            current = Db::rowToJson(q);
            const std::string currentStatus = textField(current, "status");
            if (currentStatus == "approved" || currentStatus == "archived")
                return fail("approved and archived reports are immutable; create a superseding draft");
        }
        auto pickText = [&](const char* key, const std::string& fallback) {
            return body.contains(key) ? textField(body, key) : fallback;
        };
        long long projectId = body.contains("project_id")
            ? intField(body, "project_id") : intField(current, "project_id");
        std::string type = pickText("report_type",
            pickText("type", textField(current, "report_type", "project")));
        std::string title = trim(pickText("title", textField(current, "title")));
        std::string periodStart = trim(pickText("period_start", textField(current, "period_start")));
        std::string periodEnd = trim(pickText("period_end", textField(current, "period_end")));
        std::string status = toLower(trim(pickText("status", textField(current, "status", "draft"))));
        std::string overall = toLower(trim(pickText("overall_status", textField(current, "overall_status"))));
        std::string content = pickText("content", textField(current, "content"));
        std::string findings = body.contains("findings")
            ? jsonField(body, "findings", "[]") : textField(current, "findings_json", "[]");
        std::string evidenceAt = trim(pickText("evidence_at", textField(current, "evidence_at")));
        std::string inputHash = trim(pickText("input_hash", textField(current, "input_hash")));
        std::string generatedBy = trim(pickText("generated_by", textField(current, "generated_by", "codex")));
        if (!isValidReportType(type))
            return fail("report_type must match [a-z0-9][a-z0-9_-]{0,63} exactly; values are not normalized");
        if (title.empty()) return fail("report title is required");
        if (!choice(status, {"draft", "approved", "archived"}))
            return fail("invalid report status");
        if (!overall.empty() && !choice(overall, {"pass", "warning", "fail", "informational"}))
            return fail("invalid report overall_status");
        const bool groundingChanged = reportId > 0 &&
            (projectId != intField(current, "project_id") ||
             type != textField(current, "report_type") ||
             periodStart != textField(current, "period_start") ||
             periodEnd != textField(current, "period_end"));
        if (groundingChanged &&
            !(body.contains("evidence_at") && body.contains("input_hash"))) {
            evidenceAt.clear();
            inputHash.clear();
        }
        if (status == "approved" && (trim(content).empty() || evidenceAt.empty() ||
            !validSha256Ref(inputHash)))
            return fail("approved reports require content, evidence_at, and a sha256: input_hash from a current report packet");
        if (status == "approved") {
            SQLite::Statement validEvidenceAt(db->raw(lk.token()),
                "SELECT CASE WHEN datetime(?) IS NOT NULL AND datetime(?)<=datetime('now','+5 minutes') THEN 1 ELSE 0 END");
            validEvidenceAt.bind(1, evidenceAt); validEvidenceAt.bind(2, evidenceAt);
            validEvidenceAt.executeStep();
            if (validEvidenceAt.getColumn(0).getInt() != 1)
                return fail("approved reports require a valid non-future evidence_at timestamp");
            if (!reportPacketRegisteredLocked(lk.token(), db, projectId, type, periodStart,
                                              periodEnd, evidenceAt, inputHash))
                return fail("approved reports require an exact registered report packet matching project, type, period, evidence_at, and input_hash");
        }

        std::string now = nowIsoUtc();
        std::string approved = status == "approved"
            ? (textField(current, "approved_at").empty() ? now : textField(current, "approved_at")) : "";
        SQLite::Transaction tx(db->raw(lk.token()));
        if (reportId <= 0) {
            SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO reports(project_id,report_type,title,period_start,period_end,status,
 overall_status,content,findings_json,evidence_at,input_hash,generated_by,
 created_at,updated_at,approved_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql");
            if (projectId > 0) ins.bind(1, projectId); else ins.bind(1);
            ins.bind(2, type); ins.bind(3, title); ins.bind(4, periodStart);
            ins.bind(5, periodEnd); ins.bind(6, status); ins.bind(7, overall);
            ins.bind(8, content); ins.bind(9, findings); ins.bind(10, evidenceAt);
            ins.bind(11, inputHash); ins.bind(12, generatedBy); ins.bind(13, now);
            ins.bind(14, now); ins.bind(15, approved); ins.exec();
            reportId = db->raw(lk.token()).getLastInsertRowid();
        } else {
            SQLite::Statement up(db->raw(lk.token()), R"sql(
UPDATE reports SET project_id=?,report_type=?,title=?,period_start=?,period_end=?,
 status=?,overall_status=?,content=?,findings_json=?,evidence_at=?,input_hash=?,
 generated_by=?,updated_at=?,approved_at=? WHERE id=?)sql");
            if (projectId > 0) up.bind(1, projectId); else up.bind(1);
            up.bind(2, type); up.bind(3, title); up.bind(4, periodStart);
            up.bind(5, periodEnd); up.bind(6, status); up.bind(7, overall);
            up.bind(8, content); up.bind(9, findings); up.bind(10, evidenceAt);
            up.bind(11, inputHash); up.bind(12, generatedBy); up.bind(13, now);
            up.bind(14, approved); up.bind(15, reportId); up.exec();
        }
        db->logActivity(lk.token(), "report_saved", projectId, title);
        tx.commit();
        return json{{"ok", true}, {"id", reportId}, {"status", status},
                    {"input_hash", inputHash}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json setReportStatus(Db* db, long long reportId, const std::string& statusValue) {
    auto lk = db->guard();
    std::string status = toLower(trim(statusValue));
    if (!choice(status, {"draft", "approved", "archived"}))
        return fail("invalid report status");
    SQLite::Statement report(db->raw(lk.token()),
        "SELECT status,content,evidence_at,input_hash,project_id,report_type,"
        "period_start,period_end FROM reports WHERE id=?");
    report.bind(1, reportId);
    if (!report.executeStep()) return fail("report not found", "not_found");
    const std::string currentStatus = report.getColumn(0).getString();
    if (currentStatus == status)
        return json{{"ok", true}, {"id", reportId}, {"status", status},
                    {"unchanged", true}};
    if (currentStatus == "archived" && status != "archived")
        return fail("archived reports are immutable; create a superseding draft");
    if (currentStatus == "approved" && status != "approved" && status != "archived")
        return fail("approved reports may only remain approved or be archived");
    const std::string evidenceAt = trim(report.getColumn(2).getString());
    if (status == "approved" && (trim(report.getColumn(1).getString()).empty() ||
        evidenceAt.empty() || !validSha256Ref(trim(report.getColumn(3).getString()))))
        return fail("approved reports require content, evidence_at, and a sha256: input_hash from a current report packet");
    if (status == "approved") {
        SQLite::Statement validEvidenceAt(db->raw(lk.token()),
            "SELECT CASE WHEN datetime(?) IS NOT NULL AND datetime(?)<=datetime('now','+5 minutes') THEN 1 ELSE 0 END");
        validEvidenceAt.bind(1, evidenceAt); validEvidenceAt.bind(2, evidenceAt);
        validEvidenceAt.executeStep();
        if (validEvidenceAt.getColumn(0).getInt() != 1)
            return fail("approved reports require a valid non-future evidence_at timestamp");
        const long long projectId = report.getColumn(4).isNull()
            ? 0 : report.getColumn(4).getInt64();
        if (!reportPacketRegisteredLocked(lk.token(),
                db, projectId, report.getColumn(5).getString(),
                report.getColumn(6).getString(), report.getColumn(7).getString(),
                evidenceAt, trim(report.getColumn(3).getString())))
            return fail("approved reports require an exact registered report packet matching project, type, period, evidence_at, and input_hash");
    }
    std::string now = nowIsoUtc();
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE reports SET status=?,updated_at=?,approved_at=CASE WHEN ?='approved' "
        "THEN ? ELSE approved_at END WHERE id=?");
    up.bind(1, status); up.bind(2, now); up.bind(3, status);
    up.bind(4, now); up.bind(5, reportId); up.exec();
    return json{{"ok", true}, {"id", reportId}, {"status", status}};
}

std::string buildReportContext(Db* db, long long projectId,
                               const std::string& typeValue, int days,
                               std::string* sourceHash,
                               std::string* evidenceAt,
                               std::string* periodStart,
                               std::string* periodEnd,
                               bool registerSnapshot) {
    auto packetGuard = db->guard();
    if (sourceHash) sourceHash->clear();
    if (evidenceAt) evidenceAt->clear();
    if (periodStart) periodStart->clear();
    if (periodEnd) periodEnd->clear();
    days = std::max(1, std::min(days, 3650));
    std::string type = typeValue.empty() ? "project" : typeValue;
    if (!isValidReportType(type)) return {};
    std::string cutoff = dateAfterDays(-(days - 1));
    std::string through = dateAfterDays(0);
    std::string projectName = "All projects";
    std::ostringstream body;
    {
        auto lk = db->guard();
        if (projectId > 0) {
            SQLite::Statement project(db->raw(lk.token()), "SELECT name FROM projects WHERE id=?");
            project.bind(1, projectId);
            if (!project.executeStep()) return {};
            projectName = project.getColumn(0).getString();
        }
        body << "## Scope\n\n- Project: " << projectName << "\n- Report type: " << type
             << "\n- Period start: " << cutoff
             << "\n- Period end: " << through
             << "\n- Period rule: last " << days << " days, inclusive by UTC date\n\n";

        body << "## Active work (completed records excluded)\n\n";
        std::string activeSql = R"sql(
SELECT i.id,IFNULL(p.name,''),i.type,i.status,i.priority,i.title,
 i.due_date,i.review_date,i.blocked_reason,i.updated_at,
 IFNULL((SELECT GROUP_CONCAT(name, ', ') FROM (
   SELECT s.name FROM item_sources x JOIN sources s ON s.id=x.source_id
   WHERE x.item_id=i.id ORDER BY s.name COLLATE NOCASE,s.id
 )), '')
FROM items i JOIN projects p ON p.id=i.project_id
WHERE i.status IN ('open','in_progress','blocked'))sql";
        if (projectId > 0) activeSql += " AND i.project_id=" + std::to_string(projectId);
        activeSql += " ORDER BY i.priority DESC,i.updated_at DESC,i.id DESC LIMIT 200";
        SQLite::Statement active(db->raw(lk.token()), activeSql);
        bool any = false;
        while (active.executeStep()) {
            any = true;
            body << "- I" << active.getColumn(0).getInt64() << " ["
                 << active.getColumn(2).getString() << "/"
                 << active.getColumn(3).getString() << "] "
                 << active.getColumn(5).getString() << " (" << active.getColumn(1).getString()
                 << ", P" << active.getColumn(4).getInt() << ")";
            std::string blocked = active.getColumn(8).getString();
            if (!blocked.empty()) body << " - blocked: " << blocked;
            std::string contributors = active.getColumn(10).getString();
            if (!contributors.empty()) body << "; contributors: " << contributors;
            body << "\n";
        }
        if (!any) body << "_No active work._\n";
        body << "\n";

        body << "## Completed work in period\n\n";
        std::string doneSql = R"sql(
SELECT i.id,IFNULL(p.name,''),i.type,i.title,i.completed_at,
 IFNULL((SELECT GROUP_CONCAT(name, ', ') FROM (
   SELECT s.name FROM item_sources x JOIN sources s ON s.id=x.source_id
   WHERE x.item_id=i.id ORDER BY s.name COLLATE NOCASE,s.id
 )), '')
FROM items i JOIN projects p ON p.id=i.project_id
WHERE i.status='completed' AND datetime(i.completed_at)>=datetime(?)
 AND datetime(i.completed_at)<datetime(?,'+1 day'))sql";
        if (projectId > 0) doneSql += " AND i.project_id=" + std::to_string(projectId);
        doneSql += " ORDER BY datetime(i.completed_at) DESC,i.id DESC LIMIT 200";
        SQLite::Statement done(db->raw(lk.token()), doneSql);
        done.bind(1, cutoff);
        done.bind(2, through);
        any = false;
        while (done.executeStep()) {
            any = true;
            body << "- I" << done.getColumn(0).getInt64() << " ["
                 << done.getColumn(2).getString() << "] " << done.getColumn(3).getString()
                 << " - " << done.getColumn(4).getString();
            std::string contributors = done.getColumn(5).getString();
            if (!contributors.empty()) body << "; contributors: " << contributors;
            body << "\n";
        }
        if (!any) body << "_No completed work in this period._\n";
        body << "\n## Open workflows\n\n";
        std::string workflowSql =
            "SELECT id,title,context,phase,status,current_step,next_action,blockers_json,updated_at "
            "FROM workflow_runs WHERE status IN ('active','blocked','verification')";
        if (projectId > 0) workflowSql += " AND project_id=" + std::to_string(projectId);
        workflowSql += " ORDER BY updated_at DESC,id DESC LIMIT 20";
        SQLite::Statement workflows(db->raw(lk.token()), workflowSql);
        any = false;
        while (workflows.executeStep()) {
            any = true;
            body << "- W" << workflows.getColumn(0).getInt64() << " ["
                 << workflows.getColumn(3).getString() << "/"
                 << workflows.getColumn(4).getString() << "] "
                 << workflows.getColumn(1).getString() << "; next: "
                 << workflows.getColumn(6).getString() << "\n";
        }
        if (!any) body << "_No open workflows._\n";

        body << "\n## Unresolved knowledge conflicts\n\n";
        std::string conflictSql = R"sql(
SELECT x.id,l.title,r.title,x.classification,x.reason
FROM knowledge_conflicts x JOIN knowledge_nodes l ON l.id=x.left_node_id
JOIN knowledge_nodes r ON r.id=x.right_node_id WHERE x.status='open')sql";
        if (projectId > 0)
            conflictSql += " AND (l.project_id=" + std::to_string(projectId) +
                           " OR r.project_id=" + std::to_string(projectId) + ")";
        conflictSql += " ORDER BY x.updated_at DESC,x.id DESC LIMIT 50";
        SQLite::Statement conflicts(db->raw(lk.token()), conflictSql);
        any = false;
        while (conflicts.executeStep()) {
            any = true;
            body << "- X" << conflicts.getColumn(0).getInt64() << " ["
                 << conflicts.getColumn(3).getString() << "] "
                 << conflicts.getColumn(1).getString() << " <> "
                 << conflicts.getColumn(2).getString() << ": "
                 << conflicts.getColumn(4).getString() << "\n";
        }
        if (!any) body << "_No unresolved conflicts._\n";
    }

    body << "\n## Current knowledge\n\n"
         << buildKnowledgePayload(db, projectId, "", 2, 15);
    const std::string generated = nowIsoUtc();
    PacketRenderRequest request;
    request.kind = PacketKind::Report;
    request.target = resolvePacketTarget(db, projectId, 0, projectId > 0);
    request.payloadMarkdown = body.str();
    request.sourceMaterial = request.payloadMarkdown;
    request.generatedAt = generated;
    request.reportRegistry.enabled = true;
    request.reportRegistry.reportType = type;
    request.reportRegistry.periodStart = cutoff;
    request.reportRegistry.periodEnd = through;
    request.reportRegistry.evidenceAt = generated;
    request.scopeNotes = {
        "Active and completed work are independently bounded to 200 records; workflows to 20, conflicts to 50, and knowledge to 15.",
        "Approval requires this packet's registered type, period start/end, sha256 source identity, and evidence timestamp; capture never approves a report.",
        registerSnapshot
            ? "This packet is an explicitly registered approval capture."
            : "This packet is a read-only preview and cannot ground approval until explicitly captured."
    };
    PacketRenderResult rendered = renderContextPacket(request);
    if (!rendered.ok) return {};
    if (registerSnapshot &&
        !recordReportPacket(db, rendered, request.target, type, cutoff,
                            through, generated))
        return {};
    if (sourceHash) *sourceHash = "sha256:" + rendered.sourceHash;
    if (evidenceAt) *evidenceAt = generated;
    if (periodStart) *periodStart = cutoff;
    if (periodEnd) *periodEnd = through;
    return rendered.markdown;
}

json listWorkflows(Db* db, long long projectId, const std::string& status,
                   int limit) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT w.id,w.project_id,IFNULL(p.name,'') AS project_name,w.title,w.context,
 w.phase,w.status,w.objective,w.minimum_success,w.current_step,w.next_action,
 w.blockers_json,w.changed_files_json,w.workspace,w.branch,w.commit_hash,
 w.created_at,w.updated_at,w.completed_at,
 (SELECT COUNT(*) FROM workflow_artifacts a WHERE a.workflow_id=w.id) AS artifact_count,
 (SELECT COUNT(*) FROM verification_evidence e WHERE e.workflow_id=w.id) AS evidence_count,
 (SELECT COUNT(*) FROM verification_evidence e WHERE e.workflow_id=w.id AND e.status='fail') AS failed_evidence
FROM workflow_runs w LEFT JOIN projects p ON p.id=w.project_id
WHERE (?=0 OR w.project_id=?) AND (?='' OR w.status=?)
ORDER BY CASE w.status WHEN 'blocked' THEN 0 WHEN 'active' THEN 1 WHEN 'verification' THEN 2 ELSE 3 END,
 w.updated_at DESC LIMIT ?)sql");
    q.bind(1, projectId); q.bind(2, projectId); q.bind(3, status); q.bind(4, status);
    q.bind(5, boundLimit(limit, 100));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items}};
}

json getWorkflow(Db* db, long long workflowId) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT w.*,IFNULL(p.name,'') AS project_name FROM workflow_runs w
LEFT JOIN projects p ON p.id=w.project_id WHERE w.id=?)sql");
    q.bind(1, workflowId);
    if (!q.executeStep()) return fail("workflow not found", "not_found");
    json result = Db::rowToJson(q);
    {
        SQLite::Statement a(db->raw(lk.token()),
            "SELECT * FROM workflow_artifacts WHERE workflow_id=? ORDER BY created_at,id");
        a.bind(1, workflowId); result["artifacts"] = Db::rowsToJson(a);
    }
    {
        SQLite::Statement e(db->raw(lk.token()),
            "SELECT * FROM verification_evidence WHERE workflow_id=? ORDER BY created_at,id");
        e.bind(1, workflowId); result["evidence"] = Db::rowsToJson(e);
    }
    result["ok"] = true;
    return result;
}

json saveWorkflow(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        long long workflowId = intField(body, "id");
        json current = json::object();
        if (workflowId > 0) {
            SQLite::Statement q(db->raw(lk.token()), "SELECT * FROM workflow_runs WHERE id=?");
            q.bind(1, workflowId);
            if (!q.executeStep()) return fail("workflow not found", "not_found");
            current = Db::rowToJson(q);
        }
        auto pickText = [&](const char* key, const std::string& fallback) {
            return body.contains(key) ? textField(body, key) : fallback;
        };
        auto pickJson = [&](const char* key, const std::string& fallback) {
            return body.contains(key) ? jsonField(body, key, fallback) : fallback;
        };
        long long projectId = body.contains("project_id")
            ? intField(body, "project_id") : intField(current, "project_id");
        std::string title = trim(pickText("title", textField(current, "title")));
        std::string context = toLower(trim(pickText("context", textField(current, "context", "dev"))));
        std::string phase = toLower(trim(pickText("phase", textField(current, "phase", "discover"))));
        std::string status = toLower(trim(pickText("status", textField(current, "status", "active"))));
        std::string objective = pickText("objective", textField(current, "objective"));
        std::string minimum = pickText("minimum_success", textField(current, "minimum_success"));
        std::string exceptional = pickText("exceptional_success", textField(current, "exceptional_success"));
        std::string boundaries = pickJson("boundaries", textField(current, "boundaries_json", "[]"));
        std::string stakeholders = pickJson("stakeholders", textField(current, "stakeholders_json", "[]"));
        std::string assets = pickJson("existing_assets", textField(current, "existing_assets_json", "[]"));
        std::string constraints = pickJson("constraints", textField(current, "constraints_json", "[]"));
        std::string validation = pickJson("validation", textField(current, "validation_json", "[]"));
        std::string currentStep = pickText("current_step", textField(current, "current_step"));
        std::string nextAction = pickText("next_action", textField(current, "next_action"));
        std::string blockers = pickJson("blockers", textField(current, "blockers_json", "[]"));
        std::string changedFiles = pickJson("changed_files", textField(current, "changed_files_json", "[]"));
        std::string workspace = pickText("workspace", textField(current, "workspace"));
        std::string branch = pickText("branch", textField(current, "branch"));
        std::string commit = pickText("commit_hash", textField(current, "commit_hash"));
        long long contractRevision = intField(current, "contract_revision", 1);
        if (contractRevision < 1) contractRevision = 1;
        const bool contractChanged = workflowId > 0 &&
            (objective != textField(current, "objective") ||
             minimum != textField(current, "minimum_success") ||
             exceptional != textField(current, "exceptional_success") ||
             boundaries != textField(current, "boundaries_json", "[]") ||
             stakeholders != textField(current, "stakeholders_json", "[]") ||
             constraints != textField(current, "constraints_json", "[]") ||
             validation != textField(current, "validation_json", "[]"));
        if (contractChanged) ++contractRevision;
        if (title.empty()) return fail("workflow title is required");
        if (!choice(context, {"dev", "knowledge", "mixed"})) return fail("invalid workflow context");
        if (!choice(phase, {"discover", "define", "develop", "deliver"})) return fail("invalid workflow phase");
        if (!choice(status, {"active", "blocked", "verification", "complete", "cancelled"}))
            return fail("invalid workflow status");
        if (status == "complete") {
            if (workflowId <= 0)
                return fail("workflow completion requires an existing intent contract");
            std::string gate = workflowCompletionErrorLocked(lk.token(),
                db, workflowId, &minimum, &validation, &contractRevision);
            if (!gate.empty()) return fail(gate);
        }
        std::string now = nowIsoUtc();
        const std::string oldStatus = textField(current, "status");
        std::string completed;
        if (status == "complete") {
            completed = oldStatus == "complete"
                ? textField(current, "completed_at") : now;
            if (completed.empty()) completed = now;
        }
        SQLite::Transaction tx(db->raw(lk.token()));
        if (workflowId <= 0) {
            SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO workflow_runs(project_id,title,context,phase,status,objective,
 minimum_success,exceptional_success,boundaries_json,stakeholders_json,
 existing_assets_json,constraints_json,validation_json,current_step,next_action,
 blockers_json,changed_files_json,workspace,branch,commit_hash,contract_revision,
 created_at,updated_at,completed_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql");
            if (projectId > 0) ins.bind(1, projectId); else ins.bind(1);
            ins.bind(2, title); ins.bind(3, context); ins.bind(4, phase); ins.bind(5, status);
            ins.bind(6, objective); ins.bind(7, minimum); ins.bind(8, exceptional);
            ins.bind(9, boundaries); ins.bind(10, stakeholders); ins.bind(11, assets);
            ins.bind(12, constraints); ins.bind(13, validation); ins.bind(14, currentStep);
            ins.bind(15, nextAction); ins.bind(16, blockers); ins.bind(17, changedFiles);
            ins.bind(18, workspace); ins.bind(19, branch); ins.bind(20, commit);
            ins.bind(21, contractRevision); ins.bind(22, now);
            ins.bind(23, now); ins.bind(24, completed); ins.exec();
            workflowId = db->raw(lk.token()).getLastInsertRowid();
        } else {
            SQLite::Statement up(db->raw(lk.token()), R"sql(
UPDATE workflow_runs SET project_id=?,title=?,context=?,phase=?,status=?,objective=?,
 minimum_success=?,exceptional_success=?,boundaries_json=?,stakeholders_json=?,
 existing_assets_json=?,constraints_json=?,validation_json=?,current_step=?,next_action=?,
 blockers_json=?,changed_files_json=?,workspace=?,branch=?,commit_hash=?,updated_at=?,completed_at=?
 ,contract_revision=?
WHERE id=?)sql");
            if (projectId > 0) up.bind(1, projectId); else up.bind(1);
            up.bind(2, title); up.bind(3, context); up.bind(4, phase); up.bind(5, status);
            up.bind(6, objective); up.bind(7, minimum); up.bind(8, exceptional);
            up.bind(9, boundaries); up.bind(10, stakeholders); up.bind(11, assets);
            up.bind(12, constraints); up.bind(13, validation); up.bind(14, currentStep);
            up.bind(15, nextAction); up.bind(16, blockers); up.bind(17, changedFiles);
            up.bind(18, workspace); up.bind(19, branch); up.bind(20, commit);
            up.bind(21, now); up.bind(22, completed);
            up.bind(23, contractRevision); up.bind(24, workflowId); up.exec();
        }
        db->logActivity(lk.token(), "workflow_saved", projectId, title + " [" + phase + "]");
        tx.commit();

        json warnings = json::array();
        if ((phase == "define" || phase == "develop" || phase == "deliver") && objective.empty())
            warnings.push_back("Objective is empty.");
        if ((phase == "develop" || phase == "deliver") && minimum.empty())
            warnings.push_back("Minimum success contract is empty.");
        if ((phase == "develop" || phase == "deliver") && validation == "[]")
            warnings.push_back("Validation criteria are empty.");
        if (phase == "deliver") {
            SQLite::Statement evidence(db->raw(lk.token()),
                "SELECT COUNT(*) FROM verification_evidence WHERE workflow_id=?");
            evidence.bind(1, workflowId); evidence.executeStep();
            if (evidence.getColumn(0).getInt64() == 0)
                warnings.push_back("Deliver phase has no verification evidence.");
        }
        return json{{"ok", true}, {"id", workflowId}, {"phase", phase},
                    {"status", status}, {"contract_revision", contractRevision},
                    {"contract_changed", contractChanged}, {"warnings", warnings}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json setWorkflowStatus(Db* db, long long workflowId, const std::string& value) {
    auto lk = db->guard();
    std::string status = toLower(trim(value));
    if (!choice(status, {"active", "blocked", "verification", "complete", "cancelled"}))
        return fail("invalid workflow status");
    SQLite::Statement current(db->raw(lk.token()),
        "SELECT status,completed_at FROM workflow_runs WHERE id=?");
    current.bind(1, workflowId);
    if (!current.executeStep()) return fail("workflow not found", "not_found");
    const std::string oldStatus = current.getColumn(0).getString();
    const std::string oldCompletedAt = current.getColumn(1).getString();
    if (status == "complete") {
        std::string gate = workflowCompletionErrorLocked(lk.token(), db, workflowId);
        if (!gate.empty()) return fail(gate);
    }
    std::string now = nowIsoUtc();
    std::string completed;
    if (status == "complete") {
        completed = oldStatus == "complete" ? oldCompletedAt : now;
        if (completed.empty()) completed = now;
    }
    SQLite::Statement up(db->raw(lk.token()),
        "UPDATE workflow_runs SET status=?,updated_at=?,completed_at=? WHERE id=?");
    up.bind(1, status); up.bind(2, now); up.bind(3, completed);
    up.bind(4, workflowId); up.exec();
    return json{{"ok", true}, {"id", workflowId}, {"status", status}};
}

json addWorkflowArtifact(Db* db, long long workflowId, const json& body) {
    auto lk = db->guard();
    try {
        if (!rowExists(db->raw(lk.token()), "workflow_runs", workflowId))
            return fail("workflow not found", "not_found");
        std::string phase = toLower(trim(textField(body, "phase")));
        std::string type = toLower(trim(textField(body, "artifact_type", "checkpoint")));
        std::string title = trim(textField(body, "title"));
        std::string content = textField(body, "content");
        std::string data = jsonField(body, "data", "{}");
        long long nodeId = intField(body, "knowledge_node_id");
        long long supersedes = intField(body, "supersedes_artifact_id");
        if (phase.empty()) {
            SQLite::Statement q(db->raw(lk.token()), "SELECT phase FROM workflow_runs WHERE id=?");
            q.bind(1, workflowId); q.executeStep(); phase = q.getColumn(0).getString();
        }
        if (title.empty()) title = type;
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO workflow_artifacts(workflow_id,phase,artifact_type,title,content,data_json,
 knowledge_node_id,supersedes_artifact_id,created_at) VALUES(?,?,?,?,?,?,?,?,?))sql");
        ins.bind(1, workflowId); ins.bind(2, phase); ins.bind(3, type);
        ins.bind(4, title); ins.bind(5, content); ins.bind(6, data);
        if (nodeId > 0) ins.bind(7, nodeId); else ins.bind(7);
        if (supersedes > 0) ins.bind(8, supersedes); else ins.bind(8);
        ins.bind(9, nowIsoUtc()); ins.exec();
        long long id = db->raw(lk.token()).getLastInsertRowid();
        SQLite::Statement touch(db->raw(lk.token()), "UPDATE workflow_runs SET updated_at=? WHERE id=?");
        touch.bind(1, nowIsoUtc()); touch.bind(2, workflowId); touch.exec();
        return json{{"ok", true}, {"id", id}, {"workflow_id", workflowId}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json addVerificationEvidence(Db* db, long long workflowId, const json& body) {
    auto lk = db->guard();
    try {
        SQLite::Statement workflow(db->raw(lk.token()),
            "SELECT status,contract_revision FROM workflow_runs WHERE id=?");
        workflow.bind(1, workflowId);
        if (!workflow.executeStep())
            return fail("workflow not found", "not_found");
        const std::string workflowStatus = workflow.getColumn(0).getString();
        const long long contractRevision = workflow.getColumn(1).getInt64();
        if (workflowStatus == "complete" || workflowStatus == "cancelled")
            return fail("closed workflows cannot accept evidence; reopen or create a successor first");
        std::string status = toLower(trim(textField(body, "status", "not_run")));
        if (!choice(status, {"pass", "warning", "fail", "not_run"}))
            return fail("invalid evidence status");
        std::string criterion = trim(textField(body, "criterion"));
        std::string claim = trim(textField(body, "claim"));
        std::string command = trim(textField(body, "check_command", textField(body, "command")));
        std::string started = trim(textField(body, "started_at"));
        std::string finished = trim(textField(body, "finished_at"));
        const bool hasExitCode = body.contains("exit_code") &&
            body["exit_code"].is_number_integer() && !body["exit_code"].is_boolean();
        const bool hasFailureCount = body.contains("failure_count") &&
            body["failure_count"].is_number_integer() &&
            !body["failure_count"].is_boolean();
        const long long exitCode = intField(body, "exit_code", -1);
        const long long failureCount = intField(body, "failure_count");
        if (criterion.empty()) return fail("evidence criterion is required");
        if (claim.empty()) return fail("evidence claim is required");
        if (status == "pass" && (command.empty() || started.empty() || finished.empty() ||
            !hasExitCode || !hasFailureCount || exitCode != 0 || failureCount != 0))
            return fail("passing evidence requires command, start/finish times, integer exit_code 0, and integer failure_count 0");
        if (status == "pass") {
            SQLite::Statement validTimes(db->raw(lk.token()), R"sql(
SELECT CASE WHEN datetime(?) IS NOT NULL AND datetime(?) IS NOT NULL
 AND datetime(?)<=datetime(?) AND datetime(?)<=datetime('now','+5 minutes')
 THEN 1 ELSE 0 END)sql");
            validTimes.bind(1, started); validTimes.bind(2, finished);
            validTimes.bind(3, started); validTimes.bind(4, finished);
            validTimes.bind(5, finished); validTimes.executeStep();
            if (validTimes.getColumn(0).getInt() != 1)
                return fail("passing evidence requires valid ordered timestamps and cannot finish in the future");
        }
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO verification_evidence(workflow_id,criterion,claim,check_command,
 environment,started_at,finished_at,exit_code,failure_count,status,output_excerpt,
 artifact_uri,commit_hash,contract_revision,created_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql");
        ins.bind(1, workflowId); ins.bind(2, criterion);
        ins.bind(3, claim); ins.bind(4, command);
        ins.bind(5, textField(body, "environment")); ins.bind(6, started);
        ins.bind(7, finished);
        if (hasExitCode)
            ins.bind(8, static_cast<int>(exitCode));
        else ins.bind(8);
        ins.bind(9, static_cast<int>(failureCount));
        ins.bind(10, status); ins.bind(11, clip(textField(body, "output_excerpt"), 8000));
        ins.bind(12, textField(body, "artifact_uri")); ins.bind(13, textField(body, "commit_hash"));
        ins.bind(14, contractRevision); ins.bind(15, nowIsoUtc()); ins.exec();
        long long id = db->raw(lk.token()).getLastInsertRowid();
        SQLite::Statement touch(db->raw(lk.token()), "UPDATE workflow_runs SET updated_at=? WHERE id=?");
        touch.bind(1, nowIsoUtc()); touch.bind(2, workflowId); touch.exec();
        return json{{"ok", true}, {"id", id}, {"workflow_id", workflowId},
                    {"contract_revision", contractRevision}, {"status", status}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

std::string buildWorkflowResume(Db* db, long long projectId,
                                long long exactWorkflowId) {
    auto packetGuard = db->guard();
    PacketTarget target = resolvePacketTarget(
        db, projectId, exactWorkflowId, exactWorkflowId <= 0);
    if (projectId > 0 && target.projectId != projectId) return {};
    if (exactWorkflowId > 0 && target.workflowId != exactWorkflowId) return {};

    const long long workflowId = target.workflowId;
    json w = workflowId > 0 ? getWorkflow(db, workflowId) : json::object();
    const std::string workflowStatus = w.value("status", "");
    const long long currentContractRevision =
        w.value("contract_revision", 1LL);
    const bool closedWorkflow = workflowStatus == "complete" ||
                                workflowStatus == "cancelled";
    if (closedWorkflow) {
        target.currentStep = "Historical closed workflow; inspect only.";
        target.nextAction =
            "Do not continue this workflow unless the live user explicitly reopens it or creates a successor.";
    }
    std::string workflowProject = w.value("project_name", "");
    if (workflowProject.empty()) workflowProject = "cross-project";
    std::ostringstream payload;
    if (workflowId <= 0) {
        payload << "_No active workflow matched the requested scope. Stop instead of guessing a continuation._\n";
    } else {
    payload << "## W" << workflowId << " - " << w.value("title", "") << "\n\n"
        << "- Project: " << workflowProject << "\n"
        << "- Context/phase/status: " << w.value("context", "") << " / "
        << w.value("phase", "") << " / " << w.value("status", "") << "\n"
        << "- Workspace: " << w.value("workspace", "") << "\n"
        << "- Branch/commit: " << w.value("branch", "") << " / "
        << w.value("commit_hash", "") << "\n"
        << "- Updated: " << w.value("updated_at", "") << "\n\n"
        << "### Full intent contract\n\n"
        << "Objective: " << w.value("objective", "") << "\n\n"
        << "Minimum success: " << w.value("minimum_success", "") << "\n\n"
        << "Exceptional success: " << w.value("exceptional_success", "") << "\n\n"
        << "Boundaries: " << w.value("boundaries_json", "[]") << "\n\n"
        << "Stakeholders: " << w.value("stakeholders_json", "[]") << "\n\n"
        << "Existing assets: " << w.value("existing_assets_json", "[]") << "\n\n"
        << "Constraints: " << w.value("constraints_json", "[]") << "\n\n"
        << "Validation: " << w.value("validation_json", "[]") << "\n\n"
        << "Current contract revision: " << currentContractRevision << "\n\n"
        << "### Current state\n\n"
        << "- Current step: " << w.value("current_step", "") << "\n"
        << "- Next action: " << w.value("next_action", "") << "\n"
        << "- Blockers: " << w.value("blockers_json", "[]") << "\n"
        << "- Changed files: " << w.value("changed_files_json", "[]") << "\n\n";
    payload << "### Recent artifacts\n\n";
    if (w.contains("artifacts")) {
        size_t start = w["artifacts"].size() > 12 ? w["artifacts"].size() - 12 : 0;
        for (size_t i = start; i < w["artifacts"].size(); ++i) {
            const auto& a = w["artifacts"][i];
            payload << "- A" << a["id"] << " [" << a.value("phase", "") << "/"
                    << a.value("artifact_type", "") << "] " << a.value("title", "")
                    << " - " << a.value("created_at", "") << "\n";
            if (!a.value("content", "").empty())
                payload << "  " << clip(a.value("content", ""), 1200) << "\n";
        }
    }
    payload << "\n### Verification evidence\n\n";
    if (w.contains("evidence") && !w["evidence"].empty()) {
        size_t start = w["evidence"].size() > 20 ? w["evidence"].size() - 20 : 0;
        for (size_t i = start; i < w["evidence"].size(); ++i) {
            const auto& e = w["evidence"][i];
            const long long exitCode = e.contains("exit_code") &&
                e["exit_code"].is_number_integer() ? e["exit_code"].get<long long>() : -1;
            const long long evidenceRevision = e.value("contract_revision", 1LL);
            payload << "- E" << e["id"] << " [" << e.value("status", "") << "] "
                    << e.value("criterion", "") << " - " << e.value("claim", "")
                    << " (contract rev " << evidenceRevision;
            if (evidenceRevision != currentContractRevision)
                payload << "; historical prior revision";
            payload << ")\n"
                    << "  Command/environment: " << e.value("check_command", "")
                    << " / " << e.value("environment", "") << "\n"
                    << "  Started/finished: " << e.value("started_at", "")
                    << " / " << e.value("finished_at", "")
                    << "; exit " << exitCode
                    << "; failures " << e.value("failure_count", 0) << "\n";
            if (!e.value("output_excerpt", "").empty())
                payload << "  Output: " << clip(e.value("output_excerpt", ""), 1200) << "\n";
            if (!e.value("artifact_uri", "").empty())
                payload << "  Artifact: " << e.value("artifact_uri", "") << "\n";
            if (!e.value("commit_hash", "").empty())
                payload << "  Commit: " << e.value("commit_hash", "") << "\n";
        }
    } else payload << "_No verification evidence recorded._\n";
    }

    PacketRenderRequest request;
    request.kind = PacketKind::Resume;
    request.target = std::move(target);
    request.payloadMarkdown = payload.str();
    request.sourceMaterial = request.payloadMarkdown;
    request.generatedAt = nowIsoUtc();
    request.scopeNotes = {
        exactWorkflowId > 0
            ? "The packet is pinned to exact workflow W" + std::to_string(exactWorkflowId) + "."
            : "The packet selected the highest-priority active workflow in the requested scope.",
        "Artifacts are capped at 12 and evidence records at 20; truncation retains the newest records.",
        closedWorkflow
            ? "The exact workflow is complete or cancelled and is supplied as historical read-only context."
            : "The workflow remains eligible for continuation after live-state reconciliation."
    };
    request.continuationNotes = closedWorkflow
        ? std::vector<std::string>{
            "Do not resume closed work without an explicit live request to reopen it or start a successor workflow."
          }
        : std::vector<std::string>{
            "Resume from the recorded next action only after reconciling it with the live workspace and user request."
          };
    PacketRenderResult rendered = renderContextPacket(request);
    return rendered.ok ? rendered.markdown : std::string();
}

json listLearnings(Db* db, long long projectId, const std::string& query,
                   int limit) {
    auto lk = db->guard();
    std::string needle = "%" + toLower(trim(query)) + "%";
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT l.*,IFNULL(p.name,'') AS project_name FROM learnings l
LEFT JOIN projects p ON p.id=l.project_id
WHERE l.status='active' AND (?=0 OR l.project_id=? OR l.project_id IS NULL)
 AND (?='%%' OR lower(l.task_type||' '||l.approach||' '||l.outcome||' '||l.lesson||' '||l.file_patterns||' '||l.tags) LIKE ?)
ORDER BY CASE WHEN l.project_id=? THEN 0 ELSE 1 END,l.relevance DESC,l.created_at DESC LIMIT ?)sql");
    q.bind(1, projectId); q.bind(2, projectId); q.bind(3, needle); q.bind(4, needle);
    q.bind(5, projectId); q.bind(6, std::min(boundLimit(limit, 20), 50));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items}};
}

json saveLearning(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        std::string lesson = trim(textField(body, "lesson"));
        if (lesson.empty()) return fail("lesson is required");
        long long projectId = intField(body, "project_id");
        std::string now = nowIsoUtc();
        SQLite::Statement duplicate(db->raw(lk.token()), R"sql(
SELECT id FROM learnings WHERE IFNULL(project_id,0)=? AND lesson=? AND status='active')sql");
        duplicate.bind(1, projectId); duplicate.bind(2, lesson);
        if (duplicate.executeStep())
            return json{{"ok", true}, {"id", duplicate.getColumn(0).getInt64()}, {"duplicate", true}};
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO learnings(project_id,task_type,approach,outcome,lesson,file_patterns,tags,
 relevance,status,review_after,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?))sql");
        if (projectId > 0) ins.bind(1, projectId); else ins.bind(1);
        ins.bind(2, textField(body, "task_type")); ins.bind(3, textField(body, "approach"));
        ins.bind(4, textField(body, "outcome")); ins.bind(5, lesson);
        ins.bind(6, textField(body, "file_patterns")); ins.bind(7, textField(body, "tags"));
        ins.bind(8, std::max(0.0, std::min(doubleField(body, "relevance", 1.0), 10.0)));
        ins.bind(9, textField(body, "status", "active"));
        ins.bind(10, textField(body, "review_after")); ins.bind(11, now); ins.bind(12, now);
        ins.exec();
        return json{{"ok", true}, {"id", db->raw(lk.token()).getLastInsertRowid()}, {"duplicate", false}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json listProcessingRuns(Db* db, const std::string& kind,
                        const std::string& status, int limit) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT * FROM processing_runs WHERE (?='' OR kind=?) AND (?='' OR status=?)
ORDER BY started_at DESC LIMIT ?)sql");
    q.bind(1, kind); q.bind(2, kind); q.bind(3, status); q.bind(4, status);
    q.bind(5, boundLimit(limit, 100));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items}};
}

json saveProcessingRun(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        long long id = intField(body, "id");
        std::string kind = trim(textField(body, "kind"));
        std::string status = toLower(trim(textField(body, "status", "running")));
        if (kind.empty()) return fail("processing run kind is required");
        if (!choice(status, {"running", "completed", "partial", "failed"}))
            return fail("invalid processing status");
        if (id <= 0) {
            SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO processing_runs(kind,status,input_count,output_count,details_json,error,
 started_at,finished_at) VALUES(?,?,?,?,?,?,?,?))sql");
            ins.bind(1, kind); ins.bind(2, status);
            ins.bind(3, static_cast<int>(intField(body, "input_count")));
            ins.bind(4, static_cast<int>(intField(body, "output_count")));
            ins.bind(5, jsonField(body, "details", "{}")); ins.bind(6, textField(body, "error"));
            ins.bind(7, textField(body, "started_at", nowIsoUtc()));
            ins.bind(8, textField(body, "finished_at", status == "running" ? "" : nowIsoUtc()));
            ins.exec(); id = db->raw(lk.token()).getLastInsertRowid();
        } else {
            if (!rowExists(db->raw(lk.token()), "processing_runs", id))
                return fail("processing run not found", "not_found");
            SQLite::Statement up(db->raw(lk.token()), R"sql(
UPDATE processing_runs SET status=?,input_count=?,output_count=?,details_json=?,error=?,
 finished_at=? WHERE id=?)sql");
            up.bind(1, status); up.bind(2, static_cast<int>(intField(body, "input_count")));
            up.bind(3, static_cast<int>(intField(body, "output_count")));
            up.bind(4, jsonField(body, "details", "{}")); up.bind(5, textField(body, "error"));
            up.bind(6, textField(body, "finished_at", status == "running" ? "" : nowIsoUtc()));
            up.bind(7, id); up.exec();
        }
        return json{{"ok", true}, {"id", id}, {"status", status}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

json listRetrievalEvaluations(Db* db, const std::string& engine, int limit) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        "SELECT * FROM retrieval_evaluations WHERE (?='' OR engine=?) "
        "ORDER BY run_at DESC LIMIT ?");
    q.bind(1, engine); q.bind(2, engine); q.bind(3, boundLimit(limit, 100));
    json items = Db::rowsToJson(q);
    return json{{"ok", true}, {"count", items.size()}, {"items", items}};
}

json saveRetrievalEvaluation(Db* db, const json& body) {
    auto lk = db->guard();
    try {
        std::string query = trim(textField(body, "query"));
        if (query.empty()) return fail("retrieval evaluation query is required");
        int k = std::max(1, std::min(static_cast<int>(intField(body, "k", 10)), 100));
        json expected = body.contains("expected_ids") ? body["expected_ids"] : json::array();
        json actual = body.contains("actual_ids") ? body["actual_ids"] : json::array();
        if (!expected.is_array() || !actual.is_array())
            return fail("expected_ids and actual_ids must be arrays");
        std::set<long long> wanted;
        for (const auto& item : expected) if (item.is_number_integer()) wanted.insert(item.get<long long>());
        // recall@k is measured over distinct ids. A retriever may emit several
        // chunks from one node, but repeated node ids cannot inflate recall.
        std::set<long long> matched;
        int firstRank = 0, rank = 0;
        for (const auto& item : actual) {
            if (!item.is_number_integer()) continue;
            if (++rank > k) break;
            const long long id = item.get<long long>();
            if (wanted.count(id)) {
                matched.insert(id);
                if (firstRank == 0) firstRank = rank;
            }
        }
        double recall = wanted.empty()
                            ? 1.0
                            : static_cast<double>(matched.size()) / wanted.size();
        double rr = firstRank > 0 ? 1.0 / firstRank : 0.0;
        SQLite::Statement ins(db->raw(lk.token()), R"sql(
INSERT INTO retrieval_evaluations(name,query,engine,k,expected_ids_json,
 actual_ids_json,recall,reciprocal_rank,notes,run_at) VALUES(?,?,?,?,?,?,?,?,?,?))sql");
        ins.bind(1, textField(body, "name")); ins.bind(2, query);
        ins.bind(3, textField(body, "engine", "lexical")); ins.bind(4, k);
        ins.bind(5, expected.dump()); ins.bind(6, actual.dump());
        ins.bind(7, recall); ins.bind(8, rr); ins.bind(9, textField(body, "notes"));
        ins.bind(10, textField(body, "run_at", nowIsoUtc())); ins.exec();
        return json{{"ok", true}, {"id", db->raw(lk.token()).getLastInsertRowid()},
                    {"recall_at_k", recall}, {"reciprocal_rank", rr}, {"k", k}};
    } catch (const std::exception& e) { return fail(e.what()); }
}

} // namespace devhub
