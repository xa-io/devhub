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

// Localhost JSON API used by the Discord companion bot and any automation
// (Codex context packets and local scripts). The UI is native ImGui and talks to the
// database directly - it does not go through this server.
class Server {
public:
    Server(Db* db, BuildRunner* builds, uint16_t port,
           const std::string& dataDir);
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
    std::string dataDir_;
    std::thread th_;
};

} // namespace devhub
