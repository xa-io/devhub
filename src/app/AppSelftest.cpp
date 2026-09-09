#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include "AppBootstrap.h"
#include "AppSelftest.h"
#include "BuildRunner.h"
#include "DataMaintenance.h"
#include "Db.h"
#include "DiscordBot.h"
#include "Gui.h"
#include "KnowledgeOps.h"
#include "Ops.h"
#include "PacketOps.h"
#include "Server.h"
#include "TicketImageStore.h"
#include "Version.h"
#include "VersionChecker.h"
#include "WinUtil.h"
#include "devhub/SuggestionDetector.h"
#include "devhub/TicketImage.h"
#include "devhub/Util.h"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>

using namespace devhub;

struct SelftestHttpResponse {
    DWORD status = 0;
    std::string body;
};

static uint16_t reserveSelftestLoopbackPort() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    uint16_t port = 0;
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == 0) {
        int addressLength = sizeof(address);
        if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address),
                        &addressLength) == 0)
            port = ntohs(address.sin_port);
    }
    closesocket(socketHandle);
    WSACleanup();
    return port;
}

static SelftestHttpResponse selftestHttpJson(
    const wchar_t* method, uint16_t port, const std::string& path,
    const std::string& body, const std::string& token,
    const std::string& contentType, const std::string& origin) {
    SelftestHttpResponse response;
    HINTERNET session = WinHttpOpen(
        L"XADevHub-Selftest/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return response;
    const int timeoutMs = 5000;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        return response;
    }
    const std::wstring widePath(path.begin(), path.end());
    HINTERNET request = WinHttpOpenRequest(
        connection, method, widePath.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    std::wstring headers = L"Content-Type: " + widen(contentType) + L"\r\n";
    if (!token.empty())
        headers += L"X-DevHub-Token: " + widen(token) + L"\r\n";
    if (!origin.empty())
        headers += L"Origin: " + widen(origin) + L"\r\n";
    const BOOL sent = WinHttpSendRequest(
        request, headers.c_str(), static_cast<DWORD>(-1L),
        body.empty() ? WINHTTP_NO_REQUEST_DATA
                     : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (sent && WinHttpReceiveResponse(request, nullptr)) {
        DWORD statusSize = sizeof(response.status);
        WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &statusSize,
            WINHTTP_NO_HEADER_INDEX);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) ||
                read == 0)
                break;
            response.body.append(chunk.data(), read);
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

static SelftestHttpResponse selftestPostJson(
    uint16_t port, const std::string& path, const std::string& body,
    const std::string& token = {},
    const std::string& contentType = "application/json; charset=utf-8",
    const std::string& origin = {}) {
    return selftestHttpJson(
        L"POST", port, path, body, token, contentType, origin);
}

static SelftestHttpResponse selftestGetJson(
    uint16_t port, const std::string& path, const std::string& token = {}) {
    return selftestHttpJson(
        L"GET", port, path, "", token, "application/json; charset=utf-8", "");
}

static SelftestHttpResponse selftestGetJsonEventually(
    uint16_t port, const std::string& path, const std::string& token) {
    SelftestHttpResponse response;
    for (int attempt = 0; attempt < 20; ++attempt) {
        response = selftestGetJson(port, path, token);
        if (response.status != 0) return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return response;
}

static bool selftestSendAll(SOCKET socketHandle, const char* data,
                            size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int request = static_cast<int>(
            std::min<size_t>(size - sent, 64 * 1024));
        const int written = send(socketHandle, data + sent, request, 0);
        if (written <= 0) return false;
        sent += static_cast<size_t>(written);
    }
    return true;
}

static SelftestHttpResponse selftestOversizedHttpBody(
    uint16_t port, const std::string& token, bool chunked) {
    SelftestHttpResponse response;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return response;
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        WSACleanup();
        return response;
    }

    const int timeoutMs = 5000;
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
        closesocket(socketHandle);
        WSACleanup();
        return response;
    }

    std::string headers =
        "POST /api/settings HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) +
        "\r\nContent-Type: application/json\r\nX-DevHub-Token: " + token +
        "\r\nConnection: close\r\n";
    headers += chunked ? "Transfer-Encoding: chunked\r\n\r\n"
                       : "Content-Length: 2097153\r\n\r\n";

    bool sent = selftestSendAll(socketHandle, headers.data(), headers.size());
    if (sent && chunked) {
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 33; ++i) {
            if (!selftestSendAll(socketHandle, "10000\r\n", 7) ||
                !selftestSendAll(socketHandle, chunk.data(), chunk.size()) ||
                !selftestSendAll(socketHandle, "\r\n", 2)) {
                break;
            }
        }
    }
    shutdown(socketHandle, SD_SEND);

    std::string wire;
    char buffer[4096];
    for (;;) {
        const int received = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        wire.append(buffer, static_cast<size_t>(received));
    }
    closesocket(socketHandle);
    WSACleanup();

    const size_t statusBegin = wire.find(' ');
    if (statusBegin != std::string::npos && statusBegin + 4 <= wire.size())
        response.status = static_cast<DWORD>(
            std::strtoul(wire.c_str() + statusBegin + 1, nullptr, 10));
    const size_t bodyBegin = wire.find("\r\n\r\n");
    if (bodyBegin != std::string::npos)
        response.body = wire.substr(bodyBegin + 4);
    return response;
}

int devhub::runAppSelftest(std::string_view domain) {
    const bool runAll = domain == "all";
    const bool runMigration = runAll || domain == "migration";
    const bool runApplicationFixture = runAll || domain == "operations" ||
        domain == "packets" || domain == "knowledge" || domain == "server" ||
        domain == "discord" || domain == "buildrunner";
    // Knowledge health exercises a deliberately separate project created by
    // the release/packet fixture, so that domain declares the prerequisite
    // here instead of depending on aggregate-only execution order.
    const bool runRelease = runAll || domain == "operations" ||
        domain == "packets" || domain == "knowledge";
    const bool runKnowledge = runAll || domain == "knowledge";
    const bool runPortable = runAll || domain == "operations" ||
        domain == "discord";
    if (!runMigration && !runApplicationFixture) {
        std::fprintf(stderr, "unknown selftest domain: %.*s\n",
            static_cast<int>(domain.size()), domain.data());
        return 2;
    }

    int failures = 0;
    auto check = [&](bool cond, const char* what) {
        std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
        if (!cond) ++failures;
    };
    auto requireRow = [&](SQLite::Statement& query, const char* what) {
        const bool found = query.executeStep();
        check(found, what);
        if (!found) throw std::runtime_error(what);
    };
    auto section = [&](const char* name, auto&& body) {
        try {
            body();
        } catch (const std::exception& e) {
            std::printf("  [FAIL] %s section: %s\n", name, e.what());
            ++failures;
        } catch (...) {
            std::printf("  [FAIL] %s section: unknown exception\n", name);
            ++failures;
        }
    };
    auto packetField = [](const std::string& packet, const std::string& label) {
        const std::string marker = "- " + label + ": ";
        const size_t begin = packet.find(marker);
        if (begin == std::string::npos) return std::string();
        const size_t valueBegin = begin + marker.size();
        const size_t end = packet.find('\n', valueBegin);
        return packet.substr(valueBegin, end == std::string::npos
            ? std::string::npos : end - valueBegin);
    };
    auto validPacketSha = [](const std::string& value) {
        if (value.size() != 71 || value.rfind("sha256:", 0) != 0) return false;
        for (size_t i = 7; i < value.size(); ++i) {
            const char ch = value[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                return false;
        }
        return true;
    };

    std::printf("selftest domain: %.*s\n",
        static_cast<int>(domain.size()), domain.data());
    if (runApplicationFixture) {
    std::printf("selftest: suggestion detector\n");
    SuggestionDetector det;
    check(det.analyze("Can you add an option to auto-sort the inventory?").isCandidate,
          "suggestion detected");
    check(det.analyze("the plugin crashes when I open the map").kind == "bug",
          "bug detected");
    check(!det.analyze("lol nice weather today").isCandidate, "chatter ignored");

    std::printf("selftest: optional visual branding\n");
    std::string displayName, displayNameError;
    check(normalizeAppDisplayName("  My Devhub  ", displayName,
                                  &displayNameError) &&
              displayName == "My Devhub" && displayNameError.empty(),
          "visual display name trims surrounding spaces");
    check(resolveAppDisplayName("") == DEVHUB_APP_NAME &&
              resolveAppDisplayName("   ") == DEVHUB_APP_NAME,
          "missing or blank visual name uses canonical XA DevHub");
    check(resolveAppDisplayName("My % Devhub") == "My % Devhub",
          "visual name is complete text rather than a format string");
    check(normalizeAppDisplayName(std::string(48, 'A'), displayName) &&
              displayName.size() == 48 &&
              !normalizeAppDisplayName(std::string(49, 'A'), displayName,
                                       &displayNameError) &&
              !normalizeAppDisplayName(std::string(1024, 'A'), displayName,
                                       &displayNameError) &&
              resolveAppDisplayName(std::string(49, 'A')) == DEVHUB_APP_NAME,
          "visual name accepts its exact bound and rejects oversized values");
    check(!normalizeAppDisplayName("My\nDevhub", displayName,
                                   &displayNameError) &&
              !normalizeAppDisplayName("My \xF0\x9F\x90\xB6 Devhub", displayName,
                                       &displayNameError) &&
              resolveAppDisplayName("My\tDevhub") == DEVHUB_APP_NAME,
          "control or unsupported UTF-8 display names fail safely to XA DevHub");
    }

    std::printf("selftest: database\n");
    std::error_code ec;
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path(ec);
    if (ec || tempRoot.empty()) {
        check(false, "selftest temporary directory is available");
        return 1;
    }
    std::filesystem::path testDir = tempRoot /
        ("devhub_selftest_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(testDir, ec);
    ec.clear();
    std::filesystem::create_directories(testDir, ec);
    if (ec) {
        check(false, "selftest temporary directory can be created");
        return 1;
    }
    std::string dbPath = (testDir / "devhub.db").string();
    if (runMigration) section("migration/schema", [&] {
        const std::string migrationPath = (testDir / "migration-v8.db").string();
        {
            Db fixture(migrationPath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec("DROP TABLE packet_snapshots");
            fixture.raw(lk.token()).exec("ALTER TABLE projects DROP COLUMN codex_skills");
            fixture.raw(lk.token()).exec("ALTER TABLE workflow_runs DROP COLUMN contract_revision");
            fixture.raw(lk.token()).exec("ALTER TABLE verification_evidence DROP COLUMN contract_revision");
            fixture.raw(lk.token()).exec(
                "DROP TRIGGER trg_discord_message_ticket_attachments");
            fixture.raw(lk.token()).exec("DROP TABLE ticket_attachments");
            fixture.raw(lk.token()).exec(
                "DELETE FROM settings WHERE key='discord_admin_user_ids'");
            fixture.setSetting(lk.token(), "discord_admin_user_id", "111111111111111111");
            fixture.raw(lk.token()).exec("DROP TABLE source_merges");
            fixture.raw(lk.token()).exec("PRAGMA user_version=8");
        }
        {
            Db migrated(migrationPath);
            auto lk = migrated.guard();
            check(migrated.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14,
                  "real v8-style database migrates through v9/v10/v11/v12/v13 to v14");
            SQLite::Statement column(migrated.raw(lk.token()), "PRAGMA table_info(projects)");
            bool hasSkills = false;
            while (column.executeStep())
                hasSkills = hasSkills || column.getColumn(1).getString() == "codex_skills";
            SQLite::Statement registry(migrated.raw(lk.token()),
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name='packet_snapshots'");
            SQLite::Statement workflowColumns(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('workflow_runs') "
                "WHERE name='contract_revision'");
            requireRow(workflowColumns, "selftest query workflowColumns at source line 239 returned a row");
            SQLite::Statement evidenceColumns(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('verification_evidence') "
                "WHERE name='contract_revision'");
            requireRow(evidenceColumns, "selftest query evidenceColumns at source line 243 returned a row");
            SQLite::Statement attachments(migrated.raw(lk.token()),
                "SELECT 1 FROM sqlite_master WHERE type='table' "
                "AND name='ticket_attachments'");
            SQLite::Statement attachmentDeleteTrigger(migrated.raw(lk.token()),
                "SELECT 1 FROM sqlite_master WHERE type='trigger' "
                "AND name='trg_discord_message_ticket_attachments'");
            SQLite::Statement sourceAliases(migrated.raw(lk.token()),
                "SELECT 1 FROM sqlite_master WHERE type='table' "
                "AND name='source_merges'");
            check(hasSkills && registry.executeStep() &&
                   workflowColumns.getColumn(0).getInt() == 1 &&
                   evidenceColumns.getColumn(0).getInt() == 1 &&
                   attachments.executeStep() &&
                   attachmentDeleteTrigger.executeStep() &&
                   sourceAliases.executeStep(),
                   "migration creates routing, packet registry, attachments, and contributor aliases");
            check(migrated.getSetting(lk.token(), "discord_admin_user_ids") ==
                      "111111111111111111" &&
                  migrated.getSetting(lk.token(), "discord_admin_user_id", "missing") == "missing",
                   "legacy single Discord admin migrates into editable whitelist");
        }
        const std::string v9MigrationPath =
            (testDir / "migration-v9.db").string();
        {
            Db fixture(v9MigrationPath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec(
                "DROP TRIGGER trg_discord_message_ticket_attachments");
            fixture.raw(lk.token()).exec("DROP TABLE ticket_attachments");
            fixture.raw(lk.token()).exec("DROP TABLE source_merges");
            fixture.raw(lk.token()).exec("PRAGMA user_version=9");
        }
        {
            Db migrated(v9MigrationPath);
            auto lk = migrated.guard();
            SQLite::Statement table(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='ticket_attachments'");
            requireRow(table, "selftest query table at source line 282 returned a row");
            SQLite::Statement trigger(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' "
                "AND name='trg_discord_message_ticket_attachments'");
            requireRow(trigger, "selftest query trigger at source line 286 returned a row");
            SQLite::Statement aliases(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='source_merges'");
            requireRow(aliases, "selftest query aliases at source line 290 returned a row");
            check(migrated.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14 &&
                      table.getColumn(0).getInt() == 1 &&
                      trigger.getColumn(0).getInt() == 1 &&
                      aliases.getColumn(0).getInt() == 1,
                  "persisted v9 database migrates attachment, Discord durability, aliases, cleanup, and acknowledgements to v14");
        }
        const std::string v10MigrationPath =
            (testDir / "migration-v10.db").string();
        {
            Db fixture(v10MigrationPath);
            auto lk = fixture.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement channel(fixture.raw(lk.token()),
                "INSERT INTO discord_channels(channel_id,guild_id,guild_name,"
                "channel_name,created_at) VALUES(?,?,?,?,?)");
            channel.bind(1, "123456789012345600");
            channel.bind(2, "123456789012345601");
            channel.bind(3, "migration guild");
            channel.bind(4, "migration-channel");
            channel.bind(5, now);
            channel.exec();
            const long long channelRow = fixture.raw(lk.token()).getLastInsertRowid();
            SQLite::Statement message(fixture.raw(lk.token()),
                "INSERT INTO discord_messages(channel_row_id,message_id,author,"
                "content,ingested_at,notify_channel_id,notify_message_id) "
                "VALUES(?,?,?,?,?,?,?)");
            message.bind(1, channelRow);
            message.bind(2, "123456789012345602");
            message.bind(3, "migration user");
            message.bind(4, "suggestion: migrate the existing card");
            message.bind(5, now);
            message.bind(6, "123456789012345603");
            message.bind(7, "123456789012345604");
            message.exec();
            fixture.logActivity(lk.token(),
                "discord_manual_capture", 0,
                "migration user: private suggestion body copied by an older build");
            fixture.raw(lk.token()).exec(R"sql(
DROP TRIGGER IF EXISTS trg_discord_message_notify_cards;
DROP TRIGGER IF EXISTS trg_discord_message_notify_card_update;
DROP TRIGGER IF EXISTS trg_discord_channel_notify_cards;
DROP TRIGGER IF EXISTS trg_item_notify_cards;
DROP TRIGGER IF EXISTS trg_project_notify_cards;
DROP TRIGGER IF EXISTS trg_ticket_attachment_notify_card_insert;
DROP TRIGGER IF EXISTS trg_ticket_attachment_notify_card_update;
DROP TRIGGER IF EXISTS trg_ticket_attachment_notify_card_delete;
DROP TRIGGER IF EXISTS trg_discord_notify_card_lineage;
DROP TRIGGER IF EXISTS trg_preserve_deleted_discord_sources;
)sql");
            fixture.raw(lk.token()).exec("DROP TABLE discord_notify_orphans");
            fixture.raw(lk.token()).exec("DROP TABLE discord_notify_cards");
            fixture.raw(lk.token()).exec("DROP INDEX idx_discord_manual_effect");
            fixture.raw(lk.token()).exec("DROP INDEX idx_discord_manual_pending");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_effect_updated_at");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_effect_error");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_effect_next_retry_at");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_effect_attempts");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_effect_state");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_result_images_queued");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_result_item_id");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_result_message_row_id");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_result_kind");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_last_error");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_next_retry_at");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_attempts");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_command_state");
            fixture.raw(lk.token()).exec(
                "ALTER TABLE discord_messages DROP COLUMN manual_target_message_id");
            fixture.raw(lk.token()).exec("DROP TABLE source_merges");
            fixture.raw(lk.token()).exec("PRAGMA user_version=10");
        }
        {
            Db migrated(v10MigrationPath);
            auto lk = migrated.guard();
            SQLite::Statement manualColumns(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('discord_messages') "
                "WHERE name IN ('manual_target_message_id','manual_command_state',"
                "'manual_attempts','manual_next_retry_at','manual_last_error',"
                "'manual_result_kind','manual_result_message_row_id',"
                "'manual_result_item_id','manual_result_images_queued',"
                "'manual_effect_state','manual_effect_attempts',"
                "'manual_effect_next_retry_at','manual_effect_error',"
                "'manual_effect_updated_at')");
            requireRow(manualColumns, "selftest query manualColumns at source line 387 returned a row");
            SQLite::Statement cardOutboxColumns(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('discord_notify_cards') "
                "WHERE name IN ('post_revision','edit_state','edit_attempts','edit_revision',"
                "'edit_next_retry_at','edit_last_error','edit_updated_at')");
            requireRow(cardOutboxColumns, "selftest query cardOutboxColumns at source line 392 returned a row");
            SQLite::Statement orphanColumns(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('discord_notify_orphans') "
                "WHERE name IN ('id','card_row_id','notify_channel_id',"
                "'notify_message_id','cleanup_state','cleanup_attempts',"
                "'cleanup_revision','cleanup_next_retry_at',"
                "'cleanup_last_error','created_at','updated_at')");
            requireRow(orphanColumns, "selftest query orphanColumns at source line 399 returned a row");
            SQLite::Statement reviewCleanupColumns(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM "
                "pragma_table_info('discord_review_control_cleanups') "
                "WHERE name IN ('id','card_row_id','notify_channel_id',"
                "'notify_message_id','cleanup_state','cleanup_attempts',"
                "'cleanup_revision','cleanup_next_retry_at',"
                "'cleanup_last_error','created_at','updated_at')");
            requireRow(reviewCleanupColumns, "selftest query reviewCleanupColumns at source line 407 returned a row");
            SQLite::Statement backfilledCard(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards n "
                "JOIN discord_messages m ON m.id=n.discord_message_row_id "
                "WHERE m.message_id='123456789012345602' "
                "AND n.notify_channel_id='123456789012345603' "
                "AND n.notify_message_id='123456789012345604' "
                "AND n.post_state='posted'");
            requireRow(backfilledCard, "selftest query backfilledCard at source line 415 returned a row");
            SQLite::Statement cardTriggers(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' "
                "AND name IN ('trg_discord_message_notify_cards',"
                "'trg_discord_message_notify_card_update',"
                "'trg_discord_channel_notify_cards',"
                "'trg_item_notify_cards','trg_project_notify_cards',"
                "'trg_ticket_attachment_notify_card_insert',"
                "'trg_ticket_attachment_notify_card_update',"
                "'trg_ticket_attachment_notify_card_delete',"
                "'trg_discord_notify_card_lineage')");
            requireRow(cardTriggers, "selftest query cardTriggers at source line 426 returned a row");
            SQLite::Statement deletionMarkerTrigger(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' "
                "AND name='trg_preserve_deleted_discord_sources'");
            requireRow(deletionMarkerTrigger, "selftest query deletionMarkerTrigger at source line 430 returned a row");
            SQLite::Statement backfillEdit(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards n "
                "JOIN discord_messages m ON m.id=n.discord_message_row_id "
                "WHERE m.message_id='123456789012345602' "
                "AND n.edit_state='pending' AND n.edit_revision=1");
            requireRow(backfillEdit, "selftest query backfillEdit at source line 436 returned a row");
            SQLite::Statement redactedManualActivity(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM activity_log "
                "WHERE kind='discord_manual_capture' "
                "AND detail='Manual Discord capture'");
            requireRow(redactedManualActivity, "selftest query redactedManualActivity at source line 441 returned a row");
            SQLite::Statement sourceAliasSchema(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='source_merges'");
            requireRow(sourceAliasSchema, "selftest query sourceAliasSchema at source line 445 returned a row");
            check(migrated.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14 &&
                      manualColumns.getColumn(0).getInt() == 14 &&
                      cardOutboxColumns.getColumn(0).getInt() == 7 &&
                      orphanColumns.getColumn(0).getInt() == 11 &&
                      reviewCleanupColumns.getColumn(0).getInt() == 11 &&
                      backfilledCard.getColumn(0).getInt() == 1 &&
                      cardTriggers.getColumn(0).getInt() == 9 &&
                      deletionMarkerTrigger.getColumn(0).getInt() == 1 &&
                      backfillEdit.getColumn(0).getInt() == 1 &&
                      redactedManualActivity.getColumn(0).getInt() == 1 &&
                      sourceAliasSchema.getColumn(0).getInt() == 1,
                  "persisted v10 database migrates card durability and contributor aliases");
        }
        const std::string v11MigrationPath =
            (testDir / "migration-v11.db").string();
        long long v11SourceId = 0, v11ItemId = 0;
        {
            Db fixture(v11MigrationPath);
            auto lk = fixture.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement project(fixture.raw(lk.token()),
                "INSERT INTO projects(name,slug,created_at,updated_at) "
                "VALUES('v11 contributor fixture','v11-fixture',?,?)");
            project.bind(1, now);
            project.bind(2, now);
            project.exec();
            const long long fixtureProject = fixture.raw(lk.token()).getLastInsertRowid();
            v11SourceId = ensureSourceLocked(lk.token(),
                &fixture, "v11 contributor", "discord",
                "723456789012345670");
            v11ItemId = insertItemLocked(lk.token(),
                &fixture, fixtureProject, "fix", "preserve v11 attribution",
                "", 1, v11SourceId, "test", "", "");
            fixture.raw(lk.token()).exec("DROP INDEX idx_sources_stable_identity");
            fixture.raw(lk.token()).exec("DROP TABLE source_merges");
            fixture.raw(lk.token()).exec("PRAGMA user_version=11");
        }
        {
            Db migrated(v11MigrationPath);
            auto lk = migrated.guard();
            SQLite::Statement preserved(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM item_sources WHERE item_id=? "
                "AND source_id=?");
            preserved.bind(1, v11ItemId);
            preserved.bind(2, v11SourceId);
            requireRow(preserved, "selftest query preserved at source line 491 returned a row");
            SQLite::Statement aliasIndex(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
                "AND name IN ('idx_source_merges_target',"
                "'idx_sources_stable_identity')");
            requireRow(aliasIndex, "selftest query aliasIndex at source line 496 returned a row");
            check(migrated.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14 &&
                      preserved.getColumn(0).getInt() == 1 &&
                      aliasIndex.getColumn(0).getInt() == 2,
                  "persisted v11 database gains contributor aliases without changing attribution");
        }
        const std::string v12CleanupMigrationPath =
            (testDir / "migration-v12-review-cleanup.db").string();
        long long v12LegacyCardId = 0, v12UnrelatedCardId = 0;
        long long v12CodeCardId = 0;
        {
            Db fixture(v12CleanupMigrationPath);
            seedDefaults(fixture);
            auto lk = fixture.guard();
            {
                SQLite::Statement firstProject(fixture.raw(lk.token()),
                    "SELECT id FROM projects ORDER BY id LIMIT 1");
                requireRow(firstProject, "selftest query firstProject at source line 513 returned a row");
                const long long fixtureProject =
                    firstProject.getColumn(0).getInt64();
                const long long fixtureItem = insertItemLocked(lk.token(),
                    &fixture, fixtureProject, "fix",
                    "v12 legacy cleanup fixture", "", 1, 0, "test", "", "");
                const std::string now = nowIsoUtc();
                SQLite::Statement card(fixture.raw(lk.token()),
                    "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                    "notify_message_id,post_state,edit_state,edit_attempts,"
                    "edit_revision,edit_last_error,edit_updated_at,created_at,updated_at) "
                    "VALUES(?,?,?,'posted','pending',3,1,?,?,?,?)");
                card.bind(1, fixtureItem);
                card.bind(2, "623456789012345670");
                card.bind(3, "623456789012345671");
                card.bind(4,
                    "Maximum number of edits to messages older than 1 hour reached.");
                card.bind(5, now);
                card.bind(6, now);
                card.bind(7, now);
                card.exec();
                v12LegacyCardId = fixture.raw(lk.token()).getLastInsertRowid();
                SQLite::Statement unrelated(fixture.raw(lk.token()),
                    "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                    "notify_message_id,post_state,edit_state,edit_attempts,"
                    "edit_revision,edit_last_error,edit_updated_at,created_at,updated_at) "
                    "VALUES(?,?,?,'posted','failed',5,4,?,?,?,?)");
                unrelated.bind(1, fixtureItem);
                unrelated.bind(2, "623456789012345670");
                unrelated.bind(3, "623456789012345672");
                unrelated.bind(4, "Missing Permissions");
                unrelated.bind(5, now);
                unrelated.bind(6, now);
                unrelated.bind(7, now);
                unrelated.exec();
                v12UnrelatedCardId = fixture.raw(lk.token()).getLastInsertRowid();
                SQLite::Statement codeOnly(fixture.raw(lk.token()),
                    "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                    "notify_message_id,post_state,edit_state,edit_attempts,"
                    "edit_revision,edit_last_error,edit_updated_at,created_at,updated_at) "
                    "VALUES(?,?,?,'posted','failed',5,2,?,?,?,?)");
                codeOnly.bind(1, fixtureItem);
                codeOnly.bind(2, "623456789012345670");
                codeOnly.bind(3, "623456789012345673");
                codeOnly.bind(4, "Discord REST error 30046");
                codeOnly.bind(5, now);
                codeOnly.bind(6, now);
                codeOnly.bind(7, now);
                codeOnly.exec();
                v12CodeCardId = fixture.raw(lk.token()).getLastInsertRowid();
            }
            fixture.raw(lk.token()).exec("DROP TABLE discord_review_control_cleanups");
            fixture.raw(lk.token()).exec(
                "DROP TABLE discord_notify_failure_dismissals");
            fixture.raw(lk.token()).exec("PRAGMA user_version=12");
        }
        {
            Db migrated(v12CleanupMigrationPath);
            auto lk = migrated.guard();
            SQLite::Statement repaired(migrated.raw(lk.token()),
                "SELECT edit_state,edit_attempts,edit_last_error "
                "FROM discord_notify_cards WHERE id=?");
            repaired.bind(1, v12LegacyCardId);
            requireRow(repaired, "selftest query repaired at source line 576 returned a row");
            SQLite::Statement cleanupTable(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='discord_review_control_cleanups'");
            requireRow(cleanupTable, "selftest query cleanupTable at source line 580 returned a row");
            SQLite::Statement legacyAcknowledgement(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_failure_dismissals "
                "WHERE card_row_id=? AND operation='edit' AND revision=1 "
                "AND reason='v14-legacy-old-edit-limit'");
            legacyAcknowledgement.bind(1, v12LegacyCardId);
            requireRow(legacyAcknowledgement, "selftest query legacyAcknowledgement at source line 586 returned a row");
            SQLite::Statement unrelatedAcknowledgement(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_failure_dismissals "
                "WHERE card_row_id=?");
            unrelatedAcknowledgement.bind(1, v12UnrelatedCardId);
            requireRow(unrelatedAcknowledgement, "selftest query unrelatedAcknowledgement at source line 591 returned a row");
            SQLite::Statement codeAcknowledgement(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_failure_dismissals "
                "WHERE card_row_id=? AND operation='edit' AND revision=2 "
                "AND reason='v14-legacy-old-edit-limit'");
            codeAcknowledgement.bind(1, v12CodeCardId);
            requireRow(codeAcknowledgement, "selftest query codeAcknowledgement at source line 597 returned a row");
            check(migrated.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14 &&
                      cleanupTable.getColumn(0).getInt() == 1 &&
                      repaired.getColumn(0).getString() == "failed" &&
                      repaired.getColumn(1).getInt() == 3 &&
                      repaired.getColumn(2).getString().find(
                          "Maximum number of edits") != std::string::npos &&
                      legacyAcknowledgement.getColumn(0).getInt() == 1 &&
                      unrelatedAcknowledgement.getColumn(0).getInt() == 0 &&
                      codeAcknowledgement.getColumn(0).getInt() == 1,
                  "v12 migration stops old-message retries and v14 acknowledges only phrase/code historical edit generations");
        }
        auto rejectsDamagedV12 = [&](const std::string& fileName,
                                     const std::string& damageSql) {
            const std::string path = (testDir / fileName).string();
            try {
                {
                    Db fixture(path);
                    auto lk = fixture.guard();
                    fixture.raw(lk.token()).exec(damageSql);
                }
                Db damaged(path);
            } catch (const std::exception& e) {
                return std::string(e.what()).find("schema v12") !=
                    std::string::npos;
            }
            return false;
        };
        auto repairsOwnedIndex = [&](const std::string& fileName,
                                     const std::string& damageSql,
                                     const std::string& inspectSql,
                                     const std::string& expected) {
            const std::string path = (testDir / fileName).string();
            {
                Db fixture(path);
                auto lk = fixture.guard();
                fixture.raw(lk.token()).exec(damageSql);
            }
            try {
                Db repaired(path);
                auto lk = repaired.guard();
                SQLite::Statement inspect(repaired.raw(lk.token()), inspectSql);
                return inspect.executeStep() &&
                    inspect.getColumn(0).getString() == expected;
            } catch (const std::exception&) {
                return false;
            }
        };
        check(rejectsDamagedV12(
                  "schema-v12-missing-table.db", "DROP TABLE source_merges") &&
              rejectsDamagedV12(
                  "schema-v12-missing-column.db",
                  "DROP TABLE source_merges;"
                  "CREATE TABLE source_merges("
                  "source_id INTEGER PRIMARY KEY REFERENCES sources(id));"
                  "CREATE INDEX idx_source_merges_target "
                  "ON source_merges(source_id)") &&
              rejectsDamagedV12(
                  "schema-v12-missing-foreign-key.db",
                  "DROP TABLE source_merges;"
                  "CREATE TABLE source_merges("
                  "source_id INTEGER PRIMARY KEY,"
                  "target_source_id INTEGER NOT NULL,"
                  "merged_at TEXT NOT NULL);"
                  "CREATE INDEX idx_source_merges_target "
                  "ON source_merges(target_source_id)") &&
              rejectsDamagedV12(
                  "schema-v12-duplicate-source-foreign-key.db",
                  "DROP TABLE source_merges;"
                  "CREATE TABLE source_merges("
                  "source_id INTEGER PRIMARY KEY,"
                  "target_source_id INTEGER NOT NULL,"
                  "merged_at TEXT NOT NULL,"
                  "FOREIGN KEY(source_id) REFERENCES sources(id) ON DELETE RESTRICT,"
                  "FOREIGN KEY(source_id) REFERENCES sources(id) ON DELETE RESTRICT);"
                  "CREATE INDEX idx_source_merges_target "
                  "ON source_merges(target_source_id)") &&
              repairsOwnedIndex(
                  "schema-v12-missing-index.db",
                  "DROP INDEX idx_sources_stable_identity",
                  "SELECT GROUP_CONCAT(name, ',') FROM "
                  "(SELECT name FROM pragma_index_info("
                  "'idx_sources_stable_identity') ORDER BY seqno)",
                  "platform,handle") &&
              repairsOwnedIndex(
                  "schema-v12-wrong-index-column.db",
                  "DROP INDEX idx_source_merges_target;"
                  "CREATE INDEX idx_source_merges_target "
                  "ON source_merges(source_id)",
                  "SELECT GROUP_CONCAT(name, ',') FROM "
                  "(SELECT name FROM pragma_index_info("
                   "'idx_source_merges_target') ORDER BY seqno)",
                  "target_source_id") &&
              repairsOwnedIndex(
                  "schema-v12-unique-stable-index.db",
                  "DROP INDEX idx_sources_stable_identity;"
                  "CREATE UNIQUE INDEX idx_sources_stable_identity "
                  "ON sources(platform COLLATE NOCASE,handle) "
                  "WHERE handle!=''",
                  "SELECT \"unique\" FROM pragma_index_list('sources') "
                  "WHERE name='idx_sources_stable_identity'",
                  "0"),
              "schema v12 repairs owned indexes and rejects malformed contributor tables or foreign keys");
        auto rejectsDamagedV13 = [&](const std::string& fileName,
                                     const std::string& damageSql) {
            const std::string path = (testDir / fileName).string();
            try {
                {
                    Db fixture(path);
                    auto lk = fixture.guard();
                    fixture.raw(lk.token()).exec(damageSql);
                }
                Db damaged(path);
            } catch (const std::exception& e) {
                return std::string(e.what()).find("schema v13") !=
                    std::string::npos;
            }
            return false;
        };
        check(rejectsDamagedV13(
                  "schema-v13-missing-cleanup-table.db",
                  "DROP TABLE discord_review_control_cleanups") &&
              repairsOwnedIndex(
                  "schema-v13-missing-cleanup-index.db",
                  "DROP INDEX idx_discord_review_cleanup_due",
                  "SELECT GROUP_CONCAT(name, ',') FROM "
                  "(SELECT name FROM pragma_index_info("
                  "'idx_discord_review_cleanup_due') ORDER BY seqno)",
                  "cleanup_state,cleanup_next_retry_at,updated_at,id") &&
              repairsOwnedIndex(
                  "schema-v13-wrong-cleanup-identity.db",
                  "DROP INDEX idx_discord_review_cleanup_target;"
                  "CREATE UNIQUE INDEX idx_discord_review_cleanup_target "
                  "ON discord_review_control_cleanups(notify_message_id)",
                  "SELECT GROUP_CONCAT(name, ',') FROM "
                  "(SELECT name FROM pragma_index_info("
                  "'idx_discord_review_cleanup_target') ORDER BY seqno)",
                  "notify_channel_id,notify_message_id") &&
              repairsOwnedIndex(
                  "schema-v13-nonunique-cleanup-identity.db",
                  "DROP INDEX idx_discord_review_cleanup_target;"
                  "CREATE INDEX idx_discord_review_cleanup_target "
                  "ON discord_review_control_cleanups("
                  "notify_channel_id,notify_message_id)",
                  "SELECT \"unique\" FROM pragma_index_list("
                  "'discord_review_control_cleanups') "
                  "WHERE name='idx_discord_review_cleanup_target'",
                  "1"),
              "schema v13 repairs owned indexes and rejects a malformed review-control cleanup table");
        auto rejectsDamagedV14 = [&](const std::string& fileName,
                                     const std::string& damageSql) {
            const std::string path = (testDir / fileName).string();
            try {
                {
                    Db fixture(path);
                    auto lk = fixture.guard();
                    fixture.raw(lk.token()).exec(damageSql);
                }
                Db damaged(path);
            } catch (const std::exception& e) {
                return std::string(e.what()).find("schema v14") !=
                    std::string::npos;
            }
            return false;
        };
        check(rejectsDamagedV14(
                  "schema-v14-missing-dismissal-table.db",
                  "DROP TABLE discord_notify_failure_dismissals") &&
              rejectsDamagedV14(
                  "schema-v14-wrong-dismissal-identity.db",
                  "DROP TABLE discord_notify_failure_dismissals;"
                  "CREATE TABLE discord_notify_failure_dismissals("
                  "card_row_id INTEGER NOT NULL REFERENCES "
                  "discord_notify_cards(id) ON DELETE CASCADE,"
                  "operation TEXT NOT NULL,"
                  "revision INTEGER NOT NULL,"
                  "dismissed_at TEXT NOT NULL,"
                  "reason TEXT NOT NULL DEFAULT 'operator',"
                  "PRIMARY KEY(card_row_id,operation))") &&
              rejectsDamagedV14(
                  "schema-v14-missing-dismissal-foreign-key.db",
                  "DROP TABLE discord_notify_failure_dismissals;"
                  "CREATE TABLE discord_notify_failure_dismissals("
                  "card_row_id INTEGER NOT NULL,"
                  "operation TEXT NOT NULL,"
                  "revision INTEGER NOT NULL,"
                  "dismissed_at TEXT NOT NULL,"
                  "reason TEXT NOT NULL DEFAULT 'operator',"
                  "PRIMARY KEY(card_row_id,operation,revision))") &&
              rejectsDamagedV14(
                  "schema-v14-missing-dismissal-checks.db",
                  "DROP TABLE discord_notify_failure_dismissals;"
                  "CREATE TABLE discord_notify_failure_dismissals("
                  "card_row_id INTEGER NOT NULL REFERENCES "
                  "discord_notify_cards(id) ON DELETE CASCADE,"
                  "operation TEXT NOT NULL,"
                  "revision INTEGER NOT NULL,"
                  "dismissed_at TEXT NOT NULL,"
                  "reason TEXT NOT NULL DEFAULT 'operator',"
                  "PRIMARY KEY(card_row_id,operation,revision))") &&
              rejectsDamagedV14(
                  "schema-v14-missing-dismissal-default.db",
                  "DROP TABLE discord_notify_failure_dismissals;"
                  "CREATE TABLE discord_notify_failure_dismissals("
                  "card_row_id INTEGER NOT NULL REFERENCES "
                  "discord_notify_cards(id) ON DELETE CASCADE,"
                  "operation TEXT NOT NULL "
                  "CHECK(operation IN ('create','edit')),"
                  "revision INTEGER NOT NULL CHECK(revision>=0),"
                  "dismissed_at TEXT NOT NULL,"
                  "reason TEXT NOT NULL,"
                  "PRIMARY KEY(card_row_id,operation,revision))"),
              "schema v14 rejects missing or malformed notification failure acknowledgements");
        const std::string ownerMigrationPath =
            (testDir / "migration-owner-default.db").string();
        {
            Db fixture(ownerMigrationPath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec(
                "DELETE FROM settings WHERE key='discord_admin_user_ids'");
            fixture.raw(lk.token()).exec("PRAGMA user_version=8");
        }
        {
            Db migrated(ownerMigrationPath);
            auto lk = migrated.guard();
            check(migrated.getSetting(lk.token(), "discord_admin_user_ids", "missing").empty(),
                  "existing database without a configured admin stays unprivileged");
        }
        const std::string pluralMigrationPath =
            (testDir / "migration-plural-precedence.db").string();
        {
            Db fixture(pluralMigrationPath);
            auto lk = fixture.guard();
            fixture.setSetting(lk.token(), "discord_admin_user_ids", "");
            fixture.setSetting(lk.token(), "discord_admin_user_id", "111111111111111111");
            fixture.raw(lk.token()).exec("PRAGMA user_version=8");
        }
        {
            Db migrated(pluralMigrationPath);
            auto lk = migrated.guard();
            check(migrated.getSetting(lk.token(), "discord_admin_user_ids", "missing").empty() &&
                      migrated.getSetting(lk.token(), "discord_admin_user_id", "missing") == "missing",
                  "existing plural whitelist wins over conflicting legacy row");
        }
        const std::string earlyMigrationPath =
            (testDir / "migration-v1-missing-column.db").string();
        {
            Db fixture(earlyMigrationPath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec("ALTER TABLE projects DROP COLUMN prep_command");
            fixture.raw(lk.token()).exec("PRAGMA user_version=1");
        }
        {
            Db migrated(earlyMigrationPath);
            auto lk = migrated.guard();
            SQLite::Statement column(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('projects') "
                "WHERE name='prep_command'");
            requireRow(column, "selftest query column at source line 847 returned a row");
            check(column.getColumn(0).getInt() == 1 &&
                      migrated.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14,
                  "early migrations verify missing columns before an atomic version bump");
        }
        const std::string packetRepairPath =
            (testDir / "migration-packet-duplicates.db").string();
        {
            Db fixture(packetRepairPath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec("DROP INDEX idx_packet_snapshot_identity");
            fixture.raw(lk.token()).exec(R"sql(
INSERT INTO packet_snapshots(
 packet_id,kind,scope_type,scope_start,scope_end,source_hash,packet_hash,
 generated_at,created_at)
VALUES
 ('old','review','','','','same-source','old-hash','2026-08-02T00:00:00Z','2026-08-02T00:00:00Z'),
 ('new','review','','','','same-source','new-hash','2026-08-02T00:00:00Z','2026-08-02T00:00:01Z');
)sql");
        }
        {
            Db repaired(packetRepairPath);
            auto lk = repaired.guard();
            SQLite::Statement rows(repaired.raw(lk.token()),
                "SELECT COUNT(*),MIN(packet_id) FROM packet_snapshots "
                "WHERE kind='review' AND source_hash='same-source'");
            requireRow(rows, "selftest query rows at source line 873 returned a row");
            check(rows.getColumn(0).getInt() == 1 &&
                      rows.getColumn(1).getString() == "new",
                  "packet snapshot migration deduplicates before rebuilding identity");
        }
        const std::string packetColumnsPath =
            (testDir / "migration-packet-columns.db").string();
        {
            Db fixture(packetColumnsPath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec(R"sql(
DROP INDEX idx_packet_snapshot_identity;
DROP INDEX idx_packet_snapshot_scope_project;
ALTER TABLE packet_snapshots DROP COLUMN scope_project_id;
ALTER TABLE packet_snapshots DROP COLUMN scope_type;
ALTER TABLE packet_snapshots DROP COLUMN scope_start;
ALTER TABLE packet_snapshots DROP COLUMN scope_end;
)sql");
        }
        {
            Db repaired(packetColumnsPath);
            auto lk = repaired.guard();
            SQLite::Statement columns(repaired.raw(lk.token()),
                "SELECT COUNT(*) FROM pragma_table_info('packet_snapshots') "
                "WHERE name IN ('scope_project_id','scope_type','scope_start','scope_end')");
            requireRow(columns, "selftest query columns at source line 898 returned a row");
            check(columns.getColumn(0).getInt() == 4,
                  "packet snapshot repair adds every scope column before its indexes");
        }
        const std::string downgradePath =
            (testDir / "migration-newer-schema.db").string();
        {
            Db fixture(downgradePath);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec("PRAGMA user_version=15");
        }
        bool downgradeRefused = false;
        try {
            Db olderBuild(downgradePath);
        } catch (const std::exception& e) {
            downgradeRefused =
                std::string(e.what()).find("newer XA DevHub schema") !=
                std::string::npos;
        }
        check(downgradeRefused,
              "newer database schemas are refused before migration writes");
        const std::string plaintextCredentialPath =
            (testDir / "migration-plaintext-credential.db").string();
        {
            Db fixture(plaintextCredentialPath);
            auto lk = fixture.guard();
            fixture.setSetting(lk.token(), "legacy_pat", "legacy-public-patterns");
            SQLite::Statement sealedPublicSource(fixture.raw(lk.token()),
                "SELECT value FROM settings WHERE key='legacy_pat'");
            requireRow(sealedPublicSource,
                "selftest seeded a legacy sealed public setting fixture");
            SQLite::Statement sealedPublicInsert(fixture.raw(lk.token()),
                "INSERT INTO settings(key,value) VALUES('detection_patterns',?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
            sealedPublicInsert.bind(1, sealedPublicSource.getColumn(0).getString());
            sealedPublicInsert.exec();
            fixture.raw(lk.token()).exec(
                "INSERT INTO settings(key,value) VALUES("
                "'discord_bot_token','legacy-plaintext-token') "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        }
        {
            Db migrated(plaintextCredentialPath);
            auto lk = migrated.guard();
            SQLite::Statement raw(migrated.raw(lk.token()),
                "SELECT value FROM settings WHERE key='discord_bot_token'");
            requireRow(raw, "selftest query raw at source line 934 returned a row");
            SQLite::Statement publicRaw(migrated.raw(lk.token()),
                "SELECT value FROM settings WHERE key='detection_patterns'");
            requireRow(publicRaw,
                "selftest query returned the migrated public setting");
            check(raw.getColumn(0).getString().rfind("dpapi:v1:", 0) == 0 &&
                      migrated.getSetting(lk.token(), "discord_bot_token") ==
                          "legacy-plaintext-token" &&
                      publicRaw.getColumn(0).getString() ==
                          "legacy-public-patterns" &&
                      migrated.getSetting(lk.token(), "detection_patterns") ==
                          "legacy-public-patterns",
                  "startup seals credentials and restores public settings to plaintext");
        }
        {
            Db caseSeed(migrationPath);
            {
                auto lk = caseSeed.guard();
                SQLite::Statement ins(caseSeed.raw(lk.token()),
                    "INSERT INTO projects(name,slug,created_at,updated_at) VALUES(?,?,?,?)");
                ins.bind(1, "xa devhub"); ins.bind(2, "xa-devhub-lowercase");
                ins.bind(3, nowIsoUtc()); ins.bind(4, nowIsoUtc()); ins.exec();
            }
            seedDefaults(caseSeed);
            auto lk = caseSeed.guard();
            SQLite::Statement count(caseSeed.raw(lk.token()),
                "SELECT COUNT(*) FROM projects WHERE name='XA DevHub' COLLATE NOCASE");
            requireRow(count, "selftest query count at source line 953 returned a row");
            check(count.getColumn(0).getInt64() == 1,
                  "bootstrap preserves established project records");
        }

        const std::string neutralSeedPath =
            (testDir / "neutral-starter-seed.db").string();
        {
            Db fixture(neutralSeedPath);
            seedDefaults(fixture);
            auto lk = fixture.guard();
            {
                SQLite::Statement starter(fixture.raw(lk.token()),
                    "SELECT COUNT(*) FROM projects WHERE name='My Project' "
                    "AND path='' AND build_command='' AND rules_path=''");
                requireRow(starter, "neutral starter query returned a row");
                check(starter.getColumn(0).getInt64() == 1,
                      "fresh installation receives one path-free neutral starter project");
            }
            fixture.raw(lk.token()).exec("DELETE FROM projects");
            seedDefaults(fixture);
            check(fixture.raw(lk.token()).execAndGet(
                      "SELECT COUNT(*) FROM projects").getInt64() == 0,
                  "deleted starter project is not recreated");
        }

        const std::string versionSeedPath =
            (testDir / "version-command-seed.db").string();
        {
            Db fixture(versionSeedPath);
            seedDefaults(fixture);
            auto lk = fixture.guard();
            fixture.raw(lk.token()).exec(R"sql(
INSERT INTO projects(name,slug,path,created_at,updated_at) VALUES
 ('XA Dashboard','xa-dashboard','C:\Projects\XA Dashboard',datetime('now'),datetime('now')),
 ('XA Sub Manager','xa-sub-manager','C:\Projects\XA Sub Manager',datetime('now'),datetime('now')),
 ('XA DevHub','xa-devhub','C:\Projects\DevHub',datetime('now'),datetime('now')),
 ('XA Database','xa-database','C:\Projects\XA Database',datetime('now'),datetime('now'));
)sql");
            fixture.raw(lk.token()).exec(
                "UPDATE projects SET version_command='python \"Update_Version.py\"' "
                "WHERE name='XA Dashboard'");
            fixture.raw(lk.token()).exec(
                "UPDATE projects SET version_command='' WHERE name='XA Sub Manager'");
            fixture.raw(lk.token()).exec(
                "UPDATE projects SET remote_version_url='' "
                "WHERE name IN ('XA Dashboard','XA Sub Manager')");
            fixture.raw(lk.token()).exec(
                "UPDATE projects SET version_command='' WHERE name='XA DevHub'");
            fixture.raw(lk.token()).exec(
                "UPDATE projects SET version_command='python custom_version.py' "
                "WHERE name='XA Database'");
            const std::string fixtureNow = nowIsoUtc();
            SQLite::Statement legacyOmni(fixture.raw(lk.token()),
                "INSERT INTO projects(name,slug,path,local_version_file,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?)");
            legacyOmni.bind(1, "Discord Omnibot");
            legacyOmni.bind(2, "discord-omnibot");
            legacyOmni.bind(3, "C:\\Projects\\DiscordOmnibot");
            legacyOmni.bind(4, "C:\\Projects\\DiscordOmnibot\\changelog.txt");
            legacyOmni.bind(5, fixtureNow);
            legacyOmni.bind(6, fixtureNow);
            legacyOmni.exec();
            SQLite::Statement customOmni(fixture.raw(lk.token()),
                "INSERT INTO projects(name,slug,path,local_version_file,remote_version_url,"
                "github_url,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)");
            customOmni.bind(1, "Omnibot X");
            customOmni.bind(2, "omnibot-custom");
            customOmni.bind(3, "C:\\custom");
            customOmni.bind(4, "C:\\custom\\version.txt");
            customOmni.bind(5, "https://example.invalid/version");
            customOmni.bind(6, "https://example.invalid/source");
            customOmni.bind(7, fixtureNow);
            customOmni.bind(8, fixtureNow);
            customOmni.exec();
        }
        {
            Db migrated(versionSeedPath);
            auto lk = migrated.guard();
            SQLite::Statement adapters(migrated.raw(lk.token()),
                "SELECT COUNT(*) FROM projects WHERE name IN (?,?) "
                "AND version_command=?");
            adapters.bind(1, "XA Dashboard");
            adapters.bind(2, "XA Sub Manager");
            adapters.bind(3, "python \"Update_Version.py\" --devhub");
            requireRow(adapters, "selftest query adapters at source line 1010 returned a row");
            SQLite::Statement custom(migrated.raw(lk.token()),
                "SELECT version_command FROM projects WHERE name='XA Database'");
            requireRow(custom, "selftest query custom at source line 1013 returned a row");
            check(adapters.getColumn(0).getInt() == 2,
                  "Dashboard and Sub Manager seed the DevHub version adapter");
            check(custom.getColumn(0).getString() == "python custom_version.py",
                  "version command migration preserves user-authored commands");
            SQLite::Statement sources(migrated.raw(lk.token()), R"sql(
SELECT COUNT(*) FROM projects WHERE
 (name='XA Dashboard' AND remote_version_url='https://aethertek.io/downloads/xa-dashboard/latest.json') OR
 (name='XA Sub Manager' AND remote_version_url='https://aethertek.io/downloads/xa-sub-manager/latest.json') OR
 (name='XA DevHub' AND version_command='python "Update_Version.py" --devhub') OR
 (name='Discord Omnibot' AND local_version_file='C:\Projects\DiscordOmnibot\bot.py'
  AND remote_version_url='https://raw.githubusercontent.com/xa-io/discord-omnibot/main/bot.py'
  AND github_url='https://github.com/xa-io/discord-omnibot/blob/main/bot.py')
)sql");
            requireRow(sources, "selftest query sources at source line 1027 returned a row");
            check(sources.getColumn(0).getInt() == 4,
                  "known applications seed canonical local and published version sources");
            SQLite::Statement customSources(migrated.raw(lk.token()),
                "SELECT local_version_file,remote_version_url,github_url "
                "FROM projects WHERE name='Omnibot X'");
            requireRow(customSources, "selftest query customSources at source line 1033 returned a row");
            check(customSources.getColumn(0).getString() == "C:\\custom\\version.txt" &&
                      customSources.getColumn(1).getString() ==
                          "https://example.invalid/version" &&
                      customSources.getColumn(2).getString() ==
                          "https://example.invalid/source",
                  "known-source migration preserves non-empty user-authored references");
        }

        const std::string ticketMenuPath =
            (testDir / "discord-ticket-menu.db").string();
        {
            Db fixture(ticketMenuPath);
            long long activeProject = 0;
            {
                auto lk = fixture.guard();
                auto addProject = [&](const std::string& name, bool archived,
                                      bool discordTickets) {
                    SQLite::Statement ins(fixture.raw(lk.token()),
                        "INSERT INTO projects(name,slug,archived,discord_tickets,"
                        "created_at,updated_at) VALUES(?,?,?,?,?,?)");
                    ins.bind(1, name);
                    ins.bind(2, toLower(name));
                    ins.bind(3, archived ? 1 : 0);
                    ins.bind(4, discordTickets ? 1 : 0);
                    const std::string now = nowIsoUtc();
                    ins.bind(5, now);
                    ins.bind(6, now);
                    ins.exec();
                    return fixture.raw(lk.token()).getLastInsertRowid();
                };

                const long long zeroProject =
                    addProject("Zero Ticket Project", false, true);
                activeProject =
                    addProject("Active Ticket Project", false, true);
                const long long optedOutProject =
                    addProject("Opted Out Project", false, false);
                const long long archivedProject =
                    addProject("Archived Project", true, true);

                const long long zeroCompleted = insertItemLocked(lk.token(),
                    &fixture, zeroProject, "fix", "Xagman completed only", "", 2, 0,
                    "test", "", "");
                setItemStatusLocked(lk.token(), &fixture, zeroCompleted, "completed");
                insertItemLocked(lk.token(), &fixture, zeroProject, "note", "Xagman open note", "",
                                 2, 0, "test", "", "");

                insertItemLocked(lk.token(), &fixture, activeProject, "fix", "Open fix",
                                 "Uses the xagman route for recovery",
                                 2, 0, "test", "", "");
                const long long activeProgress = insertItemLocked(lk.token(),
                    &fixture, activeProject, "implementation",
                    "Xagman implementation in progress", "", 2, 0, "test", "", "");
                setItemStatusLocked(lk.token(), &fixture, activeProgress, "in_progress");
                const long long activeBlocked = insertItemLocked(lk.token(),
                    &fixture, activeProject, "fix", "Blocked fix",
                    "Waiting on XAGMAN handoff", 2, 0,
                    "test", "", "");
                setItemStatusLocked(lk.token(), &fixture, activeBlocked, "blocked");
                const long long activeCompleted = insertItemLocked(lk.token(),
                    &fixture, activeProject, "implementation",
                    "Xagman completed implementation", "", 2, 0, "test", "", "");
                setItemStatusLocked(lk.token(), &fixture, activeCompleted, "completed");
                const long long activeWontDo = insertItemLocked(lk.token(),
                    &fixture, activeProject, "fix", "Xagman won't-do fix", "", 2, 0,
                    "test", "", "");
                setItemStatusLocked(lk.token(), &fixture, activeWontDo, "wont_do");
                insertItemLocked(lk.token(), &fixture, activeProject, "note",
                                 "Xagman open non-ticket note", "", 2, 0, "test", "",
                                 "");

                insertItemLocked(lk.token(), &fixture, optedOutProject, "fix",
                                 "Xagman opted-out open fix", "", 2, 0, "test", "", "");
                insertItemLocked(lk.token(), &fixture, archivedProject, "implementation",
                                 "Xagman archived open implementation", "", 2, 0,
                                 "test", "", "");
            }

            const std::vector<TicketMenuEntry> menu =
                ticketMenuProjects(&fixture);
            check(menu.size() == 1 &&
                      menu[0].projectId == activeProject &&
                      menu[0].name == "Active Ticket Project" &&
                      menu[0].openCount == 3,
                  "ticket menu excludes zero-ticket, archived, and opted-out projects while counting open/in-progress/blocked tickets");

            const std::vector<TicketLine> detail =
                ticketTitlesForProject(&fixture, activeProject);
            int fixCount = 0;
            int implementationCount = 0;
            for (const TicketLine& line : detail) {
                if (line.type == "fix") ++fixCount;
                if (line.type == "implementation") ++implementationCount;
            }
            check(detail.size() == 3 && fixCount == 2 &&
                      implementationCount == 1 &&
                      static_cast<int>(detail.size()) == menu[0].openCount,
                  "ticket menu count matches the active fix/implementation detail projection");

            const TicketSearchResult search =
                searchActiveTicketTitles(&fixture, "xAgMaN", 500);
            const auto hasSearchTitle = [&](const std::string& title) {
                return std::any_of(
                    search.entries.begin(), search.entries.end(),
                    [&](const TicketSearchEntry& entry) {
                        return entry.projectName == "Active Ticket Project" &&
                               entry.title == title;
                    });
            };
            check(search.totalMatches == 3 && search.entries.size() == 3 &&
                      hasSearchTitle("Open fix") &&
                      hasSearchTitle("Xagman implementation in progress") &&
                      hasSearchTitle("Blocked fix"),
                  "ticket search matches title or body case-insensitively while excluding terminal, note, archived, and opted-out work");

            const TicketSearchResult limited =
                searchActiveTicketTitles(&fixture, "xagman", 2);
            check(limited.totalMatches == 3 && limited.entries.size() == 2,
                  "ticket search reports the full match count while bounding projected results");
            check(searchActiveTicketTitles(&fixture, "   ", 500)
                      .entries.empty(),
                  "ticket search rejects an empty normalized query");

            const std::string asciiPreview = ticketSearchTitlePreview(
                std::string(60, 'a'), 50);
            const std::string unicodePreview = ticketSearchTitlePreview(
                "\xC3\xA9\xC3\xA9\xC3\xA9", 2);
            check(asciiPreview == std::string(49, 'a') +
                      "\xE2\x80\xA6" &&
                      unicodePreview == "\xC3\xA9\xE2\x80\xA6" &&
                      ticketSearchTitlePreview("line one\nline two", 50) ==
                          "line one line two",
                  "ticket search title previews are 50-character bounded, UTF-8 safe, and single-line");

            std::vector<TicketSearchEntry> pageEntries;
            for (int i = 0; i < 21; ++i) {
                TicketSearchEntry entry;
                entry.itemId = 100 + i;
                entry.projectName = "Active Ticket Project";
                entry.title = std::string(55, 't') + std::to_string(i);
                pageEntries.push_back(std::move(entry));
            }
            const std::vector<std::string> pages =
                ticketSearchPages(pageEntries, 20, 50);
            check(pages.size() == 2 &&
                      std::count(pages[0].begin(), pages[0].end(), '\n') == 20 &&
                      pages[1].find("I120 | Active Ticket Project | ") == 0 &&
                      pages[0].find(std::string(49, 't') +
                                    "\xE2\x80\xA6") != std::string::npos,
                  "ticket search pages contain only bounded identity, project, and title rows at twenty results per page");
        }

    });

    std::unique_ptr<Db> appDb;
    if (runApplicationFixture) section("application database setup", [&] {
        appDb = std::make_unique<Db>(dbPath);
        seedDefaults(*appDb);
    });
    if (appDb) {
        Db& db = *appDb;
        long long projectId = 0, keptId = 0;
        long long aliceId = 0, bobId = 0, openReviewId = 0;
        long long mergeAlias = 0, mergeTarget = 0, mergeFinal = 0;
        long long imageItemId = 0;
        long long foreignProjectId = 0;
        DiscordAttachmentMeta detectedImage;
        TicketImageSaveResult savedImage;
        TicketImageSaveResult lateProjectionImage;
        section("application and Discord acceptance", [&] {
        long long mergedId = 0;
        long long credentialReviewId = 0;
        {
            auto lk = db.guard();
            check(db.raw(lk.token()).execAndGet("PRAGMA user_version").getInt() == 14,
                  "schema migrated to v14");
            SQLite::Statement q(db.raw(lk.token()), "SELECT id FROM projects ORDER BY id LIMIT 1");
            requireRow(q, "seeded project query returned a row");
            projectId = q.getColumn(0).getInt64();
            SQLite::Statement count(db.raw(lk.token()), "SELECT COUNT(*) FROM projects");
            requireRow(count, "selftest query count at source line 1150 returned a row");
            check(count.getColumn(0).getInt64() == 1, "seeded one neutral project");
            SQLite::Statement starter(db.raw(lk.token()),
                "SELECT COUNT(*) FROM projects WHERE name='My Project' "
                "AND path='' AND build_command='' AND prep_command='' "
                "AND release_command='' AND version_command='' "
                "AND local_version_file='' AND remote_version_url='' "
                "AND github_url='' AND rules_path=''");
            requireRow(starter, "fresh neutral project query returned a row");
            check(starter.getColumn(0).getInt() == 1,
                  "fresh project seed contains no XA or machine-specific configuration");
            db.setSetting(lk.token(), "k", "v");
            check(db.getSetting(lk.token(), "k") == "v", "settings roundtrip");
            db.setSetting(lk.token(), "dashboard_completed_days", "90");
            db.setSetting(lk.token(), "app_display_name", "  My Devhub  ");
            check(resolveAppDisplayName(db.getSetting(lk.token(), "app_display_name")) ==
                      "My Devhub",
                  "visual display name persists through generic settings");
            db.setSetting(lk.token(), "app_display_name", "");
            check(resolveAppDisplayName(db.getSetting(lk.token(), "app_display_name")) ==
                      DEVHUB_APP_NAME,
                  "cleared visual display name restores canonical default");
            check(db.getSetting(lk.token(), "discord_admin_user_ids", "missing").empty() &&
                  db.getSetting(lk.token(), "discord_admin_user_id", "missing") == "missing",
                  "fresh shared database starts with no implicit Discord admin");

            aliceId = ensureSourceLocked(lk.token(), &db, "Alice", "test", "");
            bobId = ensureSourceLocked(lk.token(), &db, "Bob", "test", "");
            const long long stableDiscordSource = ensureSourceLocked(lk.token(),
                &db, "Discord Name Before", "discord", "523456789012345678");
            const long long renamedDiscordSource = ensureSourceLocked(lk.token(),
                &db, "Discord Name After", "discord", "523456789012345678");
            const long long duplicateNameDiscordSource = ensureSourceLocked(lk.token(),
                &db, "Discord Name After", "discord", "523456789012345679");
            SQLite::Statement stableIdentity(db.raw(lk.token()),
                "SELECT COUNT(*),MIN(name) FROM sources WHERE platform='discord' "
                "AND handle IN ('523456789012345678','523456789012345679')");
            requireRow(stableIdentity, "selftest query stableIdentity at source line 1196 returned a row");
            check(stableDiscordSource == renamedDiscordSource &&
                      duplicateNameDiscordSource != stableDiscordSource &&
                      stableIdentity.getColumn(0).getInt() == 2,
                  "Discord contributor identity survives renames and separates duplicate display names");
            keptId = insertItemLocked(lk.token(), &db, projectId, "implementation",
                "Unified request", "Primary details", 3, aliceId, "test", "", "");
            mergedId = insertItemLocked(lk.token(), &db, projectId, "implementation",
                "Duplicate request", "Extra detail", 2, bobId, "test", "", "");
            std::string error;
            check(mergeItemsLocked(lk.token(), &db, mergedId, keptId, error),
                  "duplicate feedback merged");
            SQLite::Statement linked(db.raw(lk.token()),
                "SELECT COUNT(*) FROM item_sources WHERE item_id=?");
            linked.bind(1, keptId);
            requireRow(linked, "merged contributor query returned a row");
            check(linked.getColumn(0).getInt() == 2,
                  "all contributors preserved by merge");
            SQLite::Statement merged(db.raw(lk.token()), "SELECT status FROM items WHERE id=?");
            merged.bind(1, mergedId);
            requireRow(merged, "merged audit item query returned a row");
            check(merged.getColumn(0).getString() == "merged",
                  "source retained as merged audit record");
            bool mergedImmutable = true;
            for (const char* attempted :
                 {"open", "in_progress", "blocked", "completed", "wont_do"}) {
                setItemStatusLocked(lk.token(), &db, mergedId, attempted);
                SQLite::Statement stillMerged(db.raw(lk.token()),
                    "SELECT status FROM items WHERE id=?");
                stillMerged.bind(1, mergedId);
                requireRow(stillMerged,
                           "merged audit item remained queryable");
                mergedImmutable = mergedImmutable &&
                    stillMerged.getColumn(0).getString() == "merged";
            }
            check(mergedImmutable,
                  "merged audit record rejects every public status transition");

            const long long carolId = ensureSourceLocked(
                lk.token(), &db, "Carol", "test", "");
            const long long batchMergeTarget = insertItemLocked(
                lk.token(), &db, projectId, "implementation",
                "Batch merge target", "Primary batch note", 3,
                aliceId, "test", "", "");
            const long long batchMergeFirst = insertItemLocked(
                lk.token(), &db, projectId, "implementation",
                "Batch duplicate one", "First extra note", 2,
                bobId, "test", "", "");
            const long long batchMergeSecond = insertItemLocked(
                lk.token(), &db, projectId, "fix",
                "Batch duplicate two", "Second extra note", 2,
                carolId, "test", "", "");
            setItemStatusLocked(lk.token(), &db, batchMergeSecond, "completed");
            std::string batchMergeError;
            check(mergeItemsLocked(
                      lk.token(), &db,
                      std::vector<long long>{batchMergeFirst, batchMergeSecond},
                      batchMergeTarget, batchMergeError),
                  "multiple tickets including a terminal source merge in one batch");
            SQLite::Statement batchTarget(db.raw(lk.token()),
                "SELECT body,(SELECT COUNT(*) FROM item_sources WHERE item_id=?),"
                "(SELECT COUNT(*) FROM item_merges WHERE target_item_id=?) "
                "FROM items WHERE id=?");
            batchTarget.bind(1, batchMergeTarget);
            batchTarget.bind(2, batchMergeTarget);
            batchTarget.bind(3, batchMergeTarget);
            requireRow(batchTarget, "batch merge target query returned a row");
            const std::string batchBody = batchTarget.getColumn(0).getString();
            const std::size_t firstBatchNote = batchBody.find("First extra note");
            const std::size_t secondBatchNote = batchBody.find("Second extra note");
            SQLite::Statement batchSources(db.raw(lk.token()),
                "SELECT COUNT(*) FROM items WHERE id IN (?,?) AND status='merged'");
            batchSources.bind(1, batchMergeFirst);
            batchSources.bind(2, batchMergeSecond);
            requireRow(batchSources, "batch merge source query returned a row");
            SQLite::Statement batchCompletionEvents(db.raw(lk.token()),
                "SELECT COUNT(*) FROM events WHERE item_id=? AND kind='completion'");
            batchCompletionEvents.bind(1, batchMergeSecond);
            requireRow(batchCompletionEvents,
                       "terminal batch source event query returned a row");
            check(batchBody.find("Primary batch note") != std::string::npos &&
                      firstBatchNote != std::string::npos &&
                      secondBatchNote != std::string::npos &&
                      firstBatchNote < secondBatchNote &&
                      batchTarget.getColumn(1).getInt64() == 3 &&
                      batchTarget.getColumn(2).getInt64() == 2 &&
                      batchSources.getColumn(0).getInt64() == 2 &&
                      batchCompletionEvents.getColumn(0).getInt64() == 0,
                  "batch merge appends every note and preserves contributors, audit rows, and terminal cleanup");
            SQLite::Statement cleanBatchMerge(db.raw(lk.token()),
                "DELETE FROM items WHERE id IN (?,?,?)");
            cleanBatchMerge.bind(1, batchMergeFirst);
            cleanBatchMerge.bind(2, batchMergeSecond);
            cleanBatchMerge.bind(3, batchMergeTarget);
            cleanBatchMerge.exec();

            const long long imageMergeTarget = insertItemLocked(lk.token(),
                &db, projectId, "implementation", "Image merge target", "", 2,
                aliceId, "test", "", "");
            const long long imageMergeSource = insertItemLocked(lk.token(),
                &db, projectId, "implementation", "Image merge source", "", 2,
                bobId, "test", "", "");
            const long long imageMergeExtraSource = insertItemLocked(
                lk.token(), &db, projectId, "implementation",
                "Image merge extra source", "", 2,
                carolId, "test", "", "");
            std::string imageMergeError;
            check(!mergeItemsLocked(
                      lk.token(), &db,
                      std::vector<long long>{imageMergeSource, imageMergeSource},
                      imageMergeTarget, imageMergeError) &&
                      imageMergeError.find("distinct") != std::string::npos,
                  "batch merge rejects duplicate source identities before mutation");
            for (int i = 0; i < 12; ++i) {
                SQLite::Statement image(db.raw(lk.token()),
                    "INSERT INTO ticket_attachments(item_id,source_channel_id,"
                    "source_message_id,attachment_id,source_role,declared_size,"
                    "state,created_at,updated_at) VALUES(?,?,?,?,?,?,"
                    "'queued',?,?)");
                image.bind(1, i < 4 ? imageMergeTarget :
                              (i < 8 ? imageMergeSource : imageMergeExtraSource));
                image.bind(2, "223456789012345678");
                image.bind(3, "223456789012345" + std::to_string(700 + i));
                image.bind(4, "323456789012345" + std::to_string(700 + i));
                image.bind(5, "suggestion");
                image.bind(6, 1);
                const std::string now = nowIsoUtc();
                image.bind(7, now); image.bind(8, now);
                image.exec();
            }
            imageMergeError.clear();
            check(!mergeItemsLocked(
                      lk.token(), &db,
                      std::vector<long long>{imageMergeSource,
                                             imageMergeExtraSource},
                      imageMergeTarget, imageMergeError) &&
                      imageMergeError.find("queued ticket attachments") !=
                          std::string::npos,
                  "batch merge waits for every asynchronous image to settle");
            SQLite::Statement settleImageMerge(db.raw(lk.token()),
                "UPDATE ticket_attachments SET state='saved',"
                "relative_path='ticket-images\\I'||item_id||'\\'||"
                "source_message_id||'-'||attachment_id||'.png',"
                "actual_size=1,sha256=?,saved_at=?,updated_at=? "
                "WHERE item_id IN (?,?,?) AND state='queued'");
            settleImageMerge.bind(1, std::string(64, 'a'));
            const std::string settledAt = nowIsoUtc();
            settleImageMerge.bind(2, settledAt);
            settleImageMerge.bind(3, settledAt);
            settleImageMerge.bind(4, imageMergeSource);
            settleImageMerge.bind(5, imageMergeTarget);
            settleImageMerge.bind(6, imageMergeExtraSource);
            settleImageMerge.exec();
            imageMergeError.clear();
            check(!mergeItemsLocked(
                      lk.token(), &db,
                      std::vector<long long>{imageMergeSource,
                                             imageMergeExtraSource},
                      imageMergeTarget, imageMergeError) &&
                      imageMergeError.find("10-attachment") !=
                          std::string::npos,
                  "batch merge refuses combined live attachment evidence above ticket caps");
            SQLite::Statement atomicBatchFailure(db.raw(lk.token()),
                "SELECT (SELECT COUNT(*) FROM items WHERE id IN (?,?) AND status='open'),"
                "(SELECT COUNT(*) FROM item_merges WHERE source_item_id IN (?,?)),"
                "(SELECT body FROM items WHERE id=?)");
            atomicBatchFailure.bind(1, imageMergeSource);
            atomicBatchFailure.bind(2, imageMergeExtraSource);
            atomicBatchFailure.bind(3, imageMergeSource);
            atomicBatchFailure.bind(4, imageMergeExtraSource);
            atomicBatchFailure.bind(5, imageMergeTarget);
            requireRow(atomicBatchFailure,
                       "failed batch merge atomicity query returned a row");
            check(atomicBatchFailure.getColumn(0).getInt64() == 2 &&
                      atomicBatchFailure.getColumn(1).getInt64() == 0 &&
                      atomicBatchFailure.getColumn(2).getString().empty(),
                  "failed batch merge leaves every source and target unchanged");
            SQLite::Statement cleanImageMerge(db.raw(lk.token()),
                "DELETE FROM items WHERE id IN (?,?,?)");
            cleanImageMerge.bind(1, imageMergeSource);
            cleanImageMerge.bind(2, imageMergeTarget);
            cleanImageMerge.bind(3, imageMergeExtraSource);
            cleanImageMerge.exec();

            setItemStatusLocked(lk.token(), &db, keptId, "completed");
            openReviewId = insertItemLocked(lk.token(),
                &db, projectId, "fix", "Open-only review item", "", 2,
                0, "test", "", "");
            credentialReviewId = insertItemLocked(lk.token(),
                &db, projectId, "fix", "Credential redaction fixture",
                             "discord_token=not-a-real-token-123456\n"
                             "OPENAI_API_KEY=not-a-real-openai-key\n"
                             "COINBASE_API_SECRET=not-a-real-coinbase-secret\n"
                             "$env:GITHUB_TOKEN=not-a-real-github-token\n"
                             "Cancellation token: fix disposal safely\n"
                             "token_count = 5\n"
                             "password validation: keep this engineering context\n"
                             "Visit https://localhost:21100 and email x@y.com\n"
                             "https://not-a-real-user-token@github.com/example/repo\n"
                             "https://example.test/api?access_token=not-a-real-query-token\n"
                             "curl -H \"Authorization: Bearer not-a-real-header-token\" https://example.test\n"
                             "command --api-key=not-a-real-cli-token\n"
                             "authorization:\n  Bearer not-a-real-wrapped-token-123456\n"
                             "Discord bot token: MTIzNDU2Nzg5MDEyMzQ1Njc4.GAbcDe.FgHiJkLmNoPqRsTuVwXyZ\n"
                             "Docs mention -----BEGIN and the PRIVATE KEY----- footer separately.\n"
                             "Evidence after the prose PEM mention remains visible.\n"
                             "-----BEGIN PRIVATE KEY-----\nQUJDREVGR0g=\n"
                             "-----END PRIVATE KEY-----\n"
                             "Evidence after the actual PEM block remains visible.\n"
                             "-----BEGIN PRIVATE KEY-----\nQUJDREVGR0g=\n"
                             "An unterminated PEM cannot consume this evidence.\n"
                             "Nor can it consume later evidence.", 1,
                             0, "test", "", "");
            insertItemLocked(lk.token(), &db, projectId, "fix", "Alice active contribution",
                             "leaderboard active entry", 2, aliceId,
                             "test", "", "");
            insertItemLocked(lk.token(), &db, projectId, "note", "Alice internal note",
                             "must not affect public contribution counts", 1,
                             aliceId, "test", "", "");
            const long long carol = ensureSourceLocked(lk.token(), &db, "Carol", "test", "");
            const long long carolShipped = insertItemLocked(lk.token(),
                &db, projectId, "fix", "Carol shipped contribution", "", 2,
                carol, "test", "", "");
            setItemStatusLocked(lk.token(), &db, carolShipped, "completed");
            SQLite::Statement routed(db.raw(lk.token()),
                "UPDATE projects SET codex_skills=? WHERE id=?");
            routed.bind(1,
                "$devhub-control, invalid!skill, $ffxiv-dalamud-plugin-builder");
            routed.bind(2, projectId);
            routed.exec();
        }
        {
            Db reopened(dbPath);
            auto reopenedLock = reopened.guard();
            check(reopened.getSetting(reopenedLock.token(), "dashboard_completed_days") == "90",
                  "Dashboard completion window persists after database reopen");
        }
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "dashboard_completed_days", "30");
        }

        std::vector<long long> excludedLeaderboardIds;
        std::string leaderboardError;
        check(parseLeaderboardExcludedSourceIds(
                  " 2,1;2\n3 ", excludedLeaderboardIds, &leaderboardError) &&
                  excludedLeaderboardIds == std::vector<long long>{1, 2, 3} &&
                  !parseLeaderboardExcludedSourceIds(
                      "1,not-an-id", excludedLeaderboardIds,
                      &leaderboardError) &&
                  excludedLeaderboardIds.empty(),
              "leaderboard exclusion preference parses canonically and fails closed");
        std::vector<LeaderboardEntry> leaderboard =
            contributorLeaderboard(&db, 10, &leaderboardError);
        check(leaderboardError.empty() && leaderboard.size() >= 3 &&
                  leaderboard[0].name == "Alice" &&
                  leaderboard[0].submitted == 2 &&
                  leaderboard[0].shipped == 1 &&
                  leaderboard[1].name == "Bob" &&
                  leaderboard[1].submitted == 1 &&
                  leaderboard[1].shipped == 1 &&
                  leaderboard[2].name == "Carol" &&
                  leaderboard[2].submitted == 1 &&
                  leaderboard[2].shipped == 1 &&
                  contributorLeaderboard(&db, 2).size() == 2,
              "leaderboard counts non-merged fixes and implementations with deterministic bounds");
        const std::vector<LeaderboardEntry> exactRenderRows = {
            {1, " @`*_~|[]<>\\\r\n\tAlice `@", 10, 4},
            {2, "Bob", 5, 3}
        };
        const std::string exactCompactLeaderboard =
            compactLeaderboardDescription(exactRenderRows);
        std::vector<LeaderboardEntry> boundedRenderRows;
        for (int i = 0; i <= 10; ++i)
            boundedRenderRows.push_back(
                {i + 1, "User" + std::to_string(i), i + 10, i});
        const std::string boundedCompactLeaderboard =
            compactLeaderboardDescription(boundedRenderRows);
        check(exactCompactLeaderboard ==
                  "Alice \xE2\x80\x94 4/10 Implemented\n"
                  "Bob \xE2\x80\x94 3/5 Implemented\n" &&
              std::count(boundedCompactLeaderboard.begin(),
                         boundedCompactLeaderboard.end(), '\n') == 10 &&
              boundedCompactLeaderboard.find("User10") == std::string::npos &&
              exactCompactLeaderboard.find('*') == std::string::npos,
              "compact leaderboard renders exact plain text and enforces its ten-row bound");
        const bool excludedBob = setLeaderboardSourceExcluded(
            &db, bobId, true, &leaderboardError);
        const std::vector<LeaderboardEntry> publicLeaderboard =
            contributorLeaderboard(&db, 10);
        check(excludedBob &&
                  std::none_of(publicLeaderboard.begin(), publicLeaderboard.end(),
                               [](const LeaderboardEntry& entry) {
                                   return entry.name == "Bob";
                               }),
              "leaderboard exclusions hide only selected contributor output");
        {
            Db reopened(dbPath);
            std::vector<long long> reopenedExcluded;
            check(loadLeaderboardExcludedSourceIds(
                      &reopened, reopenedExcluded, &leaderboardError) &&
                      std::binary_search(reopenedExcluded.begin(),
                                         reopenedExcluded.end(), bobId),
                  "leaderboard exclusions survive database reopen");
        }
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "leaderboard_excluded_source_ids", "1, malformed");
        }
        leaderboardError.clear();
        check(contributorLeaderboard(&db, 10, &leaderboardError).empty() &&
                  !leaderboardError.empty(),
              "malformed leaderboard exclusion policy exposes no contributor names");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "leaderboard_excluded_source_ids",
                          std::to_string(bobId));
        }

        long long mergeItemPaid = 0, mergeItemOwed = 0;
        {
            auto lk = db.guard();
            mergeAlias = ensureSourceLocked(lk.token(),
                &db, "Merge Source", "discord", "623456789012345670");
            mergeTarget = ensureSourceLocked(lk.token(),
                &db, "Merge Target", "discord", "623456789012345671");
            mergeFinal = ensureSourceLocked(lk.token(),
                &db, "Merge Final", "discord", "623456789012345672");
            mergeItemPaid = insertItemLocked(lk.token(),
                &db, projectId, "fix", "Contributor merge paid overlap", "",
                1, mergeAlias, "test", "", "");
            mergeItemOwed = insertItemLocked(lk.token(),
                &db, projectId, "fix", "Contributor merge owed", "",
                1, mergeAlias, "test", "", "");
            setItemStatusLocked(lk.token(), &db, mergeItemPaid, "wont_do");
            setItemStatusLocked(lk.token(), &db, mergeItemOwed, "wont_do");
            setItemSourceCreditedLocked(lk.token(),
                &db, mergeItemPaid, mergeAlias, true);
            addItemSourceLocked(lk.token(), &db, mergeItemPaid, mergeTarget);
            SQLite::Statement sourceTime(db.raw(lk.token()),
                "UPDATE item_sources SET added_at='2020-01-01T00:00:00Z' "
                "WHERE item_id=? AND source_id=?");
            sourceTime.bind(1, mergeItemPaid);
            sourceTime.bind(2, mergeAlias);
            sourceTime.exec();
            SQLite::Statement targetTime(db.raw(lk.token()),
                "UPDATE item_sources SET added_at='2021-01-01T00:00:00Z' "
                "WHERE item_id=? AND source_id=?");
            targetTime.bind(1, mergeItemPaid);
            targetTime.bind(2, mergeTarget);
            targetTime.exec();
            db.setSetting(lk.token(), "leaderboard_excluded_source_ids",
                          std::to_string(mergeAlias));
        }
        std::string sourceMergeError;
        bool firstSourceMerge = false;
        {
            auto lk = db.guard();
            firstSourceMerge = mergeSourcesLocked(lk.token(),
                &db, mergeAlias, mergeTarget, sourceMergeError);
            SQLite::Statement links(db.raw(lk.token()), R"sql(
SELECT
 SUM(CASE WHEN source_id=? THEN 1 ELSE 0 END),
 SUM(CASE WHEN source_id=? THEN 1 ELSE 0 END)
FROM item_sources WHERE item_id IN (?,?))sql");
            links.bind(1, mergeAlias);
            links.bind(2, mergeTarget);
            links.bind(3, mergeItemPaid);
            links.bind(4, mergeItemOwed);
            requireRow(links, "selftest query links at source line 1468 returned a row");
            SQLite::Statement overlap(db.raw(lk.token()),
                "SELECT COUNT(*),MAX(credited),MIN(added_at) FROM item_sources "
                "WHERE item_id=? AND source_id=?");
            overlap.bind(1, mergeItemPaid);
            overlap.bind(2, mergeTarget);
            requireRow(overlap, "selftest query overlap at source line 1474 returned a row");
            SQLite::Statement legacy(db.raw(lk.token()),
                "SELECT SUM(source_id=?),SUM(credited) FROM items WHERE id IN (?,?)");
            legacy.bind(1, mergeTarget);
            legacy.bind(2, mergeItemPaid);
            legacy.bind(3, mergeItemOwed);
            requireRow(legacy, "selftest query legacy at source line 1480 returned a row");
            SQLite::Statement lineage(db.raw(lk.token()),
                "SELECT COUNT(*) FROM source_merges WHERE source_id=? "
                "AND target_source_id=?");
            lineage.bind(1, mergeAlias);
            lineage.bind(2, mergeTarget);
            requireRow(lineage, "selftest query lineage at source line 1486 returned a row");
            check(firstSourceMerge && sourceMergeError.empty() &&
                      links.getColumn(0).getInt() == 0 &&
                      links.getColumn(1).getInt() == 2 &&
                      overlap.getColumn(0).getInt() == 1 &&
                      overlap.getColumn(1).getInt() == 1 &&
                      overlap.getColumn(2).getString() ==
                          "2020-01-01T00:00:00Z" &&
                      legacy.getColumn(0).getInt() == 2 &&
                      legacy.getColumn(1).getInt() == 1 &&
                      lineage.getColumn(0).getInt() == 1 &&
                      db.getSetting(lk.token(), "leaderboard_excluded_source_ids") ==
                          std::to_string(mergeTarget),
                  "contributor merge transfers unique attribution, paid state, legacy fields, lineage, and privacy");

            const long long stableAlias = ensureSourceLocked(lk.token(),
                &db, "Merge Source Renamed", "discord",
                "623456789012345670");
            const long long manualAlias = ensureSourceLocked(lk.token(),
                &db, "Merge Source", "", "");
            SQLite::Statement survivorName(db.raw(lk.token()),
                "SELECT name FROM sources WHERE id=?");
            survivorName.bind(1, mergeTarget);
            requireRow(survivorName, "selftest query survivorName at source line 1509 returned a row");
            check(stableAlias == mergeTarget && manualAlias == mergeTarget &&
                      survivorName.getColumn(0).getString() == "Merge Target",
                  "merged identity resolves by old handle and name without renaming survivor");

            const long long renamedSurvivor = ensureSourceLocked(lk.token(),
                &db, "Merge Source", "discord", "623456789012345671");
            const long long unrelatedSameName = ensureSourceLocked(lk.token(),
                &db, "Merge Source", "discord", "623456789012345679");
            SQLite::Statement renamedSurvivorName(db.raw(lk.token()),
                "SELECT name FROM sources WHERE id=?");
            renamedSurvivorName.bind(1, mergeTarget);
            requireRow(renamedSurvivorName, "selftest query renamedSurvivorName at source line 1521 returned a row");
            check(renamedSurvivor == mergeTarget &&
                      unrelatedSameName != mergeTarget &&
                      renamedSurvivorName.getColumn(0).getString() ==
                          "Merge Target",
                  "stable survivor mapping keeps immutable alias history and separates a third Discord identity");

            const int replayLineageBefore = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM source_merges").getInt();
            const int replayActivityBefore = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM activity_log WHERE kind='source_merged'").getInt();
            const int replayAttributionBefore = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM item_sources").getInt();
            const std::string replayExclusionsBefore =
                db.getSetting(lk.token(), "leaderboard_excluded_source_ids");
            sourceMergeError.clear();
            const bool replayedSourceMerge = mergeSourcesLocked(lk.token(),
                &db, mergeAlias, mergeTarget, sourceMergeError);
            check(replayedSourceMerge && sourceMergeError.empty() &&
                      db.raw(lk.token()).execAndGet(
                          "SELECT COUNT(*) FROM source_merges").getInt() ==
                          replayLineageBefore &&
                      db.raw(lk.token()).execAndGet(
                          "SELECT COUNT(*) FROM activity_log "
                          "WHERE kind='source_merged'").getInt() ==
                          replayActivityBefore &&
                      db.raw(lk.token()).execAndGet(
                          "SELECT COUNT(*) FROM item_sources").getInt() ==
                          replayAttributionBefore &&
                      db.getSetting(lk.token(), "leaderboard_excluded_source_ids") ==
                          replayExclusionsBefore,
                  "exact contributor merge replay is an idempotent no-op");
            sourceMergeError.clear();
            check(mergeSourcesLocked(lk.token(),
                      &db, mergeTarget, mergeFinal, sourceMergeError),
                  "surviving contributor can be merged again");
            SQLite::Statement chain(db.raw(lk.token()),
                "SELECT COUNT(*) FROM source_merges WHERE "
                "(source_id=? AND target_source_id=?) OR "
                "(source_id=? AND target_source_id=?)");
            chain.bind(1, mergeAlias);
            chain.bind(2, mergeTarget);
            chain.bind(3, mergeTarget);
            chain.bind(4, mergeFinal);
            requireRow(chain, "selftest query chain at source line 1565 returned a row");
            const long long chainedAlias = ensureSourceLocked(lk.token(),
                &db, "Merge Source Later", "discord", "623456789012345670");
            const long long chainedTarget = ensureSourceLocked(lk.token(),
                &db, "Merge Target Later", "discord", "623456789012345671");
            check(chain.getColumn(0).getInt() == 2 &&
                      chainedAlias == mergeFinal && chainedTarget == mergeFinal &&
                      db.getSetting(lk.token(), "leaderboard_excluded_source_ids") ==
                          std::to_string(mergeFinal),
                  "contributor merge chains preserve direct history and resolve every stable alias to the final survivor");
            sourceMergeError.clear();
            check(!mergeSourcesLocked(lk.token(),
                      &db, mergeAlias, unrelatedSameName, sourceMergeError) &&
                      !sourceMergeError.empty(),
                  "a stale alias cannot redirect its canonical contributor to a conflicting target");

            const long long targetHiddenSource = ensureSourceLocked(lk.token(),
                &db, "Target-hidden source", "test", "");
            const long long targetHiddenTarget = ensureSourceLocked(lk.token(),
                &db, "Target-hidden survivor", "test", "");
            db.setSetting(lk.token(), "leaderboard_excluded_source_ids",
                          std::to_string(targetHiddenTarget));
            sourceMergeError.clear();
            check(mergeSourcesLocked(lk.token(),
                      &db, targetHiddenSource, targetHiddenTarget,
                      sourceMergeError) &&
                      db.getSetting(lk.token(), "leaderboard_excluded_source_ids") ==
                          std::to_string(targetHiddenTarget),
                  "target-only leaderboard exclusion remains hidden after merge");

            sourceMergeError.clear();
            check(!mergeSourcesLocked(lk.token(),
                      &db, mergeFinal, mergeFinal, sourceMergeError) &&
                      !sourceMergeError.empty(),
                  "contributor merge rejects a self target");
            sourceMergeError.clear();
            check(!mergeSourcesLocked(lk.token(),
                      &db, 9223372036854770000LL, mergeFinal,
                      sourceMergeError) && !sourceMergeError.empty(),
                  "contributor merge rejects a missing source");

            const long long malformedSource = ensureSourceLocked(lk.token(),
                &db, "Malformed Merge Source", "test", "");
            const long long malformedTarget = ensureSourceLocked(lk.token(),
                &db, "Malformed Merge Target", "test", "");
            const long long malformedItem = insertItemLocked(lk.token(),
                &db, projectId, "note", "Malformed exclusion merge", "", 1,
                malformedSource, "test", "", "");
            setItemStatusLocked(lk.token(), &db, malformedItem, "wont_do");
            db.setSetting(lk.token(), "leaderboard_excluded_source_ids", "1, malformed");
            const int malformedActivityBefore = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM activity_log WHERE kind='source_merged'").getInt();
            sourceMergeError.clear();
            const bool malformedMerged = mergeSourcesLocked(lk.token(),
                &db, malformedSource, malformedTarget, sourceMergeError);
            SQLite::Statement unchanged(db.raw(lk.token()), R"sql(
SELECT
 (SELECT COUNT(*) FROM item_sources WHERE item_id=? AND source_id=?),
 (SELECT COUNT(*) FROM item_sources WHERE item_id=? AND source_id=?),
 (SELECT source_id FROM items WHERE id=?),
 (SELECT credited FROM items WHERE id=?),
 (SELECT COUNT(*) FROM source_merges WHERE source_id=?))sql");
            unchanged.bind(1, malformedItem);
            unchanged.bind(2, malformedSource);
            unchanged.bind(3, malformedItem);
            unchanged.bind(4, malformedTarget);
            unchanged.bind(5, malformedItem);
            unchanged.bind(6, malformedItem);
            unchanged.bind(7, malformedSource);
            requireRow(unchanged, "selftest query unchanged at source line 1634 returned a row");
            check(!malformedMerged && !sourceMergeError.empty() &&
                      unchanged.getColumn(0).getInt() == 1 &&
                      unchanged.getColumn(1).getInt() == 0 &&
                      unchanged.getColumn(2).getInt64() == malformedSource &&
                      unchanged.getColumn(3).getInt() == 0 &&
                      unchanged.getColumn(4).getInt() == 0 &&
                      db.getSetting(lk.token(), "leaderboard_excluded_source_ids") ==
                          "1, malformed" &&
                      db.raw(lk.token()).execAndGet(
                          "SELECT COUNT(*) FROM activity_log "
                          "WHERE kind='source_merged'").getInt() ==
                          malformedActivityBefore,
                  "malformed leaderboard privacy policy aborts contributor merge without mutation");
            db.setSetting(lk.token(), "leaderboard_excluded_source_ids",
                          std::to_string(bobId));
        }
        leaderboardError.clear();
        const std::vector<LeaderboardEntry> mergedLeaderboard =
            contributorLeaderboard(&db, 100, &leaderboardError);
        const auto finalEntry = std::find_if(
            mergedLeaderboard.begin(), mergedLeaderboard.end(),
            [mergeFinal](const LeaderboardEntry& entry) {
                return entry.sourceId == mergeFinal;
            });
        check(leaderboardError.empty() &&
                  finalEntry != mergedLeaderboard.end() &&
                  finalEntry->submitted == 2 && finalEntry->shipped == 0 &&
                  std::none_of(
                      mergedLeaderboard.begin(), mergedLeaderboard.end(),
                      [mergeAlias, mergeTarget](const LeaderboardEntry& entry) {
                          return entry.sourceId == mergeAlias ||
                                 entry.sourceId == mergeTarget;
                      }),
              "leaderboard shows one canonical contributor with unique merged ticket totals");

        std::vector<std::string> adminIds;
        std::string adminError;
        check(parseDiscordAdminUserIds(
                  "223456789012345678, 123456789012345678\r\n"
                  "987654321098765432; 123456789012345678",
                  adminIds, &adminError) &&
              adminIds == std::vector<std::string>{
                  "223456789012345678", "123456789012345678",
                  "987654321098765432"},
              "Discord admin whitelist parses separators and deduplicates IDs");
        check(parseDiscordAdminUserIds(" \t\r\n", adminIds, &adminError) &&
                  adminIds.empty(),
              "empty Discord admin whitelist is valid and disables privileges");
        auto invalidAdminList = [&](const std::string& value) {
            return !parseDiscordAdminUserIds(value, adminIds, &adminError) &&
                   adminIds.empty() && !adminError.empty();
        };
        check(invalidAdminList(",,;") &&
                  invalidAdminList("<@223456789012345678>") &&
                  invalidAdminList("0") &&
                  invalidAdminList("-223456789012345678") &&
                  invalidAdminList("18446744073709551616") &&
                  invalidAdminList("223456789012345678, not-an-id"),
              "malformed Discord admin whitelists fail as a whole");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_admin_user_ids",
                          "223456789012345678, 123456789012345678");
        }
        check(discordAdminUserAllowed(&db, "223456789012345678") &&
                  discordAdminUserAllowed(&db, "123456789012345678") &&
                  !discordAdminUserAllowed(&db, "22345678901234567") &&
                  !discordAdminUserAllowed(&db, "9223456789012345678"),
              "Discord admin authorization uses exact multi-user membership");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_admin_user_ids", "987654321098765432");
        }
        check(discordAdminUserAllowed(&db, "987654321098765432") &&
                  !discordAdminUserAllowed(&db, "223456789012345678"),
              "Discord admin whitelist changes apply without reconnecting");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_admin_user_ids",
                          "987654321098765432, malformed");
        }
        check(!discordAdminUserAllowed(&db, "987654321098765432"),
              "partially malformed saved whitelist fails closed at runtime");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_admin_user_ids", "");
            db.setSetting(lk.token(), "app_display_name", "My Devhub");
        }
        check(!discordAdminUserAllowed(&db, "223456789012345678") &&
                  !discordAdminUserAllowed(&db, "987654321098765432"),
              "cleared Discord admin whitelist authorizes nobody");
        {
            Db reopened(dbPath);
            auto lk = reopened.guard();
            check(reopened.getSetting(lk.token(), "discord_admin_user_ids", "missing").empty(),
                  "cleared Discord admin whitelist remains empty after restart");
            check(resolveAppDisplayName(
                      reopened.getSetting(lk.token(), "app_display_name")) == "My Devhub",
                  "visual display name remains effective after database reopen");
        }
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_admin_user_ids",
                          "123456789012345678, 987654321098765432");
        }

        // A restored or manually edited malformed detector setting must not
        // escape ingest. The hardened parser ignores it and falls back to the
        // built-in patterns.
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "detection_patterns", "[1]");
        }
        IngestOutcome malformedPatternIngest = ingestDiscordMessage(
            &db, "423456789012345660", "detector-fixture",
            "423456789012345661", "test guild", "423456789012345662",
            "Detector Fixture", "423456789012345663",
            "feature request: retain safe fallback detection",
            "2026-08-02T14:00:00Z", false, {});
        check(malformedPatternIngest.ingested &&
                  !malformedPatternIngest.failed &&
                  malformedPatternIngest.kind == "suggestion",
              "malformed persisted detection patterns fall back without disabling ingest");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "detection_patterns", "");
            db.raw(lk.token()).exec(R"sql(
CREATE TRIGGER selftest_ingest_failure
BEFORE INSERT ON discord_messages
WHEN NEW.message_id='423456789012345665'
BEGIN SELECT RAISE(ABORT,'forced ingest failure'); END
)sql");
        }
        IngestOutcome forcedIngestFailure = ingestDiscordMessage(
            &db, "423456789012345664", "failed-ingest-fixture",
            "423456789012345661", "test guild", "423456789012345665",
            "Failure Fixture", "423456789012345666",
            "feature request: this insert must roll back",
            "2026-08-02T14:01:00Z", false, {});
        int failedMessageRows = 0, failedChannelRows = 0;
        {
            auto lk = db.guard();
            failedMessageRows = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM discord_messages "
                "WHERE message_id='423456789012345665'").getInt();
            failedChannelRows = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM discord_channels "
                "WHERE channel_id='423456789012345664'").getInt();
            db.raw(lk.token()).exec("DROP TRIGGER selftest_ingest_failure");
        }
        check(forcedIngestFailure.failed &&
                  !forcedIngestFailure.ingested &&
                  !forcedIngestFailure.duplicate &&
                  !forcedIngestFailure.error.empty() &&
                  failedMessageRows == 0 && failedChannelRows == 0,
              "non-duplicate ingest failures surface and roll back message and channel state");

        const std::string editMessageId = "423456789012345680";
        IngestOutcome editSeed = ingestDiscordMessage(
            &db, "423456789012345681", "edited-message-fixture",
            "423456789012345661", "test guild", editMessageId,
            "Edit Fixture", "423456789012345682",
            "ordinary discussion before an edit",
            "2026-08-02T14:02:00Z", false, {});
        IngestOutcome becameSuggestion = reconcileDiscordMessageEdit(
            &db, "423456789012345681", "edited-message-fixture",
            "423456789012345661", "test guild", editMessageId,
            "Edit Fixture", "423456789012345682",
            "feature request: add durable edit reconciliation",
            "2026-08-02T14:02:00Z", {});
        IngestOutcome editReplay = reconcileDiscordMessageEdit(
            &db, "423456789012345681", "edited-message-fixture",
            "423456789012345661", "test guild", editMessageId,
            "Edit Fixture", "423456789012345682",
            "feature request: add durable edit reconciliation",
            "2026-08-02T14:02:00Z", {});
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement card(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(discord_message_row_id,"
                "notify_channel_id,notify_message_id,post_state,created_at,updated_at) "
                "VALUES(?,?,?,'posted',?,?)");
            card.bind(1, becameSuggestion.messageRowId);
            card.bind(2, "423456789012345683");
            card.bind(3, "423456789012345684");
            card.bind(4, now);
            card.bind(5, now);
            card.exec();
        }
        IngestOutcome changedSuggestion = reconcileDiscordMessageEdit(
            &db, "423456789012345681", "edited-message-fixture",
            "423456789012345661", "test guild", editMessageId,
            "Renamed Edit Fixture", "423456789012345682",
            "feature request: keep the edited source and card current",
            "2026-08-02T14:02:00Z", {});
        std::string editedKind, editedContent, editState;
        int editRevision = 0;
        {
            auto lk = db.guard();
            SQLite::Statement message(db.raw(lk.token()),
                "SELECT kind,content,state FROM discord_messages WHERE id=?");
            message.bind(1, becameSuggestion.messageRowId);
            requireRow(message,
                "selftest edited Discord source remains queryable");
            editedKind = message.getColumn(0).getString();
            editedContent = message.getColumn(1).getString();
            editState = message.getColumn(2).getString();
            SQLite::Statement card(db.raw(lk.token()),
                "SELECT edit_revision FROM discord_notify_cards "
                "WHERE discord_message_row_id=?");
            card.bind(1, becameSuggestion.messageRowId);
            requireRow(card,
                "selftest edited Discord source retains its card lineage");
            editRevision = card.getColumn(0).getInt();
        }
        const nlohmann::json promotedEdit = promoteSuggestion(
            &db, nullptr, becameSuggestion.messageRowId, projectId,
            "implementation", "edited Discord source fixture", "", 2);
        const long long promotedEditItem = promotedEdit.value("item_id", 0LL);
        IngestOutcome editAfterPromotion = reconcileDiscordMessageEdit(
            &db, "423456789012345681", "edited-message-fixture",
            "423456789012345661", "test guild", editMessageId,
            "Renamed Edit Fixture", "423456789012345682",
            "ordinary wording after promotion",
            "2026-08-02T14:02:00Z", {});
        std::string promotedState, promotedContent;
        long long retainedEditItem = 0;
        {
            auto lk = db.guard();
            SQLite::Statement promoted(db.raw(lk.token()),
                "SELECT state,content,COALESCE(item_id,0) "
                "FROM discord_messages WHERE id=?");
            promoted.bind(1, becameSuggestion.messageRowId);
            requireRow(promoted,
                "selftest promoted edited source remains queryable");
            promotedState = promoted.getColumn(0).getString();
            promotedContent = promoted.getColumn(1).getString();
            retainedEditItem = promoted.getColumn(2).getInt64();
        }
        discordMessageDeleted(&db, nullptr, editMessageId);
        IngestOutcome editAfterDelete = reconcileDiscordMessageEdit(
            &db, "423456789012345681", "edited-message-fixture",
            "423456789012345661", "test guild", editMessageId,
            "Edit Fixture", "423456789012345682",
            "feature request: a tombstone must never reopen",
            "2026-08-02T14:02:00Z", {});
        std::string deletedEditState, deletedEditContent;
        long long deletedEditItem = 0;
        {
            auto lk = db.guard();
            SQLite::Statement deleted(db.raw(lk.token()),
                "SELECT state,content,COALESCE(item_id,0) "
                "FROM discord_messages WHERE id=?");
            deleted.bind(1, becameSuggestion.messageRowId);
            requireRow(deleted,
                "selftest edited source tombstone remains queryable");
            deletedEditState = deleted.getColumn(0).getString();
            deletedEditContent = deleted.getColumn(1).getString();
            deletedEditItem = deleted.getColumn(2).getInt64();
        }
        check(editSeed.ingested && editSeed.kind == "none" &&
                  becameSuggestion.ingested &&
                  becameSuggestion.kind == "suggestion" &&
                  editReplay.duplicate && !editReplay.ingested &&
                  changedSuggestion.ingested && editedKind == "suggestion" &&
                  editedContent.find("edited source") != std::string::npos &&
                  editState == "new" && editRevision > 0 &&
                  promotedEdit.value("ok", false) && promotedEditItem > 0 &&
                  editAfterPromotion.ingested && promotedState == "promoted" &&
                  promotedContent == "ordinary wording after promotion" &&
                  retainedEditItem == promotedEditItem &&
                  editAfterDelete.duplicate && !editAfterDelete.ingested &&
                  deletedEditState == "deleted" && deletedEditContent.empty() &&
                  deletedEditItem == promotedEditItem,
              "Discord edits detect new candidates refresh cards remain idempotent preserve promotion and reject tombstone replay");

        section("historical ticket import", [&] {
            const std::filesystem::path importSourceRoot =
                testDir / "historical-import-source";
            const std::filesystem::path importDestinationRoot =
                testDir / "historical-import-destination";
            std::filesystem::create_directories(importSourceRoot);
            std::filesystem::create_directories(importDestinationRoot);
            Db importSource((importSourceRoot / "devhub.db").string());
            Db importDestination((importDestinationRoot / "devhub.db").string());
            long long sourceProjectId = 0;
            long long sourceContributorId = 0;
            long long sourceMessageRowId = 0;
            long long sourceCardRowId = 0;
            const long long openImportId = 7001;
            const long long progressImportId = 7002;
            const long long completedImportId = 7003;
            const long long unattributedImportId = 7004;
            const long long atomicImportId = 7005;
            const std::string historicalChannelId = "923456789012345901";
            const std::string historicalMessageId = "923456789012345902";
            const std::string historicalAttachmentId = "923456789012345903";
            const std::string historicalNotifyChannelId = "923456789012345904";
            const std::string historicalNotifyMessageId = "923456789012345905";
            const std::string importedPng("\x89PNG\r\n\x1a\n", 8);
            TicketImageSaveResult sourceAttachment;
            {
                auto sourceLock = importSource.guard();
                const std::string created = "2026-07-15T10:00:00Z";
                SQLite::Statement project(importSource.raw(sourceLock.token()),
                    "INSERT INTO projects(name,slug,description,color,archived,"
                    "sort_order,created_at,updated_at,discord_tickets) "
                    "VALUES('Historical Import Project','historical-import',"
                    "'historical ticket fixture','#123456',0,17,?,?,1)");
                project.bind(1, created);
                project.bind(2, "2026-07-15T10:05:00Z");
                project.exec();
                sourceProjectId =
                    importSource.raw(sourceLock.token()).getLastInsertRowid();
                SQLite::Statement contributor(importSource.raw(sourceLock.token()),
                    "INSERT INTO sources(name,platform,handle,notes,created_at) "
                    "VALUES('Historical Contributor','discord',"
                    "'923456789012345906','fixture note',?)");
                contributor.bind(1, created);
                contributor.exec();
                sourceContributorId =
                    importSource.raw(sourceLock.token()).getLastInsertRowid();
                SQLite::Statement contributorAlias(
                    importSource.raw(sourceLock.token()),
                    "INSERT INTO sources(name,platform,handle,notes,created_at) "
                    "VALUES('Historical Contributor Alias','discord',"
                    "'923456789012345908','merged fixture alias',?)");
                contributorAlias.bind(1, created);
                contributorAlias.exec();
                const long long sourceContributorAliasId =
                    importSource.raw(sourceLock.token()).getLastInsertRowid();
                SQLite::Statement contributorMerge(
                    importSource.raw(sourceLock.token()),
                    "INSERT INTO source_merges(source_id,target_source_id,merged_at) "
                    "VALUES(?,?,?)");
                contributorMerge.bind(1, sourceContributorAliasId);
                contributorMerge.bind(2, sourceContributorId);
                contributorMerge.bind(3, "2026-07-15T10:00:30Z");
                contributorMerge.exec();
                auto addHistoricalItem = [&](long long id, const char* title,
                                             const char* body,
                                             const char* status, int priority,
                                             const char* updated,
                                             const char* completed,
                                             bool attributed) {
                    SQLite::Statement item(importSource.raw(sourceLock.token()),
                        "INSERT INTO items(id,project_id,type,title,body,status,"
                        "priority,source_id,credited,origin,due_date,tags,created_at,"
                        "updated_at,completed_at,review_date,blocked_reason) "
                        "VALUES(?,?,'implementation',?,?,?,?,?,1,'discord','',"
                        "'historical',?,?,?,'','')");
                    item.bind(1, id);
                    item.bind(2, sourceProjectId);
                    item.bind(3, title);
                    item.bind(4, body);
                    item.bind(5, status);
                    item.bind(6, priority);
                    item.bind(7, sourceContributorId);
                    item.bind(8, created);
                    item.bind(9, updated);
                    item.bind(10, completed);
                    item.exec();
                    if (!attributed) return;
                    SQLite::Statement link(importSource.raw(sourceLock.token()),
                        "INSERT INTO item_sources(item_id,source_id,credited,added_at) "
                        "VALUES(?,?,1,?)");
                    link.bind(1, id);
                    link.bind(2, sourceContributorId);
                    link.bind(3, created);
                    link.exec();
                };
                addHistoricalItem(openImportId, "Historical open ticket",
                                  "Open historical body", "open", 2,
                                  "2026-07-15T10:01:00Z", "", true);
                addHistoricalItem(progressImportId,
                                  "Historical in-progress ticket",
                                  "In-progress historical body", "in_progress", 3,
                                  "2026-07-16T11:01:00Z", "", true);
                addHistoricalItem(completedImportId,
                                  "Historical completed ticket",
                                  "Completed historical body", "completed", 1,
                                  "2026-07-17T12:01:00Z",
                                  "2026-07-17T12:01:00Z", true);
                addHistoricalItem(unattributedImportId,
                                  "Historical unattributed ticket",
                                  "No contributor is part of this exact graph",
                                  "open", 2, "2026-07-18T12:01:00Z", "",
                                  false);
                SQLite::Statement completion(importSource.raw(sourceLock.token()),
                    "INSERT INTO events(project_id,item_id,title,kind,date,notes,"
                    "created_at) VALUES(?,?,?,'completion','2026-07-17',"
                    "'historical completion','2026-07-17T12:01:00Z')");
                completion.bind(1, sourceProjectId);
                completion.bind(2, completedImportId);
                completion.bind(3, "Historical completed ticket");
                completion.exec();
                SQLite::Statement channel(importSource.raw(sourceLock.token()),
                    "INSERT INTO discord_channels(channel_id,guild_name,channel_name,"
                    "project_id,enabled,last_read_ts,last_scan_at,created_at,guild_id) "
                    "VALUES(?,'fixture guild','historical-import',?,1,'','',?,"
                    "'923456789012345907')");
                channel.bind(1, historicalChannelId);
                channel.bind(2, sourceProjectId);
                channel.bind(3, created);
                channel.exec();
                const long long sourceChannelRowId =
                    importSource.raw(sourceLock.token()).getLastInsertRowid();
                SQLite::Statement message(importSource.raw(sourceLock.token()),
                    "INSERT INTO discord_messages(channel_row_id,message_id,author,"
                    "author_id,content,posted_at,ingested_at,kind,score,matched,state,"
                    "item_id,inferred_project_id,notify_channel_id,notify_message_id,"
                    "admin_note) VALUES(?,?,?,?,?,?,?,'suggestion',1.0,'[\"manual\"]',"
                    "'promoted',?,?,?,?,?)");
                message.bind(1, sourceChannelRowId);
                message.bind(2, historicalMessageId);
                message.bind(3, "Historical Contributor");
                message.bind(4, "923456789012345906");
                message.bind(5, "Completed historical body");
                message.bind(6, "2026-07-17T11:55:00Z");
                message.bind(7, "2026-07-17T11:56:00Z");
                message.bind(8, completedImportId);
                message.bind(9, sourceProjectId);
                message.bind(10, historicalNotifyChannelId);
                message.bind(11, historicalNotifyMessageId);
                message.bind(12, "historical admin note");
                message.exec();
                sourceMessageRowId =
                    importSource.raw(sourceLock.token()).getLastInsertRowid();
                SQLite::Statement manualResult(
                    importSource.raw(sourceLock.token()),
                    "UPDATE discord_messages SET manual_target_message_id=?,"
                    "manual_command_state='done',manual_attempts=1,"
                    "manual_result_kind='captured',manual_result_message_row_id=?,"
                    "manual_result_images_queued=1,manual_effect_state='done',"
                    "manual_effect_attempts=1,manual_effect_updated_at=? WHERE id=?");
                manualResult.bind(1, historicalMessageId);
                manualResult.bind(2, sourceMessageRowId);
                manualResult.bind(3, "2026-07-17T12:01:00Z");
                manualResult.bind(4, sourceMessageRowId);
                manualResult.exec();
            }
            sourceAttachment = saveTicketAttachmentBytes(
                importSource.path(), completedImportId, historicalMessageId,
                historicalAttachmentId, "proof.png", "image/png", importedPng);
            check(sourceAttachment.ok && sourceAttachment.created,
                  "historical import fixture attachment saved");
            {
                auto sourceLock = importSource.guard();
                SQLite::Statement attachment(importSource.raw(sourceLock.token()),
                    "INSERT INTO ticket_attachments(discord_message_row_id,item_id,"
                    "source_channel_id,source_message_id,attachment_id,source_role,"
                    "original_filename,content_type,declared_size,width,height,state,"
                    "relative_path,actual_size,sha256,error,created_at,updated_at,saved_at) "
                    "VALUES(?,?,?,?,?,'suggestion','proof.png','image/png',8,1,1,"
                    "'saved',?,?,?,'','2026-07-17T11:56:00Z',"
                    "'2026-07-17T12:01:00Z','2026-07-17T12:01:00Z')");
                attachment.bind(1, sourceMessageRowId);
                attachment.bind(2, completedImportId);
                attachment.bind(3, historicalChannelId);
                attachment.bind(4, historicalMessageId);
                attachment.bind(5, historicalAttachmentId);
                attachment.bind(6, sourceAttachment.relativePath);
                attachment.bind(7,
                    static_cast<long long>(sourceAttachment.actualSize));
                attachment.bind(8, sourceAttachment.sha256);
                attachment.exec();
                SQLite::Statement card(importSource.raw(sourceLock.token()),
                    "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                    "notify_channel_id,notify_message_id,post_state,post_attempts,"
                    "post_revision,post_next_retry_at,post_last_error,edit_state,"
                    "edit_attempts,edit_revision,edit_next_retry_at,edit_last_error,"
                    "edit_updated_at,created_at,updated_at) VALUES(?,?,?,?,"
                    "'posted',1,2,'','','failed',3,4,'','historical edit failure',"
                    "'2026-07-17T12:02:00Z','2026-07-17T11:56:00Z',"
                    "'2026-07-17T12:02:00Z')");
                card.bind(1, sourceMessageRowId);
                card.bind(2, completedImportId);
                card.bind(3, historicalNotifyChannelId);
                card.bind(4, historicalNotifyMessageId);
                card.exec();
                sourceCardRowId =
                    importSource.raw(sourceLock.token()).getLastInsertRowid();
                SQLite::Statement dismissal(importSource.raw(sourceLock.token()),
                    "INSERT INTO discord_notify_failure_dismissals(card_row_id,"
                    "operation,revision,dismissed_at,reason) VALUES(?,"
                    "'edit',4,'2026-07-18T00:00:00Z','historical fixture')");
                dismissal.bind(1, sourceCardRowId);
                dismissal.exec();
            }
            std::string sentinelBefore;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement destinationProject(
                    importDestination.raw(destinationLock.token()),
                    "INSERT INTO projects(name,slug,created_at,updated_at) "
                    "VALUES(?,?,?,?)");
                destinationProject.bind(1, "Current Destination Project");
                destinationProject.bind(2, "current-destination-project");
                destinationProject.bind(3, "2026-08-01T00:00:00Z");
                destinationProject.bind(4, "2026-08-01T00:00:00Z");
                destinationProject.exec();
                const long long destinationProjectId =
                    importDestination.raw(destinationLock.token())
                        .getLastInsertRowid();
                SQLite::Statement channel(importDestination.raw(destinationLock.token()),
                    "INSERT INTO discord_channels(channel_id,guild_name,channel_name,"
                    "enabled,last_read_ts,last_scan_at,created_at,guild_id) "
                    "VALUES(?,'current guild','current-channel',1,'','','2026-08-01T00:00:00Z',"
                    "'923456789012345907')");
                channel.bind(1, historicalChannelId);
                channel.exec();
                SQLite::Statement sentinel(importDestination.raw(destinationLock.token()),
                    "INSERT INTO items(id,project_id,type,title,body,status,priority,"
                    "origin,created_at,updated_at) VALUES(6500,?,'note',"
                    "'Current sentinel','must remain unchanged','open',2,'manual',"
                    "'2026-08-01T00:00:00Z','2026-08-01T00:00:00Z')");
                sentinel.bind(1, destinationProjectId);
                sentinel.exec();
                SQLite::Statement read(importDestination.raw(destinationLock.token()),
                    "SELECT title||'|'||body||'|'||status||'|'||updated_at "
                    "FROM items WHERE id=6500");
                requireRow(read, "historical import sentinel query returned a row");
                sentinelBefore = read.getColumn(0).getString();
            }
            const nlohmann::json selectedImport = {
                {"source_data_root", importSourceRoot.string()},
                {"item_ids", {openImportId, progressImportId,
                              completedImportId, unattributedImportId}}};
            nlohmann::json missingProject = importWorkItems(
                &importDestination, selectedImport);
            long long destinationItemsAfterMissing = 0;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement count(importDestination.raw(destinationLock.token()),
                    "SELECT COUNT(*) FROM items");
                requireRow(count, "historical missing-project count returned a row");
                destinationItemsAfterMissing = count.getColumn(0).getInt64();
            }
            check(!missingProject.value("ok", false) &&
                      missingProject.value("code", "") == "conflict" &&
                      destinationItemsAfterMissing == 1,
                  "missing import project requires exact additive project data");

            nlohmann::json approvedImport = selectedImport;
            approvedImport["add_projects"] = nlohmann::json::array({
                {{"source_project_id", sourceProjectId},
                 {"name", "Historical Import Project"}}});
            {
                auto sourceLock = importSource.guard();
                SQLite::Statement breakHash(importSource.raw(sourceLock.token()),
                    "UPDATE ticket_attachments SET sha256=? WHERE item_id=?");
                breakHash.bind(1, std::string(64, '0'));
                breakHash.bind(2, completedImportId);
                breakHash.exec();
            }
            nlohmann::json badHash = importWorkItems(
                &importDestination, approvedImport);
            {
                auto sourceLock = importSource.guard();
                SQLite::Statement restoreHash(importSource.raw(sourceLock.token()),
                    "UPDATE ticket_attachments SET sha256=? WHERE item_id=?");
                restoreHash.bind(1, sourceAttachment.sha256);
                restoreHash.bind(2, completedImportId);
                restoreHash.exec();
            }
            TicketImportFileReadResult unsafePath =
                readTicketImportAttachmentFile(
                    importSourceRoot.string(), "..\\devhub.db",
                    sourceAttachment.actualSize, sourceAttachment.sha256);
            long long destinationItemsAfterBadHash = 0;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement count(importDestination.raw(destinationLock.token()),
                    "SELECT COUNT(*) FROM items");
                requireRow(count, "historical bad-hash count returned a row");
                destinationItemsAfterBadHash = count.getColumn(0).getInt64();
            }
            check(!badHash.value("ok", false) &&
                      badHash.value("code", "") == "conflict" &&
                      !unsafePath.ok && destinationItemsAfterBadHash == 1,
                  "unsafe or hash-mismatched attachment input rejects without mutation");

            {
                auto sourceLock = importSource.guard();
                SQLite::Statement incompatibleMetadata(
                    importSource.raw(sourceLock.token()),
                    "UPDATE ticket_attachments SET content_type='image/jpeg' "
                    "WHERE item_id=?");
                incompatibleMetadata.bind(1, completedImportId);
                incompatibleMetadata.exec();
            }
            nlohmann::json rollbackImport = importWorkItems(
                &importDestination, approvedImport);
            {
                auto sourceLock = importSource.guard();
                SQLite::Statement restoreMetadata(
                    importSource.raw(sourceLock.token()),
                    "UPDATE ticket_attachments SET content_type='image/png' "
                    "WHERE item_id=?");
                restoreMetadata.bind(1, completedImportId);
                restoreMetadata.exec();
            }
            long long destinationItemsAfterRollback = 0;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement count(
                    importDestination.raw(destinationLock.token()),
                    "SELECT COUNT(*) FROM items");
                requireRow(count,
                    "historical write-rollback count returned a row");
                destinationItemsAfterRollback = count.getColumn(0).getInt64();
            }
            const bool rollbackFileExists = std::filesystem::exists(
                importDestinationRoot / sourceAttachment.relativePath);
            check(!rollbackImport.value("ok", false) &&
                      rollbackImport.value("code", "") == "internal_error" &&
                      destinationItemsAfterRollback == 1 && !rollbackFileExists,
                  "write-time attachment mismatch rolls back database rows and the new file");

            nlohmann::json imported = importWorkItems(
                &importDestination, approvedImport);
            bool importedGraphExact = false;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement exact(importDestination.raw(destinationLock.token()),
                    "SELECT "
                    "(SELECT COUNT(*) FROM items WHERE id IN (7001,7002,7003,7004)),"
                    "(SELECT COUNT(*) FROM items WHERE id=7001 AND status='open' "
                    "AND created_at='2026-07-15T10:00:00Z' "
                    "AND updated_at='2026-07-15T10:01:00Z'),"
                    "(SELECT COUNT(*) FROM items WHERE id=7002 AND status='in_progress' "
                    "AND updated_at='2026-07-16T11:01:00Z'),"
                    "(SELECT COUNT(*) FROM items WHERE id=7003 AND status='completed' "
                    "AND completed_at='2026-07-17T12:01:00Z'),"
                    "(SELECT COUNT(*) FROM item_sources WHERE item_id IN (7001,7002,7003,7004) "
                    "AND credited=1),"
                    "(SELECT COUNT(*) FROM events WHERE item_id=7003 AND kind='completion' "
                    "AND date='2026-07-17'),"
                    "(SELECT COUNT(*) FROM discord_messages WHERE item_id=7003 "
                     "AND message_id='923456789012345902' AND state='promoted'),"
                    "(SELECT COUNT(*) FROM discord_messages WHERE item_id=7003 "
                    "AND manual_command_state='done' AND manual_effect_state='done' "
                    "AND manual_result_message_row_id=id),"
                    "(SELECT COUNT(*) FROM discord_notify_cards WHERE item_id=7003 "
                    "AND post_state='posted' AND edit_state='failed'),"
                    "(SELECT COUNT(*) FROM discord_notify_failure_dismissals d "
                    "JOIN discord_notify_cards n ON n.id=d.card_row_id "
                    "WHERE n.item_id=7003 AND d.operation='edit' AND d.revision=4),"
                    "(SELECT COUNT(*) FROM ticket_attachments WHERE item_id=7003 "
                    "AND state='saved' AND sha256=?)");
                exact.bind(1, sourceAttachment.sha256);
                requireRow(exact, "historical imported graph query returned a row");
                importedGraphExact = exact.getColumn(0).getInt() == 4;
                for (int column = 1; column < 11; ++column)
                    importedGraphExact = importedGraphExact &&
                        exact.getColumn(column).getInt() ==
                            (column == 4 ? 3 : 1);
            }
            TicketImportFileReadResult restoredFile =
                readTicketImportAttachmentFile(
                    importDestinationRoot.string(), sourceAttachment.relativePath,
                    sourceAttachment.actualSize, sourceAttachment.sha256);
            check(imported.value("ok", false) &&
                      imported.value("created_count", 0) == 4 &&
                      imported.value("duplicate_count", 0) == 0 &&
                      imported.value(
                          "attachments", nlohmann::json::array()).size() == 1 &&
                      importedGraphExact && restoredFile.ok &&
                      restoredFile.bytes == importedPng,
                  "historical statuses timestamps contributors manual-result Discord lineage completion and attachment bytes survive import");

            nlohmann::json replay = importWorkItems(
                &importDestination, approvedImport);
            check(replay.value("ok", false) &&
                      replay.value("created_count", -1) == 0 &&
                      replay.value("duplicate_count", 0) == 4,
                  "exact historical import replay is an additive no-op");

            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement contributor(
                    importDestination.raw(destinationLock.token()),
                    "SELECT id FROM sources WHERE name='Historical Contributor'");
                requireRow(contributor,
                    "historical imported contributor query returned a row");
                SQLite::Statement extra(
                    importDestination.raw(destinationLock.token()),
                    "INSERT INTO item_sources(item_id,source_id,credited,added_at) "
                    "VALUES(?,?,0,'2026-08-01T00:00:00Z')");
                extra.bind(1, unattributedImportId);
                extra.bind(2, contributor.getColumn(0).getInt64());
                extra.exec();
            }
            nlohmann::json zeroLinkPayload = approvedImport;
            zeroLinkPayload["item_ids"] = {unattributedImportId};
            nlohmann::json zeroLinkConflict = importWorkItems(
                &importDestination, zeroLinkPayload);
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement remove(
                    importDestination.raw(destinationLock.token()),
                    "DELETE FROM item_sources WHERE item_id=?");
                remove.bind(1, unattributedImportId);
                remove.exec();
            }
            check(!zeroLinkConflict.value("ok", false) &&
                      zeroLinkConflict.value("code", "") == "conflict",
                  "exact replay rejects an added contributor on a source item with none");

            {
                auto sourceLock = importSource.guard();
                SQLite::Statement conflict(importSource.raw(sourceLock.token()),
                    "UPDATE items SET body='conflicting historical body' WHERE id=?");
                conflict.bind(1, openImportId);
                conflict.exec();
                SQLite::Statement atomic(importSource.raw(sourceLock.token()),
                    "INSERT INTO items(id,project_id,type,title,body,status,priority,"
                    "origin,created_at,updated_at) VALUES(?,?,'note','Atomic new item',"
                    "'must not partially import','open',2,'manual',"
                    "'2026-07-19T00:00:00Z','2026-07-19T00:00:00Z')");
                atomic.bind(1, atomicImportId);
                atomic.bind(2, sourceProjectId);
                atomic.exec();
            }
            nlohmann::json atomicPayload = approvedImport;
            atomicPayload["item_ids"] = {openImportId, atomicImportId};
            nlohmann::json idConflict = importWorkItems(
                &importDestination, atomicPayload);
            long long atomicCreated = 0;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement count(importDestination.raw(destinationLock.token()),
                    "SELECT COUNT(*) FROM items WHERE id=?");
                count.bind(1, atomicImportId);
                requireRow(count, "historical atomic conflict count returned a row");
                atomicCreated = count.getColumn(0).getInt64();
            }
            check(!idConflict.value("ok", false) &&
                      idConflict.value("code", "") == "conflict" &&
                      atomicCreated == 0,
                  "existing ID conflict rejects the complete import batch");
            {
                auto sourceLock = importSource.guard();
                SQLite::Statement restore(importSource.raw(sourceLock.token()),
                    "UPDATE items SET body='Open historical body' WHERE id=?");
                restore.bind(1, openImportId);
                restore.exec();
            }
            long long importedProjectId = 0;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement project(importDestination.raw(destinationLock.token()),
                    "SELECT id FROM projects WHERE name='Historical Import Project'");
                requireRow(project, "historical imported project query returned a row");
                importedProjectId = project.getColumn(0).getInt64();
                SQLite::Statement sameTitle(importDestination.raw(destinationLock.token()),
                    "INSERT INTO items(id,project_id,type,title,body,status,priority,"
                    "origin,created_at,updated_at) VALUES(8000,?,'note','Atomic new item',"
                    "'different destination body','open',2,'manual',"
                    "'2026-08-01T00:00:00Z','2026-08-01T00:00:00Z')");
                sameTitle.bind(1, importedProjectId);
                sameTitle.exec();
            }
            nlohmann::json titlePayload = approvedImport;
            titlePayload["item_ids"] = {atomicImportId};
            nlohmann::json titleConflict = importWorkItems(
                &importDestination, titlePayload);
            long long sentinelAfterCount = 0;
            std::string sentinelAfter;
            {
                auto destinationLock = importDestination.guard();
                SQLite::Statement count(importDestination.raw(destinationLock.token()),
                    "SELECT COUNT(*) FROM items WHERE id=?");
                count.bind(1, atomicImportId);
                requireRow(count, "historical title conflict count returned a row");
                sentinelAfterCount = count.getColumn(0).getInt64();
                SQLite::Statement sentinel(importDestination.raw(destinationLock.token()),
                    "SELECT title||'|'||body||'|'||status||'|'||updated_at "
                    "FROM items WHERE id=6500");
                requireRow(sentinel,
                    "historical import sentinel remained queryable");
                sentinelAfter = sentinel.getColumn(0).getString();
            }
            check(!titleConflict.value("ok", false) &&
                      titleConflict.value("code", "") == "conflict" &&
                      sentinelAfterCount == 0 && sentinelAfter == sentinelBefore,
                  "title conflict rejects the batch and preserves current destination tickets");

            BuildRunner importBuilds(&importDestination);
            const uint16_t importPort = reserveSelftestLoopbackPort();
            check(importPort != 0, "historical import API port reserved");
            if (importPort != 0) {
                Server importServer(&importDestination, &importBuilds, importPort,
                                    importDestinationRoot.string());
                const uint16_t started = importServer.start();
                const std::string importRendezvousPath = apiRendezvousPath(
                    importPort, importDestinationRoot.string());
                const nlohmann::json importRendezvous = nlohmann::json::parse(
                    readFileUtf8(importRendezvousPath), nullptr, false);
                const std::string token = importRendezvous.is_object()
                    ? importRendezvous.value("token", "") : std::string();
                const SelftestHttpResponse response = selftestPostJson(
                    started, "/api/work/import", approvedImport.dump(), token);
                const nlohmann::json responseBody = nlohmann::json::parse(
                    response.body, nullptr, false);
                check(started == importPort && response.status == 200 &&
                          responseBody.is_object() &&
                          responseBody.value("duplicate_count", 0) == 4 &&
                          responseBody.value(
                              "attachments", nlohmann::json::array()).size() == 1 &&
                          responseBody.value(
                              "verified_graph", nlohmann::json::object())
                              .value("attachments", 0) == 1,
                      "authenticated work import route returns exact replay manifest");
                importServer.stop();
                check(!std::filesystem::exists(importRendezvousPath),
                      "historical import server removes only its owned rendezvous on stop");
            }
        });

        try {
        std::printf("selftest: P0 disposable acceptance\n");
        BuildRunner p0Builds(&db);
        const uint16_t p0Port = reserveSelftestLoopbackPort();
        check(p0Port != 0, "disposable loopback port reserved");
        if (p0Port != 0) {
            Server p0Server(&db, &p0Builds, p0Port, testDir.string());
            const uint16_t startedPort = p0Server.start();
            const std::string p0RendezvousPath = apiRendezvousPath(
                p0Port, testDir.string());
            const std::string p0RendezvousText = readFileUtf8(p0RendezvousPath);
            const nlohmann::json p0Rendezvous = nlohmann::json::parse(
                p0RendezvousText, nullptr, false);
            const std::string apiToken = p0Rendezvous.is_object()
                ? p0Rendezvous.value("token", "") : std::string();
            const bool validApiToken = apiToken.size() == 64 &&
                std::all_of(apiToken.begin(), apiToken.end(), [](char ch) {
                    return (ch >= '0' && ch <= '9') ||
                           (ch >= 'a' && ch <= 'f');
                });
            check(validApiToken && p0Rendezvous.is_object() &&
                      p0Rendezvous.value("schema", "") ==
                          "xa-devhub.api-rendezvous/v1" &&
                      p0Rendezvous.value("origin", "") ==
                          "http://127.0.0.1:" + std::to_string(p0Port) &&
                      p0Rendezvous.value("pid", 0ULL) ==
                          static_cast<unsigned long long>(GetCurrentProcessId()),
                  "disposable API publishes a verified exact-instance rendezvous");
            Server collidingServer(&db, &p0Builds, p0Port, testDir.string());
            check(collidingServer.start() == 0 &&
                      readFileUtf8(p0RendezvousPath) == p0RendezvousText,
                  "a port collision cannot rotate the active instance rendezvous");
            const SelftestHttpResponse noTokenResponse = selftestPostJson(
                startedPort, "/api/settings", "{}");
            const SelftestHttpResponse wrongTokenResponse = selftestPostJson(
                startedPort, "/api/settings", "{}", std::string(64, '0'));
            check(noTokenResponse.status == 401 &&
                      wrongTokenResponse.status == 401,
                  "API routes reject missing and incorrect per-run tokens");

            nlohmann::json apiProjectPayload = {
                {"name", "API Project Fixture"},
                {"description", "created through the complete project API"},
                {"path", "C:\\Projects\\ApiFixture"},
                {"rules_path", "C:\\Projects\\ApiFixture\\README.md"},
                {"codex_skills", "$ffxiv-dalamud-plugin-builder, $runtests"},
                {"build_command", "python \"1. Build.py\""},
                {"prep_command", "python \"2. Prepare_release.py\""},
                {"release_command", "python \"3. Push_release.py\""},
                {"version_command", "python \"6. Update_test_version.py\""},
                {"build_cwd", ""},
                {"local_version_file",
                 "C:\\Projects\\ApiFixture\\ApiFixture\\ApiFixture.csproj"},
                {"remote_version_url", "https://example.invalid/x.json"},
                {"remote_version_key", "ApiFixture"},
                {"github_url", "https://example.invalid/ApiFixture"},
                {"aliases", "api fixture, project fixture"},
                {"discord_tickets", false}
            };
            const SelftestHttpResponse projectCreateResponse = selftestPostJson(
                startedPort, "/api/projects", apiProjectPayload.dump(), apiToken);
            const nlohmann::json projectCreateBody = nlohmann::json::parse(
                projectCreateResponse.body, nullptr, false);
            const long long apiProjectId = projectCreateBody.is_object()
                ? projectCreateBody.value("id", 0LL) : 0;
            const SelftestHttpResponse projectReadResponse = selftestGetJson(
                startedPort, "/api/projects/" + std::to_string(apiProjectId),
                apiToken);
            const nlohmann::json projectReadBody = nlohmann::json::parse(
                projectReadResponse.body, nullptr, false);
            check(projectCreateResponse.status == 201 && apiProjectId > 0 &&
                      projectCreateBody.value("created", false) &&
                      projectReadResponse.status == 200 &&
                      projectReadBody.is_object() &&
                      projectReadBody.value("id", 0LL) == apiProjectId &&
                      projectReadBody.value("name", std::string()) ==
                          "API Project Fixture" &&
                      projectReadBody.value("path", std::string()) ==
                          "C:\\Projects\\ApiFixture" &&
                      projectReadBody.value("build_command", std::string()) ==
                          "python \"1. Build.py\"" &&
                      projectReadBody.value("discord_tickets", 1LL) == 0,
                  "authenticated project create returns complete exact readback");

            apiProjectPayload["description"] =
                "updated through the complete project API";
            apiProjectPayload["aliases"] = "updated fixture";
            apiProjectPayload["discord_tickets"] = true;
            const SelftestHttpResponse projectUpdateResponse = selftestPostJson(
                startedPort,
                "/api/projects/" + std::to_string(apiProjectId),
                apiProjectPayload.dump(), apiToken);
            const SelftestHttpResponse incompleteProjectResponse =
                selftestPostJson(startedPort,
                    "/api/projects/" + std::to_string(apiProjectId),
                    R"json({"name":"partial overwrite"})json", apiToken);
            const SelftestHttpResponse projectUpdatedReadResponse =
                selftestGetJson(startedPort,
                    "/api/projects/" + std::to_string(apiProjectId), apiToken);
            const nlohmann::json projectUpdatedReadBody = nlohmann::json::parse(
                projectUpdatedReadResponse.body, nullptr, false);
            check(projectUpdateResponse.status == 200 &&
                      incompleteProjectResponse.status == 400 &&
                      projectUpdatedReadResponse.status == 200 &&
                      projectUpdatedReadBody.value("description", std::string()) ==
                          "updated through the complete project API" &&
                      projectUpdatedReadBody.value("aliases", std::string()) ==
                          "updated fixture" &&
                      projectUpdatedReadBody.value("discord_tickets", 0LL) == 1,
                  "project update is full-field atomic and rejects partial overwrite");

            nlohmann::json apiWorkPayload = {
                {"project_id", apiProjectId},
                {"type", "fix"},
                {"title", "API work create fixture"},
                {"body", "Reproduce the transition and preserve fail-closed handling."},
                {"priority", 4},
                {"due_date", ""},
                {"review_date", "2026-09-01"},
                {"tags", "api,reliability"}
            };
            const SelftestHttpResponse workCreateResponse = selftestPostJson(
                startedPort, "/api/work", apiWorkPayload.dump(), apiToken);
            const nlohmann::json workCreateBody = nlohmann::json::parse(
                workCreateResponse.body, nullptr, false);
            const long long apiWorkId = workCreateBody.is_object()
                ? workCreateBody.value("id", 0LL) : 0;
            const SelftestHttpResponse workReadResponse = selftestGetJson(
                startedPort, "/api/work/" + std::to_string(apiWorkId), apiToken);
            const nlohmann::json workReadBody = nlohmann::json::parse(
                workReadResponse.body, nullptr, false);
            const SelftestHttpResponse workReplayResponse = selftestPostJson(
                startedPort, "/api/work", apiWorkPayload.dump(), apiToken);
            const nlohmann::json workReplayBody = nlohmann::json::parse(
                workReplayResponse.body, nullptr, false);
            apiWorkPayload["body"] = "Conflicting body";
            const SelftestHttpResponse workConflictResponse = selftestPostJson(
                startedPort, "/api/work", apiWorkPayload.dump(), apiToken);
            const nlohmann::json workConflictBody = nlohmann::json::parse(
                workConflictResponse.body, nullptr, false);
            const SelftestHttpResponse invalidWorkResponse = selftestPostJson(
                startedPort, "/api/work",
                R"json({"project_id":1,"title":"partial"})json", apiToken);
            check(workCreateResponse.status == 201 && apiWorkId > 0 &&
                      workCreateBody.value("created", false) &&
                      !workCreateBody.value("duplicate", true) &&
                      workReadResponse.status == 200 &&
                      workReadBody.value("id", 0LL) == apiWorkId &&
                      workReadBody.value("project_id", 0LL) == apiProjectId &&
                      workReadBody.value("type", std::string()) == "fix" &&
                      workReadBody.value("status", std::string()) == "open" &&
                      workReadBody.value("priority", 0LL) == 4 &&
                      workReadBody.value("review_date", std::string()) ==
                          "2026-09-01" &&
                      workReadBody.value("origin", std::string()) == "api" &&
                      workReplayResponse.status == 200 &&
                      workReplayBody.value("id", 0LL) == apiWorkId &&
                      !workReplayBody.value("created", true) &&
                      workReplayBody.value("duplicate", false) &&
                      workConflictResponse.status == 409 &&
                      workConflictBody.value("existing_id", 0LL) == apiWorkId &&
                      invalidWorkResponse.status == 400,
                  "authenticated work create is strict idempotent and exactly readable");

            const nlohmann::json titlePayload = {
                {"title", "API title-only fixture"},
                {"expected_updated_at",
                 workReadBody.value("updated_at", std::string())}
            };
            const SelftestHttpResponse titleResponse = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(apiWorkId) + "/title",
                titlePayload.dump(), apiToken);
            const nlohmann::json titleBody = nlohmann::json::parse(
                titleResponse.body, nullptr, false);
            nlohmann::json staleTitlePayload = titlePayload;
            staleTitlePayload["title"] = "Stale title must not save";
            const SelftestHttpResponse staleTitleResponse = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(apiWorkId) + "/title",
                staleTitlePayload.dump(), apiToken);
            nlohmann::json extraTitlePayload = titlePayload;
            extraTitlePayload["body"] = "must not be accepted";
            const SelftestHttpResponse extraTitleResponse = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(apiWorkId) + "/title",
                extraTitlePayload.dump(), apiToken);
            check(titleResponse.status == 200 && titleBody.is_object() &&
                      titleBody.value("title", std::string()) ==
                          "API title-only fixture" &&
                      titleBody.value("body", std::string()) ==
                          workReadBody.value("body", std::string()) &&
                      titleBody.value("status", std::string()) ==
                          workReadBody.value("status", std::string()) &&
                      titleBody.value("updated_at", std::string()) !=
                          workReadBody.value("updated_at", std::string()) &&
                      staleTitleResponse.status == 409 &&
                      extraTitleResponse.status == 400,
                  "title-only API preserves ticket data and rejects stale or expanded writes");

            nlohmann::json firstMergeSourcePayload = {
                {"project_id", apiProjectId},
                {"type", "implementation"},
                {"title", "First API merge source"},
                {"body", "First API source note."},
                {"priority", 2},
                {"due_date", ""},
                {"review_date", ""},
                {"tags", "merge"}
            };
            nlohmann::json secondMergeSourcePayload = firstMergeSourcePayload;
            secondMergeSourcePayload["title"] = "Second API merge source";
            secondMergeSourcePayload["body"] = "Second API source note.";
            const SelftestHttpResponse firstMergeCreate = selftestPostJson(
                startedPort, "/api/work", firstMergeSourcePayload.dump(),
                apiToken);
            const SelftestHttpResponse secondMergeCreate = selftestPostJson(
                startedPort, "/api/work", secondMergeSourcePayload.dump(),
                apiToken);
            const nlohmann::json firstMergeCreateBody = nlohmann::json::parse(
                firstMergeCreate.body, nullptr, false);
            const nlohmann::json secondMergeCreateBody = nlohmann::json::parse(
                secondMergeCreate.body, nullptr, false);
            const long long firstMergeId = firstMergeCreateBody.value("id", 0LL);
            const long long secondMergeId = secondMergeCreateBody.value("id", 0LL);
            const SelftestHttpResponse completeSecondMerge = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(secondMergeId) + "/status",
                R"json({"status":"completed"})json", apiToken);
            const SelftestHttpResponse firstMergeRead = selftestGetJson(
                startedPort, "/api/work/" + std::to_string(firstMergeId),
                apiToken);
            const SelftestHttpResponse secondMergeRead = selftestGetJson(
                startedPort, "/api/work/" + std::to_string(secondMergeId),
                apiToken);
            const nlohmann::json firstMergeBody = nlohmann::json::parse(
                firstMergeRead.body, nullptr, false);
            const nlohmann::json secondMergeBody = nlohmann::json::parse(
                secondMergeRead.body, nullptr, false);
            const nlohmann::json mergePayload = {
                {"expected_updated_at",
                 titleBody.value("updated_at", std::string())},
                {"sources", nlohmann::json::array({
                    nlohmann::json{
                        {"id", firstMergeId},
                        {"expected_updated_at",
                         firstMergeBody.value("updated_at", std::string())}},
                    nlohmann::json{
                        {"id", secondMergeId},
                        {"expected_updated_at",
                         secondMergeBody.value("updated_at", std::string())}}
                })}
            };
            nlohmann::json duplicateMergePayload = mergePayload;
            duplicateMergePayload["sources"][1] =
                duplicateMergePayload["sources"][0];
            const SelftestHttpResponse duplicateMergeResponse = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(apiWorkId) + "/merge",
                duplicateMergePayload.dump(), apiToken);
            const SelftestHttpResponse mergeResponse = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(apiWorkId) + "/merge",
                mergePayload.dump(), apiToken);
            const nlohmann::json mergeBody = nlohmann::json::parse(
                mergeResponse.body, nullptr, false);
            const SelftestHttpResponse staleMergeResponse = selftestPostJson(
                startedPort,
                "/api/work/" + std::to_string(apiWorkId) + "/merge",
                mergePayload.dump(), apiToken);
            const nlohmann::json mergedTarget = mergeBody.is_object()
                ? mergeBody.value("target", nlohmann::json::object())
                : nlohmann::json::object();
            const nlohmann::json mergedSources = mergeBody.is_object()
                ? mergeBody.value("sources", nlohmann::json::array())
                : nlohmann::json::array();
            const std::string mergedApiBody = mergedTarget.value(
                "body", std::string());
            check(firstMergeCreate.status == 201 &&
                      secondMergeCreate.status == 201 &&
                      completeSecondMerge.status == 200 &&
                      firstMergeRead.status == 200 &&
                      secondMergeRead.status == 200 &&
                      secondMergeBody.value("status", std::string()) ==
                          "completed" &&
                      duplicateMergeResponse.status == 400 &&
                      mergeResponse.status == 200 && mergeBody.is_object() &&
                      mergeBody.value("ok", false) &&
                      mergedTarget.value("id", 0LL) == apiWorkId &&
                      mergedTarget.value("title", std::string()) ==
                          "API title-only fixture" &&
                      mergedTarget.value("merged_source_count", 0LL) == 2 &&
                      mergedApiBody.find("First API source note.") !=
                          std::string::npos &&
                      mergedApiBody.find("Second API source note.") !=
                          std::string::npos &&
                      mergedSources.size() == 2 &&
                      mergedSources[0].value("status", std::string()) ==
                          "merged" &&
                      mergedSources[0].value("merged_into_id", 0LL) ==
                          apiWorkId &&
                      mergedSources[1].value("status", std::string()) ==
                          "merged" &&
                      mergedSources[1].value("merged_into_id", 0LL) ==
                          apiWorkId &&
                      mergedSources[1].value("completed_at", std::string())
                          .empty() &&
                      staleMergeResponse.status == 409,
                  "authenticated batch merge preserves notes and audit identities with exact concurrency");
            if (apiProjectId > 0) {
                auto lk = db.guard();
                SQLite::Statement activityDelete(db.raw(lk.token()),
                    "DELETE FROM activity_log WHERE project_id=?");
                activityDelete.bind(1, apiProjectId);
                activityDelete.exec();
                SQLite::Statement projectDelete(db.raw(lk.token()),
                    "DELETE FROM projects WHERE id=?");
                projectDelete.bind(1, apiProjectId);
                projectDelete.exec();
            }

            long long buildsBefore = 0;
            {
                auto lk = db.guard();
                SQLite::Statement count(db.raw(lk.token()),
                    "SELECT COUNT(*) FROM builds WHERE project_id=?");
                count.bind(1, projectId);
                requireRow(count, "selftest query count at source line 1822 returned a row");
                buildsBefore = count.getColumn(0).getInt64();
            }
            const SelftestHttpResponse overrideResponse = selftestPostJson(
                startedPort,
                "/api/projects/" + std::to_string(projectId) + "/build",
                R"json({"command":"calc.exe"})json", apiToken);
            long long buildsAfter = 0;
            {
                auto lk = db.guard();
                SQLite::Statement count(db.raw(lk.token()),
                    "SELECT COUNT(*) FROM builds WHERE project_id=?");
                count.bind(1, projectId);
                requireRow(count, "selftest query count at source line 1835 returned a row");
                buildsAfter = count.getColumn(0).getInt64();
            }
            check(startedPort == p0Port && overrideResponse.status == 400 &&
                      overrideResponse.body.find("configured per project") !=
                          std::string::npos &&
                      buildsAfter == buildsBefore,
                  "build API rejects command override before creating a job");

            {
                auto lk = db.guard();
                db.setSetting(lk.token(), "detection_patterns", "");
            }
            const SelftestHttpResponse settingsResponse = selftestPostJson(
                startedPort, "/api/settings",
                R"json({"detection_patterns":"[1]"})json", apiToken);
            std::string storedPatterns;
            {
                auto lk = db.guard();
                storedPatterns = db.getSetting(lk.token(), "detection_patterns", "missing");
            }
            check(settingsResponse.status == 400 && storedPatterns.empty(),
                  "settings API rejects malformed detection patterns without writing");
            const std::string canonicalPatterns =
                SuggestionDetector::patternsToJson({
                    DetectionPattern{"please add", "suggestion", 0.65}}).dump();
            {
                auto lk = db.guard();
                db.setSetting(lk.token(), "discord_bot_token",
                              "not-a-real-api-settings-token");
            }
            const SelftestHttpResponse publicSettingWrite = selftestPostJson(
                startedPort, "/api/settings",
                nlohmann::json({{"detection_patterns", canonicalPatterns}}).dump(),
                apiToken);
            const SelftestHttpResponse publicSettingRead = selftestGetJson(
                startedPort, "/api/settings", apiToken);
            const nlohmann::json publicSettings = nlohmann::json::parse(
                publicSettingRead.body, nullptr, false);
            std::string rawPatterns, rawCredential;
            {
                auto lk = db.guard();
                SQLite::Statement patterns(db.raw(lk.token()),
                    "SELECT value FROM settings WHERE key='detection_patterns'");
                SQLite::Statement credential(db.raw(lk.token()),
                    "SELECT value FROM settings WHERE key='discord_bot_token'");
                requireRow(patterns,
                    "selftest API roundtrip persisted detection patterns");
                requireRow(credential,
                    "selftest API roundtrip retained the credential setting");
                rawPatterns = patterns.getColumn(0).getString();
                rawCredential = credential.getColumn(0).getString();
            }
            const nlohmann::json expectedPatterns = nlohmann::json::parse(
                canonicalPatterns, nullptr, false);
            const nlohmann::json publicPatterns = nlohmann::json::parse(
                publicSettings.is_object()
                    ? publicSettings.value("detection_patterns", "") : "",
                nullptr, false);
            const nlohmann::json storedPatternJson = nlohmann::json::parse(
                rawPatterns, nullptr, false);
            const bool publicSettingsRoundTrip =
                publicSettingWrite.status == 200 &&
                publicSettingRead.status == 200 &&
                publicSettings.is_object() &&
                !expectedPatterns.is_discarded() &&
                publicPatterns == expectedPatterns &&
                publicSettings.value("discord_bot_token", "") ==
                    "[redacted]" &&
                storedPatternJson == expectedPatterns &&
                rawPatterns.rfind("dpapi:v1:", 0) != 0 &&
                rawCredential.rfind("dpapi:v1:", 0) == 0;
            if (!publicSettingsRoundTrip) {
                std::printf(
                    "selftest detail: setting statuses=%lu/%lu expected=%s "
                    "public=%s stored=%s redacted=%d sealed=%d\n",
                    static_cast<unsigned long>(publicSettingWrite.status),
                    static_cast<unsigned long>(publicSettingRead.status),
                    canonicalPatterns.c_str(), publicPatterns.dump().c_str(),
                    storedPatternJson.dump().c_str(),
                    publicSettings.is_object() &&
                        publicSettings.value("discord_bot_token", "") ==
                            "[redacted]" ? 1 : 0,
                    rawCredential.rfind("dpapi:v1:", 0) == 0 ? 1 : 0);
            }
            check(publicSettingsRoundTrip,
                  "public settings round-trip as plaintext while credentials stay sealed and redacted");
            {
                auto lk = db.guard();
                db.setSetting(lk.token(), "detection_patterns", "");
            }
            const SelftestHttpResponse plainTextResponse = selftestPostJson(
                startedPort, "/api/settings", R"json({"stale_days":"7"})json",
                apiToken, "text/plain");
            const SelftestHttpResponse originResponse = selftestPostJson(
                startedPort, "/api/settings", R"json({"stale_days":"7"})json",
                apiToken, "application/json", "https://example.invalid");
            const SelftestHttpResponse unknownSettingResponse = selftestPostJson(
                startedPort, "/api/settings", R"json({"discord_bot_token":"x"})json",
                apiToken);
            const SelftestHttpResponse sentinelResponse = selftestPostJson(
                startedPort, "/api/settings", R"json({"stale_days":"[redacted]"})json",
                apiToken);
            check(plainTextResponse.status == 400 &&
                      originResponse.status == 400 &&
                      unknownSettingResponse.status == 403 &&
                      sentinelResponse.status == 400,
                  "API rejects browser-shaped posts and unsafe settings writes");
            const SelftestHttpResponse oversizedLengthResponse =
                selftestOversizedHttpBody(startedPort, apiToken, false);
            const SelftestHttpResponse healthAfterLength = selftestGetJsonEventually(
                startedPort, "/api/health", apiToken);
            const SelftestHttpResponse oversizedChunkedResponse =
                selftestOversizedHttpBody(startedPort, apiToken, true);
            const SelftestHttpResponse healthAfterChunked = selftestGetJsonEventually(
                startedPort, "/api/health", apiToken);
            if (healthAfterLength.status != 200 ||
                healthAfterChunked.status != 200)
                std::printf(
                    "selftest detail: post-cap health statuses=%lu/%lu\n",
                    static_cast<unsigned long>(healthAfterLength.status),
                    static_cast<unsigned long>(healthAfterChunked.status));
            check(oversizedLengthResponse.status == 413,
                  "HTTP parser rejects oversized Content-Length before body allocation");
            check(healthAfterLength.status == 200,
                  "health remains responsive after oversized Content-Length");
            check(oversizedChunkedResponse.status == 413,
                  "HTTP parser rejects an oversized chunked body before appending beyond the cap");
            check(healthAfterChunked.status == 200,
                  "health remains responsive after an oversized chunked body");
            p0Server.stop();
            check(!std::filesystem::exists(p0RendezvousPath),
                  "API shutdown removes its owned rendezvous");
        }

        std::string failedBuildError;
        const long long failedBuildId = p0Builds.start(
            projectId, "build", "cmd /C exit /B 3", testDir.string(), "",
            failedBuildError);
        nlohmann::json failedBuildStatus;
        bool failedBuildPersisted = false;
        const auto failedBuildDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (failedBuildId > 0 &&
               std::chrono::steady_clock::now() < failedBuildDeadline) {
            failedBuildStatus = p0Builds.status(failedBuildId, 0);
            if (failedBuildStatus.value("done", false)) {
                auto lk = db.guard();
                SQLite::Statement persisted(db.raw(lk.token()),
                    "SELECT status,exit_code FROM builds WHERE id=?");
                persisted.bind(1, failedBuildId);
                failedBuildPersisted = persisted.executeStep() &&
                    persisted.getColumn(0).getString() == "failed" &&
                    persisted.getColumn(1).getInt() == 3;
                if (failedBuildPersisted) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(failedBuildId > 0 && failedBuildError.empty() &&
                   failedBuildStatus.value("status", "") == "failed" &&
                   failedBuildStatus.value("exit_code", -1) == 3 &&
                   failedBuildPersisted,
                "deliberate exit-3 build is durably reported as failed");

        BuildRunner injectedReadFailureBuilds(
            &db, BuildRunnerTestHooks{0, ERROR_READ_FAULT});
        std::string injectedReadError;
        const long long injectedReadId = injectedReadFailureBuilds.start(
            projectId, "build", "cmd /C exit /B 0", testDir.string(), "",
            injectedReadError);
        nlohmann::json injectedReadStatus;
        const auto injectedReadDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (injectedReadId > 0 &&
               std::chrono::steady_clock::now() < injectedReadDeadline) {
            injectedReadStatus = injectedReadFailureBuilds.status(
                injectedReadId, 0);
            if (injectedReadStatus.value("done", false)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        injectedReadFailureBuilds.stop();
        std::string injectedPersistedStatus;
        {
            auto lk = db.guard();
            SQLite::Statement persisted(db.raw(lk.token()),
                "SELECT status FROM builds WHERE id=?");
            persisted.bind(1, injectedReadId);
            if (persisted.executeStep())
                injectedPersistedStatus = persisted.getColumn(0).getString();
        }
        check(injectedReadId > 0 && injectedReadError.empty() &&
                  injectedReadStatus.value("done", false) &&
                  injectedReadStatus.value("status", "") == "error" &&
                  injectedReadStatus.value("output", "").find(
                      "child output read failed (error 30)") !=
                      std::string::npos &&
                  injectedPersistedStatus == "error",
              "injected output-pipe read failure overrides a zero child exit and persists its Win32 diagnostic");

        } catch (const std::exception& e) {
            std::printf("  [FAIL] P0 disposable acceptance section: %s\n",
                        e.what());
            ++failures;
        }

        try {
        std::printf("selftest: P1 monotonic build stream\n");
        BuildRunner streamingBuilds(&db);
        const std::filesystem::path streamingFixture =
            testDir / "build-stream-fixture.txt";
        const bool streamingFixtureWritten = writeFileUtf8(
            streamingFixture.string(), std::string(5 * 1024 * 1024, '='));
        std::string streamingError;
        const long long streamingId = streamingBuilds.start(
            projectId, "build",
            "type \"" + streamingFixture.string() + "\"",
            testDir.string(), "", streamingError);
        size_t streamingOffset = 0;
        size_t lastStreamingOffset = 0;
        bool streamingMonotonic = true;
        bool streamedPastCap = false;
        bool streamingDone = false;
        nlohmann::json streamingStatus;
        const auto streamingDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (streamingId > 0 &&
               std::chrono::steady_clock::now() < streamingDeadline) {
            streamingStatus = streamingBuilds.status(streamingId, streamingOffset);
            const size_t next = streamingStatus.value(
                "next_offset", streamingOffset);
            streamingMonotonic = streamingMonotonic && next >= lastStreamingOffset;
            lastStreamingOffset = next;
            streamingOffset = next;
            streamedPastCap = streamedPastCap || next > 2 * 1024 * 1024;
            if (streamingStatus.value("done", false)) {
                streamingDone = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        streamingBuilds.stop();
        check(streamingFixtureWritten && streamingId > 0 &&
                  streamingError.empty() && streamingDone &&
                  streamingMonotonic && streamedPastCap &&
                  streamingStatus.value("status", "") == "success" &&
                  streamingStatus.value("dropped_before", size_t{0}) > 0,
              "build offsets remain absolute and monotonic beyond the 2 MiB live window");

        } catch (const std::exception& e) {
            std::printf("  [FAIL] P1 monotonic build stream section: %s\n",
                        e.what());
            ++failures;
        }

        try {
        std::printf("selftest: P1 owned worker shutdown\n");
        BuildRunnerTestHooks ownedBuildHooks;
        ownedBuildHooks.injectedTerminationError = ERROR_PRIVILEGE_NOT_HELD;
        BuildRunner ownedBuilds(&db, ownedBuildHooks);
        std::string ownedBuildError;
        const long long ownedBuildId = ownedBuilds.start(
            projectId, "build", "ping -n 30 127.0.0.1 >NUL",
            testDir.string(), "", ownedBuildError);
        std::string duplicateBuildError;
        const long long duplicateBuildId = ownedBuilds.start(
            projectId, "build", "cmd /C exit /B 0", testDir.string(), "",
            duplicateBuildError);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto ownedStopStarted = std::chrono::steady_clock::now();
        ownedBuilds.stop();
        const auto ownedStopElapsed = std::chrono::steady_clock::now() -
                                      ownedStopStarted;
        const nlohmann::json ownedBuildStatus =
            ownedBuilds.status(ownedBuildId, 0);
        check(ownedBuildId > 0 && ownedBuildError.empty() &&
                   duplicateBuildId == 0 &&
                   duplicateBuildError.find("already") != std::string::npos &&
                   ownedStopElapsed < std::chrono::seconds(5) &&
                   ownedBuildStatus.value("done", false) &&
                    ownedBuildStatus.value("status", "") == "error" &&
                    ownedBuildStatus.value("output", "").find(
                        "stopped during application shutdown") !=
                        std::string::npos &&
                    ownedBuildStatus.value("output", "").find(
                        "child termination failed (error 1314)") !=
                        std::string::npos,
              "owned build stop rejects a duplicate, terminates the child, preserves its injected Win32 diagnostic, and joins");

        } catch (const std::exception& e) {
            std::printf("  [FAIL] P1 owned worker shutdown section: %s\n",
                        e.what());
            ++failures;
        }

        const std::string revokedAdminId = "823456789012345678";
        std::string priorAdminIds;
        {
            auto lk = db.guard();
            priorAdminIds = db.getSetting(lk.token(), "discord_admin_user_ids", "");
            db.setSetting(lk.token(), "discord_admin_user_ids", revokedAdminId);
        }
        ManualCommandStageOutcome revokedStage = stageManualCaptureCommand(
            &db, "823456789012345670", "revoked-admin-fixture",
            "823456789012345671", "test guild", "823456789012345672",
            "Revoked Admin", revokedAdminId, "<@1> capture",
            "2026-08-02T15:30:00Z", "823456789012345673", "capture this");
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_admin_user_ids", "923456789012345678");
        }
        ManualCaptureCommand revokedCommand;
        const bool revokedSelected =
            nextPendingManualCaptureCommand(&db, revokedCommand);
        ManualCommandFailureOutcome revokedFailure;
        if (revokedSelected &&
            !discordAdminUserAllowed(&db, revokedCommand.adminId)) {
            revokedFailure = recordManualCaptureCommandFailure(
                &db, revokedCommand.commandRowId,
                "author is no longer an authorized admin",
                /*retryable=*/false);
        }
        std::string revokedState;
        int revokedTargetRows = -1;
        {
            auto lk = db.guard();
            SQLite::Statement commandState(db.raw(lk.token()),
                "SELECT manual_command_state FROM discord_messages WHERE id=?");
            commandState.bind(1, revokedStage.command.commandRowId);
            if (commandState.executeStep())
                revokedState = commandState.getColumn(0).getString();
            SQLite::Statement targetRows(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_messages WHERE message_id=?");
            targetRows.bind(1, revokedStage.command.targetMessageId);
            requireRow(targetRows, "selftest query targetRows at source line 2032 returned a row");
            revokedTargetRows = targetRows.getColumn(0).getInt();
            db.setSetting(lk.token(), "discord_admin_user_ids", priorAdminIds);
        }
        check(revokedStage.accepted && revokedSelected &&
                  revokedCommand.commandRowId ==
                      revokedStage.command.commandRowId &&
                  revokedFailure.terminal &&
                  revokedState == "failed" && revokedTargetRows == 0,
              "revoked pending manual command terminates before target capture");

        try {
        std::printf("selftest: Discord mapped direct intake and admin reply lineage\n");
        const std::string intakePath =
            (testDir / "discord-manual-intake.db").string();
        Db intakeDb(intakePath);
        long long intakeProjectId = 0;
        {
            auto lk = intakeDb.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement project(intakeDb.raw(lk.token()),
                "INSERT INTO projects(name,slug,created_at,updated_at) "
                "VALUES('Discord intake fixture','discord-intake-fixture',?,?)");
            project.bind(1, now);
            project.bind(2, now);
            project.exec();
            intakeProjectId = intakeDb.raw(lk.token()).getLastInsertRowid();
        }

        const std::string mappedChannel = "523456789012345670";
        const std::string mappedDirectMessage = "523456789012345671";
        const std::string intakeAdminId = "523456789012345672";
        const std::string creditedUserId = "523456789012345673";
        ManualCommandStageOutcome mappedDirect = stageManualCaptureCommand(
            &intakeDb, mappedChannel, "mapped-project", "523456789012345674",
            "intake guild", mappedDirectMessage, "Intake Admin", intakeAdminId,
            "<@999999999999999999> <@523456789012345673> fix crash in mapped request",
            "2026-08-22T12:00:00Z", mappedDirectMessage, "", {});
        {
            auto lk = intakeDb.guard();
            SQLite::Statement map(intakeDb.raw(lk.token()),
                "UPDATE discord_channels SET project_id=? WHERE channel_id=?");
            map.bind(1, intakeProjectId);
            map.bind(2, mappedChannel);
            map.exec();
        }
        IngestOutcome mappedCapture = completeManualCaptureCommand(
            &intakeDb, mappedDirect.command, "Credited User", creditedUserId,
            "fix crash in mapped request", "2026-08-22T12:00:00Z", {});
        MappedManualPromotionOutcome mappedPromotion =
            promoteMappedManualCapture(
                &intakeDb, nullptr, mappedCapture.messageRowId);
        MappedManualPromotionOutcome mappedReplay =
            promoteMappedManualCapture(
                &intakeDb, nullptr, mappedCapture.messageRowId);
        {
            auto lk = intakeDb.guard();
            SQLite::Statement mapped(intakeDb.raw(lk.token()), R"sql(
SELECT m.state,m.author,m.author_id,COALESCE(m.item_id,0),i.project_id,
       i.priority,s.name,s.handle,x.credited,i.credited,m.kind,i.type
  FROM discord_messages m
  JOIN items i ON i.id=m.item_id
  JOIN sources s ON s.id=i.source_id
  JOIN item_sources x ON x.item_id=i.id AND x.source_id=s.id
 WHERE m.id=?
)sql");
            mapped.bind(1, mappedCapture.messageRowId);
            requireRow(mapped,
                       "mapped direct intake promotion returned a row");
            check(mappedDirect.accepted && mappedCapture.ingested &&
                      mappedPromotion.mapped && mappedPromotion.ok &&
                      !mappedPromotion.duplicate &&
                      mappedPromotion.itemId > 0 && mappedReplay.mapped &&
                      mappedReplay.ok && mappedReplay.duplicate &&
                      mappedReplay.itemId == mappedPromotion.itemId &&
                      mapped.getColumn(0).getString() == "promoted" &&
                      mapped.getColumn(1).getString() == "Credited User" &&
                      mapped.getColumn(2).getString() == creditedUserId &&
                      mapped.getColumn(3).getInt64() ==
                          mappedPromotion.itemId &&
                      mapped.getColumn(4).getInt64() == intakeProjectId &&
                      mapped.getColumn(5).getInt() ==
                          kDiscordReviewNormalPriority &&
                      mapped.getColumn(6).getString() == "Credited User" &&
                      mapped.getColumn(7).getString() == creditedUserId &&
                       mapped.getColumn(8).getInt() == 1 &&
                       mapped.getColumn(9).getInt() == 1 &&
                       mapped.getColumn(10).getString() == "bug" &&
                       mapped.getColumn(11).getString() == "fix",
                  "mapped direct admin intake promotes once at normal priority as a fix, preserves classification, and credits the selected Discord contributor");
        }

        const std::string commonChannel = "523456789012345680";
        const std::string commonDirectMessage = "523456789012345681";
        ManualCommandStageOutcome commonDirect = stageManualCaptureCommand(
            &intakeDb, commonChannel, "common", "523456789012345674",
            "intake guild", commonDirectMessage, "Intake Admin", intakeAdminId,
            "<@999999999999999999> common request",
            "2026-08-22T12:01:00Z", commonDirectMessage, "", {});
        IngestOutcome commonCapture = completeManualCaptureCommand(
            &intakeDb, commonDirect.command, "Intake Admin", intakeAdminId,
            "common request", "2026-08-22T12:01:00Z", {});
        MappedManualPromotionOutcome commonPromotion =
            promoteMappedManualCapture(
                &intakeDb, nullptr, commonCapture.messageRowId);
        {
            auto lk = intakeDb.guard();
            SQLite::Statement common(intakeDb.raw(lk.token()),
                "SELECT state,COALESCE(item_id,0) FROM discord_messages WHERE id=?");
            common.bind(1, commonCapture.messageRowId);
            requireRow(common,
                       "common-channel direct intake returned a row");
            check(commonDirect.accepted && commonCapture.ingested &&
                      !commonPromotion.mapped && commonPromotion.ok &&
                      commonPromotion.itemId == 0 &&
                      common.getColumn(0).getString() == "new" &&
                      common.getColumn(1).getInt64() == 0,
                  "common-channel direct admin intake stays pending for explicit project selection");
        }

        const std::string replyChannel = "523456789012345690";
        const std::string sourceMessage = "523456789012345691";
        IngestOutcome replySource = manualCaptureSuggestion(
            &intakeDb, replyChannel, "mapped-project", "523456789012345674",
            "intake guild", sourceMessage, "Original User",
            "523456789012345692", "original request",
            "2026-08-22T12:02:00Z", "", {});
        nlohmann::json replySourcePromotion = promoteSuggestion(
            &intakeDb, nullptr, replySource.messageRowId, intakeProjectId,
            "", "");
        const long long replyItemId =
            replySourcePromotion.value("item_id", 0LL);
        const std::string firstAdminPost = "523456789012345693";
        const std::string secondAdminPost = "523456789012345694";
        const std::string firstFollowup = "first admin follow-up";
        const std::string secondFollowup = "second admin-post follow-up";
        ManualCommandStageOutcome firstAdminCommand = stageManualCaptureCommand(
            &intakeDb, replyChannel, "mapped-project", "523456789012345674",
            "intake guild", firstAdminPost, "Intake Admin", intakeAdminId,
            "<@999999999999999999> first follow-up",
            "2026-08-22T12:03:00Z", sourceMessage, firstFollowup, {});
        ManualCommandStageOutcome secondAdminCommand = stageManualCaptureCommand(
            &intakeDb, replyChannel, "mapped-project", "523456789012345674",
            "intake guild", secondAdminPost, "Intake Admin", intakeAdminId,
            "<@999999999999999999> second follow-up",
            "2026-08-22T12:04:00Z", firstAdminPost, secondFollowup, {});
        IngestOutcome firstAppend = completeManualCaptureCommand(
            &intakeDb, firstAdminCommand.command, "Original User",
            "523456789012345692", "original request",
            "2026-08-22T12:02:00Z", {});
        ManualCaptureCommand resolvedSecondCommand;
        const bool secondSelected = nextPendingManualCaptureCommand(
            &intakeDb, resolvedSecondCommand);
        IngestOutcome secondAppend = completeManualCaptureCommand(
            &intakeDb, resolvedSecondCommand, "Original User",
            "523456789012345692", "original request",
            "2026-08-22T12:02:00Z", {});
        {
            auto lk = intakeDb.guard();
            SQLite::Statement item(intakeDb.raw(lk.token()),
                "SELECT body FROM items WHERE id=?");
            item.bind(1, replyItemId);
            requireRow(item, "admin-post follow-up item returned a row");
            const std::string body = item.getColumn(0).getString();
            const std::size_t firstAt = body.find(firstFollowup);
            const std::size_t secondAt = body.find(secondFollowup);
            SQLite::Statement result(intakeDb.raw(lk.token()),
                "SELECT manual_target_message_id,manual_result_item_id "
                "FROM discord_messages WHERE id=?");
            result.bind(1, secondAdminCommand.command.commandRowId);
            requireRow(result,
                       "admin-post follow-up command returned a row");
            check(replySource.ingested &&
                      replySourcePromotion.value("ok", false) &&
                      replyItemId > 0 && firstAdminCommand.accepted &&
                      secondAdminCommand.accepted &&
                      secondAdminCommand.command.targetMessageId ==
                          firstAdminPost && firstAppend.appendedToItem &&
                      firstAppend.itemId == replyItemId && secondSelected &&
                      resolvedSecondCommand.commandRowId ==
                          secondAdminCommand.command.commandRowId &&
                      resolvedSecondCommand.targetMessageId == sourceMessage &&
                      secondAppend.appendedToItem &&
                      secondAppend.itemId == replyItemId &&
                      result.getColumn(0).getString() == sourceMessage &&
                      result.getColumn(1).getInt64() == replyItemId &&
                      firstAt != std::string::npos &&
                      body.find(firstFollowup,
                                firstAt + firstFollowup.size()) ==
                          std::string::npos &&
                      secondAt != std::string::npos &&
                      body.find(secondFollowup,
                                secondAt + secondFollowup.size()) ==
                          std::string::npos,
                  "replying to a completed admin post resolves persisted source lineage and appends each follow-up once");
        }
        } catch (const std::exception& e) {
            std::printf("  [FAIL] Discord manual intake section: %s\n",
                        e.what());
            ++failures;
        }

        const std::string reviewSourceChannelId = "423456789012345670";
        const std::string reviewNotifyChannelId = "423456789012345690";
        auto createReviewCard = [&](const std::string& sourceMessageId,
                                    const std::string& notifyMessageId,
                                    const std::string& content,
                                    long long inferredProjectId) {
            IngestOutcome capture = ingestDiscordMessage(
                &db, reviewSourceChannelId, "private-admin-review",
                "423456789012345669", "test guild", sourceMessageId,
                "Review Fixture", "423456789012345668", content,
                "2026-07-20T15:58:47Z", false, {});
            auto lk = db.guard();
            if (inferredProjectId > 0) {
                SQLite::Statement mapped(db.raw(lk.token()),
                    "UPDATE discord_messages SET inferred_project_id=? WHERE id=?");
                mapped.bind(1, inferredProjectId);
                mapped.bind(2, capture.messageRowId);
                mapped.exec();
            } else {
                SQLite::Statement unmapped(db.raw(lk.token()),
                    "UPDATE discord_messages SET inferred_project_id=NULL WHERE id=?");
                unmapped.bind(1, capture.messageRowId);
                unmapped.exec();
            }
            const std::string now = nowIsoUtc();
            SQLite::Statement card(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                "notify_channel_id,notify_message_id,post_state,created_at,updated_at) "
                "VALUES(?,NULL,?,?,'posted',?,?)");
            card.bind(1, capture.messageRowId);
            card.bind(2, reviewNotifyChannelId);
            card.bind(3, notifyMessageId);
            card.bind(4, now);
            card.bind(5, now);
            card.exec();
            return capture;
        };

        const std::string approvalSourceMessageId = "423456789012345691";
        const std::string approvalNotifyMessageId = "423456789012345701";
        IngestOutcome approvalCapture = createReviewCard(
            approvalSourceMessageId, approvalNotifyMessageId,
            "suggestion: approve this private review fixture", projectId);
        DiscordReviewOutcome unauthorizedReview = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, approvalNotifyMessageId,
            "111111111111111111", true, "unauthorized note");
        check(approvalCapture.ingested && unauthorizedReview.matched &&
                  !unauthorizedReview.authorized && !unauthorizedReview.ok &&
                  pendingDiscordReviewCard(&db, reviewNotifyChannelId,
                                           approvalNotifyMessageId),
              "private review cards ignore unauthorized approval and reply notes");

        const std::string approvedNote =
            "Approved from the private review reply with extra context.";
        DiscordReviewOutcome approvedReview = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, approvalNotifyMessageId,
            "123456789012345678", true, approvedNote);
        DiscordReviewOutcome approvalReplay = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, approvalNotifyMessageId,
            "987654321098765432", true, approvedNote);
        DiscordReviewOutcome oppositeReplay = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, approvalNotifyMessageId,
            "123456789012345678", false);
        {
            auto lk = db.guard();
            SQLite::Statement approved(db.raw(lk.token()),
                "SELECT m.state,COALESCE(m.item_id,0),m.admin_note,i.body,"
                "i.priority "
                "FROM discord_messages m LEFT JOIN items i ON i.id=m.item_id "
                "WHERE m.id=?");
            approved.bind(1, approvalCapture.messageRowId);
            requireRow(approved, "selftest query approved at source line 2114 returned a row");
            const std::string itemBody = approved.getColumn(3).getString();
            const std::size_t firstNote = itemBody.find(approvedNote);
            check(approvedReview.matched && approvedReview.authorized &&
                      approvedReview.ok && !approvedReview.duplicate &&
                      approvedReview.itemId > 0 &&
                      approvalReplay.ok && approvalReplay.duplicate &&
                      approvalReplay.itemId == approvedReview.itemId &&
                      oppositeReplay.ok && oppositeReplay.duplicate &&
                      oppositeReplay.itemId == approvedReview.itemId &&
                      approved.getColumn(0).getString() == "promoted" &&
                      approved.getColumn(1).getInt64() == approvedReview.itemId &&
                      approved.getColumn(2).getString() == approvedNote &&
                      approved.getColumn(4).getInt() ==
                          kDiscordReviewNormalPriority &&
                      firstNote != std::string::npos &&
                      itemBody.find(approvedNote, firstNote + approvedNote.size()) ==
                          std::string::npos &&
                      !pendingDiscordReviewCard(&db, reviewNotifyChannelId,
                                                approvalNotifyMessageId) &&
                      terminalDiscordReviewCard(&db, reviewNotifyChannelId,
                                                approvalNotifyMessageId),
                  "authorized reply approval promotes once at normal priority and appends its note exactly once");
        }

        auto verifyPriorityApproval =
            [&](const std::string& sourceMessageId,
                const std::string& notifyMessageId, int priority,
                const std::string& label) {
                IngestOutcome capture = createReviewCard(
                    sourceMessageId, notifyMessageId,
                    "suggestion: approve this " + label +
                        " priority review fixture",
                    projectId);
                DiscordReviewOutcome review = reviewNotificationCard(
                    &db, nullptr, reviewNotifyChannelId, notifyMessageId,
                    "123456789012345678", true, "", priority);
                auto lk = db.guard();
                SQLite::Statement promoted(db.raw(lk.token()),
                    "SELECT m.state,COALESCE(m.item_id,0),i.priority "
                    "FROM discord_messages m LEFT JOIN items i ON i.id=m.item_id "
                    "WHERE m.id=?");
                promoted.bind(1, capture.messageRowId);
                requireRow(promoted, "selftest query promoted at source line 2157 returned a row");
                const std::string assertionLabel =
                    "authorized " + label +
                    " priority approval creates the requested canonical priority";
                check(capture.ingested && review.matched &&
                          review.authorized && review.ok &&
                          !review.duplicate && review.itemId > 0 &&
                          promoted.getColumn(0).getString() == "promoted" &&
                          promoted.getColumn(1).getInt64() == review.itemId &&
                          promoted.getColumn(2).getInt() == priority,
                      assertionLabel.c_str());
            };
        verifyPriorityApproval(
            "423456789012345694", "423456789012345704",
            kDiscordReviewHighPriority, "high");
        verifyPriorityApproval(
            "423456789012345695", "423456789012345705",
            kDiscordReviewLowPriority, "low");

        const std::string rejectNotifyMessageId = "423456789012345702";
        IngestOutcome rejectCapture = createReviewCard(
            "423456789012345692", rejectNotifyMessageId,
            "bug: reject this private review fixture", projectId);
        DiscordReviewOutcome rejectedReview = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, rejectNotifyMessageId,
            "123456789012345678", false);
        DiscordReviewOutcome rejectedReplay = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, rejectNotifyMessageId,
            "987654321098765432", true, "must not reopen");
        {
            auto lk = db.guard();
            SQLite::Statement rejected(db.raw(lk.token()),
                "SELECT state,COALESCE(item_id,0),admin_note "
                "FROM discord_messages WHERE id=?");
            rejected.bind(1, rejectCapture.messageRowId);
            requireRow(rejected, "selftest query rejected at source line 2192 returned a row");
            check(rejectCapture.ingested && rejectedReview.matched &&
                      rejectedReview.authorized && rejectedReview.ok &&
                      !rejectedReview.duplicate &&
                      rejectedReview.state == "dismissed" &&
                      rejectedReplay.ok && rejectedReplay.duplicate &&
                      rejected.getColumn(0).getString() == "dismissed" &&
                      rejected.getColumn(1).getInt64() == 0 &&
                      rejected.getColumn(2).getString().empty() &&
                      !pendingDiscordReviewCard(&db, reviewNotifyChannelId,
                                                rejectNotifyMessageId) &&
                      terminalDiscordReviewCard(&db, reviewNotifyChannelId,
                                                rejectNotifyMessageId),
                  "reject reactions are replay-safe and never reopen or create a ticket");
        }

        const std::string unmappedNotifyMessageId = "423456789012345703";
        IngestOutcome unmappedCapture = createReviewCard(
            "423456789012345693", unmappedNotifyMessageId,
            "suggestion: leave this general review fixture pending", 0);
        DiscordReviewOutcome unmappedReview = reviewNotificationCard(
            &db, nullptr, reviewNotifyChannelId, unmappedNotifyMessageId,
            "123456789012345678", true, "must not persist without a project");
        {
            auto lk = db.guard();
            SQLite::Statement pending(db.raw(lk.token()),
                "SELECT state,COALESCE(item_id,0),admin_note "
                "FROM discord_messages WHERE id=?");
            pending.bind(1, unmappedCapture.messageRowId);
            requireRow(pending, "selftest query pending at source line 2221 returned a row");
            const std::vector<DiscordReviewCard> pendingCards =
                pendingDiscordReviewCards(&db, 50);
            const auto hasCard = [&](const std::string& messageId) {
                return std::any_of(
                    pendingCards.begin(), pendingCards.end(),
                    [&](const DiscordReviewCard& card) {
                        return card.channelId == reviewNotifyChannelId &&
                               card.messageId == messageId;
                    });
            };
            check(unmappedCapture.ingested && unmappedReview.matched &&
                      unmappedReview.authorized && !unmappedReview.ok &&
                      unmappedReview.error.find("no project") != std::string::npos &&
                      pending.getColumn(0).getString() == "new" &&
                      pending.getColumn(1).getInt64() == 0 &&
                      pending.getColumn(2).getString().empty() &&
                      hasCard(unmappedNotifyMessageId) &&
                      !terminalDiscordReviewCard(
                          &db, reviewNotifyChannelId, unmappedNotifyMessageId) &&
                      !hasCard(approvalNotifyMessageId) &&
                      !hasCard(rejectNotifyMessageId),
                   "unmapped approval remains pending while reconnect seeding lists only active cards");
        }

        IngestOutcome pendingEditCapture = createReviewCard(
            "423456789012345696", "423456789012345706",
            "suggestion: edit this pending ticket before assignment", 0);
        PendingSuggestionEdit pendingNotesEdit;
        pendingNotesEdit.content =
            "Revised pending ticket notes before a project is assigned.";
        pendingNotesEdit.adminNote =
            "Operator finalized this note in the pending editor.";
        nlohmann::json pendingNotesSaved = savePendingSuggestionEdit(
            &db, nullptr, pendingEditCapture.messageRowId, pendingNotesEdit);
        {
            auto lk = db.guard();
            SQLite::Statement pendingEdit(db.raw(lk.token()),
                "SELECT m.state,m.content,m.admin_note,m.inferred_project_id,"
                "n.edit_state FROM discord_messages m "
                "JOIN discord_notify_cards n "
                "ON n.discord_message_row_id=m.id WHERE m.id=?");
            pendingEdit.bind(1, pendingEditCapture.messageRowId);
            requireRow(pendingEdit, "selftest query pendingEdit at source line 2264 returned a row");
            check(pendingNotesSaved.value("ok", false) &&
                      pendingNotesSaved.value("pending", false) &&
                      pendingEdit.getColumn(0).getString() == "new" &&
                      pendingEdit.getColumn(1).getString() ==
                          pendingNotesEdit.content &&
                      pendingEdit.getColumn(2).getString() ==
                          pendingNotesEdit.adminNote &&
                      pendingEdit.getColumn(3).isNull() &&
                      pendingEdit.getColumn(4).getString() == "pending",
                  "pending editor saves revised notes without creating or assigning a ticket");
        }

        PendingSuggestionEdit assignedPendingEdit = pendingNotesEdit;
        assignedPendingEdit.projectId = projectId;
        assignedPendingEdit.title = "Final pending editor ticket";
        assignedPendingEdit.type = "implementation";
        assignedPendingEdit.priority = 3;
        assignedPendingEdit.content =
            "Final ticket notes saved from the pending editor.";
        nlohmann::json assignedPendingSaved = savePendingSuggestionEdit(
            &db, nullptr, pendingEditCapture.messageRowId, assignedPendingEdit);
        const long long assignedPendingItem =
            assignedPendingSaved.value("item_id", 0LL);
        {
            auto lk = db.guard();
            SQLite::Statement promotedPendingEdit(db.raw(lk.token()), R"sql(
SELECT m.state,m.content,m.admin_note,m.inferred_project_id,
       i.project_id,i.title,i.type,i.priority,i.body,
       COALESCE((SELECT MIN(credited) FROM item_sources
                  WHERE item_id=i.id),0),n.item_id
  FROM discord_messages m JOIN items i ON i.id=m.item_id
  JOIN discord_notify_cards n ON n.discord_message_row_id=m.id
 WHERE m.id=?)sql");
            promotedPendingEdit.bind(1, pendingEditCapture.messageRowId);
            requireRow(promotedPendingEdit,
                       "selftest query promotedEdit at source line 2299 returned a row");
            check(assignedPendingSaved.value("ok", false) &&
                      assignedPendingItem > 0 &&
                      promotedPendingEdit.getColumn(0).getString() == "promoted" &&
                      promotedPendingEdit.getColumn(1).getString() ==
                          assignedPendingEdit.content &&
                      promotedPendingEdit.getColumn(2).getString() ==
                          assignedPendingEdit.adminNote &&
                      promotedPendingEdit.getColumn(3).getInt64() == projectId &&
                      promotedPendingEdit.getColumn(4).getInt64() == projectId &&
                      promotedPendingEdit.getColumn(5).getString() ==
                          assignedPendingEdit.title &&
                      promotedPendingEdit.getColumn(6).getString() ==
                          assignedPendingEdit.type &&
                      promotedPendingEdit.getColumn(7).getInt() ==
                          assignedPendingEdit.priority &&
                      promotedPendingEdit.getColumn(8).getString().find(
                          assignedPendingEdit.adminNote) != std::string::npos &&
                      promotedPendingEdit.getColumn(9).getInt() == 1 &&
                      promotedPendingEdit.getColumn(10).getInt64() ==
                          assignedPendingItem,
                  "pending editor assigns and promotes one fully edited credited ticket");
        }
        nlohmann::json terminalPendingEdit = savePendingSuggestionEdit(
            &db, nullptr, pendingEditCapture.messageRowId, pendingNotesEdit);
        nlohmann::json terminalAssignedPendingEdit = savePendingSuggestionEdit(
            &db, nullptr, pendingEditCapture.messageRowId, assignedPendingEdit);
        check(!terminalPendingEdit.value("ok", false) &&
                  !terminalAssignedPendingEdit.value("ok", false) &&
                  terminalPendingEdit.value("error", "").find(
                      "no longer pending") != std::string::npos &&
                  terminalAssignedPendingEdit.value("error", "").find(
                      "no longer pending") != std::string::npos,
              "neither pending editor save path can revise a source after promotion wins");

        {
            auto lk = db.guard();
            SQLite::Statement legacy(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET edit_state='idle',"
                "edit_attempts=0,edit_next_retry_at='',edit_last_error='' "
                "WHERE notify_message_id IN (?,?)");
            legacy.bind(1, approvalNotifyMessageId);
            legacy.bind(2, rejectNotifyMessageId);
            legacy.exec();
        }
        const int reconciledLegacyControls =
            reconcileLegacyTerminalReviewControls(&db);
        const int reconciledLegacyReplay =
            reconcileLegacyTerminalReviewControls(&db);
        {
            auto lk = db.guard();
            SQLite::Statement queued(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_review_control_cleanups "
                "WHERE notify_channel_id=? AND notify_message_id IN (?,?) "
                "AND cleanup_state='pending'");
            queued.bind(1, reviewNotifyChannelId);
            queued.bind(2, approvalNotifyMessageId);
            queued.bind(3, rejectNotifyMessageId);
            requireRow(queued, "selftest query queued at source line 2357 returned a row");
            SQLite::Statement untouchedEdits(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards "
                "WHERE notify_message_id IN (?,?) AND edit_state='idle'");
            untouchedEdits.bind(1, approvalNotifyMessageId);
            untouchedEdits.bind(2, rejectNotifyMessageId);
            requireRow(untouchedEdits, "selftest query untouchedEdits at source line 2363 returned a row");
            SQLite::Statement pendingNotQueued(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_review_control_cleanups "
                "WHERE notify_message_id=?");
            pendingNotQueued.bind(1, unmappedNotifyMessageId);
            requireRow(pendingNotQueued, "selftest query pendingNotQueued at source line 2368 returned a row");
            check(reconciledLegacyControls >= 2 && reconciledLegacyReplay == 0 &&
                      queued.getColumn(0).getInt() == 2 &&
                      untouchedEdits.getColumn(0).getInt() == 2 &&
                      pendingNotQueued.getColumn(0).getInt() == 0 &&
                      db.getSetting(lk.token(),
                          "discord_terminal_review_control_cleanup_v2") == "1",
                  "legacy terminal review cards queue all-reaction cleanup once without editing historical cards");
        }
        long long inFlightCleanupId = 0;
        long long inFlightCleanupCardId = 0;
        int inFlightCleanupRevision = 0;
        {
            auto lk = db.guard();
            SQLite::Statement selectCleanup(db.raw(lk.token()),
                "SELECT id,card_row_id,cleanup_revision "
                "FROM discord_review_control_cleanups "
                "WHERE notify_message_id=?");
            selectCleanup.bind(1, approvalNotifyMessageId);
            requireRow(selectCleanup, "selftest query selectCleanup at source line 2387 returned a row");
            inFlightCleanupId = selectCleanup.getColumn(0).getInt64();
            inFlightCleanupCardId = selectCleanup.getColumn(1).getInt64();
            inFlightCleanupRevision = selectCleanup.getColumn(2).getInt();
            SQLite::Statement claimFixture(db.raw(lk.token()),
                "UPDATE discord_review_control_cleanups "
                "SET cleanup_state='deleting',cleanup_attempts=1 "
                "WHERE id=?");
            claimFixture.bind(1, inFlightCleanupId);
            claimFixture.exec();
        }
        const bool restagedInFlightCleanup = stageReviewControlCleanup(
            &db, inFlightCleanupCardId, reviewNotifyChannelId,
            approvalNotifyMessageId);
        int staleCleanupDelete = 0;
        {
            auto lk = db.guard();
            SQLite::Statement staleCallback(db.raw(lk.token()),
                "DELETE FROM discord_review_control_cleanups WHERE id=? "
                "AND cleanup_state='deleting' AND cleanup_attempts=1 "
                "AND cleanup_revision=?");
            staleCallback.bind(1, inFlightCleanupId);
            staleCallback.bind(2, inFlightCleanupRevision);
            staleCleanupDelete = staleCallback.exec();
            SQLite::Statement current(db.raw(lk.token()),
                "SELECT cleanup_state,cleanup_attempts,cleanup_revision "
                "FROM discord_review_control_cleanups WHERE id=?");
            current.bind(1, inFlightCleanupId);
            requireRow(current, "selftest query current at source line 2415 returned a row");
            check(restagedInFlightCleanup && staleCleanupDelete == 0 &&
                      current.getColumn(0).getString() == "pending" &&
                      current.getColumn(1).getInt() == 0 &&
                      current.getColumn(2).getInt() ==
                          inFlightCleanupRevision + 1,
                  "new terminal all-reaction cleanup generation survives an older in-flight callback");
        }
        long long retryCleanupId = 0;
        int retryCleanupRevision = 0;
        {
            auto lk = db.guard();
            SQLite::Statement selectCleanup(db.raw(lk.token()),
                "SELECT id,cleanup_revision "
                "FROM discord_review_control_cleanups "
                "WHERE notify_message_id=?");
            selectCleanup.bind(1, rejectNotifyMessageId);
            requireRow(selectCleanup, "selftest query selectCleanup at source line 2432 returned a row");
            retryCleanupId = selectCleanup.getColumn(0).getInt64();
            retryCleanupRevision = selectCleanup.getColumn(1).getInt();
            SQLite::Statement failCleanup(db.raw(lk.token()),
                "UPDATE discord_review_control_cleanups "
                "SET cleanup_state='failed',cleanup_attempts=5,"
                "cleanup_last_error='fixture failure' WHERE id=?");
            failCleanup.bind(1, retryCleanupId);
            failCleanup.exec();
        }
        const bool retriedReviewCleanup =
            retryFailedReviewControlCleanup(&db, nullptr, retryCleanupId);
        {
            auto lk = db.guard();
            SQLite::Statement retried(db.raw(lk.token()),
                "SELECT cleanup_state,cleanup_attempts,cleanup_revision,"
                "cleanup_last_error FROM discord_review_control_cleanups "
                "WHERE id=?");
            retried.bind(1, retryCleanupId);
            requireRow(retried, "selftest query retried at source line 2451 returned a row");
            check(retriedReviewCleanup &&
                      retried.getColumn(0).getString() == "pending" &&
                      retried.getColumn(1).getInt() == 0 &&
                      retried.getColumn(2).getInt() ==
                          retryCleanupRevision + 1 &&
                      retried.getColumn(3).getString().empty(),
                  "terminal all-reaction cleanup failures remain visible and operator-retryable");
        }

        detectedImage.attachmentId = "123456789012345672";
        detectedImage.sourceChannelId = "123456789012345670";
        detectedImage.sourceMessageId = "123456789012345671";
        detectedImage.sourceRole = "suggestion";
        detectedImage.filename = "..\\untrusted screenshot.PNG";
        detectedImage.contentType = "image/png";
        detectedImage.sizeBytes = 8;
        detectedImage.width = 1;
        detectedImage.height = 1;
        IngestOutcome imageCapture = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", detectedImage.sourceMessageId, "Image User",
            "123456789012345668", "suggestion: keep the screenshot with this ticket",
            "2026-07-17T15:00:00Z", false, {detectedImage});
        check(imageCapture.ingested && imageCapture.kind == "suggestion" &&
                  imageCapture.imageCount == 1 &&
                  !std::filesystem::exists(testDir / "ticket-images"),
              "pending Discord image capture stores metadata without a file write");

        DiscordAttachmentMeta detectedFile = detectedImage;
        detectedFile.attachmentId = "123456789012345801";
        detectedFile.sourceMessageId = "123456789012345800";
        detectedFile.filename = "..\\operator notes.txt";
        detectedFile.contentType = "text/plain";
        detectedFile.sizeBytes = 24;
        detectedFile.width = 0;
        detectedFile.height = 0;
        IngestOutcome fileCapture = ingestDiscordMessage(
            &db, detectedFile.sourceChannelId, "devhub",
            "123456789012345669", "test guild", detectedFile.sourceMessageId,
            "File User", "123456789012345802",
            "suggestion: keep the attached notes with this ticket",
            "2026-08-01T15:00:00Z", false, {detectedFile});
        check(fileCapture.ingested && fileCapture.kind == "suggestion" &&
                  fileCapture.imageCount == 1 &&
                  !std::filesystem::exists(testDir / "ticket-files"),
              "pending Discord text attachment stores metadata without a file write");
        const std::string fileBytes = "plain text ticket evidence";
        TicketImageSaveResult savedFile = saveTicketAttachmentBytes(
            dbPath, 131, detectedFile.sourceMessageId,
            detectedFile.attachmentId, detectedFile.filename,
            detectedFile.contentType, fileBytes);
        check(savedFile.ok && savedFile.created && !savedFile.isImage &&
                  savedFile.contentType == "text/plain" &&
                  savedFile.actualSize == fileBytes.size() &&
                  savedFile.relativePath ==
                      "ticket-files\\I131\\123456789012345800-123456789012345801.bin" &&
                  savedFile.relativePath.find("operator") == std::string::npos &&
                  std::filesystem::is_regular_file(savedFile.absolutePath),
              "non-raster attachment bytes use an inert hashed generated path");
        IngestOutcome imageReplay = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", detectedImage.sourceMessageId, "Image User",
            "123456789012345668", "suggestion: keep the screenshot with this ticket",
            "2026-07-17T15:00:00Z", false, {detectedImage});
        long long imageAttachmentRow = 0;
        {
            auto lk = db.guard();
            SQLite::Statement q(db.raw(lk.token()),
                "SELECT id FROM ticket_attachments WHERE source_message_id=?");
            q.bind(1, detectedImage.sourceMessageId);
            requireRow(q, "selftest query q at source line 2523 returned a row");
            imageAttachmentRow = q.getColumn(0).getInt64();
            SQLite::Statement count(db.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments WHERE source_message_id=?");
            count.bind(1, detectedImage.sourceMessageId);
            requireRow(count, "selftest query count at source line 2528 returned a row");
            check(imageReplay.duplicate && imageReplay.imageCount == 1 &&
                      count.getColumn(0).getInt() == 1,
                   "Discord image metadata capture is idempotent on replay");
        }

        DiscordAttachmentMeta channelCascadeImage = detectedImage;
        channelCascadeImage.attachmentId = "123456789012345679";
        channelCascadeImage.sourceChannelId = "123456789012345680";
        channelCascadeImage.sourceMessageId = "123456789012345681";
        IngestOutcome channelCascadeCapture = ingestDiscordMessage(
            &db, channelCascadeImage.sourceChannelId, "temporary-images",
            "123456789012345669", "test guild",
            channelCascadeImage.sourceMessageId, "Temporary User",
            "123456789012345682", "suggestion: temporary channel image",
            "2026-07-17T15:00:30Z", false, {channelCascadeImage});
        {
            auto lk = db.guard();
            SQLite::Statement del(db.raw(lk.token()),
                "DELETE FROM discord_channels WHERE channel_id=?");
            del.bind(1, channelCascadeImage.sourceChannelId);
            del.exec();
            SQLite::Statement remaining(db.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments "
                "WHERE source_message_id=?");
            remaining.bind(1, channelCascadeImage.sourceMessageId);
            requireRow(remaining, "selftest query remaining at source line 2554 returned a row");
            check(channelCascadeCapture.ingested &&
                      remaining.getColumn(0).getInt() == 0,
                  "deleting a monitored channel removes pending image metadata without violating schema checks");
        }

        const std::string tombstoneChannelId = "123456789012345720";
        const std::string tombstoneMessageId = "123456789012345721";
        IngestOutcome tombstoneCapture = ingestDiscordMessage(
            &db, tombstoneChannelId, "retained-deletions",
            "123456789012345669", "test guild", tombstoneMessageId,
            "Tombstone User", "123456789012345722",
            "suggestion: retain only my deleted message id",
            "2026-07-17T15:00:45Z", false, {});
        discordMessageDeleted(&db, nullptr, tombstoneMessageId);
        bool tombstoneBlockedChannelDelete = false;
        {
            auto lk = db.guard();
            try {
                SQLite::Statement del(db.raw(lk.token()),
                    "DELETE FROM discord_channels WHERE channel_id=?");
                del.bind(1, tombstoneChannelId);
                del.exec();
            } catch (const std::exception&) {
                tombstoneBlockedChannelDelete = true;
            }
        }
        IngestOutcome tombstoneLateReplay = ingestDiscordMessage(
            &db, tombstoneChannelId, "retained-deletions",
            "123456789012345669", "test guild", tombstoneMessageId,
            "Late Replay User", "123456789012345722",
            "suggestion: this late callback must stay deleted",
            "2026-07-17T15:00:46Z", false, {});
        {
            auto lk = db.guard();
            SQLite::Statement retained(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_messages m "
                "JOIN discord_channels c ON c.id=m.channel_row_id "
                "WHERE c.channel_id=? AND m.message_id=? "
                "AND m.state='deleted' AND m.author='' AND m.content=''");
            retained.bind(1, tombstoneChannelId);
            retained.bind(2, tombstoneMessageId);
            requireRow(retained, "selftest query retained at source line 2596 returned a row");
            SQLite::Statement cards(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards n "
                "JOIN discord_messages m ON m.id=n.discord_message_row_id "
                "WHERE m.message_id=?");
            cards.bind(1, tombstoneMessageId);
            requireRow(cards, "selftest query cards at source line 2602 returned a row");
            check(tombstoneCapture.ingested &&
                      tombstoneBlockedChannelDelete &&
                      tombstoneLateReplay.duplicate &&
                      retained.getColumn(0).getInt() == 1 &&
                      cards.getColumn(0).getInt() == 0,
                  "channel deletion preserves a cardless tombstone that blocks late source replay");
        }

        DiscordAttachmentMeta targetImage = detectedImage;
        targetImage.attachmentId = "123456789012345674";
        targetImage.sourceMessageId = "123456789012345673";
        targetImage.sourceRole = "suggestion";
        DiscordAttachmentMeta replyImage = detectedImage;
        replyImage.attachmentId = "123456789012345676";
        replyImage.sourceMessageId = "123456789012345675";
        replyImage.sourceRole = "admin_reply";
        IngestOutcome manualImages = manualCaptureSuggestion(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", targetImage.sourceMessageId, "Image-only User",
            "123456789012345667", "Image attachment submitted from Discord",
            "2026-07-17T15:01:00Z", "please review both images",
            {targetImage, replyImage});
        check(manualImages.ingested && manualImages.imageCount == 2 &&
                  !std::filesystem::exists(testDir / "ticket-images"),
              "reply capture associates target and admin-reply images without saving them");
        {
            auto lk = db.guard();
            SQLite::Statement permanentCopy(db.raw(lk.token()),
                "SELECT COUNT(*) FROM activity_log "
                "WHERE kind='discord_manual_capture'");
            requireRow(permanentCopy, "selftest query permanentCopy at source line 2633 returned a row");
            check(permanentCopy.getColumn(0).getInt() == 0,
                  "manual capture recent activity does not duplicate author or content into permanent history");
        }
        dismissSuggestion(&db, nullptr, manualImages.messageRowId);
        check(!std::filesystem::exists(testDir / "ticket-images"),
              "dismissing an image suggestion never creates a local file");

        DiscordAttachmentMeta deletedPendingImage = detectedImage;
        deletedPendingImage.sourceMessageId = "123456789012345687";
        deletedPendingImage.attachmentId = "123456789012345688";
        deletedPendingImage.filename = "deleted-before-promotion.png";
        IngestOutcome deletedPending = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild",
            deletedPendingImage.sourceMessageId, "Deleted User",
            "123456789012345686", "suggestion: delete this before promotion",
            "2026-07-17T15:01:30Z", false, {deletedPendingImage});
        long long deletedPendingCard = 0;
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement card(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                "notify_channel_id,notify_message_id,post_state,created_at,updated_at) "
                "VALUES(?,NULL,?,?,'posted',?,?)");
            card.bind(1, deletedPending.messageRowId);
            card.bind(2, "123456789012345690");
            card.bind(3, "123456789012345689");
            card.bind(4, now);
            card.bind(5, now);
            card.exec();
            deletedPendingCard = db.raw(lk.token()).getLastInsertRowid();
        }
        discordMessageDeleted(
            &db, nullptr, deletedPendingImage.sourceMessageId);
        {
            auto lk = db.guard();
            SQLite::Statement scrubbed(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_messages WHERE id=? "
                "AND state='deleted' AND author='' AND author_id='' "
                "AND content='' AND posted_at='' AND ingested_at='' "
                "AND admin_note='' AND manual_target_message_id='' "
                "AND manual_command_state='' AND manual_result_kind='' "
                "AND manual_effect_state='' AND score=0 AND matched='[]'");
            scrubbed.bind(1, deletedPending.messageRowId);
            requireRow(scrubbed, "selftest query scrubbed at source line 2679 returned a row");
            SQLite::Statement queuedCard(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards WHERE id=? "
                "AND discord_message_row_id=? AND item_id IS NULL "
                "AND edit_state='pending' AND edit_revision=1");
            queuedCard.bind(1, deletedPendingCard);
            queuedCard.bind(2, deletedPending.messageRowId);
            requireRow(queuedCard, "selftest query queuedCard at source line 2686 returned a row");
            SQLite::Statement removedMetadata(db.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments "
                "WHERE source_message_id=?");
            removedMetadata.bind(1, deletedPendingImage.sourceMessageId);
            requireRow(removedMetadata, "selftest query removedMetadata at source line 2691 returned a row");
            check(deletedPending.ingested &&
                      scrubbed.getColumn(0).getInt() == 1 &&
                      queuedCard.getColumn(0).getInt() == 1 &&
                      removedMetadata.getColumn(0).getInt() == 0,
                  "pending source deletion scrubs content and durably queues its card tombstone");
        }
        IngestOutcome deletedAutomaticReplay = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild",
            deletedPendingImage.sourceMessageId, "Late Automatic User",
            "123456789012345686", "suggestion: resurrect deleted source",
            "2026-07-17T15:01:31Z", false, {deletedPendingImage});
        IngestOutcome deletedManualReplay = manualCaptureSuggestion(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild",
            deletedPendingImage.sourceMessageId, "Late Manual User",
            "123456789012345686", "suggestion: resurrect deleted source",
            "2026-07-17T15:01:31Z", "late admin note", {deletedPendingImage});
        nlohmann::json deletedPromotion = promoteSuggestion(
            &db, nullptr, deletedPending.messageRowId, projectId,
            "implementation", "must not promote");
        nlohmann::json deletedDismissal = dismissSuggestion(
            &db, nullptr, deletedPending.messageRowId);
        {
            auto lk = db.guard();
            SQLite::Statement terminal(db.raw(lk.token()),
                "SELECT m.state,m.author,m.content,m.admin_note,n.edit_revision,"
                "(SELECT COUNT(*) FROM ticket_attachments a "
                "WHERE a.discord_message_row_id=m.id) "
                "FROM discord_messages m JOIN discord_notify_cards n "
                "ON n.discord_message_row_id=m.id WHERE m.id=?");
            terminal.bind(1, deletedPending.messageRowId);
            requireRow(terminal, "selftest query terminal at source line 2724 returned a row");
            check(deletedAutomaticReplay.duplicate &&
                      deletedManualReplay.duplicate &&
                      !deletedPromotion.value("ok", false) &&
                      !deletedDismissal.value("ok", false) &&
                      terminal.getColumn(0).getString() == "deleted" &&
                      terminal.getColumn(1).getString().empty() &&
                      terminal.getColumn(2).getString().empty() &&
                      terminal.getColumn(3).getString().empty() &&
                      terminal.getColumn(4).getInt() == 1 &&
                      terminal.getColumn(5).getInt() == 0,
                  "deleted source tombstone rejects late automatic/manual ingest, promotion, and dismissal");
        }
        IngestOutcome deletedDuringCreate = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", "123456789012345707",
            "Create Race User", "123456789012345708",
            "suggestion: delete while notification create is in flight",
            "2026-07-17T15:01:35Z", false, {});
        long long deletedDuringCreateCard = 0;
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement card(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                "notify_channel_id,notify_message_id,post_state,post_attempts,"
                "created_at,updated_at) VALUES(?,NULL,?,'','posting',1,?,?)");
            card.bind(1, deletedDuringCreate.messageRowId);
            card.bind(2, "123456789012345690");
            card.bind(3, now);
            card.bind(4, now);
            card.exec();
            deletedDuringCreateCard = db.raw(lk.token()).getLastInsertRowid();
        }
        discordMessageDeleted(&db, nullptr, "123456789012345707");
        {
            auto lk = db.guard();
            SQLite::Statement staleCreate(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET notify_message_id=?,"
                "post_state='posted' WHERE id=? AND post_state='posting' "
                "AND post_attempts=1 AND post_revision=0");
            staleCreate.bind(1, "123456789012345709");
            staleCreate.bind(2, deletedDuringCreateCard);
            const int staleChanged = staleCreate.exec();
            SQLite::Statement current(db.raw(lk.token()),
                "SELECT m.state,m.author,m.content,n.notify_message_id,"
                "n.post_state,n.post_attempts,n.post_revision "
                "FROM discord_messages m JOIN discord_notify_cards n "
                "ON n.discord_message_row_id=m.id WHERE n.id=?");
            current.bind(1, deletedDuringCreateCard);
            requireRow(current, "selftest query current at source line 2774 returned a row");
            check(deletedDuringCreate.ingested && staleChanged == 0 &&
                      current.getColumn(0).getString() == "deleted" &&
                      current.getColumn(1).getString().empty() &&
                      current.getColumn(2).getString().empty() &&
                      current.getColumn(3).getString().empty() &&
                      current.getColumn(4).getString() == "pending" &&
                      current.getColumn(5).getInt() == 0 &&
                      current.getColumn(6).getInt() == 1,
                  "source scrub invalidates an in-flight card create before stale content can be acknowledged");
        }
        DiscordAttachmentMeta deletedCommandImage = detectedImage;
        deletedCommandImage.sourceMessageId = "123456789012345704";
        deletedCommandImage.attachmentId = "123456789012345706";
        deletedCommandImage.sourceRole = "admin_reply";
        ManualCommandStageOutcome deletedCommand = stageManualCaptureCommand(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", "123456789012345704",
            "Deleted Admin", "123456789012345705",
            "<@123456789012345693> capture this", "2026-07-17T15:01:40Z",
            targetImage.sourceMessageId, "command must stay deleted",
            {deletedCommandImage});
        discordMessageDeleted(&db, nullptr, "123456789012345704");
        ManualCommandStageOutcome deletedCommandReplay = stageManualCaptureCommand(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", "123456789012345704",
            "Late Admin", "123456789012345705",
            "<@123456789012345693> resurrect", "2026-07-17T15:01:41Z",
            targetImage.sourceMessageId, "late note", {deletedCommandImage});
        {
            auto lk = db.guard();
            SQLite::Statement terminal(db.raw(lk.token()),
                "SELECT state,author,content,admin_note,manual_command_state,"
                "manual_target_message_id,(SELECT COUNT(*) FROM "
                "ticket_attachments a WHERE a.discord_message_row_id=m.id) "
                "FROM discord_messages m WHERE message_id=?");
            terminal.bind(1, "123456789012345704");
            requireRow(terminal, "selftest query terminal at source line 2811 returned a row");
            check(deletedCommand.accepted &&
                      deletedCommandReplay.duplicate &&
                      !deletedCommandReplay.accepted &&
                      deletedCommandReplay.state == "deleted" &&
                      terminal.getColumn(0).getString() == "deleted" &&
                      terminal.getColumn(1).getString().empty() &&
                      terminal.getColumn(2).getString().empty() &&
                      terminal.getColumn(3).getString().empty() &&
                      terminal.getColumn(4).getString().empty() &&
                      terminal.getColumn(5).getString().empty() &&
                      terminal.getColumn(6).getInt() == 0,
                  "deleted manual-command tombstone rejects duplicate backfill without restoring lifecycle or attachments");
        }
        {
            auto lk = db.guard();
            SQLite::Statement failCard(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET edit_state='failed',"
                "edit_attempts=5,edit_last_error=? WHERE id=?");
            failCard.bind(
                1, "permission failure at https://cdn.discordapp.com/private/card");
            failCard.bind(2, deletedPendingCard);
            failCard.exec();
        }
        const bool tombstoneRetry =
            retryFailedNotifyCard(&db, nullptr, deletedPendingCard);
        {
            auto lk = db.guard();
            SQLite::Statement retried(db.raw(lk.token()),
                "SELECT edit_state,edit_attempts,edit_revision,edit_last_error "
                "FROM discord_notify_cards WHERE id=?");
            retried.bind(1, deletedPendingCard);
            requireRow(retried, "selftest query retried at source line 2843 returned a row");
            check(tombstoneRetry &&
                      retried.getColumn(0).getString() == "pending" &&
                      retried.getColumn(1).getInt() == 0 &&
                      retried.getColumn(2).getInt() == 2 &&
                      retried.getColumn(3).getString().empty(),
                  "operator retry revives a terminal card edit with a new revision");
        }

        long long imageCardRow = 0;
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement card(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                "notify_channel_id,notify_message_id,created_at,updated_at) "
                "VALUES(?,NULL,?,'',?,?)");
            card.bind(1, imageCapture.messageRowId);
            card.bind(2, "123456789012345690");
            card.bind(3, now);
            card.bind(4, now);
            card.exec();
            imageCardRow = db.raw(lk.token()).getLastInsertRowid();
            SQLite::Statement failCreate(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET post_state='failed',"
                "post_attempts=5,post_last_error=? WHERE id=?");
            failCreate.bind(
                1, "permission failure at https://cdn.discordapp.com/private/create");
            failCreate.bind(2, imageCardRow);
            failCreate.exec();
        }
        const bool createRetry = retryFailedNotifyCard(&db, nullptr, imageCardRow);
        {
            auto lk = db.guard();
            SQLite::Statement retried(db.raw(lk.token()),
                "SELECT post_state,post_attempts,post_revision,post_last_error "
                "FROM discord_notify_cards WHERE id=?");
            retried.bind(1, imageCardRow);
            requireRow(retried, "selftest query retried at source line 2881 returned a row");
            SQLite::Statement freshClaim(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET post_state='posting',"
                "post_attempts=1 WHERE id=? AND post_state='pending' "
                "AND post_attempts=0 AND post_revision=1");
            freshClaim.bind(1, imageCardRow);
            const int claimed = freshClaim.exec();
            SQLite::Statement staleFailure(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET post_state='failed' "
                "WHERE id=? AND post_state='posting' AND post_attempts=1 "
                "AND post_revision=0");
            staleFailure.bind(1, imageCardRow);
            const int staleChanged = staleFailure.exec();
            SQLite::Statement current(db.raw(lk.token()),
                "SELECT post_state,post_attempts,post_revision "
                "FROM discord_notify_cards WHERE id=?");
            current.bind(1, imageCardRow);
            requireRow(current, "selftest query current at source line 2898 returned a row");
            check(createRetry &&
                      retried.getColumn(0).getString() == "pending" &&
                      retried.getColumn(1).getInt() == 0 &&
                      retried.getColumn(2).getInt() == 1 &&
                      retried.getColumn(3).getString().empty() &&
                      claimed == 1 && staleChanged == 0 &&
                      current.getColumn(0).getString() == "posting" &&
                      current.getColumn(1).getInt() == 1 &&
                      current.getColumn(2).getInt() == 1,
                  "operator retry revives a terminal card create with a new post generation");
            SQLite::Statement restore(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET post_state='pending',"
                "post_attempts=0 WHERE id=?");
            restore.bind(1, imageCardRow);
            restore.exec();
        }
        long long dismissedEditCard = 0, dismissedCreateCard = 0;
        long long dismissedOldMessageCard = 0;
        long long dismissedOldMessageCodeCard = 0;
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement editFailure(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                "notify_message_id,post_state,post_revision,edit_state,"
                "edit_attempts,edit_revision,edit_last_error,edit_updated_at,"
                "created_at,updated_at) "
                "VALUES(?,?,?,'posted',2,'failed',5,7,?,?,?,?)");
            editFailure.bind(1, keptId);
            editFailure.bind(2, "823456789012345700");
            editFailure.bind(3, "823456789012345701");
            editFailure.bind(4, "Missing Permissions");
            editFailure.bind(5, now);
            editFailure.bind(6, now);
            editFailure.bind(7, now);
            editFailure.exec();
            dismissedEditCard = db.raw(lk.token()).getLastInsertRowid();

            SQLite::Statement createFailure(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                "notify_message_id,post_state,post_attempts,post_revision,"
                "post_last_error,edit_state,created_at,updated_at) "
                "VALUES(?,?,'','failed',5,4,?,'idle',?,?)");
            createFailure.bind(1, keptId);
            createFailure.bind(2, "823456789012345700");
            createFailure.bind(
                3, "Maximum number of edits to messages older than 1 hour reached.");
            createFailure.bind(4, now);
            createFailure.bind(5, now);
            createFailure.exec();
            dismissedCreateCard = db.raw(lk.token()).getLastInsertRowid();

            SQLite::Statement oldMessageFailure(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                "notify_message_id,post_state,edit_state,edit_attempts,"
                "edit_revision,edit_last_error,edit_updated_at,created_at,updated_at) "
                "VALUES(?,?,?,'posted','failed',5,3,?,?,?,?)");
            oldMessageFailure.bind(1, keptId);
            oldMessageFailure.bind(2, "823456789012345700");
            oldMessageFailure.bind(3, "823456789012345702");
            oldMessageFailure.bind(
                4, "Maximum number of edits to messages older than 1 hour reached.");
            oldMessageFailure.bind(5, now);
            oldMessageFailure.bind(6, now);
            oldMessageFailure.bind(7, now);
            oldMessageFailure.exec();
            dismissedOldMessageCard = db.raw(lk.token()).getLastInsertRowid();

            SQLite::Statement oldMessageCodeFailure(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(item_id,notify_channel_id,"
                "notify_message_id,post_state,edit_state,edit_attempts,"
                "edit_revision,edit_last_error,edit_updated_at,created_at,updated_at) "
                "VALUES(?,?,?,'posted','failed',5,2,?,?,?,?)");
            oldMessageCodeFailure.bind(1, keptId);
            oldMessageCodeFailure.bind(2, "823456789012345700");
            oldMessageCodeFailure.bind(3, "823456789012345703");
            oldMessageCodeFailure.bind(4, "Discord REST error 30046");
            oldMessageCodeFailure.bind(5, now);
            oldMessageCodeFailure.bind(6, now);
            oldMessageCodeFailure.bind(7, now);
            oldMessageCodeFailure.exec();
            dismissedOldMessageCodeCard = db.raw(lk.token()).getLastInsertRowid();
        }
        const bool editAlertDismissed = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool editAlertDismissedAgain = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool staleAlertDismissed = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 6);
        const bool invalidAlertDismissed = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "unknown", 7);
        const std::vector<NotifyFailureAlertTarget> confirmedPhraseTarget = {
            {dismissedOldMessageCard, 3}};
        const NotifyFailureDismissResult bulkDismissed =
            dismissOldMessageNotifyCardAlerts(&db, confirmedPhraseTarget);
        const NotifyFailureDismissResult bulkDismissedAgain =
            dismissOldMessageNotifyCardAlerts(&db, confirmedPhraseTarget);
        const std::vector<NotifyFailureAlertTarget> confirmedCodeTarget = {
            {dismissedOldMessageCodeCard, 2}};
        const NotifyFailureDismissResult codeBulkDismissed =
            dismissOldMessageNotifyCardAlerts(&db, confirmedCodeTarget);
        {
            auto lk = db.guard();
            SQLite::Statement preserved(db.raw(lk.token()),
                "SELECT post_state,edit_state,edit_attempts,edit_revision,"
                "edit_last_error,notify_channel_id,notify_message_id "
                "FROM discord_notify_cards WHERE id=?");
            preserved.bind(1, dismissedEditCard);
            requireRow(preserved, "selftest query preserved at source line 3007 returned a row");
            SQLite::Statement acknowledgements(db.raw(lk.token()),
                "SELECT "
                "SUM(CASE WHEN card_row_id=? AND operation='edit' "
                "AND revision=7 AND reason='operator' THEN 1 ELSE 0 END),"
                "SUM(CASE WHEN card_row_id=? AND operation='create' "
                "THEN 1 ELSE 0 END),"
                "SUM(CASE WHEN card_row_id=? AND operation='edit' "
                "AND revision=3 AND reason='operator-old-message-limit' "
                "THEN 1 ELSE 0 END),"
                "SUM(CASE WHEN card_row_id=? AND operation='edit' "
                "AND revision=2 AND reason='operator-old-message-limit' "
                "THEN 1 ELSE 0 END) "
                "FROM discord_notify_failure_dismissals");
            acknowledgements.bind(1, dismissedEditCard);
            acknowledgements.bind(2, dismissedCreateCard);
            acknowledgements.bind(3, dismissedOldMessageCard);
            acknowledgements.bind(4, dismissedOldMessageCodeCard);
            requireRow(acknowledgements, "selftest query acknowledgements at source line 3025 returned a row");
            SQLite::Statement active(db.raw(lk.token()), R"sql(
WITH failures AS (
 SELECT id,'create' AS operation,post_revision AS revision
   FROM discord_notify_cards WHERE post_state='failed'
 UNION ALL
 SELECT id,'edit',edit_revision
   FROM discord_notify_cards WHERE edit_state='failed'
)
SELECT COUNT(*) FROM failures f
 WHERE f.id IN (?,?,?,?)
   AND NOT EXISTS(
     SELECT 1 FROM discord_notify_failure_dismissals d
      WHERE d.card_row_id=f.id AND d.operation=f.operation
        AND d.revision=f.revision))sql");
            active.bind(1, dismissedEditCard);
            active.bind(2, dismissedCreateCard);
            active.bind(3, dismissedOldMessageCard);
            active.bind(4, dismissedOldMessageCodeCard);
            requireRow(active, "selftest query active at source line 3044 returned a row");
            check(editAlertDismissed && editAlertDismissedAgain &&
                      !staleAlertDismissed && !invalidAlertDismissed &&
                      bulkDismissed.ok && bulkDismissed.changed == 1 &&
                      bulkDismissedAgain.ok &&
                      bulkDismissedAgain.changed == 0 &&
                      codeBulkDismissed.ok &&
                      codeBulkDismissed.changed == 1 &&
                      preserved.getColumn(0).getString() == "posted" &&
                      preserved.getColumn(1).getString() == "failed" &&
                      preserved.getColumn(2).getInt() == 5 &&
                      preserved.getColumn(3).getInt() == 7 &&
                      preserved.getColumn(4).getString() ==
                          "Missing Permissions" &&
                      preserved.getColumn(5).getString() ==
                          "823456789012345700" &&
                      preserved.getColumn(6).getString() ==
                          "823456789012345701" &&
                      acknowledgements.getColumn(0).getInt() == 1 &&
                      acknowledgements.getColumn(1).getInt() == 0 &&
                      acknowledgements.getColumn(2).getInt() == 1 &&
                      acknowledgements.getColumn(3).getInt() == 1 &&
                      active.getColumn(0).getInt() == 1,
                  "confirmed-snapshot bulk dismissal handles phrase/code forms, preserves lineage, and leaves same-text creates active");
        }
        const bool createAlertDismissed = dismissFailedNotifyCardAlert(
            &db, dismissedCreateCard, "create", 4);
        const bool createAlertDismissedAgain = dismissFailedNotifyCardAlert(
            &db, dismissedCreateCard, "create", 4);
        const bool editAlertRestored = restoreFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool editAlertRestoredAgain = restoreFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool editAlertRedismissed = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool dismissedRevisionRetried =
            retryFailedNotifyCard(&db, nullptr, dismissedEditCard);
        int visibleNewRevision = 0;
        {
            auto lk = db.guard();
            SQLite::Statement failNewRevision(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET edit_state='failed',"
                "edit_attempts=5,edit_last_error='new revision failure' "
                "WHERE id=? AND edit_state='pending' AND edit_revision=8");
            failNewRevision.bind(1, dismissedEditCard);
            const int failed = failNewRevision.exec();
            SQLite::Statement active(db.raw(lk.token()), R"sql(
SELECT COUNT(*) FROM discord_notify_cards n
 WHERE n.id=? AND n.edit_state='failed'
   AND NOT EXISTS(
     SELECT 1 FROM discord_notify_failure_dismissals d
      WHERE d.card_row_id=n.id AND d.operation='edit'
        AND d.revision=n.edit_revision))sql");
            active.bind(1, dismissedEditCard);
            requireRow(active, "selftest query active at source line 3098 returned a row");
            visibleNewRevision =
                failed == 1 ? active.getColumn(0).getInt() : -1;
        }
        const bool staleRevisionRedismissed = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool staleRevisionRestored = restoreFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 7);
        const bool newRevisionDismissed = dismissFailedNotifyCardAlert(
            &db, dismissedEditCard, "edit", 8);
        {
            auto lk = db.guard();
            SQLite::Statement finalState(db.raw(lk.token()),
                "SELECT edit_state,edit_revision,edit_last_error,"
                "notify_channel_id,notify_message_id,"
                "(SELECT COUNT(*) FROM discord_notify_failure_dismissals d "
                " WHERE d.card_row_id=n.id) "
                "FROM discord_notify_cards n WHERE n.id=?");
            finalState.bind(1, dismissedEditCard);
            requireRow(finalState, "selftest query finalState at source line 3117 returned a row");
            check(createAlertDismissed && createAlertDismissedAgain &&
                      editAlertRestored && !editAlertRestoredAgain &&
                      editAlertRedismissed && dismissedRevisionRetried &&
                      visibleNewRevision == 1 &&
                      !staleRevisionRedismissed &&
                      !staleRevisionRestored && newRevisionDismissed &&
                      finalState.getColumn(0).getString() == "failed" &&
                      finalState.getColumn(1).getInt() == 8 &&
                      finalState.getColumn(2).getString() ==
                          "new revision failure" &&
                      finalState.getColumn(3).getString() ==
                          "823456789012345700" &&
                      finalState.getColumn(4).getString() ==
                          "823456789012345701" &&
                      finalState.getColumn(5).getInt() == 2,
                  "acknowledged failures can be restored and a later failed revision becomes visible without losing lineage");
            SQLite::Statement cleanup(db.raw(lk.token()),
                "DELETE FROM discord_notify_cards WHERE id IN (?,?,?,?)");
            cleanup.bind(1, dismissedEditCard);
            cleanup.bind(2, dismissedCreateCard);
            cleanup.bind(3, dismissedOldMessageCard);
            cleanup.bind(4, dismissedOldMessageCodeCard);
            const int cardsRemoved = cleanup.exec();
            SQLite::Statement dismissalCleanup(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_failure_dismissals "
                "WHERE card_row_id IN (?,?,?,?)");
            dismissalCleanup.bind(1, dismissedEditCard);
            dismissalCleanup.bind(2, dismissedCreateCard);
            dismissalCleanup.bind(3, dismissedOldMessageCard);
            dismissalCleanup.bind(4, dismissedOldMessageCodeCard);
            requireRow(dismissalCleanup, "selftest query dismissalCleanup at source line 3148 returned a row");
            check(cardsRemoved == 4 &&
                      dismissalCleanup.getColumn(0).getInt() == 0,
                  "failure acknowledgement history cascades only when its canonical cards are explicitly removed");
        }
        long long orphanCleanupRow = 0;
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement orphan(db.raw(lk.token()),
                "INSERT INTO discord_notify_orphans(card_row_id,"
                "notify_channel_id,notify_message_id,cleanup_state,"
                "cleanup_attempts,cleanup_last_error,created_at,updated_at) "
                "VALUES(?,?,?,'failed',5,?,?,?)");
            orphan.bind(1, imageCardRow);
            orphan.bind(2, "123456789012345710");
            orphan.bind(3, "123456789012345711");
            orphan.bind(4,
                "temporary https://discord.com/channels/private cleanup error");
            orphan.bind(5, now);
            orphan.bind(6, now);
            orphan.exec();
            orphanCleanupRow = db.raw(lk.token()).getLastInsertRowid();
        }
        const bool orphanRetry = retryFailedNotifyOrphan(
            &db, nullptr, orphanCleanupRow);
        {
            auto lk = db.guard();
            SQLite::Statement claim(db.raw(lk.token()),
                "UPDATE discord_notify_orphans SET cleanup_state='deleting',"
                "cleanup_attempts=1 WHERE id=? AND cleanup_state='pending' "
                "AND cleanup_revision=1");
            claim.bind(1, orphanCleanupRow);
            const int claimed = claim.exec();
            SQLite::Statement staleFailure(db.raw(lk.token()),
                "UPDATE discord_notify_orphans SET cleanup_state='failed' "
                "WHERE id=? AND cleanup_state='deleting' "
                "AND cleanup_attempts=1 AND cleanup_revision=0");
            staleFailure.bind(1, orphanCleanupRow);
            const int staleChanged = staleFailure.exec();
            SQLite::Statement current(db.raw(lk.token()),
                "SELECT cleanup_state,cleanup_attempts,cleanup_revision,"
                "cleanup_last_error FROM discord_notify_orphans WHERE id=?");
            current.bind(1, orphanCleanupRow);
            requireRow(current, "selftest query current at source line 3192 returned a row");
            check(orphanRetry && claimed == 1 && staleChanged == 0 &&
                      current.getColumn(0).getString() == "deleting" &&
                      current.getColumn(1).getInt() == 1 &&
                      current.getColumn(2).getInt() == 1 &&
                      current.getColumn(3).getString().empty(),
                  "terminal exact-orphan cleanup retry uses a fresh generation against stale callbacks");
            SQLite::Statement remove(db.raw(lk.token()),
                "DELETE FROM discord_notify_orphans WHERE id=?");
            remove.bind(1, orphanCleanupRow);
            remove.exec();
        }
        nlohmann::json promotedImage = promoteSuggestion(
            &db, nullptr, imageCapture.messageRowId, projectId, "implementation", "");
        imageItemId = promotedImage.value("item_id", 0LL);
        check(promotedImage.value("ok", false) && imageItemId > 0 &&
                  promotedImage.value("images_pending", 0) == 1 &&
                  !std::filesystem::exists(testDir / "ticket-images"),
              "promotion creates the item first and leaves image bytes queued while bot is offline");

        int itemCountBeforeFollowup = 0;
        {
            auto lk = db.guard();
            SQLite::Statement count(db.raw(lk.token()), "SELECT COUNT(*) FROM items");
            requireRow(count, "selftest query count at source line 3216 returned a row");
            itemCountBeforeFollowup = count.getColumn(0).getInt();
        }
        DiscordAttachmentMeta lateTargetImage = detectedImage;
        lateTargetImage.attachmentId = "123456789012345683";
        lateTargetImage.filename = "original-message-follow-up.png";
        DiscordAttachmentMeta lateReplyImage = detectedImage;
        lateReplyImage.attachmentId = "123456789012345684";
        lateReplyImage.sourceMessageId = "123456789012345685";
        lateReplyImage.sourceRole = "admin_reply";
        lateReplyImage.filename = "admin-reply-follow-up.png";
        IngestOutcome promotedFollowup = manualCaptureSuggestion(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", detectedImage.sourceMessageId, "Image User",
            "123456789012345668", "suggestion: keep the screenshot with this ticket",
            "2026-07-17T15:03:00Z", "reference the screenshots attached",
            {detectedImage, lateTargetImage, lateReplyImage});
        {
            auto lk = db.guard();
            SQLite::Statement itemCount(db.raw(lk.token()), "SELECT COUNT(*) FROM items");
            requireRow(itemCount, "selftest query itemCount at source line 3236 returned a row");
            SQLite::Statement attachments(db.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments "
                "WHERE item_id=? AND state='queued'");
            attachments.bind(1, imageItemId);
            requireRow(attachments, "selftest query attachments at source line 3241 returned a row");
            SQLite::Statement body(db.raw(lk.token()), "SELECT body FROM items WHERE id=?");
            body.bind(1, imageItemId);
            requireRow(body, "selftest query body at source line 3244 returned a row");
            check(promotedFollowup.appendedToItem &&
                      promotedFollowup.itemId == imageItemId &&
                      promotedFollowup.imagesQueued == 2 &&
                      promotedFollowup.imageCount == 3 &&
                      itemCount.getColumn(0).getInt() == itemCountBeforeFollowup &&
                      attachments.getColumn(0).getInt() == 3 &&
                      body.getColumn(0).getString().find(
                          "reference the screenshots attached") != std::string::npos &&
                      !std::filesystem::exists(testDir / "ticket-images"),
                  "reply to promoted source appends note and new target/reply attachments to the same item");
        }
        IngestOutcome promotedFollowupReplay = manualCaptureSuggestion(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", detectedImage.sourceMessageId, "Image User",
            "123456789012345668", "suggestion: keep the screenshot with this ticket",
            "2026-07-17T15:03:00Z", "",
            {lateTargetImage, lateReplyImage});
        check(promotedFollowupReplay.duplicate &&
                  !promotedFollowupReplay.appendedToItem &&
                  promotedFollowupReplay.itemId == imageItemId &&
                  promotedFollowupReplay.imagesQueued == 0 &&
                  promotedFollowupReplay.imageCount == 3,
              "replayed promoted-ticket images remain an idempotent no-op");

        const std::string durableNote =
            "durable retry note must be appended exactly once";
        ManualCommandStageOutcome durableCommand = stageManualCaptureCommand(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", "123456789012345691", "DevHub Admin",
            "123456789012345692", "<@123456789012345693> " + durableNote,
            "2026-07-17T15:03:10Z", detectedImage.sourceMessageId,
            durableNote, {});
        ManualCaptureCommand dueCommand;
        const bool dueBeforeFailure =
            nextPendingManualCaptureCommand(&db, dueCommand);
        ManualCommandFailureOutcome transientFailure =
            recordManualCaptureCommandFailure(
                &db, dueCommand.commandRowId,
                "temporary https://cdn.discordapp.com/attachments/private retry",
                true);
        {
            auto lk = db.guard();
            SQLite::Statement retry(db.raw(lk.token()),
                "UPDATE discord_messages SET manual_next_retry_at='' WHERE id=?");
            retry.bind(1, dueCommand.commandRowId);
            retry.exec();
        }
        ManualCaptureCommand recoveredCommand;
        const bool recoveredAfterFailure =
            nextPendingManualCaptureCommand(&db, recoveredCommand);
        IngestOutcome durableComplete = completeManualCaptureCommand(
            &db, recoveredCommand, "Image User", "123456789012345668",
            "suggestion: keep the screenshot with this ticket",
            "2026-07-17T15:00:00Z", {});
        IngestOutcome durableReplay = completeManualCaptureCommand(
            &db, recoveredCommand, "Image User", "123456789012345668",
            "suggestion: keep the screenshot with this ticket",
            "2026-07-17T15:00:00Z", {});
        ManualCommandStageOutcome durableDuplicate = stageManualCaptureCommand(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", "123456789012345691", "DevHub Admin",
            "123456789012345692", "<@123456789012345693> " + durableNote,
            "2026-07-17T15:03:10Z", detectedImage.sourceMessageId,
            durableNote, {});
        {
            auto lk = db.guard();
            SQLite::Statement lifecycle(db.raw(lk.token()),
                "SELECT manual_command_state,manual_attempts,manual_last_error,"
                "manual_result_kind,manual_result_item_id,manual_effect_state "
                "FROM discord_messages WHERE id=?");
            lifecycle.bind(1, dueCommand.commandRowId);
            requireRow(lifecycle, "selftest query lifecycle at source line 3316 returned a row");
            SQLite::Statement body(db.raw(lk.token()), "SELECT body FROM items WHERE id=?");
            body.bind(1, imageItemId);
            requireRow(body, "selftest query body at source line 3319 returned a row");
            const std::string durableBody = body.getColumn(0).getString();
            const std::size_t firstNote = durableBody.find(durableNote);
            check(durableCommand.accepted && durableCommand.state == "pending" &&
                      dueBeforeFailure && dueCommand.commandRowId > 0 &&
                      transientFailure.retryScheduled &&
                      !transientFailure.terminal && transientFailure.attempts == 1 &&
                      transientFailure.error.find("cdn.discordapp.com") ==
                          std::string::npos &&
                      recoveredAfterFailure &&
                      durableComplete.appendedToItem &&
                      durableComplete.itemId == imageItemId &&
                      durableReplay.duplicate && !durableReplay.appendedToItem &&
                      durableDuplicate.duplicate &&
                      durableDuplicate.state == "done" &&
                      lifecycle.getColumn(0).getString() == "done" &&
                      lifecycle.getColumn(1).getInt() == 1 &&
                      lifecycle.getColumn(2).getString().empty() &&
                      lifecycle.getColumn(3).getString() == "appended" &&
                      lifecycle.getColumn(4).getInt64() == imageItemId &&
                      lifecycle.getColumn(5).getString() == "pending" &&
                      firstNote != std::string::npos &&
                      durableBody.find(durableNote, firstNote + durableNote.size()) ==
                          std::string::npos,
                  "manual command retries survive durable state and append a non-empty note exactly once");
        }
        ManualCaptureEffect durableEffect;
        const bool effectClaimed =
            nextPendingManualCaptureEffect(&db, durableEffect);
        const bool effectCompleted = effectClaimed &&
            completeManualCaptureEffect(
                &db, durableEffect.commandRowId, durableEffect.attempt);
        check(effectClaimed && effectCompleted &&
                  durableEffect.resultKind == "appended" &&
                  durableEffect.itemId == imageItemId &&
                  durableEffect.attempt == 1,
              "manual command persists and completes its post-commit effect independently");

        const std::string effectMergeSourceMessage =
            "123456789012345740";
        IngestOutcome effectMergeSource = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", effectMergeSourceMessage,
            "Merge User", "123456789012345741", "merge effect source",
            "2026-07-17T15:03:20Z");
        nlohmann::json effectMergePromoted = promoteSuggestion(
            &db, nullptr, effectMergeSource.messageRowId, projectId, "fix", "");
        const long long effectMergeSourceItem =
            effectMergePromoted.value("item_id", 0LL);
        ManualCommandStageOutcome effectMergeCommand = stageManualCaptureCommand(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", "123456789012345742",
            "DevHub Admin", "123456789012345692",
            "<@123456789012345693> note before merge",
            "2026-07-17T15:03:21Z", effectMergeSourceMessage,
            "note before merge", {});
        ManualCaptureCommand effectMergeDue;
        const bool effectMergeDueFound =
            nextPendingManualCaptureCommand(&db, effectMergeDue);
        IngestOutcome effectMergeCompleted = completeManualCaptureCommand(
            &db, effectMergeDue, "Merge User", "123456789012345741",
            "merge effect source", "2026-07-17T15:03:20Z", {});
        long long effectMergeTargetItem = 0;
        bool effectItemsMerged = false;
        {
            auto lk = db.guard();
            effectMergeTargetItem = insertItemLocked(lk.token(),
                &db, projectId, "fix", "merge effect target", "", 2, 0,
                "test", "", "");
            std::string mergeError;
            effectItemsMerged = mergeItemsLocked(lk.token(),
                &db, effectMergeSourceItem, effectMergeTargetItem, mergeError);
        }
        ManualCaptureEffect effectAfterMerge;
        const bool mergedEffectClaimed =
            nextPendingManualCaptureEffect(&db, effectAfterMerge);
        const bool mergedEffectCompleted = mergedEffectClaimed &&
            completeManualCaptureEffect(
                &db, effectAfterMerge.commandRowId, effectAfterMerge.attempt);
        check(effectMergeSource.ingested &&
                  effectMergePromoted.value("ok", false) &&
                  effectMergeCommand.accepted && effectMergeDueFound &&
                  effectMergeCompleted.appendedToItem && effectItemsMerged &&
                  mergedEffectClaimed && mergedEffectCompleted &&
                  effectAfterMerge.itemId == effectMergeTargetItem,
              "pending manual effects follow their item through an allowed merge");

        ManualCommandStageOutcome terminalCommand = stageManualCaptureCommand(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", "123456789012345694", "DevHub Admin",
            "123456789012345692", "<@123456789012345693> terminal",
            "2026-07-17T15:03:11Z", "123456789012345695", "terminal", {});
        ManualCommandFailureOutcome terminalFailure =
            recordManualCaptureCommandFailure(
                &db, terminalCommand.command.commandRowId,
                "missing media.discordapp.net/attachments/private target", false);
        {
            auto lk = db.guard();
            SQLite::Statement terminal(db.raw(lk.token()),
                "SELECT manual_command_state,manual_last_error "
                "FROM discord_messages WHERE id=?");
            terminal.bind(1, terminalCommand.command.commandRowId);
            requireRow(terminal, "selftest query terminal at source line 3421 returned a row");
            check(terminalCommand.accepted && terminalFailure.terminal &&
                      !terminalFailure.retryScheduled &&
                      terminal.getColumn(0).getString() == "failed" &&
                      terminal.getColumn(1).getString().find("media.discordapp.net") ==
                          std::string::npos,
                  "terminal manual command failures are persisted visibly without CDN URLs");
        }

        const std::string manualReopenPath =
            (testDir / "manual-reopen.db").string();
        const std::string reopenChannel = "123456789012345760";
        const std::string reopenTarget = "123456789012345761";
        const std::string reopenCommand = "123456789012345762";
        const std::string reopenAttachment = "123456789012345763";
        long long reopenCommandRow = 0;
        {
            Db stagedDb(manualReopenPath);
            IngestOutcome target = ingestDiscordMessage(
                &stagedDb, reopenChannel, "durable-reopen",
                "123456789012345764", "reopen guild", reopenTarget,
                "Evidence User", "123456789012345765", "hello",
                "2026-07-17T15:05:00Z");
            DiscordAttachmentMeta reply = detectedImage;
            reply.sourceChannelId = reopenChannel;
            reply.sourceMessageId = reopenCommand;
            reply.attachmentId = reopenAttachment;
            reply.sourceRole = "admin_reply";
            ManualCommandStageOutcome staged = stageManualCaptureCommand(
                &stagedDb, reopenChannel, "durable-reopen",
                "123456789012345764", "reopen guild", reopenCommand,
                "DevHub Admin", "123456789012345766",
                "<@123456789012345767> reopen evidence",
                "2026-07-17T15:05:01Z", reopenTarget, "reopen evidence",
                {reply});
            reopenCommandRow = staged.command.commandRowId;
            check(target.ingested && staged.accepted && reopenCommandRow > 0,
                  "manual reply command and attachment are staged before database close");
        }
        {
            Db reopened(manualReopenPath);
            ManualCaptureCommand pending;
            const bool recovered =
                nextPendingManualCaptureCommand(&reopened, pending);
            IngestOutcome completed = completeManualCaptureCommand(
                &reopened, pending, "Evidence User", "123456789012345765",
                "hello", "2026-07-17T15:05:00Z",
                pending.commandAttachments);
            auto lk = reopened.guard();
            SQLite::Statement moved(reopened.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments a "
                "JOIN discord_messages m ON m.id=a.discord_message_row_id "
                "WHERE m.message_id=? AND a.source_message_id=? "
                "AND a.attachment_id=? AND a.source_role='admin_reply' "
                "AND a.state='captured'");
            moved.bind(1, reopenTarget);
            moved.bind(2, reopenCommand);
            moved.bind(3, reopenAttachment);
            requireRow(moved, "selftest query moved at source line 3479 returned a row");
            check(recovered && pending.commandRowId == reopenCommandRow &&
                      pending.commandAttachments.size() == 1 &&
                      completed.ingested && moved.getColumn(0).getInt() == 1,
                  "database reopen recovers command metadata and transfers reply evidence once");
        }
        {
            Db reopened(manualReopenPath);
            ManualCaptureEffect effect;
            const bool claimed = nextPendingManualCaptureEffect(&reopened, effect);
            const bool completed = claimed && completeManualCaptureEffect(
                &reopened, effect.commandRowId, effect.attempt);
            auto lk = reopened.guard();
            SQLite::Statement state(reopened.raw(lk.token()),
                "SELECT manual_command_state,manual_effect_state "
                "FROM discord_messages WHERE id=?");
            state.bind(1, reopenCommandRow);
            requireRow(state, "selftest query state at source line 3496 returned a row");
            check(claimed && completed && effect.resultKind == "captured" &&
                      effect.messageRowId > 0 &&
                      state.getColumn(0).getString() == "done" &&
                      state.getColumn(1).getString() == "done",
                  "reopened completed command reconciles its durable result without recapture");
        }

        DiscordAttachmentMeta legacyFollowupImage = detectedImage;
        legacyFollowupImage.sourceMessageId = "123456789012345686";
        legacyFollowupImage.attachmentId = "123456789012345687";
        legacyFollowupImage.filename = "legacy-ticket-follow-up.png";
        IngestOutcome legacyImageCapture = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", legacyFollowupImage.sourceMessageId, "Legacy User",
            "123456789012345688", "suggestion: older ticket without image metadata",
            "2026-07-17T15:04:00Z", false, {});
        nlohmann::json legacyImagePromotion = promoteSuggestion(
            &db, nullptr, legacyImageCapture.messageRowId, projectId,
            "implementation", "");
        const long long legacyImageItemId =
            legacyImagePromotion.value("item_id", 0LL);
        IngestOutcome legacyImageFollowup = manualCaptureSuggestion(
            &db, detectedImage.sourceChannelId, "devhub", "123456789012345669",
            "test guild", legacyFollowupImage.sourceMessageId, "Legacy User",
            "123456789012345688", "suggestion: older ticket without image metadata",
            "2026-07-17T15:04:00Z", "", {legacyFollowupImage});
        check(legacyImageCapture.ingested && legacyImageItemId > 0 &&
                  legacyImageFollowup.appendedToItem &&
                  legacyImageFollowup.itemId == legacyImageItemId &&
                  legacyImageFollowup.imagesQueued == 1 &&
                  legacyImageFollowup.imageCount == 1 &&
                  !std::filesystem::exists(testDir / "ticket-images"),
              "promoted legacy ticket with no attachment rows accepts later image evidence");

        long long legacyMismatchRow = 0;
        long long manualRetryRow = 0;
        {
            auto lk = db.guard();
            SQLite::Statement ids(db.raw(lk.token()),
                "SELECT id,attachment_id FROM ticket_attachments "
                "WHERE attachment_id IN (?,?) ORDER BY id");
            ids.bind(1, lateTargetImage.attachmentId);
            ids.bind(2, lateReplyImage.attachmentId);
            while (ids.executeStep()) {
                const std::string attachmentId = ids.getColumn(1).getString();
                if (attachmentId == lateTargetImage.attachmentId)
                    legacyMismatchRow = ids.getColumn(0).getInt64();
                else if (attachmentId == lateReplyImage.attachmentId)
                    manualRetryRow = ids.getColumn(0).getInt64();
            }
            SQLite::Statement fail(db.raw(lk.token()),
                "UPDATE ticket_attachments SET state='failed',error=? "
                "WHERE id=? AND state='queued'");
            fail.bind(1,
                "downloaded image signature does not match Discord metadata");
            fail.bind(2, legacyMismatchRow);
            fail.exec();
            SQLite::Statement failOther(db.raw(lk.token()),
                "UPDATE ticket_attachments SET state='failed',error=? "
                "WHERE id=? AND state='queued'");
            failOther.bind(1, "attachment is no longer available on Discord");
            failOther.bind(2, manualRetryRow);
            failOther.exec();
        }
        const int legacyRequeued = requeueLegacyTicketImageFailures(&db);
        {
            auto lk = db.guard();
            SQLite::Statement states(db.raw(lk.token()),
                "SELECT id,state,error FROM ticket_attachments WHERE id IN (?,?)");
            states.bind(1, legacyMismatchRow);
            states.bind(2, manualRetryRow);
            std::map<long long, std::pair<std::string, std::string>> byId;
            while (states.executeStep()) {
                byId[states.getColumn(0).getInt64()] = {
                    states.getColumn(1).getString(),
                    states.getColumn(2).getString()};
            }
            check(legacyMismatchRow > 0 && manualRetryRow > 0 &&
                      legacyRequeued == 1 &&
                      byId[legacyMismatchRow].first == "queued" &&
                      byId[legacyMismatchRow].second.empty() &&
                      byId[manualRetryRow].first == "failed",
                  "legacy Discord MIME failures become immediate work once without changing unrelated failures");
        }
        const bool manualRetryApplied =
            retryFailedTicketImage(&db, nullptr, manualRetryRow);
        const bool manualRetryReplay =
            retryFailedTicketImage(&db, nullptr, manualRetryRow);
        {
            auto lk = db.guard();
            SQLite::Statement state(db.raw(lk.token()),
                "SELECT state,error FROM ticket_attachments WHERE id=?");
            state.bind(1, manualRetryRow);
            requireRow(state, "selftest query state at source line 3590 returned a row");
            check(manualRetryApplied && !manualRetryReplay &&
                      state.getColumn(0).getString() == "queued" &&
                      state.getColumn(1).getString().empty(),
                  "failed image retry is a guarded one-way transition to immediate queued work");
        }

        const std::string png("\x89PNG\r\n\x1a\n", 8);
        savedImage = saveTicketImageBytes(
            dbPath, imageItemId, detectedImage.sourceMessageId,
            detectedImage.attachmentId, png);
        check(savedImage.ok && savedImage.created &&
                  std::filesystem::is_regular_file(savedImage.absolutePath) &&
                  savedImage.absolutePath.find("..\\untrusted") == std::string::npos,
              "promoted image storage uses validated bytes and generated contained names");
        const std::string webpSignature = "RIFF1234WEBP";
        TicketImageSaveResult transformedImage = saveTicketImageBytes(
            dbPath, imageItemId, detectedImage.sourceMessageId,
            "123456789012345677", webpSignature);
        check(transformedImage.ok && transformedImage.created &&
                  transformedImage.contentType == "image/webp" &&
                  endsWith(transformedImage.relativePath, ".webp") &&
                  !ticketImageMimeMatchesKind("image/png", TicketImageKind::Webp) &&
                  std::filesystem::is_regular_file(transformedImage.absolutePath),
              "supported WebP signatures save with magic-derived MIME and extension");
        TicketImageSaveResult unsupportedImage = saveTicketImageBytes(
            dbPath, imageItemId, detectedImage.sourceMessageId,
            "123456789012345678", "<html>not an image</html>");
        check(!unsupportedImage.ok && unsupportedImage.relativePath.empty(),
              "unsupported downloaded bytes are rejected before a final path or file exists");
        {
            auto lk = db.guard();
            SQLite::Statement saved(db.raw(lk.token()),
                "UPDATE ticket_attachments SET state='saved',content_type=?,"
                "relative_path=?,actual_size=?,sha256=?,error='',updated_at=?,saved_at=? "
                "WHERE id=? AND state='queued'");
            saved.bind(1, savedImage.contentType);
            saved.bind(2, savedImage.relativePath);
            saved.bind(3, static_cast<long long>(savedImage.actualSize));
            saved.bind(4, savedImage.sha256);
            const std::string now = nowIsoUtc();
            saved.bind(5, now); saved.bind(6, now);
            saved.bind(7, imageAttachmentRow);
            saved.exec();
            SQLite::Statement longBody(db.raw(lk.token()),
                "UPDATE items SET body=? WHERE id=?");
            longBody.bind(1, std::string(5000, 'x'));
            longBody.bind(2, imageItemId);
            longBody.exec();
        }
        lateProjectionImage = saveTicketImageBytes(
            dbPath, imageItemId, "123456789012345697",
            "123456789012345698", png);
        check(lateProjectionImage.ok &&
                  std::filesystem::is_regular_file(
                      lateProjectionImage.absolutePath),
              "late packet-projection image has a real validated local file");
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            for (int i = 0; i < 25; ++i) {
                SQLite::Statement failed(db.raw(lk.token()),
                    "INSERT INTO ticket_attachments(discord_message_row_id,item_id,"
                    "source_channel_id,source_message_id,attachment_id,source_role,"
                    "original_filename,content_type,declared_size,width,height,state,"
                    "error,created_at,updated_at) "
                    "VALUES(NULL,?,?,?,?,?,'failed projection.png','image/png',"
                    "128,1,1,'failed',?,?,?)");
                failed.bind(1, imageItemId);
                failed.bind(2, detectedImage.sourceChannelId);
                failed.bind(3, std::to_string(700000000000000000LL + i));
                failed.bind(4, std::to_string(710000000000000000LL + i));
                failed.bind(5, "suggestion");
                failed.bind(6, i == 24
                    ? "retry media.discordapp.net/attachments/private/evidence.png"
                    : "attachment unavailable");
                failed.bind(7, now);
                failed.bind(8, now);
                failed.exec();
            }
            SQLite::Statement lateSaved(db.raw(lk.token()),
                "INSERT INTO ticket_attachments(discord_message_row_id,item_id,"
                "source_channel_id,source_message_id,attachment_id,source_role,"
                "original_filename,content_type,declared_size,width,height,state,"
                "relative_path,actual_size,sha256,error,created_at,updated_at,saved_at) "
                "VALUES(NULL,?,?,?,?,?,'late-saved.png',?,?,1,1,'saved',?,?,?,'',?,?,?)");
            lateSaved.bind(1, imageItemId);
            lateSaved.bind(2, detectedImage.sourceChannelId);
            lateSaved.bind(3, "123456789012345697");
            lateSaved.bind(4, "123456789012345698");
            lateSaved.bind(5, "suggestion");
            lateSaved.bind(6, lateProjectionImage.contentType);
            lateSaved.bind(7, static_cast<long long>(lateProjectionImage.actualSize));
            lateSaved.bind(8, lateProjectionImage.relativePath);
            lateSaved.bind(9, static_cast<long long>(lateProjectionImage.actualSize));
            lateSaved.bind(10, lateProjectionImage.sha256);
            lateSaved.bind(11, now);
            lateSaved.bind(12, now);
            lateSaved.bind(13, now);
            lateSaved.exec();
        }
        discordMessageDeleted(&db, nullptr, detectedImage.sourceMessageId);
        {
            auto lk = db.guard();
            // Simulate the asynchronous post callback landing after promotion
            // and source deletion. The item-owned card lineage must still be
            // addressable by its own durable row id.
            SQLite::Statement callback(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET notify_message_id=?,"
                "post_state='posted',post_attempts=1,updated_at=? "
                "WHERE id=?");
            callback.bind(1, "123456789012345696");
            callback.bind(2, nowIsoUtc());
            callback.bind(3, imageCardRow);
            callback.exec();
            SQLite::Statement retained(db.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments "
                "WHERE id=? AND state='saved' AND discord_message_row_id=?");
            retained.bind(1, imageAttachmentRow);
            retained.bind(2, imageCapture.messageRowId);
            requireRow(retained, "selftest query retained at source line 3710 returned a row");
            SQLite::Statement sourceTombstone(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_messages WHERE message_id=? "
                "AND state='deleted' AND item_id=? AND author='' AND content='' "
                "AND posted_at='' AND ingested_at='' AND admin_note=''");
            sourceTombstone.bind(1, detectedImage.sourceMessageId);
            sourceTombstone.bind(2, imageItemId);
            requireRow(sourceTombstone, "selftest query sourceTombstone at source line 3717 returned a row");
            SQLite::Statement cardRetained(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards WHERE id=? "
                "AND discord_message_row_id=? AND item_id=? "
                "AND notify_message_id='123456789012345696' "
                "AND post_state='posted' AND post_attempts=1 "
                "AND edit_state='pending' AND edit_revision=1");
            cardRetained.bind(1, imageCardRow);
            cardRetained.bind(2, imageCapture.messageRowId);
            cardRetained.bind(3, imageItemId);
            requireRow(cardRetained, "selftest query cardRetained at source line 3727 returned a row");
            check(retained.getColumn(0).getInt() == 1 &&
                      sourceTombstone.getColumn(0).getInt() == 1 &&
                      cardRetained.getColumn(0).getInt() == 1 &&
                      std::filesystem::is_regular_file(savedImage.absolutePath),
                  "Discord source deletion keeps a scrubbed terminal marker, promoted image, and async-safe card lineage");
        }
        DiscordAttachmentMeta promotedLateImage = detectedImage;
        promotedLateImage.attachmentId = "123456789012345700";
        promotedLateImage.filename = "must-not-return.png";
        int promotedAttachmentCount = 0;
        {
            auto lk = db.guard();
            SQLite::Statement count(db.raw(lk.token()),
                "SELECT COUNT(*) FROM ticket_attachments WHERE item_id=?");
            count.bind(1, imageItemId);
            requireRow(count, "selftest query count at source line 3743 returned a row");
            promotedAttachmentCount = count.getColumn(0).getInt();
        }
        IngestOutcome promotedAutomaticReplay = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", detectedImage.sourceMessageId,
            "Late Automatic User", "123456789012345668",
            "suggestion: recreate deleted promoted source",
            "2026-07-17T15:04:00Z", false, {promotedLateImage});
        IngestOutcome promotedManualReplay = manualCaptureSuggestion(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", detectedImage.sourceMessageId,
            "Late Manual User", "123456789012345668",
            "suggestion: recreate deleted promoted source",
            "2026-07-17T15:04:00Z", "must not append", {promotedLateImage});
        nlohmann::json promotedReplay = promoteSuggestion(
            &db, nullptr, imageCapture.messageRowId, projectId,
            "implementation", "must remain the same item");
        nlohmann::json promotedDismiss = dismissSuggestion(
            &db, nullptr, imageCapture.messageRowId);
        {
            auto lk = db.guard();
            SQLite::Statement terminal(db.raw(lk.token()),
                "SELECT m.state,m.author,m.content,m.admin_note,"
                "(SELECT COUNT(*) FROM ticket_attachments a WHERE a.item_id=?) "
                "FROM discord_messages m WHERE m.id=?");
            terminal.bind(1, imageItemId);
            terminal.bind(2, imageCapture.messageRowId);
            requireRow(terminal, "selftest query terminal at source line 3771 returned a row");
            check(promotedAutomaticReplay.duplicate &&
                      promotedManualReplay.duplicate &&
                      promotedManualReplay.itemId == imageItemId &&
                      promotedReplay.value("ok", false) &&
                      promotedReplay.value("duplicate", false) &&
                      promotedReplay.value("item_id", 0LL) == imageItemId &&
                      !promotedDismiss.value("ok", false) &&
                      terminal.getColumn(0).getString() == "deleted" &&
                      terminal.getColumn(1).getString().empty() &&
                      terminal.getColumn(2).getString().empty() &&
                      terminal.getColumn(3).getString().empty() &&
                      terminal.getColumn(4).getInt() == promotedAttachmentCount,
                  "promoted deletion tombstone rejects late ingest/evidence while preserving idempotent item lookup");
        }
        int revisionBeforeExplicitRefresh = 0;
        {
            auto lk = db.guard();
            SQLite::Statement baseline(db.raw(lk.token()),
                "SELECT edit_revision FROM discord_notify_cards WHERE id=?");
            baseline.bind(1, imageCardRow);
            requireRow(baseline, "selftest query baseline at source line 3792 returned a row");
            revisionBeforeExplicitRefresh = baseline.getColumn(0).getInt();
        }
        refreshTicketNotifyCard(&db, nullptr, imageItemId);
        refreshTicketNotifyCard(&db, nullptr, imageItemId);
        {
            auto lk = db.guard();
            SQLite::Statement revision(db.raw(lk.token()),
                "SELECT edit_state,edit_attempts,edit_revision "
                "FROM discord_notify_cards WHERE id=?");
            revision.bind(1, imageCardRow);
            requireRow(revision, "selftest query revision at source line 3803 returned a row");
            check(revision.getColumn(0).getString() == "pending" &&
                      revision.getColumn(1).getInt() == 0 &&
                      revisionBeforeExplicitRefresh >= 1 &&
                      revision.getColumn(2).getInt() ==
                          revisionBeforeExplicitRefresh + 2,
                  "card refreshes durably coalesce behind the latest revision");
        }
        {
            auto lk = db.guard();
            SQLite::Transaction tx(db.raw(lk.token()));
            SQLite::Statement failedImage(db.raw(lk.token()),
                "SELECT id FROM ticket_attachments WHERE item_id=? "
                "AND state='failed' ORDER BY id LIMIT 1");
            failedImage.bind(1, imageItemId);
            const bool foundFailedImage = failedImage.executeStep();
            const long long failedImageId = foundFailedImage
                ? failedImage.getColumn(0).getInt64() : 0;
            SQLite::Statement state(db.raw(lk.token()),
                "UPDATE ticket_attachments SET state='queued',updated_at=? "
                "WHERE id=? AND state='failed'");
            state.bind(1, nowIsoUtc());
            state.bind(2, failedImageId);
            const int changed = state.exec();
            SQLite::Statement dirty(db.raw(lk.token()),
                "SELECT edit_state,edit_revision FROM discord_notify_cards "
                "WHERE id=?");
            dirty.bind(1, imageCardRow);
            requireRow(dirty, "selftest query dirty at source line 3831 returned a row");
            check(foundFailedImage && changed == 1 &&
                      dirty.getColumn(0).getString() == "pending" &&
                      dirty.getColumn(1).getInt() ==
                          revisionBeforeExplicitRefresh + 3,
                  "image state and notification-card dirty revision commit atomically");
            SQLite::Statement restore(db.raw(lk.token()),
                "UPDATE ticket_attachments SET state='failed',updated_at=? "
                "WHERE id=? AND state='queued'");
            restore.bind(1, nowIsoUtc());
            restore.bind(2, failedImageId);
            restore.exec();
            tx.commit();
        }
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "notify_channel_id",
                          "123456789012345690");
        }
        {
            auto lk = db.guard();
            SQLite::Statement removeCard(db.raw(lk.token()),
                "DELETE FROM discord_notify_cards WHERE id=?");
            removeCard.bind(1, imageCardRow);
            removeCard.exec();
        }
        ensureTicketNotifyCard(
            &db, nullptr, imageCapture.messageRowId, imageItemId);
        {
            auto lk = db.guard();
            SQLite::Statement restored(db.raw(lk.token()),
                "SELECT COUNT(*) FROM discord_notify_cards "
                "WHERE discord_message_row_id=? AND item_id=? "
                "AND notify_channel_id=? AND notify_message_id='' "
                "AND post_state='pending'");
            restored.bind(1, imageCapture.messageRowId);
            restored.bind(2, imageItemId);
            restored.bind(3, "123456789012345690");
            requireRow(restored, "selftest query restored at source line 3869 returned a row");
            check(restored.getColumn(0).getInt() == 1,
                  "missing promoted card recreates from scrubbed source/item lineage");
        }
        });

        if (runRelease) section("release and review acceptance", [&] {
        std::string release = buildReleaseDraft(&db, projectId, 30);
        check(release.find("Unified request") != std::string::npos &&
              release.find("Alice") != std::string::npos &&
              release.find("Bob") != std::string::npos &&
              release.find("$devhub-reports") != std::string::npos &&
              release.find("$ffxiv-dalamud-plugin-builder") != std::string::npos &&
              release.find("$devhub-control") == std::string::npos,
              "release packet includes shipped work, contributors, and public skill route");
        check(validPacketSha(packetField(release, "Source SHA-256")) &&
              validPacketSha(packetField(release, "Packet SHA-256")),
              "release packet carries valid lowercase SHA-256 identities");

        long long moveDestinationProjectId = 0;
        long long moveArchivedProjectId = 0;
        long long moveActiveItemId = 0;
        long long moveChannelRowId = 0;
        {
            auto lk = db.guard();
            const std::string now = nowIsoUtc();
            SQLite::Statement foreignProject(db.raw(lk.token()),
                "INSERT INTO projects(name,slug,created_at,updated_at) "
                "VALUES(?,?,?,?)");
            foreignProject.bind(1, "Cross Project Fixture");
            foreignProject.bind(2, "cross-project-fixture");
            foreignProject.bind(3, now);
            foreignProject.bind(4, now);
            foreignProject.exec();
            foreignProjectId = db.raw(lk.token()).getLastInsertRowid();

            SQLite::Statement destination(db.raw(lk.token()),
                "INSERT INTO projects(name,slug,path,created_at,updated_at) "
                "VALUES(?,?,?,?,?)");
            destination.bind(1, "Move Destination");
            destination.bind(2, "move-destination");
            destination.bind(3, "C:\\move-destination");
            destination.bind(4, now);
            destination.bind(5, now);
            destination.exec();
            moveDestinationProjectId = db.raw(lk.token()).getLastInsertRowid();

            SQLite::Statement archived(db.raw(lk.token()),
                "INSERT INTO projects(name,slug,path,archived,created_at,updated_at) "
                "VALUES(?,?,?,1,?,?)");
            archived.bind(1, "Archived Move Destination");
            archived.bind(2, "archived-move-destination");
            archived.bind(3, "C:\\archived-move-destination");
            archived.bind(4, now);
            archived.bind(5, now);
            archived.exec();
            moveArchivedProjectId = db.raw(lk.token()).getLastInsertRowid();

            moveActiveItemId = insertItemLocked(lk.token(),
                &db, projectId, "implementation", "Move active fixture",
                "Move body stays attached", 3, aliceId, "discord", "", "");
            SQLite::Statement channel(db.raw(lk.token()),
                "INSERT INTO discord_channels(channel_id,guild_name,channel_name,"
                "project_id,created_at) VALUES(?,?,?,?,?)");
            channel.bind(1, "823456789012345600");
            channel.bind(2, "Move Guild");
            channel.bind(3, "move-source");
            channel.bind(4, projectId);
            channel.bind(5, now);
            channel.exec();
            moveChannelRowId = db.raw(lk.token()).getLastInsertRowid();
            SQLite::Statement message(db.raw(lk.token()),
                "INSERT INTO discord_messages(channel_row_id,message_id,author,"
                "content,ingested_at,kind,state,item_id,inferred_project_id) "
                "VALUES(?,?,?,?,?,'suggestion','promoted',?,?)");
            message.bind(1, moveChannelRowId);
            message.bind(2, "823456789012345601");
            message.bind(3, "Move User");
            message.bind(4, "Move source provenance");
            message.bind(5, now);
            message.bind(6, moveActiveItemId);
            message.bind(7, projectId);
            message.exec();
            const long long moveMessageRowId = db.raw(lk.token()).getLastInsertRowid();
            SQLite::Statement image(db.raw(lk.token()),
                "INSERT INTO ticket_attachments(discord_message_row_id,item_id,"
                "source_channel_id,source_message_id,attachment_id,source_role,"
                "declared_size,state,created_at,updated_at) "
                "VALUES(?,?,?,?,?,'suggestion',1,'queued',?,?)");
            image.bind(1, moveMessageRowId);
            image.bind(2, moveActiveItemId);
            image.bind(3, "823456789012345600");
            image.bind(4, "823456789012345601");
            image.bind(5, "823456789012345602");
            image.bind(6, now);
            image.bind(7, now);
            image.exec();
            SQLite::Statement card(db.raw(lk.token()),
                "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                "notify_channel_id,notify_message_id,post_state,edit_state,"
                "created_at,updated_at) "
                "VALUES(?,?,?,?,'posted','idle',?,?)");
            card.bind(1, moveMessageRowId);
            card.bind(2, moveActiveItemId);
            card.bind(3, "823456789012345603");
            card.bind(4, "823456789012345604");
            card.bind(5, now);
            card.bind(6, now);
            card.exec();
            const long long moveCardRowId = db.raw(lk.token()).getLastInsertRowid();

            ItemEdit activeEdit;
            activeEdit.projectId = projectId;
            activeEdit.title = "Move active fixture";
            activeEdit.type = "implementation";
            activeEdit.status = "open";
            activeEdit.priority = 3;
            activeEdit.body = "Move body stays attached";
            bool moved = true;
            std::string moveError;
            const bool noOpSaved = saveItemEditLocked(lk.token(),
                &db, moveActiveItemId, activeEdit, &moved, moveError);
            SQLite::Statement noOpState(db.raw(lk.token()),
                "SELECT edit_state,edit_revision FROM discord_notify_cards "
                "WHERE id=?");
            noOpState.bind(1, moveCardRowId);
            requireRow(noOpState, "selftest query noOpState at source line 3982 returned a row");
            check(noOpSaved && !moved && moveError.empty() &&
                      noOpState.getColumn(0).getString() == "idle" &&
                      noOpState.getColumn(1).getInt() == 0 &&
                      db.raw(lk.token()).execAndGet(
                          "SELECT COUNT(*) FROM activity_log "
                          "WHERE kind='item_moved'").getInt() == 0,
                  "same-project item edit is a move no-op");

            activeEdit.projectId = 999999999;
            activeEdit.title = "Must roll back";
            check(!saveItemEditLocked(lk.token(),
                      &db, moveActiveItemId, activeEdit, &moved, moveError) &&
                      moveError.find("not found") != std::string::npos,
                  "item move rejects a missing destination");
            activeEdit.projectId = moveArchivedProjectId;
            check(!saveItemEditLocked(lk.token(),
                      &db, moveActiveItemId, activeEdit, &moved, moveError) &&
                      moveError.find("archived") != std::string::npos,
                  "item move rejects an archived destination");
            SQLite::Statement unchanged(db.raw(lk.token()),
                "SELECT project_id,title FROM items WHERE id=?");
            unchanged.bind(1, moveActiveItemId);
            requireRow(unchanged, "selftest query unchanged at source line 4005 returned a row");
            check(unchanged.getColumn(0).getInt64() == projectId &&
                      unchanged.getColumn(1).getString() ==
                          "Move active fixture",
                  "rejected item moves do not partially save editor fields");

            ItemEdit mergeLinkedEdit;
            mergeLinkedEdit.projectId = moveDestinationProjectId;
            mergeLinkedEdit.title = "Unified request";
            mergeLinkedEdit.type = "implementation";
            mergeLinkedEdit.status = "completed";
            mergeLinkedEdit.priority = 3;
            mergeLinkedEdit.body = "irrelevant because validation fails first";
            check(!saveItemEditLocked(lk.token(),
                      &db, keptId, mergeLinkedEdit, &moved, moveError) &&
                      moveError.find("merge-linked") != std::string::npos,
                  "item move preserves same-project merge audit lineage");

            activeEdit.projectId = moveDestinationProjectId;
            activeEdit.title = "Move active fixture";
            check(saveItemEditLocked(lk.token(),
                      &db, moveActiveItemId, activeEdit, &moved, moveError) &&
                      moved && moveError.empty(),
                  "active item moves to another project");
            SQLite::Statement movedState(db.raw(lk.token()), R"sql(
SELECT i.project_id,i.title,i.body,i.status,
 (SELECT COUNT(*) FROM item_sources x WHERE x.item_id=i.id),
 (SELECT COUNT(*) FROM ticket_attachments a WHERE a.item_id=i.id),
 (SELECT COUNT(*) FROM discord_messages m WHERE m.item_id=i.id),
 (SELECT COUNT(*) FROM discord_notify_cards c WHERE c.item_id=i.id)
FROM items i WHERE i.id=?)sql");
            movedState.bind(1, moveActiveItemId);
            requireRow(movedState, "selftest query movedState at source line 4037 returned a row");
            SQLite::Statement provenance(db.raw(lk.token()), R"sql(
SELECT c.project_id,m.inferred_project_id,n.edit_state,n.edit_revision
FROM discord_messages m
JOIN discord_channels c ON c.id=m.channel_row_id
JOIN discord_notify_cards n ON n.discord_message_row_id=m.id
WHERE m.item_id=?)sql");
            provenance.bind(1, moveActiveItemId);
            requireRow(provenance, "selftest query provenance at source line 4045 returned a row");
            check(movedState.getColumn(0).getInt64() ==
                          moveDestinationProjectId &&
                      movedState.getColumn(1).getString() ==
                          "Move active fixture" &&
                      movedState.getColumn(2).getString() ==
                          "Move body stays attached" &&
                      movedState.getColumn(3).getString() == "open" &&
                      movedState.getColumn(4).getInt() == 1 &&
                      movedState.getColumn(5).getInt() == 1 &&
                      movedState.getColumn(6).getInt() == 1 &&
                      movedState.getColumn(7).getInt() == 1 &&
                      provenance.getColumn(0).getInt64() == projectId &&
                      provenance.getColumn(1).getInt64() == projectId &&
                      provenance.getColumn(2).getString() == "pending" &&
                      provenance.getColumn(3).getInt() == 1,
                  "item move preserves linked evidence and source provenance while dirtying its posted card");

            const long long completedMoveItem = insertItemLocked(lk.token(),
                &db, projectId, "note", "Move completed fixture", "", 2, 0,
                "test", "", "");
            setItemStatusLocked(lk.token(), &db, completedMoveItem, "completed");
            ItemEdit completedEdit;
            completedEdit.projectId = moveDestinationProjectId;
            completedEdit.title = "Move completed fixture";
            completedEdit.type = "note";
            completedEdit.status = "completed";
            completedEdit.priority = 2;
            check(saveItemEditLocked(lk.token(),
                      &db, completedMoveItem, completedEdit, &moved,
                      moveError) && moved,
                  "completed item moves to another project");
            SQLite::Statement completion(db.raw(lk.token()),
                "SELECT i.project_id,e.project_id FROM items i JOIN events e "
                "ON e.item_id=i.id AND e.kind='completion' WHERE i.id=?");
            completion.bind(1, completedMoveItem);
            requireRow(completion, "selftest query completion at source line 4081 returned a row");
            check(completion.getColumn(0).getInt64() ==
                          moveDestinationProjectId &&
                      completion.getColumn(1).getInt64() ==
                          moveDestinationProjectId,
                  "completed item move carries its calendar event");
        }
        const std::string movedReview = buildReviewMarkdown(
            &db, moveDestinationProjectId, {moveActiveItemId});
        check(buildReviewMarkdown(
                  &db, projectId, {moveActiveItemId}).empty() &&
                  movedReview.find(
                      "Operator-selected subset: 1 active ticket") !=
                      std::string::npos &&
                  movedReview.find("Move active fixture") != std::string::npos,
              "selected review scope follows the moved global item ID");

        std::string review = buildReviewMarkdown(&db, projectId);
        std::string reviewAgain = buildReviewMarkdown(&db, projectId);
        const std::string savedImagePacketPath = ticketAttachmentAbsolutePath(
            dbPath, savedImage.relativePath);
        const std::string lateProjectionImagePacketPath =
            ticketAttachmentAbsolutePath(
                dbPath, lateProjectionImage.relativePath);
        check(review.find("Open-only review item") != std::string::npos &&
              review.find("Unified request") == std::string::npos &&
              review.find("$devhub-development") != std::string::npos &&
              review.find("Authority: context-only") != std::string::npos &&
              review.find("Workspace continuity: after a plan or mode transition") != std::string::npos &&
              review.find("BEGIN XA DEVHUB UNTRUSTED PAYLOAD") != std::string::npos &&
              review.find("BEGIN XA DEVHUB UNTRUSTED RECORDED METADATA") != std::string::npos &&
              review.find("not-a-real-token-123456") == std::string::npos &&
              review.find("not-a-real-openai-key") == std::string::npos &&
              review.find("not-a-real-coinbase-secret") == std::string::npos &&
              review.find("not-a-real-github-token") == std::string::npos &&
              review.find("not-a-real-user-token") == std::string::npos &&
              review.find("not-a-real-query-token") == std::string::npos &&
              review.find("not-a-real-header-token") == std::string::npos &&
              review.find("not-a-real-cli-token") == std::string::npos &&
              review.find("not-a-real-wrapped-token") == std::string::npos &&
              review.find("MTIzNDU2Nzg5MDEyMzQ1Njc4") == std::string::npos &&
              review.find("credential value redacted") != std::string::npos &&
              review.find("credential continuation redacted") != std::string::npos &&
              review.find("private key block redacted") != std::string::npos &&
              review.find("Evidence after the prose PEM mention remains visible.") !=
                  std::string::npos &&
              review.find("Evidence after the actual PEM block remains visible.") !=
                  std::string::npos &&
              review.find("unterminated private key block ended") !=
                  std::string::npos &&
              review.find("An unterminated PEM cannot consume this evidence.") !=
                  std::string::npos &&
              review.find("Nor can it consume later evidence.") !=
                  std::string::npos &&
              review.find("Cancellation token: fix disposal safely") != std::string::npos &&
              review.find("token_count = 5") != std::string::npos &&
              review.find("password validation: keep this engineering context") != std::string::npos &&
               review.find("https://localhost:21100 and email x@y.com") != std::string::npos &&
               review.find(savedImagePacketPath) != std::string::npos &&
               review.find(lateProjectionImagePacketPath) != std::string::npos &&
               review.find("sha256:" + savedImage.sha256) != std::string::npos &&
               review.find("cdn.discordapp.com") == std::string::npos &&
               review.find("media.discordapp.net") == std::string::npos &&
               review.find("[redacted URL]") != std::string::npos &&
               review.find("Ignored an invalid project skill token") != std::string::npos &&
              review.find("$devhub-control") == std::string::npos,
              "AI review packet preserves context, ranks every saved path, and redacts legacy URLs/secrets");
        check(validPacketSha(packetField(review, "Source SHA-256")) &&
              validPacketSha(packetField(review, "Packet SHA-256")) &&
              packetField(review, "Source SHA-256") ==
                  packetField(reviewAgain, "Source SHA-256"),
              "packet hashes are lowercase sha256 and source identity is stable");

        const std::vector<long long> selectedReviewIds{
            openReviewId, imageItemId};
        const std::string selectedReview = buildReviewMarkdown(
            &db, projectId, selectedReviewIds);
        const std::string selectedReviewReordered = buildReviewMarkdown(
            &db, projectId,
            {imageItemId, openReviewId, imageItemId, openReviewId});
        check(selectedReview.find("Operator-selected subset: 2 active tickets") !=
                      std::string::npos &&
                  selectedReview.find("Open-only review item") != std::string::npos &&
                  selectedReview.find("Credential redaction fixture") ==
                      std::string::npos &&
                  selectedReview.find(savedImagePacketPath) != std::string::npos &&
                  selectedReview.find("Authority: context-only") != std::string::npos &&
                  selectedReview.find("BEGIN XA DEVHUB UNTRUSTED PAYLOAD") !=
                      std::string::npos &&
                  validPacketSha(packetField(selectedReview,
                                              "Source SHA-256")) &&
                  validPacketSha(packetField(selectedReviewReordered,
                                              "Source SHA-256")) &&
                  packetField(selectedReview, "Source SHA-256") ==
                      packetField(selectedReviewReordered, "Source SHA-256"),
              "selected review packet contains only normalized chosen active tickets and evidence");
        long long foreignReviewItem = 0;
        {
            auto lk = db.guard();
            SQLite::Statement otherProject(db.raw(lk.token()),
                "SELECT id FROM projects WHERE id=?");
            otherProject.bind(1, foreignProjectId);
            requireRow(otherProject, "selftest query otherProject at source line 4179 returned a row");
            foreignProjectId = otherProject.getColumn(0).getInt64();
            foreignReviewItem = insertItemLocked(lk.token(),
                &db, foreignProjectId, "fix",
                "Foreign review selection", "", 2, 0, "test", "", "");
        }
        std::vector<long long> oversizedSelection(201, openReviewId);
        check(buildReviewMarkdown(&db, projectId, {}).empty() &&
                  buildReviewMarkdown(&db, projectId, {keptId}).empty() &&
                  buildReviewMarkdown(&db, projectId, {foreignReviewItem}).empty() &&
                  buildReviewMarkdown(&db, projectId, {999999999}).empty() &&
                  buildReviewMarkdown(&db, projectId, oversizedSelection).empty(),
              "selected review packet fails closed for empty terminal foreign stale and oversized scopes");
        {
            auto lk = db.guard();
            SQLite::Statement activity(db.raw(lk.token()),
                "DELETE FROM activity_log WHERE kind='item_moved' "
                "AND project_id=?");
            activity.bind(1, moveDestinationProjectId);
            activity.exec();
            SQLite::Statement destination(db.raw(lk.token()),
                "DELETE FROM projects WHERE id=?");
            destination.bind(1, moveDestinationProjectId);
            destination.exec();
            SQLite::Statement channel(db.raw(lk.token()),
                "DELETE FROM discord_channels WHERE id=?");
            channel.bind(1, moveChannelRowId);
            channel.exec();
            SQLite::Statement archived(db.raw(lk.token()),
                "DELETE FROM projects WHERE id=?");
            archived.bind(1, moveArchivedProjectId);
            archived.exec();
        }
        });

        if (runKnowledge) section("knowledge workflow and report acceptance", [&] {
        nlohmann::json knowledgeSpec = {
            {"project_id", projectId}, {"kind", "research"},
            {"title", "Knowledge metabolism test"},
            {"preamble", "Future Codex sessions use this to verify the v8 knowledge path."},
            {"summary", "A dated, source-backed fact used by the self-test."},
            {"body", "The DevHub self-test captured this immutable source and distilled claim."},
            {"freshness", "snapshot"}, {"volatility", "fast"},
            {"source", {{"uri", "test://knowledge/source-1"},
                        {"title", "Self-test source"}, {"raw_text", "source block B1"}}},
            {"claims", nlohmann::json::array({
                {{"claim_text", "The v8 knowledge path preserves claim provenance."},
                 {"source_locator", "B1"}, {"freshness", "snapshot"},
                 {"confidence", "high"}}
            })}
        };
        nlohmann::json knowledge = saveKnowledgeNode(&db, 0, knowledgeSpec);
        long long knowledgeId = knowledge.value("id", 0LL);
        check(knowledge.value("ok", false) && knowledgeId > 0,
              "knowledge node and immutable source saved");
        nlohmann::json detail = getKnowledgeNode(&db, knowledgeId);
        check(detail.value("ok", false) && detail["claims"].size() == 1 &&
              detail["sources"].size() == 1,
              "knowledge detail includes claims and provenance");
        nlohmann::json reused = saveKnowledgeNode(&db, 0, knowledgeSpec);
        check(reused.value("duplicate", false) && reused.value("id", 0LL) == knowledgeId,
              "exact knowledge capture is idempotent");
        nlohmann::json sourceOnly = saveKnowledgeSource(&db, {
            {"project_id", foreignProjectId}, {"uri", "test://knowledge/source-only"},
            {"title", "Source without a knowledge node"}, {"raw_text", "source-only"}
        });
        nlohmann::json sourceOnlyHealth = knowledgeHealth(&db, foreignProjectId);
        check(sourceOnly.value("ok", false) &&
              sourceOnlyHealth.value("sources", 0LL) == 1,
              "knowledge health counts project sources before any node exists");
        std::string knowledgePacket = buildKnowledgeContext(
            &db, projectId, "knowledge metabolism", 3, 10);
        check(knowledgePacket.find("$devhub-knowledge") != std::string::npos &&
              knowledgePacket.find("$ffxiv-dalamud-plugin-builder") != std::string::npos &&
              knowledgePacket.find("$devhub-control") == std::string::npos &&
              knowledgePacket.find("K" + std::to_string(knowledgeId)) != std::string::npos &&
              validPacketSha(packetField(knowledgePacket, "Source SHA-256")) &&
              validPacketSha(packetField(knowledgePacket, "Packet SHA-256")),
              "knowledge packet routes retrieval skill and preserves provenance context");

        nlohmann::json other = saveKnowledgeNode(&db, 0, {
            {"project_id", projectId}, {"kind", "research"},
            {"title", "Alternative knowledge claim"},
            {"summary", "A second node for explicit reconciliation testing."},
            {"freshness", "timeless"},
            {"claims", nlohmann::json::array({
                {{"claim_text", "A source-less claim remains readable at L3."},
                 {"freshness", "timeless"}, {"confidence", "medium"}}
            })}
        });
        long long otherId = other.value("id", 0LL);
        nlohmann::json negativeIdNode = saveKnowledgeNode(&db, -1, {
            {"project_id", projectId}, {"kind", "note"},
            {"title", "Negative id normalization fixture"},
            {"summary", "A negative route id creates a normal node."}
        });
        check(negativeIdNode.value("ok", false) &&
              negativeIdNode.value("id", 0LL) > 0 &&
              getKnowledgeNode(&db, negativeIdNode.value("id", 0LL))
                  .value("ok", false),
              "negative knowledge node ids normalize to create semantics");
        nlohmann::json foreignNode = saveKnowledgeNode(&db, 0, {
            {"project_id", foreignProjectId}, {"kind", "research"},
            {"title", "Foreign conflict endpoint"},
            {"summary", "Exercises the right-hand project health counter."}
        });
        nlohmann::json crossProjectConflict = createKnowledgeConflict(&db, {
            {"left_node_id", knowledgeId},
            {"right_node_id", foreignNode.value("id", 0LL)},
            {"classification", "ambiguous"},
            {"reason", "right endpoint health fixture"}
        });
        check(crossProjectConflict.value("ok", false) &&
              knowledgeHealth(&db, foreignProjectId)
                  .value("open_conflicts", 0LL) == 1,
              "knowledge health counts open conflicts from either endpoint");
        check(resolveKnowledgeConflict(
                  &db, crossProjectConflict.value("id", 0LL),
                  {{"resolution", "Cross-project health fixture complete."}})
                  .value("ok", false),
              "cross-project health fixture is reconciled");
        const nlohmann::json firstKnowledgeLink = linkKnowledgeNodes(
            &db, {{"from_node_id", knowledgeId}, {"to_node_id", otherId},
                  {"relation", "related"}});
        nlohmann::json conflict = createKnowledgeConflict(&db, {
            {"left_node_id", knowledgeId}, {"right_node_id", otherId},
            {"classification", "ambiguous"}, {"reason", "self-test disagreement"}
        });
        const nlohmann::json updatedKnowledgeLink = linkKnowledgeNodes(
            &db, {{"from_node_id", knowledgeId}, {"to_node_id", otherId},
                  {"relation", "related"}, {"strength", 0.5}});
        check(firstKnowledgeLink.value("ok", false) &&
                  firstKnowledgeLink.value("id", 0LL) > 0 &&
                  updatedKnowledgeLink.value("id", 0LL) ==
                      firstKnowledgeLink.value("id", 0LL),
              "knowledge link upsert returns its own stable id on insert and update");
        std::string conflictPacket = buildKnowledgeContext(
            &db, projectId, "knowledge", 3, 10);
        check(conflict.value("ok", false) &&
              conflictPacket.find("self-test disagreement") != std::string::npos &&
              conflictPacket.find("open conflicts 1") != std::string::npos &&
              conflictPacket.find("A source-less claim remains readable at L3.") !=
                  std::string::npos,
              "knowledge packet emits bounded contradiction context and source-less claims");
        check(resolveKnowledgeConflict(&db, conflict.value("id", 0LL),
                  {{"resolution", "Both are retained as separate test contexts."}})
                  .value("ok", false),
              "knowledge conflict reconciled without deleting source history");

        nlohmann::json workflow = saveWorkflow(&db, {
            {"project_id", projectId}, {"title", "Self-test delivery workflow"},
            {"context", "mixed"}, {"phase", "develop"},
            {"objective", "Exercise durable development state."},
            {"minimum_success", "Evidence and resume packet are saved."},
            {"validation", nlohmann::json::array({"schema v9", "resume packet"})},
            {"current_step", "Run verification"}, {"next_action", "Record evidence"},
            {"workspace", testDir.string()}
        });
        long long workflowId = workflow.value("id", 0LL);
        check(workflow.value("ok", false) && workflowId > 0 &&
              addWorkflowArtifact(&db, workflowId, {
                  {"artifact_type", "checkpoint"}, {"title", "Schema ready"},
                  {"content", "Migration and knowledge writes completed."}
              }).value("ok", false),
              "intent workflow and checkpoint saved");
        check(!saveWorkflow(&db, {
                  {"id", workflowId}, {"status", "complete"},
                  {"minimum_success", ""},
                  {"validation", nlohmann::json::array({"schema v9", "resume packet"})}
              }).value("ok", false),
              "explicit blank minimum cannot bypass the completion contract");
        check(!addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Typed fields required."},
                  {"check_command", "typed-test"}, {"status", "pass"},
                  {"started_at", nowIsoUtc()}, {"finished_at", nowIsoUtc()},
                  {"exit_code", false}, {"failure_count", "oops"}
              }).value("ok", false) &&
              !addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Both fields required."},
                  {"check_command", "typed-test"}, {"status", "pass"},
                  {"started_at", nowIsoUtc()}, {"finished_at", nowIsoUtc()},
                  {"exit_code", 0}
              }).value("ok", false),
              "passing evidence requires explicit integer result fields");
        check(!addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Future pass must fail."},
                  {"check_command", "future-test"}, {"status", "pass"},
                  {"started_at", "2999-01-01T00:00:00Z"},
                  {"finished_at", "2999-01-01T00:01:00Z"},
                  {"exit_code", 0}, {"failure_count", 0}
              }).value("ok", false) &&
              !addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Reversed pass must fail."},
                  {"check_command", "reverse-test"}, {"status", "pass"},
                  {"started_at", "2026-01-02T00:00:00Z"},
                  {"finished_at", "2026-01-01T00:00:00Z"},
                  {"exit_code", 0}, {"failure_count", 0}
              }).value("ok", false),
              "future and reversed passing evidence is rejected at insertion");
        check(!addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Incomplete claimed pass."},
                  {"status", "pass"}
              }).value("ok", false) &&
              !setWorkflowStatus(&db, workflowId, "complete").value("ok", false),
              "incomplete evidence and premature completion are rejected");
        check(addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Schema and knowledge operations work."},
                  {"check_command", "devhub.exe --selftest"}, {"status", "pass"},
                  {"environment", "temporary self-test database"},
                  {"started_at", nowIsoUtc()}, {"finished_at", nowIsoUtc()},
                  {"exit_code", 0}, {"failure_count", 0},
                  {"output_excerpt", "knowledge operations passed"}
              }).value("ok", false) &&
              addVerificationEvidence(&db, workflowId, {
                  {"criterion", "resume packet"}, {"claim", "Initial resume check failed."},
                  {"status", "fail"},
                  {"check_command", "curl -H \"Authorization: Bearer not-a-real-resume-token\" https://example.test"},
                  {"output_excerpt", "expected test history"}
              }).value("ok", false) &&
              addVerificationEvidence(&db, workflowId, {
                  {"criterion", "resume packet"}, {"claim", "Exact workflow state is resumable."},
                  {"check_command", "buildWorkflowResume"}, {"status", "pass"},
                  {"environment", "temporary self-test database"},
                  {"started_at", nowIsoUtc()}, {"finished_at", nowIsoUtc()},
                  {"exit_code", 0}, {"failure_count", 0},
                  {"output_excerpt", "workflow title present"}
              }).value("ok", false) &&
              buildWorkflowResume(&db, projectId).find("Self-test delivery workflow") != std::string::npos,
              "fresh evidence supersedes historical failure and resume is exact");
        long long otherProjectId = 0;
        {
            auto lk = db.guard();
            SQLite::Statement otherProject(db.raw(lk.token()),
                "SELECT id FROM projects WHERE id<>? ORDER BY id LIMIT 1");
            otherProject.bind(1, projectId);
            requireRow(otherProject,
                       "cross-project review fixture query returned a row");
            otherProjectId = otherProject.getColumn(0).getInt64();
        }
        check(buildWorkflowResume(&db, otherProjectId, workflowId).empty(),
              "exact resume rejects a mismatched workflow/project scope");
        check(setWorkflowStatus(&db, workflowId, "complete").value("ok", false),
              "workflow completion requires current evidence for every criterion");
        nlohmann::json completedWorkflow = getWorkflow(&db, workflowId);
        const std::string firstCompletedAt = completedWorkflow.value("completed_at", "");
        nlohmann::json repeatedComplete = setWorkflowStatus(&db, workflowId, "complete");
        check(repeatedComplete.value("ok", false) &&
              getWorkflow(&db, workflowId).value("completed_at", "") == firstCompletedAt,
              "repeated completion preserves the original completed_at timestamp");
        check(setWorkflowStatus(&db, workflowId, "active").value("ok", false) &&
              getWorkflow(&db, workflowId).value("completed_at", "").empty() &&
              setWorkflowStatus(&db, workflowId, "complete").value("ok", false) &&
              !getWorkflow(&db, workflowId).value("completed_at", "").empty(),
              "leaving complete clears and re-entering complete restores completion time");
        std::string closedResume = buildWorkflowResume(&db, 0, workflowId);
        check(closedResume.find("historical read-only context") != std::string::npos &&
              closedResume.find("Do not resume closed work") != std::string::npos &&
              closedResume.find("not-a-real-resume-token") == std::string::npos &&
              closedResume.find("credential-bearing command redacted") != std::string::npos &&
              validPacketSha(packetField(closedResume, "Source SHA-256")) &&
              closedResume.find("$devhub-development") != std::string::npos &&
              closedResume.find("$devhub-control") == std::string::npos,
              "closed exact resume is historical and keeps the correct skill route");
        check(!addVerificationEvidence(&db, workflowId, {
                  {"criterion", "schema v9"}, {"claim", "Closed write must fail."},
                  {"status", "warning"}
              }).value("ok", false),
              "closed workflows reject new verification evidence");
        nlohmann::json revisedContract = saveWorkflow(&db, {
            {"id", workflowId}, {"status", "active"},
            {"minimum_success", "Revised contract requires new evidence."}
        });
        check(revisedContract.value("contract_changed", false) &&
              revisedContract.value("contract_revision", 0LL) == 2 &&
              !setWorkflowStatus(&db, workflowId, "complete").value("ok", false),
              "workflow contract changes invalidate evidence from older revisions");
        std::string revisedResume = buildWorkflowResume(&db, 0, workflowId);
        check(revisedResume.find("Current contract revision: 2") != std::string::npos &&
              revisedResume.find("historical prior revision") != std::string::npos,
              "resume packet labels evidence from older contract revisions");

        nlohmann::json legacyWorkflow = saveWorkflow(&db, {
            {"project_id", projectId}, {"title", "Legacy evidence workflow"},
            {"minimum_success", "Legacy evidence must be fresh and ordered."},
            {"validation", nlohmann::json::array({"legacy timestamp"})}
        });
        const long long legacyWorkflowId = legacyWorkflow.value("id", 0LL);
        {
            auto lk = db.guard();
            SQLite::Statement legacy(db.raw(lk.token()), R"sql(
INSERT INTO verification_evidence(
 workflow_id,criterion,claim,check_command,started_at,finished_at,
 exit_code,failure_count,status,created_at)
VALUES(?,?,?,?,?,?,?,?,?,?))sql");
            legacy.bind(1, legacyWorkflowId); legacy.bind(2, "legacy timestamp");
            legacy.bind(3, "Legacy reversed/future row"); legacy.bind(4, "legacy-command");
            legacy.bind(5, "2999-01-02T00:00:00Z");
            legacy.bind(6, "2999-01-01T00:00:00Z");
            legacy.bind(7, 0); legacy.bind(8, 0); legacy.bind(9, "pass");
            legacy.bind(10, nowIsoUtc()); legacy.exec();
        }
        check(!setWorkflowStatus(&db, legacyWorkflowId, "complete").value("ok", false),
              "completion rejects legacy future or reversed evidence rows");

        {
            auto lk = db.guard();
            const long long futureItem = insertItemLocked(lk.token(),
                &db, projectId, "fix", "Future completion must not enter reports",
                "future-dated fixture", 1, 0, "test", "", "");
            SQLite::Statement future(db.raw(lk.token()),
                "UPDATE items SET status='completed',completed_at='2999-01-01T00:00:00Z' "
                "WHERE id=?");
            future.bind(1, futureItem); future.exec();
        }

        const char* invalidReportTypes[] = {
            "weekly status",
            "weekly: **approve**",
            "Weekly",
            " weekly",
            "weekly\ninput_hash: sha256:injected",
            "authorization: Bearer placeholder",
            "weekly!",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        };
        long long reportCountBeforeInvalid = 0;
        {
            auto lk = db.guard();
            reportCountBeforeInvalid = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM reports").getInt64();
        }
        bool invalidContextTypesRejected = true;
        bool invalidRendererTypesRejected = true;
        bool invalidSavedTypesRejected = true;
        for (const char* invalidType : invalidReportTypes) {
            std::string staleHash = "stale", staleEvidence = "stale";
            std::string staleStart = "stale", staleEnd = "stale";
            invalidContextTypesRejected = invalidContextTypesRejected &&
                buildReportContext(
                    &db, projectId, invalidType, 30, &staleHash, &staleEvidence,
                    &staleStart, &staleEnd, true).empty() &&
                staleHash.empty() && staleEvidence.empty() &&
                staleStart.empty() && staleEnd.empty();

            PacketRenderRequest invalidTypeRequest;
            invalidTypeRequest.kind = PacketKind::Report;
            invalidTypeRequest.payloadMarkdown = "untrusted report payload";
            invalidTypeRequest.generatedAt = "2026-07-15T00:00:00Z";
            invalidTypeRequest.reportRegistry.enabled = true;
            invalidTypeRequest.reportRegistry.reportType = invalidType;
            invalidTypeRequest.reportRegistry.periodStart = "2026-07-01";
            invalidTypeRequest.reportRegistry.periodEnd = "2026-07-15";
            invalidTypeRequest.reportRegistry.evidenceAt =
                invalidTypeRequest.generatedAt;
            invalidRendererTypesRejected = invalidRendererTypesRejected &&
                !renderContextPacket(invalidTypeRequest).ok;

            invalidSavedTypesRejected = invalidSavedTypesRejected &&
                !saveReport(&db, {
                    {"project_id", projectId}, {"report_type", invalidType},
                    {"title", "Invalid type fixture"}, {"status", "draft"},
                    {"content", "must not be stored"}
                }).value("ok", false);
        }
        long long reportCountAfterInvalid = 0;
        {
            auto lk = db.guard();
            reportCountAfterInvalid = db.raw(lk.token()).execAndGet(
                "SELECT COUNT(*) FROM reports").getInt64();
        }
        check(invalidContextTypesRejected && invalidRendererTypesRejected &&
              invalidSavedTypesRejected &&
              reportCountAfterInvalid == reportCountBeforeInvalid,
              "report types reject prose, Markdown, case folding, field injection, credentials, and invalid identifiers without side effects");

        std::string aiReviewPreview = buildReportContext(
            &db, projectId, "ai_review", 1);
        nlohmann::json decisionReport = saveReport(&db, {
            {"project_id", projectId}, {"report_type", "decision"},
            {"title", "Canonical custom report type fixture"},
            {"status", "draft"}, {"content", "custom type accepted"}
        });
        check(aiReviewPreview.find("report_type: ai_review") != std::string::npos &&
              decisionReport.value("ok", false) &&
              getReport(&db, decisionReport.value("id", 0LL))
                  .value("report_type", "") == "decision",
              "canonical custom report types are accepted and preserved exactly");

        std::string previewHash, previewEvidenceAt, previewPeriodStart, previewPeriodEnd;
        std::string reportPreview = buildReportContext(
            &db, projectId, "validation", 30, &previewHash, &previewEvidenceAt,
            &previewPeriodStart, &previewPeriodEnd, false);
        const size_t previewTrusted = reportPreview.find(
            "--- BEGIN XA DEVHUB TRUSTED APPROVAL REGISTRY KEYS ");
        const size_t previewMetadata = reportPreview.find(
            "--- BEGIN XA DEVHUB UNTRUSTED RECORDED METADATA ");
        const size_t previewPayload = reportPreview.find(
            "--- BEGIN XA DEVHUB UNTRUSTED PAYLOAD ");
        check(!reportPreview.empty() &&
              reportPreview.find("report_type: validation") != std::string::npos &&
              reportPreview.find("period_start: " + previewPeriodStart) != std::string::npos &&
              reportPreview.find("period_end: " + previewPeriodEnd) != std::string::npos &&
              reportPreview.find("evidence_at: " + previewEvidenceAt) != std::string::npos &&
              reportPreview.find("input_hash: " + previewHash) != std::string::npos &&
              reportPreview.find("read-only preview") != std::string::npos &&
              previewTrusted < previewMetadata && previewMetadata < previewPayload,
              "report preview emits five trusted approval keys before separate untrusted boundaries");
        {
            auto lk = db.guard();
            check(db.raw(lk.token()).execAndGet(
                      "SELECT COUNT(*) FROM packet_snapshots WHERE kind='report'")
                      .getInt64() == 0,
                  "report preview emits trusted keys without registering a capture");
        }

        PacketRenderRequest incompleteReportRequest;
        incompleteReportRequest.kind = PacketKind::Report;
        incompleteReportRequest.payloadMarkdown = "untrusted report payload";
        check(!renderContextPacket(incompleteReportRequest).ok,
              "shared renderer rejects report packets without trusted registry keys");

        std::string reportHash, reportEvidenceAt, reportPeriodStart, reportPeriodEnd;
        std::string reportPacket = buildReportContext(
            &db, projectId, "validation", 30, &reportHash, &reportEvidenceAt,
            &reportPeriodStart, &reportPeriodEnd, true);
        const size_t captureTrusted = reportPacket.find(
            "--- BEGIN XA DEVHUB TRUSTED APPROVAL REGISTRY KEYS ");
        const size_t capturePayload = reportPacket.find(
            "--- BEGIN XA DEVHUB UNTRUSTED PAYLOAD ");
        check(reportPacket.find("Source SHA-256:") != std::string::npos &&
              reportPacket.find("completed records excluded") != std::string::npos &&
              reportPacket.find("$devhub-reports") != std::string::npos &&
              reportPacket.find("$ffxiv-dalamud-plugin-builder") != std::string::npos &&
              reportPacket.find("$devhub-control") == std::string::npos &&
              reportPacket.find("Future completion must not enter reports") == std::string::npos &&
              reportPacket.find("Period start: " + reportPeriodStart) != std::string::npos &&
              reportPacket.find("Period end: " + reportPeriodEnd) != std::string::npos &&
              reportPacket.find("report_type: validation") != std::string::npos &&
              reportPacket.find("period_start: " + reportPeriodStart) != std::string::npos &&
              reportPacket.find("period_end: " + reportPeriodEnd) != std::string::npos &&
              reportPacket.find("evidence_at: " + reportEvidenceAt) != std::string::npos &&
              reportPacket.find("input_hash: " + reportHash) != std::string::npos &&
              reportPacket.find("explicitly registered approval capture") != std::string::npos &&
              captureTrusted < capturePayload && previewHash == reportHash &&
              validPacketSha(packetField(reportPacket, "Source SHA-256")) &&
              validPacketSha(packetField(reportPacket, "Packet SHA-256")),
              "captured report emits exact trusted registry keys and separate untrusted payload");
        check(!saveReport(&db, {
                  {"project_id", projectId}, {"report_type", "validation"},
                  {"title", "Ungrounded approval"}, {"status", "approved"},
                  {"content", "Must not approve without packet evidence."}
              }).value("ok", false),
              "report approval rejects missing packet evidence identity");
        check(!saveReport(&db, {
                  {"project_id", projectId}, {"report_type", "validation"},
                  {"title", "Unregistered hash approval"}, {"status", "approved"},
                  {"content", "Shape alone is not provenance."},
                  {"period_start", reportPeriodStart}, {"period_end", reportPeriodEnd},
                  {"evidence_at", reportEvidenceAt},
                  {"input_hash", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
              }).value("ok", false),
              "report approval rejects unregistered but well-shaped evidence identity");
        {
            auto lk = db.guard();
            SQLite::Statement registry(db.raw(lk.token()),
                "SELECT COUNT(*) FROM packet_snapshots WHERE kind='report' AND source_hash=?");
            registry.bind(1, reportHash.substr(7));
            requireRow(registry, "report registry query returned a row");
            check(registry.getColumn(0).getInt64() == 1,
                  "explicit report capture registers one approval snapshot");
        }
        nlohmann::json report = saveReport(&db, {
            {"project_id", projectId}, {"report_type", "validation"},
            {"title", "Self-test validation report"}, {"overall_status", "pass"},
            {"content", "All v9 knowledge-path checks passed."},
            {"period_start", reportPeriodStart}, {"period_end", reportPeriodEnd},
            {"evidence_at", reportEvidenceAt}, {"input_hash", reportHash}
        });
        const long long reportId = report.value("id", 0LL);
        check(report.value("ok", false) &&
              setReportStatus(&db, reportId, "approved").value("ok", false),
              "report draft and approval lifecycle saved");
        const std::string approvedAt = getReport(&db, reportId).value("approved_at", "");
        nlohmann::json repeatedApproval = setReportStatus(&db, reportId, "approved");
        check(repeatedApproval.value("unchanged", false) && !approvedAt.empty() &&
              getReport(&db, reportId).value("approved_at", "") == approvedAt,
              "repeated report approval is idempotent");
        check(!saveReport(&db, {
                  {"id", reportId}, {"content", "Mutated after approval"}
              }).value("ok", false) &&
              setReportStatus(&db, reportId, "archived").value("ok", false) &&
              !saveReport(&db, {
                  {"id", reportId}, {"title", "Mutated after archive"}
              }).value("ok", false),
              "approved and archived report content is immutable");

        nlohmann::json scopeDraft = saveReport(&db, {
            {"project_id", projectId}, {"report_type", "validation"},
            {"title", "Scope mutation draft"}, {"content", "Grounded draft."},
            {"period_start", reportPeriodStart}, {"period_end", reportPeriodEnd},
            {"evidence_at", reportEvidenceAt}, {"input_hash", reportHash}
        });
        nlohmann::json changedScope = saveReport(&db, {
            {"id", scopeDraft.value("id", 0LL)}, {"period_start", "1900-01-01"}
        });
        nlohmann::json changedScopeRow = getReport(&db, scopeDraft.value("id", 0LL));
        check(changedScope.value("ok", false) &&
              changedScopeRow.value("evidence_at", "").empty() &&
              changedScopeRow.value("input_hash", "").empty() &&
              !setReportStatus(&db, scopeDraft.value("id", 0LL), "approved")
                  .value("ok", false),
              "report scope mutation clears grounding and requires a new capture");

        long long deletedProjectId = 0;
        {
            auto lk = db.guard();
            SQLite::Statement project(db.raw(lk.token()),
                "INSERT INTO projects(name,slug,created_at,updated_at) VALUES(?,?,?,?)");
            project.bind(1, "Deleted scope fixture");
            project.bind(2, "deleted-scope-fixture");
            project.bind(3, nowIsoUtc()); project.bind(4, nowIsoUtc());
            project.exec(); deletedProjectId = db.raw(lk.token()).getLastInsertRowid();
        }
        std::string deletedHash, deletedEvidence, deletedStart, deletedEnd;
        const std::string deletedScopePacket = buildReportContext(
            &db, deletedProjectId, "validation", 1,
            &deletedHash, &deletedEvidence, &deletedStart, &deletedEnd, true);
        nlohmann::json deletedScopeDraft = saveReport(&db, {
            {"project_id", deletedProjectId}, {"report_type", "validation"},
            {"title", "Deleted project scope draft"}, {"content", "Scoped content."},
            {"period_start", deletedStart}, {"period_end", deletedEnd},
            {"evidence_at", deletedEvidence}, {"input_hash", deletedHash}
        });
        {
            auto lk = db.guard();
            SQLite::Statement remove(db.raw(lk.token()), "DELETE FROM projects WHERE id=?");
            remove.bind(1, deletedProjectId); remove.exec();
        }
        check(!deletedScopePacket.empty() && validPacketSha(deletedHash) &&
              !deletedEvidence.empty() && !deletedStart.empty() &&
              deletedStart == deletedEnd && deletedScopeDraft.value("ok", false) &&
              !setReportStatus(&db, deletedScopeDraft.value("id", 0LL), "approved")
                  .value("ok", false),
              "one-day report bounds are exact and project deletion cannot widen scope");
        check(saveLearning(&db, {
                  {"project_id", projectId}, {"task_type", "selftest"},
                  {"approach", "Use a temporary SQLite database."},
                  {"outcome", "success"}, {"lesson", "Test provenance and resume together."}
              }).value("ok", false) &&
              listLearnings(&db, projectId, "provenance", 3).value("count", 0) == 1,
              "bounded project learning saved and retrieved");
        const nlohmann::json duplicateRecall = saveRetrievalEvaluation(&db, {
            {"name", "selftest-duplicate"}, {"query", "knowledge metabolism"},
            {"expected_ids", nlohmann::json::array({knowledgeId})},
            {"actual_ids", nlohmann::json::array({knowledgeId, knowledgeId})},
            {"k", 2}});
        const nlohmann::json malformedRank = saveRetrievalEvaluation(&db, {
            {"name", "selftest-rank"}, {"query", "knowledge rank"},
            {"expected_ids", nlohmann::json::array({knowledgeId})},
            {"actual_ids", nlohmann::json::array({"not-an-id", knowledgeId})},
            {"k", 1}});
        check(duplicateRecall.value("recall_at_k", 0.0) == 1.0 &&
                  malformedRank.value("recall_at_k", 0.0) == 1.0 &&
                  malformedRank.value("reciprocal_rank", 0.0) == 1.0,
              "retrieval metrics count distinct ids and only valid candidates consume rank");
        nlohmann::json health = knowledgeHealth(&db, projectId);
        check(health.value("critical", 1LL) == 0,
              "knowledge freshness health has no critical failures");
        });

        if (runPortable) section("portable export and notification atomicity", [&] {
        const std::string rawWindowsDash(
            1, static_cast<char>(0x97));
        const std::string rawPortableMarkdown =
            "Portable" + rawWindowsDash + "Markdown";
        const std::string rawPortableJson =
            "Portable" + rawWindowsDash + "JSON";
        long long legacyBuildId = 0;
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "discord_bot_token", "not-a-real-exported-secret");
            db.setSetting(lk.token(), "dashboard_completed_days", "30");
            db.setSetting(lk.token(), "app_display_name", "My Devhub");
            SQLite::Statement sealedToken(db.raw(lk.token()),
                "SELECT value FROM settings WHERE key='discord_bot_token'");
            requireRow(sealedToken, "selftest query sealedToken at source line 4757 returned a row");
            check(sealedToken.getColumn(0).getString().rfind("dpapi:v1:", 0) == 0 &&
                      db.getSetting(lk.token(), "discord_bot_token") ==
                          "not-a-real-exported-secret",
                  "credential settings are DPAPI-sealed at rest and decrypt for this profile");
            SQLite::Statement projectDescription(db.raw(lk.token()),
                "UPDATE projects SET description=? WHERE id=?");
            projectDescription.bind(1, rawPortableMarkdown);
            projectDescription.bind(2, projectId);
            projectDescription.exec();
            SQLite::Statement legacyBuild(db.raw(lk.token()),
                "INSERT INTO builds(project_id,command,cwd,status,exit_code,"
                "output,started_at,finished_at) "
                "VALUES(?,?,'','success',0,?,?,?)");
            legacyBuild.bind(1, projectId);
            legacyBuild.bind(2, "legacy Windows output fixture");
            legacyBuild.bind(3, rawPortableJson);
            legacyBuild.bind(4, "2026-07-22T00:00:00Z");
            legacyBuild.bind(5, "2026-07-22T00:00:01Z");
            legacyBuild.exec();
            legacyBuildId = db.raw(lk.token()).getLastInsertRowid();
        }
        MaintenanceResult maintenance = runAutomaticDailyMaintenance(&db);
        check(maintenance.ok && std::filesystem::exists(testDir / "backups") &&
              std::filesystem::exists(testDir / "exports") &&
              readFileUtf8((testDir / "exports" / "devhub-latest.json").string()) ==
                  readFileUtf8(maintenance.secondaryPath) &&
              readFileUtf8((testDir / "exports" / "devhub-latest.md").string()) ==
                  readFileUtf8(maintenance.primaryPath),
              "automatic backup and atomic latest portable export");
        nlohmann::json portable = nlohmann::json::parse(readFileUtf8(maintenance.secondaryPath));
        const bool portableLeaderboardPreference =
            portable.contains("settings") && portable["settings"].is_array() &&
            std::any_of(portable["settings"].begin(), portable["settings"].end(),
                        [&](const nlohmann::json& row) {
                            return row.value("key", "") ==
                                       "leaderboard_excluded_source_ids" &&
                                   row.value("value", "") ==
                                       std::to_string(bobId);
                        });
        const bool portableDashboardCompletedPreference =
            portable.contains("settings") && portable["settings"].is_array() &&
            std::any_of(portable["settings"].begin(), portable["settings"].end(),
                        [&](const nlohmann::json& row) {
                            return row.value("key", "") ==
                                       "dashboard_completed_days" &&
                                   row.value("value", "") == "30";
                        });
        auto portableContributorEdge =
            [&](long long sourceId, long long targetId) {
                return portable.contains("source_merges") &&
                    portable["source_merges"].is_array() &&
                    std::any_of(portable["source_merges"].begin(),
                                portable["source_merges"].end(),
                                [&](const nlohmann::json& row) {
                                    return row.value("source_id", 0LL) == sourceId &&
                                        row.value("target_source_id", 0LL) == targetId;
                                });
            };
        const bool portableContributorChain =
            portableContributorEdge(mergeAlias, mergeTarget) &&
            portableContributorEdge(mergeTarget, mergeFinal);
        const bool portableAliasIdentity =
            portable.contains("sources") && portable["sources"].is_array() &&
            std::any_of(portable["sources"].begin(), portable["sources"].end(),
                        [&](const nlohmann::json& row) {
                            return row.value("id", 0LL) == mergeAlias &&
                                row.value("handle", "") ==
                                    "623456789012345670";
                        });
        const bool portableBuildUtf8 =
            portable.contains("builds") && portable["builds"].is_array() &&
            std::any_of(portable["builds"].begin(), portable["builds"].end(),
                        [&](const nlohmann::json& row) {
                            return row.value("id", 0LL) == legacyBuildId &&
                                   row.value("output", "") ==
                                       "Portable\xE2\x80\x94JSON";
                        });
        const std::string portableMarkdown = readFileUtf8(maintenance.primaryPath);
        const bool markdownUsesCanonicalContributor =
            portableMarkdown.find("- Contributors: Merge Final") !=
                std::string::npos &&
            portableMarkdown.find("- Contributors: Merge Source") ==
                std::string::npos &&
            portableMarkdown.find("- Contributors: Merge Target") ==
                std::string::npos;
        bool legacyTextUnchanged = false;
        {
            auto lk = db.guard();
            SQLite::Statement rawBuild(db.raw(lk.token()),
                "SELECT output FROM builds WHERE id=?");
            rawBuild.bind(1, legacyBuildId);
            requireRow(rawBuild, "selftest query rawBuild at source line 4849 returned a row");
            SQLite::Statement rawProject(db.raw(lk.token()),
                "SELECT description FROM projects WHERE id=?");
            rawProject.bind(1, projectId);
            requireRow(rawProject, "selftest query rawProject at source line 4853 returned a row");
            legacyTextUnchanged =
                rawBuild.getColumn(0).getString() == rawPortableJson &&
                rawProject.getColumn(0).getString() == rawPortableMarkdown;
        }
        check(portable.value("format_version", 0) >= 3 &&
              portable.contains("knowledge_nodes") && portable.contains("workflow_runs") &&
              portable.contains("reports") && portable.contains("packet_snapshots") &&
              portable.contains("verification_evidence") &&
              portableLeaderboardPreference && portableContributorChain &&
              portableDashboardCompletedPreference &&
              portableAliasIdentity && portableBuildUtf8 &&
              portableMarkdown.find("Portable\xE2\x80\x94Markdown") !=
                  std::string::npos &&
              legacyTextUnchanged && markdownUsesCanonicalContributor &&
              portable.dump().find("not-a-real-exported-secret") == std::string::npos &&
              portable.dump().find("discord_bot_token") == std::string::npos &&
              portable.dump().find("discord_admin_user_ids") == std::string::npos &&
              portable.dump().find("app_display_name") == std::string::npos &&
              portable.dump().find("My Devhub") == std::string::npos &&
              portable.dump().find("123456789012345678") == std::string::npos &&
               portable.dump().find("987654321098765432") == std::string::npos,
               "portable export normalizes legacy Windows bytes without mutating stored data and preserves contributor lineage");

        IngestOutcome atomicLineageCapture = ingestDiscordMessage(
            &db, detectedImage.sourceChannelId, "devhub",
            "123456789012345669", "test guild", "123456789012345699",
            "Atomic User", "123456789012345698",
            "suggestion: create card lineage in the promotion transaction",
            "2026-07-17T16:00:00Z", false, {});
        nlohmann::json atomicPromotion = promoteSuggestion(
            &db, nullptr, atomicLineageCapture.messageRowId, projectId,
            "implementation", "Atomic promotion lineage");
        long long atomicCardId = 0;
        {
            auto lk = db.guard();
            SQLite::Statement committed(db.raw(lk.token()),
                "SELECT n.id,x.credited,i.credited FROM discord_notify_cards n "
                "JOIN discord_messages m ON m.id=n.discord_message_row_id "
                "JOIN items i ON i.id=m.item_id "
                "JOIN item_sources x ON x.item_id=i.id AND x.source_id=i.source_id "
                "WHERE m.id=? AND m.state='promoted' AND m.item_id=? "
                "AND n.item_id=m.item_id AND n.notify_channel_id=? "
                "AND n.notify_message_id='' AND n.post_state='pending'");
            committed.bind(1, atomicLineageCapture.messageRowId);
            committed.bind(2, atomicPromotion.value("item_id", 0LL));
            committed.bind(3, "123456789012345690");
            const bool hasCommittedCard = committed.executeStep();
            if (hasCommittedCard)
                atomicCardId = committed.getColumn(0).getInt64();
            check(atomicLineageCapture.ingested &&
                      atomicPromotion.value("ok", false) &&
                      hasCommittedCard && atomicCardId > 0 &&
                      committed.getColumn(1).getInt() == 1 &&
                      committed.getColumn(2).getInt() == 1,
                  "promotion commits its item, credited source, legacy credit, "
                  "and canonical card lineage atomically");
        }
        {
            auto lk = db.guard();
            SQLite::Statement link(db.raw(lk.token()),
                "UPDATE item_sources SET credited=0 WHERE item_id=?");
            link.bind(1, atomicPromotion.value("item_id", 0LL));
            link.exec();
            SQLite::Statement legacy(db.raw(lk.token()),
                "UPDATE items SET credited=0 WHERE id=?");
            legacy.bind(1, atomicPromotion.value("item_id", 0LL));
            legacy.exec();
        }
        nlohmann::json atomicPromotionReplay = promoteSuggestion(
            &db, nullptr, atomicLineageCapture.messageRowId, projectId,
            "implementation", "must not replace approved promotion");
        {
            auto lk = db.guard();
            SQLite::Statement repaired(db.raw(lk.token()),
                "SELECT x.credited,i.credited FROM items i "
                "JOIN item_sources x ON x.item_id=i.id AND x.source_id=i.source_id "
                "WHERE i.id=?");
            repaired.bind(1, atomicPromotion.value("item_id", 0LL));
            const bool hasRepairedCredit = repaired.executeStep();
            check(atomicPromotionReplay.value("ok", false) &&
                      atomicPromotionReplay.value("duplicate", false) &&
                      atomicPromotionReplay.value("item_id", 0LL) ==
                          atomicPromotion.value("item_id", 0LL) &&
                      hasRepairedCredit &&
                      repaired.getColumn(0).getInt() == 1 &&
                      repaired.getColumn(1).getInt() == 1,
                  "duplicate promotion replay repairs the original Discord "
                  "contributor credit without replacing the item");
        }
        {
            auto lk = db.guard();
            SQLite::Statement failCreate(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET post_state='failed',"
                "post_attempts=5,post_last_error='old channel' WHERE id=?");
            failCreate.bind(1, atomicCardId);
            failCreate.exec();
        }
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "notify_channel_id",
                          "123456789012345701");
        }
        int confirmedCreateRevision = -1;
        {
            auto lk = db.guard();
            SQLite::Statement revision(db.raw(lk.token()),
                "SELECT post_revision FROM discord_notify_cards WHERE id=?");
            revision.bind(1, atomicCardId);
            requireRow(revision, "selftest query revision at source line 4962 returned a row");
            confirmedCreateRevision = revision.getColumn(0).getInt();
        }
        const bool staleRetargetCreate = recreateFailedNotifyCard(
            &db, nullptr, atomicCardId, "create",
            confirmedCreateRevision + 1);
        const bool retargetCreate = recreateFailedNotifyCard(
            &db, nullptr, atomicCardId, "create",
            confirmedCreateRevision);
        {
            auto lk = db.guard();
            SQLite::Statement posted(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET notify_message_id=?,"
                "post_state='posted' WHERE id=?");
            posted.bind(1, "123456789012345703");
            posted.bind(2, atomicCardId);
            posted.exec();
            SQLite::Statement failEdit(db.raw(lk.token()),
                "UPDATE discord_notify_cards SET edit_state='failed',"
                "edit_attempts=5,edit_last_error='message deleted' WHERE id=?");
            failEdit.bind(1, atomicCardId);
            failEdit.exec();
            SQLite::Statement legacy(db.raw(lk.token()),
                "UPDATE discord_messages SET notify_channel_id=?,"
                "notify_message_id=? WHERE id=?");
            legacy.bind(1, "123456789012345701");
            legacy.bind(2, "123456789012345703");
            legacy.bind(3, atomicLineageCapture.messageRowId);
            legacy.exec();
        }
        {
            auto lk = db.guard();
            db.setSetting(lk.token(), "notify_channel_id",
                          "123456789012345702");
        }
        int confirmedEditRevision = -1;
        {
            auto lk = db.guard();
            SQLite::Statement revision(db.raw(lk.token()),
                "SELECT edit_revision FROM discord_notify_cards WHERE id=?");
            revision.bind(1, atomicCardId);
            requireRow(revision, "selftest query revision at source line 5003 returned a row");
            confirmedEditRevision = revision.getColumn(0).getInt();
        }
        const bool staleRetargetEdit = recreateFailedNotifyCard(
            &db, nullptr, atomicCardId, "edit",
            confirmedEditRevision + 1);
        const bool retargetEdit = recreateFailedNotifyCard(
            &db, nullptr, atomicCardId, "edit",
            confirmedEditRevision);
        {
            auto lk = db.guard();
            SQLite::Statement replacement(db.raw(lk.token()),
                "SELECT n.notify_channel_id,n.notify_message_id,n.post_state,"
                "n.post_attempts,n.post_revision,n.edit_state,n.edit_attempts,"
                "n.edit_revision,m.notify_channel_id,m.notify_message_id "
                "FROM discord_notify_cards n JOIN discord_messages m "
                "ON m.id=n.discord_message_row_id WHERE n.id=?");
            replacement.bind(1, atomicCardId);
            requireRow(replacement, "selftest query replacement at source line 5021 returned a row");
            check(!staleRetargetCreate && retargetCreate &&
                      !staleRetargetEdit && retargetEdit &&
                      replacement.getColumn(0).getString() ==
                          "123456789012345702" &&
                      replacement.getColumn(1).getString().empty() &&
                      replacement.getColumn(2).getString() == "pending" &&
                      replacement.getColumn(3).getInt() == 0 &&
                      replacement.getColumn(4).getInt() == 2 &&
                      replacement.getColumn(5).getString() == "idle" &&
                      replacement.getColumn(6).getInt() == 0 &&
                      replacement.getColumn(7).getInt() == 3 &&
                      replacement.getColumn(8).getString() ==
                          "123456789012345702" &&
                      replacement.getColumn(9).getString().empty(),
                  "revision-confirmed replacement rejects stale create/edit prompts and retargets fresh generations with synchronized legacy receipt");
        }
        });
    }
    appDb.reset();
    std::filesystem::remove_all(testDir, ec);

    std::printf(failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
