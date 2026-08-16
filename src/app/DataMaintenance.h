#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace devhub {

class Db;

struct MaintenanceResult {
    bool ok = false;
    std::string message;
    std::string primaryPath;
    std::string secondaryPath;
};

MaintenanceResult createDatabaseBackup(Db* db, int retentionCount = 14);
MaintenanceResult exportPortableData(Db* db, int retentionCount = 14);

// Runs each half at most once per local day. A successful backup is stamped
// independently, so a persistently failing export cannot repeat VACUUM on
// every launch.
MaintenanceResult runAutomaticDailyMaintenance(Db* db);

// Owns the one-at-a-time maintenance worker. No operation is detached; stop()
// joins the worker before Db can be destroyed.
class MaintenanceRunner {
public:
    explicit MaintenanceRunner(Db* db) : db_(db) {}
    ~MaintenanceRunner() { stop(); }

    MaintenanceRunner(const MaintenanceRunner&) = delete;
    MaintenanceRunner& operator=(const MaintenanceRunner&) = delete;

    bool requestBackup(int retentionCount);
    bool requestExport(int retentionCount);
    bool requestAutomatic();
    bool busy() const noexcept { return busy_.load(); }
    bool takeCompleted(MaintenanceResult& result);
    void stop();

private:
    enum class Kind { Backup, Export, Automatic };
    bool request(Kind kind, int retentionCount);

    Db* db_ = nullptr;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopping_{false};
    std::mutex workerMu_;
    std::thread worker_;
    std::mutex resultMu_;
    MaintenanceResult completed_;
    bool resultReady_ = false;
};

} // namespace devhub
