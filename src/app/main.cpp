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
#include <string>
#include <thread>

using namespace devhub;

static std::string exeDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    auto slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : p.substr(0, slash);
}

static std::string gTerminateCrashLogPath;

static void terminateWithCrashLog() noexcept {
    try {
        std::ofstream crash(gTerminateCrashLogPath,
                            std::ios::binary | std::ios::app);
        if (crash) {
            crash << "\n=== std::terminate at " << nowIsoUtc() << " ===\n";
            if (const std::exception_ptr current = std::current_exception()) {
                try {
                    std::rethrow_exception(current);
                } catch (const std::exception& e) {
                    crash << "exception: " << e.what() << "\n";
                } catch (...) {
                    crash << "exception: unknown\n";
                }
            }
            for (const std::string& line : appLogSnapshot())
                crash << line << "\n";
            crash.flush();
        }
    } catch (...) {
        // std::terminate cannot recover; crash logging is best effort only.
    }
    std::abort();
}

// In the windowed build there is no console; when launched from a terminal
// (for --selftest / --help / --headless) attach to the parent's console so
// printf output is visible. Preserve an inherited pipe/file first: the build
// wrapper deliberately captures self-test output and must not have that handle
// replaced with CONOUT$ by the windowed executable.
static void attachParentConsole() {
#if !defined(DEVHUB_CONSOLE)
    const HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD stdoutType = stdoutHandle && stdoutHandle != INVALID_HANDLE_VALUE
        ? GetFileType(stdoutHandle) : FILE_TYPE_UNKNOWN;
    if (stdoutType == FILE_TYPE_PIPE || stdoutType == FILE_TYPE_DISK) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

static int checkDatabase(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "database does not exist: %s\n", path.c_str());
        return 2;
    }
    try {
        Db db(path); // opening performs the normal forward-only migration
        auto lk = db.guard();
        const int schema = db.raw(lk.token()).execAndGet("PRAGMA user_version").getInt();
        const std::string integrity = db.raw(lk.token()).execAndGet("PRAGMA integrity_check").getString();
        SQLite::Statement foreignKeys(db.raw(lk.token()), "PRAGMA foreign_key_check");
        int foreignKeyErrors = 0;
        while (foreignKeys.executeStep()) ++foreignKeyErrors;
        std::printf("database check\n  path: %s\n  schema: v%d\n  integrity: %s\n"
                    "  foreign_key_errors: %d\n",
                    path.c_str(), schema, integrity.c_str(), foreignKeyErrors);
        return schema == 14 && integrity == "ok" && foreignKeyErrors == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "database check failed: %s\n", e.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    uint16_t port = 21100;
    std::string dataDir = exeDir() + "\\data";
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") { attachParentConsole(); return runAppSelftest(); }
        if (a == "--check-db" && i + 1 < argc) {
            attachParentConsole();
            return checkDatabase(argv[++i]);
        }
        if (a == "--version") {
            attachParentConsole();
            std::printf("%s v%s\n", DEVHUB_APP_NAME, DEVHUB_VERSION);
            return 0;
        }
        if (a == "--headless" || a == "--no-browser") headless = true;
        if (a == "--port") {
            if (i + 1 >= argc) {
                attachParentConsole();
                std::fprintf(stderr,
                             "--port must be an integer from 1024 to 65535\n");
                return 2;
            }
            const std::string text = argv[++i];
            size_t used = 0;
            long parsed = 0;
            try {
                parsed = std::stol(text, &used);
            } catch (...) {
                used = 0;
            }
            if (used != text.size() || parsed < 1024 || parsed > 65535) {
                attachParentConsole();
                std::fprintf(stderr,
                             "--port must be an integer from 1024 to 65535\n");
                return 2;
            }
            port = static_cast<uint16_t>(parsed);
        }
        if (a == "--data-dir" && i + 1 < argc) dataDir = argv[++i];
        if (a == "--help") {
            attachParentConsole();
            std::printf("%s v%s\n"
                        "  --port N       listen port (default 21100)\n"
                        "  --data-dir P   database directory (default <exe>\\data)\n"
                        "  --headless     run the server without a window (bot/automation)\n"
                        "  --selftest     run built-in checks and exit\n"
                        "  --check-db P   migrate and verify a stopped/copy database\n",
                        DEVHUB_APP_NAME, DEVHUB_VERSION);
            return 0;
        }
    }

    // Single instance: focus the existing window instead of double-running
    // (two instances would fight over the port and the database).
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\XADevHubSingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"XADevHubNative", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    CreateDirectoryA(dataDir.c_str(), nullptr);
    gTerminateCrashLogPath = dataDir + "\\crash.log";
    std::set_terminate(terminateWithCrashLog);
    std::string dbPath = dataDir + "\\devhub.db";

    int rc = 0;
    try {
        Db db(dbPath);
        seedDefaults(db);
        MaintenanceRunner maintenance(&db);
        maintenance.requestAutomatic();
        BuildRunner builds(&db);
        VersionChecker versions(&db);
        DiscordBot discord(&db);
        Server server(&db, &builds, port, dataDir);
        server.discordStatus = [&discord]() {
            DiscordBot::Status s = discord.status();
            return nlohmann::json{{"state", s.state},
                                  {"bot_user", s.botUser},
                                  {"guilds", s.guilds},
                                  {"error", s.error},
                                  {"warn", s.warn},
                                  {"last_log", s.lastLog}};
        };
        server.bot = &discord;
        if (server.start() == 0) {
            appLog("[api] port " + std::to_string(port) +
                   " is unavailable; local automation is disabled this session");
            MessageBoxA(
                nullptr,
                "The XA DevHub API port is unavailable. The native UI will "
                "continue, but local automation cannot reach this instance.",
                "XA DevHub API unavailable", MB_OK | MB_ICONWARNING);
        }

        // Embedded Discord monitor: connects automatically when a token has
        // been saved on the Discord settings page.
        {
            std::string token;
            {
                auto lk = db.guard();
                token = db.getSetting(lk.token(), "discord_bot_token");
            }
            if (!trim(token).empty()) discord.start(token);
        }

        if (headless) {
            attachParentConsole();
            std::printf("%s v%s (headless)\n  db:  %s\n  api: http://127.0.0.1:%u/\n"
                        "Press Ctrl+C to stop.\n",
                        DEVHUB_APP_NAME, DEVHUB_VERSION, dbPath.c_str(), port);
            fflush(stdout);
            static HANDLE stopEvent =
                CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stopEvent ||
                !SetConsoleCtrlHandler([](DWORD type) -> BOOL {
                    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT &&
                        type != CTRL_CLOSE_EVENT && type != CTRL_LOGOFF_EVENT &&
                        type != CTRL_SHUTDOWN_EVENT)
                        return FALSE;
                    SetEvent(stopEvent);
                    return TRUE;
                }, TRUE)) {
                throw std::runtime_error(
                    "could not install the headless shutdown handler");
            }
            WaitForSingleObject(stopEvent, INFINITE);
            SetConsoleCtrlHandler(nullptr, FALSE);
            CloseHandle(stopEvent);
            stopEvent = nullptr;
            std::printf("stopping...\n");
            fflush(stdout);
        } else {
            rc = runGui(&db, &builds, &versions, &discord, &maintenance, port);
        }
        server.stop();
        discord.stop();
        versions.stop();
        builds.stop();
        maintenance.stop();
    } catch (const std::exception& e) {
        attachParentConsole();
        std::fprintf(stderr, "fatal: %s\n", e.what());
        MessageBoxA(nullptr, e.what(), "XA DevHub - fatal error", MB_OK | MB_ICONERROR);
        rc = 1;
    }

    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return rc;
}
