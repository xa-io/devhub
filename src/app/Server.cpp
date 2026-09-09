#include "Server.h"
#include "BuildRunner.h"
#include "Db.h"
#include "Gui.h"
#include "KnowledgeOps.h"
#include "Ops.h"
#include "Version.h"
#include "WinUtil.h"
#include "devhub/SuggestionDetector.h"
#include "devhub/Util.h"
#include "devhub/VersionUtil.h"

#include <crow.h>
#include <nlohmann/json.hpp>

#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace devhub {

namespace {

constexpr size_t kMaxJsonBodyBytes = 1 * 1024 * 1024;
constexpr size_t kMaxHttpBodyBytes = 2 * 1024 * 1024;
constexpr char kApiRendezvousSchema[] = "xa-devhub.api-rendezvous/v1";
static_assert(kMaxHttpBodyBytes == CROW_HTTP_BODY_LIMIT,
              "Crow transport cap and application cap must match");

bool constantTimeTokenEqual(const std::string& supplied,
                            const std::string& expected) {
    if (expected.empty() || supplied.size() != expected.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned char>(supplied[i]) ^
                      static_cast<unsigned char>(expected[i]);
    }
    return difference == 0;
}

bool loopbackHostAllowed(std::string host, uint16_t port) {
    host = toLower(trim(host));
    const std::string suffix = ":" + std::to_string(port);
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" ||
           host == "127.0.0.1" + suffix || host == "localhost" + suffix ||
           host == "[::1]" + suffix;
}

void endJsonError(crow::response& response, int status,
                  const char* message) {
    response.code = status;
    response.set_header("Content-Type", "application/json");
    const char* code = status == 401 ? "unauthorized"
        : status == 413 ? "payload_too_large" : "invalid_request";
    response.write(
        json{{"ok", false}, {"code", code}, {"error", message}}.dump());
    response.end();
}

struct ApiGuard {
    struct context {};

    std::string token;
    uint16_t port = 0;

    void before_handle(crow::request& request, crow::response& response,
                       context&) {
        if (!loopbackHostAllowed(request.get_header_value("Host"), port)) {
            endJsonError(response, 400, "unexpected Host header");
            return;
        }
        if (!constantTimeTokenEqual(
                request.get_header_value("X-DevHub-Token"), token)) {
            endJsonError(response, 401,
                         "missing or invalid X-DevHub-Token");
            return;
        }
        // The project-owned Crow overlay rejects oversized Content-Length and
        // chunked bodies in the parser before appending beyond 2 MiB. Retain
        // this application check as defense in depth and apply the stricter
        // JSON cap below.
        if (request.body.size() > kMaxHttpBodyBytes)
            endJsonError(response, 413, "request body exceeds 2 MiB");
    }

    void after_handle(crow::request&, crow::response&, context&) {}
};

std::string makeApiToken() {
    std::array<unsigned char, 32> raw{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr, raw.data(), static_cast<ULONG>(raw.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0)
        throw std::runtime_error("could not generate the API token");
    static constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(raw.size() * 2);
    for (const unsigned char byte : raw) {
        token.push_back(hex[byte >> 4]);
        token.push_back(hex[byte & 0x0f]);
    }
    return token;
}

std::wstring currentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("could not open the current process token");

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        throw std::runtime_error("could not size the current user identity");
    }
    std::vector<unsigned char> storage(needed);
    if (!GetTokenInformation(token, TokenUser, storage.data(), needed,
                             &needed)) {
        CloseHandle(token);
        throw std::runtime_error("could not read the current user identity");
    }
    CloseHandle(token);

    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidText))
        throw std::runtime_error("could not encode the current user identity");
    std::wstring result(sidText);
    LocalFree(sidText);
    return result;
}

std::string canonicalLoopbackOrigin(uint16_t port) {
    if (port == 0) throw std::runtime_error("API port must be positive");
    return "http://127.0.0.1:" + std::to_string(port);
}

std::filesystem::path localApiRendezvousRoot() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr)
        throw std::runtime_error("could not resolve LocalAppData for API discovery");
    const std::filesystem::path root(raw);
    CoTaskMemFree(raw);
    return root / L"XA DevHub" / L"api";
}

json apiRendezvousRecord(const std::string& token, uint16_t port) {
    return json{
        {"schema", kApiRendezvousSchema},
        {"origin", canonicalLoopbackOrigin(port)},
        {"pid", static_cast<unsigned long long>(GetCurrentProcessId())},
        {"token", token},
    };
}

json readApiRendezvous(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("could not reopen the API rendezvous");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 4096) {
        CloseHandle(file);
        throw std::runtime_error("API rendezvous size is invalid");
    }
    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, contents.data(),
                             static_cast<DWORD>(contents.size()), &read,
                             nullptr) &&
                    read == contents.size();
    CloseHandle(file);
    if (!ok) throw std::runtime_error("could not read back the API rendezvous");
    try {
        return json::parse(contents);
    } catch (const std::exception&) {
        throw std::runtime_error("API rendezvous JSON is invalid");
    }
}

void verifyApiRendezvous(const std::wstring& path, const std::string& token,
                         uint16_t port) {
    const json record = readApiRendezvous(path);
    if (!record.is_object() ||
        record.value("schema", "") != kApiRendezvousSchema ||
        record.value("origin", "") != canonicalLoopbackOrigin(port) ||
        record.value("pid", 0ULL) !=
            static_cast<unsigned long long>(GetCurrentProcessId()) ||
        !constantTimeTokenEqual(record.value("token", ""), token))
        throw std::runtime_error("API rendezvous verification failed");
}

void writeCurrentUserOnlyRendezvous(const std::string& pathUtf8,
                                    const std::string& token, uint16_t port) {
    const std::filesystem::path destination = widen(pathUtf8);
    std::error_code directoryError;
    std::filesystem::create_directories(destination.parent_path(), directoryError);
    if (directoryError)
        throw std::runtime_error("could not create the local API rendezvous directory");

    const std::wstring path = destination.wstring();
    const std::wstring temporary = path + L".tmp-" +
        std::to_wstring(GetCurrentProcessId());
    const DWORD temporaryAttributes = GetFileAttributesW(temporary.c_str());
    if (temporaryAttributes != INVALID_FILE_ATTRIBUTES &&
        !DeleteFileW(temporary.c_str()))
        throw std::runtime_error("could not clear a stale API rendezvous temporary file");

    const std::wstring descriptorText =
        L"D:P(A;;FA;;;" + currentUserSidString() + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptorText.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("could not secure the API rendezvous");

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("could not create the API rendezvous");

    const std::string contents = apiRendezvousRecord(token, port).dump() + "\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, contents.data(),
                              static_cast<DWORD>(contents.size()), &written,
                              nullptr) &&
                    written == contents.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("could not write the API rendezvous");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("could not publish the API rendezvous atomically");
    }
    try {
        verifyApiRendezvous(path, token, port);
    } catch (...) {
        DeleteFileW(path.c_str());
        throw;
    }
}

HANDLE acquireApiPortLease(uint16_t port) {
    if (port == 0) throw std::runtime_error("API port must be positive");
    const std::wstring name = L"Global\\XADevHub.ApiPort.v1." +
        std::to_wstring(port);
    SetLastError(ERROR_SUCCESS);
    HANDLE lease = CreateMutexW(nullptr, FALSE, name.c_str());
    const DWORD error = GetLastError();
    if (lease == nullptr) {
        if (error == ERROR_ACCESS_DENIED) return nullptr;
        throw std::runtime_error("could not acquire the API port lease");
    }
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(lease);
        return nullptr;
    }
    return lease;
}

void releaseApiPortLease(HANDLE& lease) {
    if (lease == nullptr || lease == INVALID_HANDLE_VALUE) return;
    CloseHandle(lease);
    lease = nullptr;
}

void removeApiRendezvousIfOwned(const std::string& pathUtf8,
                                const std::string& token, uint16_t port) {
    if (pathUtf8.empty()) return;
    try {
        const std::wstring path = widen(pathUtf8);
        const json record = readApiRendezvous(path);
        if (record.value("origin", "") == canonicalLoopbackOrigin(port) &&
            record.value("pid", 0ULL) ==
                static_cast<unsigned long long>(GetCurrentProcessId()) &&
            constantTimeTokenEqual(record.value("token", ""), token))
            DeleteFileW(path.c_str());
    } catch (...) {
        // A missing, stale, or replacement record belongs to no cleanup path
        // here. Never delete a rendezvous unless this instance proves ownership.
    }
}

} // namespace

std::string apiRendezvousPath(uint16_t port,
                              const std::string& rendezvousRootOverride) {
    const std::filesystem::path root = rendezvousRootOverride.empty()
        ? localApiRendezvousRoot()
        : std::filesystem::path(widen(rendezvousRootOverride));
    const std::filesystem::path path =
        root / ("api-token-v1-" + std::to_string(port) + ".json");
    const std::u8string pathUtf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(pathUtf8.data()),
                       pathUtf8.size());
}

struct Server::Impl {
    crow::App<ApiGuard> app;
    std::string token;
    HANDLE portLease = nullptr;
};

Server::Server(Db* db, BuildRunner* builds, uint16_t port,
               const std::string& rendezvousRootOverride)
    : impl_(std::make_unique<Impl>()), db_(db), builds_(builds), port_(port),
      rendezvousRootOverride_(rendezvousRootOverride) {
    impl_->token = makeApiToken();
    auto& guard = impl_->app.get_middleware<ApiGuard>();
    guard.token = impl_->token;
    guard.port = port_;
}

Server::~Server() { stop(); }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static crow::response jsonRes(const json& j, int code = 200) {
    crow::response res(code, j.dump());
    res.set_header("Content-Type", "application/json");
    return res;
}

static crow::response errRes(int code, const std::string& msg) {
    const char* stableCode = code == 401 ? "unauthorized"
        : code == 403 ? "forbidden"
        : code == 404 ? "not_found"
        : code == 409 ? "conflict"
        : code == 413 ? "payload_too_large"
        : code >= 500 ? "internal_error" : "invalid_request";
    return jsonRes(json{{"ok", false}, {"code", stableCode},
                        {"error", msg}}, code);
}

static crow::response internalErrRes(int status, const char* code,
                                     const std::string& detail) {
    appLog(std::string("[api] ") + code + ": " +
           redactHttpUrls(detail));
    return jsonRes(json{{"ok", false}, {"code", code}}, status);
}

static bool jsonRequest(const crow::request& req, json& body,
                        crow::response& rejected) {
    if (!req.get_header_value("Origin").empty()) {
        rejected = errRes(400, "cross-origin requests are not accepted");
        return false;
    }
    if (req.body.size() > kMaxJsonBodyBytes) {
        rejected = errRes(413, "request body exceeds 1 MiB");
        return false;
    }
    if (!req.body.empty()) {
        std::string contentType =
            toLower(trim(req.get_header_value("Content-Type")));
        const size_t separator = contentType.find(';');
        if (separator != std::string::npos)
            contentType = trim(contentType.substr(0, separator));
        if (contentType != "application/json") {
            rejected = errRes(400, "Content-Type must be application/json");
            return false;
        }
    }
    body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        body = json::object();
        rejected = errRes(400, "invalid JSON object");
        return false;
    }
    return true;
}

static constexpr size_t kMaxSettingValueBytes = 64 * 1024;

static bool apiWritableSettingKey(const std::string& key) {
    static const std::set<std::string> writable = {
        "stale_days", "release_draft_days", "backup_retention",
        "dashboard_completed_days", "show_log_panel",
        "leaderboard_excluded_source_ids", "app_display_name",
        // Detector automation remains supported, but only through the exact
        // parser used by runtime ingest. Identity and Discord routing settings
        // remain native-UI only.
        "detection_patterns"
    };
    if (writable.find(key) != writable.end()) return true;
    if (key.size() <= 64 && key.rfind("ui_sec_", 0) == 0 &&
        key.size() > std::string("ui_sec_").size()) {
        return std::all_of(key.begin() + 7, key.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_';
        });
    }
    return false;
}

static bool normalizeBoundedInteger(const std::string& value, int minimum,
                                    int maximum, std::string& normalized) {
    const std::string text = trim(value);
    if (text.empty()) return false;
    int parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed < minimum || parsed > maximum)
        return false;
    normalized = std::to_string(parsed);
    return true;
}

static bool normalizeSettingValue(const std::string& key, const json& value,
                                  std::string& normalized,
                                  std::string& error) {
    normalized = value.is_string() ? value.get<std::string>() : value.dump();
    if (normalized.size() > kMaxSettingValueBytes) {
        error = "setting '" + key + "' value is too large";
        return false;
    }
    if (normalized == "[redacted]") {
        error = "the [redacted] sentinel cannot be written";
        return false;
    }

    if (key == "stale_days" || key == "release_draft_days") {
        if (!normalizeBoundedInteger(normalized, 1, 3650, normalized)) {
            error = key + " must be an integer from 1 to 3650";
            return false;
        }
        return true;
    }
    if (key == "backup_retention") {
        if (!normalizeBoundedInteger(normalized, 3, 90, normalized)) {
            error = "backup_retention must be an integer from 3 to 90";
            return false;
        }
        return true;
    }
    if (key == "dashboard_completed_days") {
        std::string canonical;
        if (!normalizeBoundedInteger(normalized, 7, 365, canonical) ||
            (canonical != "7" && canonical != "30" && canonical != "60" &&
             canonical != "90" && canonical != "180" && canonical != "365")) {
            error = "dashboard_completed_days must be 7, 30, 60, 90, 180, or 365";
            return false;
        }
        normalized = std::move(canonical);
        return true;
    }
    if (key == "show_log_panel" || key.rfind("ui_sec_", 0) == 0) {
        if (normalized != "0" && normalized != "1") {
            error = key + " must be 0 or 1";
            return false;
        }
        return true;
    }
    if (key == "leaderboard_excluded_source_ids") {
        std::vector<long long> ids;
        if (!parseLeaderboardExcludedSourceIds(normalized, ids, &error))
            return false;
        std::ostringstream canonical;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) canonical << ',';
            canonical << ids[i];
        }
        normalized = canonical.str();
        return true;
    }
    if (key == "app_display_name") {
        std::string displayName;
        if (!normalizeAppDisplayName(normalized, displayName, &error))
            return false;
        normalized = std::move(displayName);
        return true;
    }
    if (key != "detection_patterns" || trim(normalized).empty()) return true;

    const json parsed = json::parse(normalized, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
        error = "detection_patterns must be a JSON array";
        return false;
    }
    const auto patterns = SuggestionDetector::patternsFromJson(parsed);
    if (patterns.empty() || patterns.size() != parsed.size()) {
        error = "detection_patterns contains an invalid pattern";
        return false;
    }
    normalized = SuggestionDetector::patternsToJson(patterns).dump();
    return true;
}

static crow::response opRes(const json& result, int successCode = 200) {
    if (result.value("ok", true)) return jsonRes(result, successCode);
    const std::string detail = result.value("error", "operation failed");
    const std::string code = result.value("code", "invalid_request");
    const int status = code == "not_found" ? 404
        : (code == "duplicate" || code == "conflict") ? 409
        : code == "internal_error" ? 500 : 400;
    return internalErrRes(status, code.c_str(), detail);
}

static std::string queryText(const crow::request& req, const char* name) {
    if (auto value = req.url_params.get(name)) return value;
    return {};
}

static bool queryPositiveId(const crow::request& req, const char* name,
                            bool required, long long& value,
                            std::string& error) {
    value = 0;
    const char* raw = req.url_params.get(name);
    if (!raw) {
        if (required) error = std::string(name) + " is required";
        return !required;
    }
    try {
        std::string text(raw);
        size_t used = 0;
        value = std::stoll(text, &used);
        if (used != text.size() || value <= 0) throw std::invalid_argument("id");
        return true;
    } catch (...) {
        error = std::string(name) + " must be a positive integer";
        value = 0;
        return false;
    }
}

static bool queryBoundedInt(const crow::request& req, const char* name,
                            int fallback, int minimum, int maximum,
                            int& value, std::string& error) {
    value = fallback;
    const char* raw = req.url_params.get(name);
    if (!raw) return true;
    try {
        std::string text(raw);
        size_t used = 0;
        const long long parsed = std::stoll(text, &used);
        if (used != text.size() || parsed < minimum || parsed > maximum)
            throw std::invalid_argument("bounded integer");
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        error = std::string(name) + " must be an integer from " +
                std::to_string(minimum) + " to " + std::to_string(maximum);
        return false;
    }
}

static bool queryPositiveInt(const crow::request& req, const char* name,
                             int fallback, int maximum, int& value,
                             std::string& error) {
    return queryBoundedInt(req, name, fallback, 1, maximum, value, error);
}

static bool queryNonNegativeSize(const crow::request& req, const char* name,
                                 size_t fallback, size_t& value,
                                 std::string& error) {
    value = fallback;
    const char* raw = req.url_params.get(name);
    if (!raw) return true;
    try {
        const std::string text(raw);
        if (text.empty() || text.front() == '-')
            throw std::invalid_argument("non-negative size");
        size_t used = 0;
        const unsigned long long parsed = std::stoull(text, &used);
        if (used != text.size() ||
            parsed > static_cast<unsigned long long>(
                         std::numeric_limits<size_t>::max()))
            throw std::out_of_range("non-negative size");
        value = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        error = std::string(name) + " must be a non-negative integer";
        value = fallback;
        return false;
    }
}

static crow::response markdownRes(const std::string& markdown) {
    crow::response response(200, markdown);
    response.set_header("Content-Type", "text/markdown; charset=utf-8");
    return response;
}

static bool recordExists(Db* db, const char* table, long long id) {
    auto lk = db->guard();
    SQLite::Statement q(db->raw(lk.token()),
        std::string("SELECT 1 FROM ") + table + " WHERE id=?");
    q.bind(1, id);
    return q.executeStep();
}

static bool packetInputsWithinBudget(Db* db, long long projectId,
                                     std::string& reason) {
    constexpr long long kMaxSkillsBytes = 64 * 1024;
    constexpr long long kMaxPacketInputBytes = 1 * 1024 * 1024;
    auto lock = db->guard();
    SQLite::Statement query(db->raw(lock.token()), R"sql(
SELECT
 length(CAST(IFNULL(p.codex_skills,'') AS BLOB)) AS skill_bytes,
 length(CAST(IFNULL(p.name,'') || IFNULL(p.path,'') ||
             IFNULL(p.rules_path,'') AS BLOB)) AS project_bytes,
 COALESCE((SELECT SUM(bytes) FROM (
   SELECT length(CAST(IFNULL(title,'') ||
          substr(IFNULL(body,''),1,4000) ||
          substr(IFNULL(blocked_reason,''),1,800) AS BLOB)) AS bytes
   FROM items WHERE project_id=? AND status IN ('open','in_progress','blocked')
   ORDER BY priority DESC,updated_at DESC,id DESC LIMIT 200)),0) AS active_bytes,
 COALESCE((SELECT SUM(bytes) FROM (
   SELECT length(CAST(IFNULL(title,'') AS BLOB)) AS bytes
   FROM items WHERE project_id=? AND status='completed'
   ORDER BY completed_at DESC,id DESC LIMIT 400)),0) AS completed_bytes,
 COALESCE((SELECT SUM(length(CAST(IFNULL(s.name,'') AS BLOB)))
   FROM sources s WHERE EXISTS (
     SELECT 1 FROM item_sources x JOIN items i ON i.id=x.item_id
     WHERE x.source_id=s.id AND i.project_id=?)),0) AS contributor_bytes,
 COALESCE((SELECT length(CAST(
     IFNULL(title,'') || IFNULL(context,'') || IFNULL(objective,'') ||
     IFNULL(minimum_success,'') || IFNULL(validation_json,'') ||
     IFNULL(current_step,'') || IFNULL(next_action,'') ||
     IFNULL(blockers_json,'') || IFNULL(workspace,'') AS BLOB))
   FROM workflow_runs WHERE project_id=?
     AND status IN ('active','blocked','verification')
   ORDER BY updated_at DESC,id DESC LIMIT 1),0) AS workflow_bytes
FROM projects p WHERE p.id=?)sql");
    query.bind(1, projectId);
    query.bind(2, projectId);
    query.bind(3, projectId);
    query.bind(4, projectId);
    query.bind(5, projectId);
    if (!query.executeStep()) {
        reason = "project not found";
        return false;
    }
    const long long skillBytes = query.getColumn(0).getInt64();
    if (skillBytes < 0 || skillBytes > kMaxSkillsBytes) {
        reason = "project skill routing exceeds the 64 KiB packet budget";
        return false;
    }
    long long totalBytes = skillBytes;
    for (int column = 1; column <= 5; ++column) {
        const long long value = query.getColumn(column).getInt64();
        if (value < 0 || totalBytes > kMaxPacketInputBytes - value) {
            reason = "packet inputs exceed the 1 MiB render budget";
            return false;
        }
        totalBytes += value;
    }
    if (totalBytes > kMaxPacketInputBytes) {
        reason = "packet inputs exceed the 1 MiB render budget";
        return false;
    }
    return true;
}

// Exact work-item readback for status automation. Unlike /api/work, this
// includes terminal items so a completed mutation can be verified by ID.
// The caller owns the database guard.
static json getWorkItemLocked(Db::Held held, Db* db, long long itemId) {
    SQLite::Statement q(db->raw(held), R"sql(
SELECT i.id,i.project_id,p.name AS project_name,i.type,i.title,i.body,i.status,
 i.priority,i.due_date,i.review_date,i.blocked_reason,i.created_at,i.updated_at,
 i.completed_at,COALESCE(i.source_id,0) AS source_id,i.credited,i.origin,i.tags,
 IFNULL(GROUP_CONCAT(s.name, ', '),'') AS contributors,
 (SELECT COUNT(*) FROM item_sources sx WHERE sx.item_id=i.id) AS contributor_link_count,
 (SELECT COUNT(*) FROM events e WHERE e.item_id=i.id) AS event_count,
 (SELECT COUNT(*) FROM discord_messages m WHERE m.item_id=i.id) AS discord_message_count,
 (SELECT COUNT(*) FROM discord_notify_cards n WHERE n.item_id=i.id) AS notification_card_count,
 (SELECT COUNT(*) FROM ticket_attachments a WHERE a.item_id=i.id) AS attachment_count,
 (SELECT COUNT(*) FROM discord_notify_failure_dismissals d
   JOIN discord_notify_cards dn ON dn.id=d.card_row_id
   WHERE dn.item_id=i.id) AS notification_acknowledgement_count,
 COALESCE((SELECT target_item_id FROM item_merges m
   WHERE m.source_item_id=i.id),0) AS merged_into_id,
 (SELECT COUNT(*) FROM item_merges m
   WHERE m.target_item_id=i.id) AS merged_source_count
FROM items i JOIN projects p ON p.id=i.project_id
LEFT JOIN item_sources x ON x.item_id=i.id LEFT JOIN sources s ON s.id=x.source_id
WHERE i.id=? GROUP BY i.id)sql");
    q.bind(1, itemId);
    if (!q.executeStep())
        return json{{"ok", false}, {"code", "not_found"},
                    {"error", "work item not found"}};
    json result = Db::rowToJson(q);
    result["ok"] = true;
    return result;
}

struct ProjectSaveInput {
    std::string name;
    std::string description;
    std::string path;
    std::string rulesPath;
    std::string codexSkills;
    std::string buildCommand;
    std::string prepCommand;
    std::string releaseCommand;
    std::string versionCommand;
    std::string buildCwd;
    std::string localVersionFile;
    std::string remoteVersionUrl;
    std::string remoteVersionKey;
    std::string githubUrl;
    std::string aliases;
    bool discordTickets = true;
};

static bool projectStringField(const json& body, const char* field,
                               size_t maximum, std::string& value,
                               std::string& error) {
    if (!body.contains(field)) {
        error = std::string("project payload is missing '") + field + "'";
        return false;
    }
    const json& candidate = body[field];
    if (!candidate.is_string()) {
        error = std::string("project field '") + field + "' must be a string";
        return false;
    }
    value = candidate.get<std::string>();
    if (value.size() > maximum) {
        error = std::string("project field '") + field + "' is too large";
        return false;
    }
    if (value.find('\0') != std::string::npos) {
        error = std::string("project field '") + field +
                "' contains a null character";
        return false;
    }
    return true;
}

static bool parseProjectSaveInput(const json& body, ProjectSaveInput& input,
                                  std::string& error) {
    static const std::set<std::string> allowed = {
        "name", "description", "path", "rules_path", "codex_skills",
        "build_command", "prep_command", "release_command",
        "version_command", "build_cwd", "local_version_file",
        "remote_version_url", "remote_version_key", "github_url", "aliases",
        "discord_tickets"
    };
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "unexpected project field '" + it.key() + "'";
            return false;
        }
    }
    if (body.size() != allowed.size()) {
        error = "project save requires every editable project field";
        return false;
    }

    constexpr size_t kMaxProjectNameBytes = 255;
    constexpr size_t kMaxProjectFieldBytes = 16 * 1024;
    if (!projectStringField(body, "name", kMaxProjectNameBytes,
                            input.name, error) ||
        !projectStringField(body, "description", kMaxProjectFieldBytes,
                            input.description, error) ||
        !projectStringField(body, "path", kMaxProjectFieldBytes,
                            input.path, error) ||
        !projectStringField(body, "rules_path", kMaxProjectFieldBytes,
                            input.rulesPath, error) ||
        !projectStringField(body, "codex_skills", kMaxProjectFieldBytes,
                            input.codexSkills, error) ||
        !projectStringField(body, "build_command", kMaxProjectFieldBytes,
                            input.buildCommand, error) ||
        !projectStringField(body, "prep_command", kMaxProjectFieldBytes,
                            input.prepCommand, error) ||
        !projectStringField(body, "release_command", kMaxProjectFieldBytes,
                            input.releaseCommand, error) ||
        !projectStringField(body, "version_command", kMaxProjectFieldBytes,
                            input.versionCommand, error) ||
        !projectStringField(body, "build_cwd", kMaxProjectFieldBytes,
                            input.buildCwd, error) ||
        !projectStringField(body, "local_version_file",
                            kMaxProjectFieldBytes, input.localVersionFile,
                            error) ||
        !projectStringField(body, "remote_version_url",
                            kMaxProjectFieldBytes, input.remoteVersionUrl,
                            error) ||
        !projectStringField(body, "remote_version_key",
                            kMaxProjectFieldBytes, input.remoteVersionKey,
                            error) ||
        !projectStringField(body, "github_url", kMaxProjectFieldBytes,
                            input.githubUrl, error) ||
        !projectStringField(body, "aliases", kMaxProjectFieldBytes,
                            input.aliases, error))
        return false;

    input.name = trim(input.name);
    if (input.name.empty()) {
        error = "project name cannot be blank";
        return false;
    }
    if (!body["discord_tickets"].is_boolean()) {
        error = "project field 'discord_tickets' must be boolean";
        return false;
    }
    input.discordTickets = body["discord_tickets"].get<bool>();
    return true;
}

static json getProjectLocked(Db::Held held, Db* db, long long projectId) {
    SQLite::Statement q(db->raw(held), R"sql(
SELECT id,name,slug,description,path,rules_path,codex_skills,build_command,
 prep_command,release_command,version_command,build_cwd,local_version_file,
 remote_version_url,remote_version_key,github_url,aliases,discord_tickets,
 created_at,updated_at
FROM projects WHERE id=?)sql");
    q.bind(1, projectId);
    if (!q.executeStep())
        return json{{"ok", false}, {"code", "not_found"},
                    {"error", "project not found"}};
    json result = Db::rowToJson(q);
    result["ok"] = true;
    return result;
}

static crow::response saveProject(Db* db, long long projectId,
                                  const json& body) {
    ProjectSaveInput input;
    std::string error;
    if (!parseProjectSaveInput(body, input, error))
        return errRes(400, error);

    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        const std::string savedAt = nowIsoUtc();
        const bool created = projectId == 0;
        if (created) {
            SQLite::Statement insert(db->raw(lk.token()), R"sql(
INSERT INTO projects(
 name,slug,description,path,rules_path,codex_skills,build_command,prep_command,
 release_command,version_command,build_cwd,local_version_file,
 remote_version_url,remote_version_key,github_url,aliases,discord_tickets,
 created_at,updated_at)
VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?))sql");
            insert.bind(1, input.name);
            insert.bind(2, slugify(input.name));
            insert.bind(3, input.description);
            insert.bind(4, input.path);
            insert.bind(5, input.rulesPath);
            insert.bind(6, input.codexSkills);
            insert.bind(7, input.buildCommand);
            insert.bind(8, input.prepCommand);
            insert.bind(9, input.releaseCommand);
            insert.bind(10, input.versionCommand);
            insert.bind(11, input.buildCwd);
            insert.bind(12, input.localVersionFile);
            insert.bind(13, input.remoteVersionUrl);
            insert.bind(14, input.remoteVersionKey);
            insert.bind(15, input.githubUrl);
            insert.bind(16, input.aliases);
            insert.bind(17, input.discordTickets ? 1 : 0);
            insert.bind(18, savedAt);
            insert.bind(19, savedAt);
            insert.exec();
            projectId = db->raw(lk.token()).getLastInsertRowid();
            db->logActivity(lk.token(), "project_created", projectId,
                            input.name);
        } else {
            SQLite::Statement exists(db->raw(lk.token()),
                "SELECT 1 FROM projects WHERE id=?");
            exists.bind(1, projectId);
            if (!exists.executeStep()) return errRes(404, "project not found");

            SQLite::Statement update(db->raw(lk.token()), R"sql(
UPDATE projects SET
 name=?,description=?,path=?,rules_path=?,codex_skills=?,build_command=?,
 prep_command=?,release_command=?,version_command=?,build_cwd=?,
 local_version_file=?,remote_version_url=?,remote_version_key=?,github_url=?,
 aliases=?,discord_tickets=?,updated_at=?
WHERE id=?)sql");
            update.bind(1, input.name);
            update.bind(2, input.description);
            update.bind(3, input.path);
            update.bind(4, input.rulesPath);
            update.bind(5, input.codexSkills);
            update.bind(6, input.buildCommand);
            update.bind(7, input.prepCommand);
            update.bind(8, input.releaseCommand);
            update.bind(9, input.versionCommand);
            update.bind(10, input.buildCwd);
            update.bind(11, input.localVersionFile);
            update.bind(12, input.remoteVersionUrl);
            update.bind(13, input.remoteVersionKey);
            update.bind(14, input.githubUrl);
            update.bind(15, input.aliases);
            update.bind(16, input.discordTickets ? 1 : 0);
            update.bind(17, savedAt);
            update.bind(18, projectId);
            update.exec();
        }
        tx.commit();
        return jsonRes(json{{"ok", true}, {"id", projectId},
                            {"created", created}}, created ? 201 : 200);
    } catch (const std::exception& e) {
        const std::string detail = e.what();
        if (detail.find("UNIQUE") != std::string::npos)
            return errRes(409, "another project already uses that name");
        return internalErrRes(500, "project_save_failed", detail);
    }
}

struct WorkSaveInput {
    long long projectId = 0;
    std::string type;
    std::string title;
    std::string body;
    int priority = 2;
    std::string dueDate;
    std::string reviewDate;
    std::string tags;
};

static bool workStringField(const json& body, const char* field,
                            size_t maximum, std::string& value,
                            std::string& error) {
    if (!body.contains(field)) {
        error = std::string("work payload is missing '") + field + "'";
        return false;
    }
    const json& candidate = body[field];
    if (!candidate.is_string()) {
        error = std::string("work field '") + field + "' must be a string";
        return false;
    }
    value = candidate.get<std::string>();
    if (value.size() > maximum) {
        error = std::string("work field '") + field + "' is too large";
        return false;
    }
    if (value.find('\0') != std::string::npos) {
        error = std::string("work field '") + field +
                "' contains a null character";
        return false;
    }
    return true;
}

static bool exactObjectFields(const json& value,
                              const std::set<std::string>& allowed,
                              const std::string& label,
                              std::string& error) {
    if (!value.is_object()) {
        error = label + " must be an object";
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "unexpected " + label + " field '" + it.key() + "'";
            return false;
        }
    }
    if (value.size() != allowed.size()) {
        error = label + " requires every documented field";
        return false;
    }
    return true;
}

struct WorkTitleInput {
    std::string title;
    std::string expectedUpdatedAt;
};

static bool parseWorkTitleInput(const json& body, WorkTitleInput& input,
                                std::string& error) {
    static const std::set<std::string> allowed = {
        "title", "expected_updated_at"
    };
    if (!exactObjectFields(body, allowed, "work title payload", error) ||
        !workStringField(body, "title", 511, input.title, error) ||
        !workStringField(body, "expected_updated_at", 64,
                         input.expectedUpdatedAt, error))
        return false;
    input.title = trim(input.title);
    input.expectedUpdatedAt = trim(input.expectedUpdatedAt);
    if (input.title.empty()) {
        error = "work title cannot be blank";
        return false;
    }
    if (input.expectedUpdatedAt.empty()) {
        error = "expected_updated_at cannot be blank";
        return false;
    }
    return true;
}

struct WorkMergeSourceInput {
    long long id = 0;
    std::string expectedUpdatedAt;
};

struct WorkMergeInput {
    std::string expectedUpdatedAt;
    std::vector<WorkMergeSourceInput> sources;
};

static bool parseWorkMergeInput(const json& body, long long targetId,
                                WorkMergeInput& input,
                                std::string& error) {
    static const std::set<std::string> allowed = {
        "expected_updated_at", "sources"
    };
    static const std::set<std::string> sourceAllowed = {
        "id", "expected_updated_at"
    };
    if (!exactObjectFields(body, allowed, "work merge payload", error) ||
        !workStringField(body, "expected_updated_at", 64,
                         input.expectedUpdatedAt, error))
        return false;
    input.expectedUpdatedAt = trim(input.expectedUpdatedAt);
    if (input.expectedUpdatedAt.empty()) {
        error = "expected_updated_at cannot be blank";
        return false;
    }
    if (!body["sources"].is_array() || body["sources"].empty() ||
        body["sources"].size() > 200) {
        error = "sources must contain 1 through 200 entries";
        return false;
    }

    std::set<long long> distinct{targetId};
    input.sources.clear();
    input.sources.reserve(body["sources"].size());
    for (const json& source : body["sources"]) {
        if (!exactObjectFields(source, sourceAllowed,
                               "work merge source", error))
            return false;
        if (!source["id"].is_number_integer()) {
            error = "work merge source id must be an integer";
            return false;
        }
        WorkMergeSourceInput parsed;
        parsed.id = source["id"].get<long long>();
        if (parsed.id <= 0 || !distinct.insert(parsed.id).second) {
            error = "work merge source and target ids must be distinct and positive";
            return false;
        }
        if (!workStringField(source, "expected_updated_at", 64,
                             parsed.expectedUpdatedAt, error))
            return false;
        parsed.expectedUpdatedAt = trim(parsed.expectedUpdatedAt);
        if (parsed.expectedUpdatedAt.empty()) {
            error = "source expected_updated_at cannot be blank";
            return false;
        }
        input.sources.push_back(std::move(parsed));
    }
    return true;
}

static bool parseWorkSaveInput(const json& body, WorkSaveInput& input,
                               std::string& error) {
    static const std::set<std::string> allowed = {
        "project_id", "type", "title", "body", "priority", "due_date",
        "review_date", "tags"
    };
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "unexpected work field '" + it.key() + "'";
            return false;
        }
    }
    if (body.size() != allowed.size()) {
        error = "work create requires every editable creation field";
        return false;
    }
    if (!body["project_id"].is_number_integer()) {
        error = "work field 'project_id' must be an integer";
        return false;
    }
    input.projectId = body["project_id"].get<long long>();
    if (input.projectId <= 0) {
        error = "work field 'project_id' must be positive";
        return false;
    }
    if (!body["priority"].is_number_integer()) {
        error = "work field 'priority' must be an integer";
        return false;
    }
    input.priority = body["priority"].get<int>();
    if (input.priority < 1 || input.priority > 4) {
        error = "work priority must be P1 through P4";
        return false;
    }

    constexpr size_t kMaxWorkTitleBytes = 511;
    constexpr size_t kMaxWorkBodyBytes = 16 * 1024 - 1;
    constexpr size_t kMaxWorkDateBytes = 10;
    constexpr size_t kMaxWorkTagsBytes = 2048;
    if (!workStringField(body, "type", 32, input.type, error) ||
        !workStringField(body, "title", kMaxWorkTitleBytes,
                         input.title, error) ||
        !workStringField(body, "body", kMaxWorkBodyBytes,
                         input.body, error) ||
        !workStringField(body, "due_date", kMaxWorkDateBytes,
                         input.dueDate, error) ||
        !workStringField(body, "review_date", kMaxWorkDateBytes,
                         input.reviewDate, error) ||
        !workStringField(body, "tags", kMaxWorkTagsBytes,
                         input.tags, error))
        return false;

    input.type = toLower(trim(input.type));
    input.title = trim(input.title);
    input.body = trim(input.body);
    input.dueDate = trim(input.dueDate);
    input.reviewDate = trim(input.reviewDate);
    input.tags = trim(input.tags);
    if (input.type != "fix" && input.type != "implementation" &&
        input.type != "reference" && input.type != "note") {
        error = "work type must be fix, implementation, reference, or note";
        return false;
    }
    if (input.title.empty()) {
        error = "work title cannot be blank";
        return false;
    }
    const auto validDate = [](const std::string& value) {
        if (value.empty()) return true;
        if (value.size() != 10 || value[4] != '-' || value[7] != '-')
            return false;
        for (size_t i = 0; i < value.size(); ++i) {
            if (i == 4 || i == 7) continue;
            if (value[i] < '0' || value[i] > '9') return false;
        }
        const int year = std::stoi(value.substr(0, 4));
        const int month = std::stoi(value.substr(5, 2));
        const int day = std::stoi(value.substr(8, 2));
        if (year < 1 || month < 1 || month > 12 || day < 1) return false;
        static constexpr int daysByMonth[] = {
            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
        };
        int maximum = daysByMonth[month - 1];
        if (month == 2 &&
            (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
            maximum = 29;
        return day <= maximum;
    };
    if (!validDate(input.dueDate) || !validDate(input.reviewDate)) {
        error = "work dates must be blank or valid YYYY-MM-DD values";
        return false;
    }
    return true;
}

static crow::response saveWorkItem(Db* db, const json& body) {
    WorkSaveInput input;
    std::string error;
    if (!parseWorkSaveInput(body, input, error))
        return errRes(400, error);

    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement project(db->raw(lk.token()),
            "SELECT archived FROM projects WHERE id=?");
        project.bind(1, input.projectId);
        if (!project.executeStep()) return errRes(404, "project not found");
        if (project.getColumn(0).getInt() != 0)
            return errRes(409, "archived projects cannot receive new work");

        SQLite::Statement duplicate(db->raw(lk.token()), R"sql(
SELECT id,type,body,priority,due_date,review_date,tags
FROM items
WHERE project_id=? AND title=? COLLATE NOCASE
  AND status IN ('open','in_progress','blocked')
ORDER BY id LIMIT 1)sql");
        duplicate.bind(1, input.projectId);
        duplicate.bind(2, input.title);
        if (duplicate.executeStep()) {
            const long long existingId = duplicate.getColumn(0).getInt64();
            const bool exactReplay =
                duplicate.getColumn(1).getString() == input.type &&
                duplicate.getColumn(2).getString() == input.body &&
                duplicate.getColumn(3).getInt() == input.priority &&
                duplicate.getColumn(4).getString() == input.dueDate &&
                duplicate.getColumn(5).getString() == input.reviewDate &&
                duplicate.getColumn(6).getString() == input.tags;
            if (exactReplay) {
                tx.commit();
                return jsonRes(json{{"ok", true}, {"id", existingId},
                                    {"created", false},
                                    {"duplicate", true}});
            }
            return jsonRes(json{{"ok", false}, {"code", "conflict"},
                                {"error", "an active work item already uses that title"},
                                {"existing_id", existingId}}, 409);
        }

        const long long itemId = insertItemLocked(
            lk.token(), db, input.projectId, input.type, input.title,
            input.body, input.priority, 0, "api", input.dueDate, input.tags);
        SQLite::Statement review(db->raw(lk.token()),
            "UPDATE items SET review_date=? WHERE id=?");
        review.bind(1, input.reviewDate);
        review.bind(2, itemId);
        review.exec();
        db->logActivity(lk.token(), "item_added", input.projectId,
                        input.title);
        tx.commit();
        return jsonRes(json{{"ok", true}, {"id", itemId},
                            {"created", true}, {"duplicate", false}}, 201);
    } catch (const std::exception& e) {
        return internalErrRes(500, "work_save_failed", e.what());
    }
}

static crow::response updateWorkTitle(Db* db, DiscordBot* bot,
                                      long long itemId,
                                      const json& body) {
    WorkTitleInput input;
    std::string error;
    if (!parseWorkTitleInput(body, input, error))
        return errRes(400, error);

    try {
        auto lk = db->guard();
        json current = getWorkItemLocked(lk.token(), db, itemId);
        if (!current.value("ok", false)) return opRes(current);
        if (current.value("status", "") == "merged")
            return errRes(409, "merged audit records cannot change title");
        const std::string currentUpdatedAt =
            current.value("updated_at", std::string());
        if (currentUpdatedAt != input.expectedUpdatedAt)
            return errRes(409, "work item changed after it was reviewed");
        if (current.value("title", std::string()) == input.title)
            return jsonRes(current);

        SQLite::Transaction tx(db->raw(lk.token()));
        SQLite::Statement update(db->raw(lk.token()),
            "UPDATE items SET title=?,updated_at=? "
            "WHERE id=? AND updated_at=? AND status<>'merged'");
        update.bind(1, input.title);
        update.bind(2, nextIsoUtcAfter(currentUpdatedAt));
        update.bind(3, itemId);
        update.bind(4, currentUpdatedAt);
        if (update.exec() != 1)
            return errRes(409, "work item changed before its title could be saved");
        updateNotifyCardForItemLocked(
            lk.token(), db, bot, itemId, current.value("status", ""));
        tx.commit();
        return jsonRes(getWorkItemLocked(lk.token(), db, itemId));
    } catch (const std::exception& e) {
        return internalErrRes(500, "work_title_update_failed", e.what());
    }
}

static crow::response mergeWorkItems(Db* db, DiscordBot* bot,
                                     long long targetId,
                                     const json& body) {
    WorkMergeInput input;
    std::string error;
    if (!parseWorkMergeInput(body, targetId, input, error))
        return errRes(400, error);

    try {
        auto lk = db->guard();
        json target = getWorkItemLocked(lk.token(), db, targetId);
        if (!target.value("ok", false)) return opRes(target);
        if (target.value("status", "") == "merged")
            return errRes(409, "a merged audit record cannot be a merge target");
        if (target.value("updated_at", std::string()) !=
            input.expectedUpdatedAt)
            return errRes(409, "merge target changed after it was reviewed");

        std::vector<long long> sourceIds;
        sourceIds.reserve(input.sources.size());
        for (const WorkMergeSourceInput& sourceInput : input.sources) {
            json source = getWorkItemLocked(lk.token(), db, sourceInput.id);
            if (!source.value("ok", false)) return opRes(source);
            if (source.value("status", "") == "merged")
                return errRes(409, "a merged audit record cannot be merged again");
            if (source.value("project_id", 0LL) !=
                target.value("project_id", 0LL))
                return errRes(409, "tickets can only be merged within one project");
            if (source.value("updated_at", std::string()) !=
                sourceInput.expectedUpdatedAt)
                return errRes(409, "merge source changed after it was reviewed");
            sourceIds.push_back(sourceInput.id);
        }

        if (!mergeItemsLocked(lk.token(), db, sourceIds, targetId,
                              error, bot))
            return errRes(409, error.empty() ? "ticket merge was rejected" : error);

        json sources = json::array();
        for (long long sourceId : sourceIds)
            sources.push_back(getWorkItemLocked(lk.token(), db, sourceId));
        return jsonRes(json{
            {"ok", true},
            {"target", getWorkItemLocked(lk.token(), db, targetId)},
            {"sources", std::move(sources)},
        });
    } catch (const std::exception& e) {
        return internalErrRes(500, "work_merge_failed", e.what());
    }
}

// "YYYY-MM-DD" for N days ago (UTC).
static std::string daysAgoIso(int days) {
    std::time_t t = std::time(nullptr) - (std::time_t)days * 86400;
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// ---------------------------------------------------------------------------
// server
// ---------------------------------------------------------------------------

uint16_t Server::start() {
    auto& app = impl_->app;
    app.loglevel(crow::LogLevel::Warning);
    Db* db = db_;
    BuildRunner* builds = builds_;

    CROW_ROUTE(app, "/")([this]() {
        return jsonRes(json{{"app", DEVHUB_APP_NAME},
                            {"version", DEVHUB_VERSION},
                            {"note", "native app - this port only serves the JSON API"},
                            {"port", port_}});
    });

    CROW_ROUTE(app, "/api/health")([this]() {
        json j{{"ok", true},
               {"app", DEVHUB_APP_NAME},
               {"version", DEVHUB_VERSION},
               {"port", port_}};
        if (discordStatus) {
            json discord = discordStatus();
            const bool warning =
                discord.value("warn", std::string()).size() > 0;
            discord.erase("error");
            discord.erase("warn");
            discord.erase("last_log");
            discord["has_warning"] = warning;
            j["discord"] = std::move(discord);
        }
        return jsonRes(j);
    });

    // ---- stats ----
    CROW_ROUTE(app, "/api/stats")([db]() {
        auto lk = db->guard();
        json j;
        {
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT COUNT(*) FROM projects WHERE archived=0");
            q.executeStep();
            j["projects"] = q.getColumn(0).getInt64();
        }
        {
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT type, COUNT(*) FROM items WHERE status IN ('open','in_progress','blocked') "
                "GROUP BY type");
            json by = json::object();
            long long total = 0;
            while (q.executeStep()) {
                by[q.getColumn(0).getString()] = q.getColumn(1).getInt64();
                total += q.getColumn(1).getInt64();
            }
            j["open_by_type"] = by;
            j["open_total"] = total;
        }
        {
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT COUNT(*) FROM items WHERE status='completed' AND completed_at>=?");
            q.bind(1, daysAgoIso(7));
            q.executeStep();
            j["completed_week"] = q.getColumn(0).getInt64();
        }
        {
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT COUNT(*) FROM discord_messages WHERE state='new' AND kind!='none'");
            q.executeStep();
            j["discord_pending"] = q.getColumn(0).getInt64();
        }
        {
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT COUNT(*) FROM item_sources x JOIN items i ON i.id=x.item_id "
                "WHERE i.status='completed' AND x.credited=0");
            q.executeStep();
            j["credits_owed"] = q.getColumn(0).getInt64();
        }
        return jsonRes(j);
    });

    // ---- projects (read + build trigger for automation) ----
    CROW_ROUTE(app, "/api/projects")([db]() {
        auto lk = db->guard();
        // Explicit projection. Filesystem paths, skill routing, version URLs,
        // and pipeline commands are operator configuration and never belong
        // in the default API response.
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT p.id,p.name,p.slug,p.description,p.color,p.archived,p.sort_order,
 p.created_at,p.updated_at,p.repo_url,p.discord_tickets,
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status IN ('open','in_progress','blocked') AND i.type='fix') AS open_fixes,
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status IN ('open','in_progress','blocked') AND i.type='implementation') AS open_impls,
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status IN ('open','in_progress','blocked') AND i.type NOT IN ('fix','implementation')) AS open_other,
 (SELECT COUNT(*) FROM items i WHERE i.project_id=p.id AND i.status='completed') AS completed_count
FROM projects p ORDER BY p.archived, p.sort_order, p.name COLLATE NOCASE)sql");
        return jsonRes(Db::rowsToJson(q));
    });

    // Exact operator-config readback plus full-field create/update. The list
    // route above intentionally remains a path/command-free projection.
    CROW_ROUTE(app, "/api/projects/<int>")([db](int id) {
        if (id <= 0) return errRes(400, "project id must be positive");
        auto lk = db->guard();
        json result = getProjectLocked(lk.token(), db, id);
        return result.value("ok", false)
            ? jsonRes(result) : errRes(404, "project not found");
    });

    CROW_ROUTE(app, "/api/projects").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return saveProject(db, 0, body);
    });

    CROW_ROUTE(app, "/api/projects/<int>").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        if (id <= 0) return errRes(400, "project id must be positive");
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return saveProject(db, id, body);
    });

    // Trigger a configured pipeline action. Body: {kind:
    // build|prep|release|version, confirm? (release), choice?/version? (version)}.
    // Commands and working directories are project configuration owned by the
    // native UI; request bodies must never replace either value.
    CROW_ROUTE(app, "/api/projects/<int>/build").methods("POST"_method)(
        [db, builds](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        if (body.contains("command") || body.contains("cwd"))
            return errRes(400, "command and cwd are configured per project");
        std::string kind = body.value("kind", "build");
        std::string command, cwd;
        {
            auto lk = db->guard();
            SQLite::Statement q(db->raw(lk.token()),
                "SELECT build_command, prep_command, release_command, "
                "version_command, build_cwd, path FROM projects WHERE id=?");
            q.bind(1, id);
            if (!q.executeStep()) return errRes(404, "project not found");
            if (kind == "prep") command = q.getColumn(1).getString();
            else if (kind == "release") command = q.getColumn(2).getString();
            else if (kind == "version") command = q.getColumn(3).getString();
            else command = q.getColumn(0).getString();
            cwd = q.getColumn(4).getString();
            if (cwd.empty()) cwd = q.getColumn(5).getString();
        }
        std::string stdinData = "\n\n\n";
        if (kind == "release") {
            if (!body.value("confirm", false))
                return errRes(409, "release requires {\"confirm\":true}");
            stdinData = "yes\nyes\n\n";
        } else if (kind == "version") {
            int choice = body.value("choice", 0);
            if (choice == 1) stdinData = "1\n\n";
            else if (choice == 2) stdinData = "2\n\n";
            else if (choice == 3) {
                const std::string requested =
                    body.value("version", std::string());
                if (!parseVersion(requested).valid)
                    return errRes(400, "version must be x.y[.z[.w]]");
                stdinData = "3\n" + requested + "\n\n";
            }
            else return errRes(400, "version requires choice 1|2|3 (+version for 3)");
        }

        std::string error;
        long long buildId = builds->start(id, kind, command, cwd, stdinData, error);
        if (buildId == 0)
            return internalErrRes(409, "build_conflict", error);
        return jsonRes(json{{"ok", true}, {"build_id", buildId}});
    });

    CROW_ROUTE(app, "/api/builds/<int>")(
        [builds](const crow::request& req, int id) {
        if (id <= 0) return errRes(400, "build id must be positive");
        size_t offset = 0;
        std::string error;
        if (!queryNonNegativeSize(req, "offset", 0, offset, error))
            return errRes(400, error);
        json j = builds->status(id, offset);
        if (j.contains("error"))
            return internalErrRes(404, "build_not_found",
                                  j["error"].get<std::string>());
        return jsonRes(j);
    });

    // ---- discord: ingest + channel listing (external collectors) ----
    CROW_ROUTE(app, "/api/discord/channels")([db]() {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT c.*, p.name AS project_name FROM discord_channels c "
            "LEFT JOIN projects p ON p.id=c.project_id "
            "ORDER BY c.guild_name, c.channel_name");
        return jsonRes(Db::rowsToJson(q));
    });

    CROW_ROUTE(app, "/api/discord/channels").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        std::string channelId = trim(body.value("channel_id", ""));
        if (channelId.empty()) return errRes(400, "channel_id is required");
        auto lk = db->guard();
        SQLite::Statement ins(db->raw(lk.token()),
            "INSERT INTO discord_channels(channel_id,guild_name,channel_name,project_id,"
            "enabled,created_at) VALUES(?,?,?,?,?,?) "
            "ON CONFLICT(channel_id) DO UPDATE SET guild_name=excluded.guild_name, "
            "channel_name=excluded.channel_name");
        ins.bind(1, channelId);
        ins.bind(2, body.value("guild_name", ""));
        ins.bind(3, body.value("channel_name", ""));
        long long pid = body.value("project_id", 0LL);
        if (pid > 0) ins.bind(4, pid); else ins.bind(4);
        ins.bind(5, body.value("enabled", 1));
        ins.bind(6, nowIsoUtc());
        ins.exec();
        return jsonRes(json{{"ok", true}});
    });

    // Server-wide monitors: the bot watches every text channel it can see in
    // these guilds (plus any individually registered channels).
    CROW_ROUTE(app, "/api/discord/guilds")([db]() {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT * FROM discord_guilds ORDER BY guild_name, guild_id");
        return jsonRes(Db::rowsToJson(q));
    });

    CROW_ROUTE(app, "/api/discord/guilds").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        std::string guildId = trim(body.value("guild_id", ""));
        if (guildId.empty()) return errRes(400, "guild_id is required");
        auto lk = db->guard();
        SQLite::Statement ins(db->raw(lk.token()),
            "INSERT INTO discord_guilds(guild_id,guild_name,enabled,created_at) "
            "VALUES(?,?,?,?) ON CONFLICT(guild_id) DO UPDATE SET "
            "guild_name=CASE WHEN excluded.guild_name!='' THEN excluded.guild_name "
            "ELSE guild_name END");
        ins.bind(1, guildId);
        ins.bind(2, body.value("guild_name", ""));
        ins.bind(3, body.value("enabled", 1));
        ins.bind(4, nowIsoUtc());
        ins.exec();
        return jsonRes(json{{"ok", true}});
    });

    CROW_ROUTE(app, "/api/discord/ingest").methods("POST"_method)(
        [db, this](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        if (!body.contains("messages") || !body["messages"].is_array())
            return errRes(400, "expected {messages:[...]}");
        constexpr size_t kMaxIngestBatch = 200;
        if (body["messages"].size() > kMaxIngestBatch)
            return errRes(413, "at most 200 messages per request");
        int ingested = 0, duplicates = 0, failed = 0;
        int suggestions = 0, bugs = 0, skippedDisabled = 0, imagesStaged = 0;
        for (const auto& m : body["messages"]) {
            std::vector<DiscordAttachmentMeta> attachments;
            if (m.contains("attachments") && m["attachments"].is_array()) {
                for (const auto& a : m["attachments"]) {
                    if (!a.is_object()) continue;
                    DiscordAttachmentMeta meta;
                    meta.attachmentId = a.value(
                        "attachment_id", a.value("id", std::string()));
                    meta.sourceChannelId = m.value("channel_id", "");
                    meta.sourceMessageId = m.value("message_id", "");
                    meta.sourceRole = "suggestion";
                    meta.filename = a.value("filename", "");
                    meta.contentType = a.value("content_type", "");
                    meta.sizeBytes = a.value("size", std::uint64_t{0});
                    meta.width = a.value("width", std::uint32_t{0});
                    meta.height = a.value("height", std::uint32_t{0});
                    meta.ephemeral = a.value("ephemeral", false);
                    attachments.push_back(std::move(meta));
                }
            }
            IngestOutcome r = ingestDiscordMessage(
                db, m.value("channel_id", ""), m.value("channel_name", ""),
                m.value("guild_id", ""), m.value("guild_name", ""),
                m.value("message_id", ""), m.value("author", ""),
                m.value("author_id", ""), m.value("content", ""),
                m.value("posted_at", ""), /*asCommand=*/false, attachments);
            if (r.ingested && r.kind != "none")
                notifyDetection(db, bot, r.messageRowId,
                                m.value("guild_id", ""),
                                m.value("channel_id", ""),
                                m.value("channel_name", ""),
                                m.value("message_id", ""), m.value("author", ""),
                                m.value("content", ""), r.kind, r.score);
            if (r.ingested) ++ingested;
            if (r.duplicate) ++duplicates;
            if (r.failed) ++failed;
            if (r.skippedDisabled) ++skippedDisabled;
            if (r.kind == "suggestion") ++suggestions;
            if (r.kind == "bug") ++bugs;
            imagesStaged += r.imageCount;
        }
        return jsonRes(json{{"ok", failed == 0}, {"ingested", ingested},
                            {"duplicates", duplicates},
                            {"failed", failed},
                            {"suggestions_found", suggestions},
                            {"bugs_found", bugs},
                            {"images_staged", imagesStaged},
                            {"skipped_disabled", skippedDisabled}});
    });

    // ---- AI review bundle export ----
    CROW_ROUTE(app, "/api/export/review")([db](const crow::request& req) {
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", true, projectId, error))
            return errRes(400, error);
        if (!recordExists(db, "projects", projectId))
            return errRes(404, "project not found");
        if (!packetInputsWithinBudget(db, projectId, error))
            return errRes(413, error);
        std::string md = buildReviewMarkdown(db, projectId);
        if (md.empty()) return errRes(500, "review packet generation failed");
        return markdownRes(md);
    });

    CROW_ROUTE(app, "/api/export/release")([db](const crow::request& req) {
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", true, projectId, error))
            return errRes(400, error);
        if (!recordExists(db, "projects", projectId))
            return errRes(404, "project not found");
        int days = 30;
        if (!queryPositiveInt(req, "days", 30, 3650, days, error))
            return errRes(400, error);
        if (!packetInputsWithinBudget(db, projectId, error))
            return errRes(413, error);
        std::string md = buildReleaseDraft(db, projectId, days);
        if (md.empty()) return errRes(500, "release packet generation failed");
        return markdownRes(md);
    });

    // Active cross-project work queue for local scripts/automation.
    CROW_ROUTE(app, "/api/work")([db]() {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()), R"sql(
SELECT i.id,i.project_id,p.name AS project_name,i.type,i.title,i.body,i.status,
 i.priority,i.due_date,i.review_date,i.blocked_reason,i.created_at,i.updated_at,
 IFNULL(GROUP_CONCAT(s.name, ', '),'') AS contributors
FROM items i JOIN projects p ON p.id=i.project_id
LEFT JOIN item_sources x ON x.item_id=i.id LEFT JOIN sources s ON s.id=x.source_id
WHERE p.archived=0 AND i.status IN ('open','in_progress','blocked')
GROUP BY i.id ORDER BY i.priority DESC,i.updated_at)sql");
        return jsonRes(Db::rowsToJson(q));
    });

    // Creates one canonical open ticket through the same item operation used
    // by the native UI. Exact active replays are idempotent; a same-title
    // active item with different content is a conflict that requires review.
    CROW_ROUTE(app, "/api/work").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return saveWorkItem(db, body);
    });

    // Additive reconciliation from one explicitly selected historical DevHub
    // data root. The operation preflights the complete batch, preserves exact
    // ticket/provenance/attachment history, and never updates or deletes a
    // destination ticket. Exact complete replays are reported as duplicates.
    CROW_ROUTE(app, "/api/work/import").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        json result = importWorkItems(db, body);
        if (result.value("ok", false)) return jsonRes(result);
        const std::string code = result.value("code", "invalid_request");
        const std::string detail = result.value("error", "ticket import failed");
        if (code == "internal_error")
            return internalErrRes(500, "ticket_import_failed", detail);
        const int status = code == "not_found" ? 404
            : code == "conflict" ? 409
            : code == "payload_too_large" ? 413 : 400;
        return errRes(status, detail);
    });

    CROW_ROUTE(app, "/api/work/<int>")([db](int id) {
        if (id <= 0) return errRes(400, "work item id must be positive");
        auto lk = db->guard();
        return opRes(getWorkItemLocked(lk.token(), db, id));
    });

    // Strict title-only mutation with exact optimistic concurrency. No body,
    // status, priority, attribution, or evidence field is accepted here.
    CROW_ROUTE(app, "/api/work/<int>/title").methods("POST"_method)(
        [this, db](const crow::request& req, int id) {
        if (id <= 0) return errRes(400, "work item id must be positive");
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return updateWorkTitle(db, bot, id, body);
    });

    // One same-project batch merge. The target and every source are bound to
    // the exact updated_at values the caller reviewed before the transaction.
    CROW_ROUTE(app, "/api/work/<int>/merge").methods("POST"_method)(
        [this, db](const crow::request& req, int id) {
        if (id <= 0) return errRes(400, "work item id must be positive");
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return mergeWorkItems(db, bot, id, body);
    });

    // Body: {status: open|in_progress|blocked|completed|wont_do}. The shared
    // lifecycle helper owns completion timestamps, calendar/activity records,
    // reopen cleanup, and durable Discord-card refresh state.
    CROW_ROUTE(app, "/api/work/<int>/status").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        if (id <= 0) return errRes(400, "work item id must be positive");
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        if (!body.contains("status") || !body["status"].is_string())
            return errRes(400, "status must be a string");
        const std::string status = toLower(trim(body["status"].get<std::string>()));
        if (status != "open" && status != "in_progress" &&
            status != "blocked" && status != "completed" &&
            status != "wont_do")
            return errRes(400, "invalid work item status");

        auto lk = db->guard();
        json current = getWorkItemLocked(lk.token(), db, id);
        if (!current.value("ok", false)) return opRes(current);
        if (current.value("status", "") == "merged")
            return errRes(409, "merged audit records cannot change status");
        setItemStatusLocked(lk.token(), db, id, status);
        return opRes(getWorkItemLocked(lk.token(), db, id));
    });

    // ---- advanced knowledge, reports and resumable development control ----
    CROW_ROUTE(app, "/api/knowledge")([db](const crow::request& req) {
        long long projectId = 0;
        int limit = 50;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error) ||
            !queryPositiveInt(req, "limit", 50, 200, limit, error))
            return errRes(400, error);
        return opRes(listKnowledgeNodes(
            db, projectId, queryText(req, "q"),
            queryText(req, "kind"), queryText(req, "status"),
            limit));
    });

    CROW_ROUTE(app, "/api/knowledge").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveKnowledgeNode(db, 0, body), 201);
    });

    CROW_ROUTE(app, "/api/knowledge/<int>")([db](int id) {
        return opRes(getKnowledgeNode(db, id));
    });

    CROW_ROUTE(app, "/api/knowledge/<int>").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveKnowledgeNode(db, id, body));
    });

    CROW_ROUTE(app, "/api/knowledge/<int>/claims").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(addKnowledgeClaim(db, id, body), 201);
    });

    CROW_ROUTE(app, "/api/knowledge/sources").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveKnowledgeSource(db, body), 201);
    });

    CROW_ROUTE(app, "/api/knowledge/links").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(linkKnowledgeNodes(db, body), 201);
    });

    CROW_ROUTE(app, "/api/knowledge/conflicts")([db](const crow::request& req) {
        long long projectId = 0;
        int limit = 100;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error) ||
            !queryPositiveInt(req, "limit", 100, 200, limit, error))
            return errRes(400, error);
        return opRes(listKnowledgeConflicts(
            db, projectId, queryText(req, "status"), limit));
    });

    CROW_ROUTE(app, "/api/knowledge/conflicts").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(createKnowledgeConflict(db, body), 201);
    });

    CROW_ROUTE(app, "/api/knowledge/conflicts/<int>/resolve").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(resolveKnowledgeConflict(db, id, body));
    });

    CROW_ROUTE(app, "/api/knowledge/health")([db](const crow::request& req) {
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error))
            return errRes(400, error);
        return opRes(knowledgeHealth(db, projectId));
    });

    CROW_ROUTE(app, "/api/knowledge/context")([db](const crow::request& req) {
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error))
            return errRes(400, error);
        if (projectId > 0 && !recordExists(db, "projects", projectId))
            return errRes(404, "project not found");
        int level = 2, limit = 30;
        if (!queryBoundedInt(req, "level", 2, 0, 3, level, error) ||
            !queryPositiveInt(req, "limit", 30, 200, limit, error))
            return errRes(400, error);
        std::string md = buildKnowledgeContext(
            db, projectId, queryText(req, "q"), level, limit);
        if (md.empty()) return errRes(500, "knowledge packet generation failed");
        return markdownRes(md);
    });

    CROW_ROUTE(app, "/api/reports")([db](const crow::request& req) {
        long long projectId = 0;
        int limit = 100;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error) ||
            !queryPositiveInt(req, "limit", 100, 200, limit, error))
            return errRes(400, error);
        return opRes(listReports(
            db, projectId, queryText(req, "type"),
            queryText(req, "status"), limit));
    });

    CROW_ROUTE(app, "/api/reports").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveReport(db, body), 201);
    });

    CROW_ROUTE(app, "/api/reports/context")([db](const crow::request& req) {
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error))
            return errRes(400, error);
        if (projectId > 0 && !recordExists(db, "projects", projectId))
            return errRes(404, "project not found");
        int days = 30;
        if (!queryPositiveInt(req, "days", 30, 3650, days, error))
            return errRes(400, error);
        std::string md = buildReportContext(
            db, projectId, queryText(req, "type"),
            days);
        if (md.empty()) return errRes(500, "report packet generation failed");
        return markdownRes(md);
    });

    // Explicit capture is a write: it registers the exact report packet so a
    // later approval can prove its project/type/period/hash/evidence identity.
    CROW_ROUTE(app, "/api/reports/context").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error))
            return errRes(400, error);
        if (projectId > 0 && !recordExists(db, "projects", projectId))
            return errRes(404, "project not found");
        int days = 30;
        if (!queryPositiveInt(req, "days", 30, 3650, days, error))
            return errRes(400, error);
        std::string md = buildReportContext(
            db, projectId, queryText(req, "type"),
            days,
            nullptr, nullptr, nullptr, nullptr, true);
        if (md.empty()) return errRes(500, "report packet capture failed");
        return markdownRes(md);
    });

    CROW_ROUTE(app, "/api/reports/<int>")([db](int id) {
        return opRes(getReport(db, id));
    });

    CROW_ROUTE(app, "/api/reports/<int>/status").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(setReportStatus(db, id, body.value("status", "")));
    });

    CROW_ROUTE(app, "/api/workflows")([db](const crow::request& req) {
        long long projectId = 0;
        int limit = 100;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error) ||
            !queryPositiveInt(req, "limit", 100, 200, limit, error))
            return errRes(400, error);
        return opRes(listWorkflows(
            db, projectId, queryText(req, "status"), limit));
    });

    CROW_ROUTE(app, "/api/workflows").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveWorkflow(db, body), 201);
    });

    CROW_ROUTE(app, "/api/workflows/resume")([db](const crow::request& req) {
        long long projectId = 0;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error))
            return errRes(400, error);
        if (projectId > 0 && !recordExists(db, "projects", projectId))
            return errRes(404, "project not found");
        std::string md = buildWorkflowResume(db, projectId);
        if (md.empty()) return errRes(500, "resume packet generation failed");
        return markdownRes(md);
    });

    CROW_ROUTE(app, "/api/workflows/<int>/resume")([db](int id) {
        if (id <= 0) return errRes(400, "workflow id must be positive");
        if (!recordExists(db, "workflow_runs", id))
            return errRes(404, "workflow not found");
        std::string md = buildWorkflowResume(db, 0, id);
        if (md.empty()) return errRes(500, "resume packet generation failed");
        return markdownRes(md);
    });

    CROW_ROUTE(app, "/api/workflows/<int>")([db](int id) {
        return opRes(getWorkflow(db, id));
    });

    CROW_ROUTE(app, "/api/workflows/<int>/status").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(setWorkflowStatus(db, id, body.value("status", "")));
    });

    CROW_ROUTE(app, "/api/workflows/<int>/artifacts").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(addWorkflowArtifact(db, id, body), 201);
    });

    CROW_ROUTE(app, "/api/workflows/<int>/evidence").methods("POST"_method)(
        [db](const crow::request& req, int id) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(addVerificationEvidence(db, id, body), 201);
    });

    CROW_ROUTE(app, "/api/learnings")([db](const crow::request& req) {
        long long projectId = 0;
        int limit = 20;
        std::string error;
        if (!queryPositiveId(req, "project_id", false, projectId, error) ||
            !queryPositiveInt(req, "limit", 20, 200, limit, error))
            return errRes(400, error);
        return opRes(listLearnings(
            db, projectId, queryText(req, "q"), limit));
    });

    CROW_ROUTE(app, "/api/learnings").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveLearning(db, body), 201);
    });

    CROW_ROUTE(app, "/api/processing-runs")([db](const crow::request& req) {
        int limit = 100;
        std::string error;
        if (!queryPositiveInt(req, "limit", 100, 200, limit, error))
            return errRes(400, error);
        return opRes(listProcessingRuns(
            db, queryText(req, "kind"), queryText(req, "status"),
            limit));
    });

    CROW_ROUTE(app, "/api/processing-runs").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveProcessingRun(db, body), 201);
    });

    CROW_ROUTE(app, "/api/retrieval-evaluations")([db](const crow::request& req) {
        int limit = 100;
        std::string error;
        if (!queryPositiveInt(req, "limit", 100, 200, limit, error))
            return errRes(400, error);
        return opRes(listRetrievalEvaluations(
            db, queryText(req, "engine"), limit));
    });

    CROW_ROUTE(app, "/api/retrieval-evaluations").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        return opRes(saveRetrievalEvaluation(db, body), 201);
    });

    // ---- settings (bot reads discord_bot_token etc.) ----
    CROW_ROUTE(app, "/api/settings")([db]() {
        auto lk = db->guard();
        SQLite::Statement q(db->raw(lk.token()), "SELECT key, value FROM settings");
        json j = json::object();
        while (q.executeStep()) {
            const std::string key = q.getColumn(0).getString();
            j[key] = isPublicSettingKey(key)
                ? db->getSetting(lk.token(), key) : "[redacted]";
        }
        return jsonRes(j);
    });

    CROW_ROUTE(app, "/api/settings").methods("POST"_method)(
        [db](const crow::request& req) {
        json body; crow::response rejected;
        if (!jsonRequest(req, body, rejected)) return rejected;
        std::vector<std::pair<std::string, std::string>> pending;
        pending.reserve(body.size());
        for (const auto& [key, value] : body.items()) {
            if (!apiWritableSettingKey(key))
                return errRes(403, "setting is not API-writable: " + key);
            std::string normalized, error;
            if (!normalizeSettingValue(key, value, normalized, error))
                return errRes(error.find("too large") != std::string::npos
                                  ? 413 : 400,
                              error);
            pending.emplace_back(key, std::move(normalized));
        }
        auto lk = db->guard();
        SQLite::Transaction transaction(db->raw(lk.token()));
        for (const auto& [key, value] : pending) db->setSetting(lk.token(), key, value);
        transaction.commit();
        return jsonRes(json{{"ok", true}});
    });

    try {
        impl_->portLease = acquireApiPortLease(port_);
    } catch (const std::exception& e) {
        appLog(std::string("[server] API port lease unavailable: ") + e.what());
        return 0;
    }
    if (impl_->portLease == nullptr) {
        appLog("[server] loopback port " + std::to_string(port_) +
               " is already owned by another XA DevHub instance");
        return 0;
    }

    try {
        th_ = std::thread([this]() {
            try {
                impl_->app.bindaddr("127.0.0.1")
                    .port(port_)
                    .timeout(15)
                    .concurrency(4)
                    .run();
            } catch (const std::exception& e) {
                appLog(std::string("[server] worker failed: ") + e.what());
            } catch (...) {
                appLog("[server] worker failed with an unknown exception");
            }
        });
    } catch (const std::exception& e) {
        appLog(std::string("[server] API worker could not start: ") + e.what());
        releaseApiPortLease(impl_->portLease);
        return 0;
    } catch (...) {
        appLog("[server] API worker could not start with an unknown exception");
        releaseApiPortLease(impl_->portLease);
        return 0;
    }
    const std::cv_status started = impl_->app.wait_for_server_start(
        std::chrono::seconds(5));
    if (started == std::cv_status::timeout || !impl_->app.is_bound()) {
        appLog("[server] loopback port " + std::to_string(port_) +
               " could not be bound; the local API is disabled");
        impl_->app.stop();
        if (th_.joinable()) th_.join();
        releaseApiPortLease(impl_->portLease);
        return 0;
    }
    try {
        // The machine-wide lease remains held after Crow binds and through
        // cleanup, so SO_REUSEADDR cannot let another DevHub rotate the token.
        rendezvousPath_ = apiRendezvousPath(port_, rendezvousRootOverride_);
        writeCurrentUserOnlyRendezvous(rendezvousPath_, impl_->token, port_);
    } catch (const std::exception& e) {
        appLog(std::string("[server] API rendezvous unavailable: ") + e.what());
        impl_->app.stop();
        if (th_.joinable()) th_.join();
        rendezvousPath_.clear();
        releaseApiPortLease(impl_->portLease);
        return 0;
    }
    return port_;
}

void Server::stop() {
    if (th_.joinable()) {
        impl_->app.stop();
        th_.join();
    }
    removeApiRendezvousIfOwned(rendezvousPath_, impl_->token, port_);
    releaseApiPortLease(impl_->portLease);
    rendezvousPath_.clear();
}

} // namespace devhub
