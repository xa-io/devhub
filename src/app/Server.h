#pragma once
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace devhub {

class Db;
class BuildRunner;
class DiscordBot;

// Resolve the exact per-user rendezvous for one canonical loopback instance.
// Production callers omit the override and use LocalAppData; native self-tests
// supply an isolated root so they never touch the live client rendezvous.
std::string apiRendezvousPath(
    uint16_t port, const std::string& rendezvousRootOverride = {});

// Localhost JSON API used by the Discord companion bot and any automation
// (Codex context packets and local scripts). The UI is native ImGui and talks to the
// database directly - it does not go through this server.
class Server {
public:
    Server(Db* db, BuildRunner* builds, uint16_t port,
           const std::string& rendezvousRootOverride = {});
    ~Server();

    uint16_t start(); // returns the bound port (0 on failure)
    void stop();

    // Optional: embedded Discord bot status, reported in /api/health.
    std::function<nlohmann::json()> discordStatus;
    // Optional: embedded bot, used to post notification cards for
    // detections that arrive through /api/discord/ingest.
    DiscordBot* bot = nullptr;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Db* db_;
    BuildRunner* builds_;
    uint16_t port_;
    std::string rendezvousRootOverride_;
    std::string rendezvousPath_;
    std::thread th_;
};

} // namespace devhub
