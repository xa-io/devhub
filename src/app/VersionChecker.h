#pragma once
#include "devhub/VersionUtil.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace devhub {

class Db;

struct VersionInfo {
    std::string local;   // parsed from local_version_file
    std::string remote;  // parsed from remote_version_url payload
    VersionState state = VersionState::Unknown;
    std::string error;
    bool checked = false;
};

// Compares each project's in-development version against its published
// version. Local files are read directly; remote payloads are fetched over
// WinHTTP on a background thread (one fetch per distinct URL per refresh).
class VersionChecker {
public:
    explicit VersionChecker(Db* db) : db_(db) {}
    ~VersionChecker() { stop(); }
    VersionChecker(const VersionChecker&) = delete;
    VersionChecker& operator=(const VersionChecker&) = delete;

    void refreshAsync();               // no-op while a refresh is running
    bool busy() const { return busy_; }
    VersionInfo get(long long projectId);
    void stop();                       // idempotent; joins the owned refresh

private:
    void worker();

    Db* db_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopping_{false};
    std::mutex threadMu_; // guards worker_
    std::thread worker_;
    std::mutex mu_; // guards results_
    std::map<long long, VersionInfo> results_;
};

// Simple blocking HTTPS/HTTP GET. Returns body, or "" on failure.
std::string httpGet(const std::string& url);

} // namespace devhub
