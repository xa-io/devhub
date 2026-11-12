#pragma once
#include "devhub/VersionUtil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace devhub {

class Db;
class DiscordBot;

struct PublicPluginRelease {
    std::string plugin, product, repository, version, tag, url, notes;
};

// These selectors require stable manifest data and public GitHub evidence.
bool selectPublicPluginRelease(const std::string& manifest,
                               const std::string& remoteKey,
                               const std::string& projectName,
                               const std::string& projectGithub,
                               PublicPluginRelease& release);
bool qualifyPublicPluginRelease(PublicPluginRelease& release,
                                const std::string& repositoryPayload,
                                const std::string& releasePayload,
                                const std::string& changelog,
                                bool canonicalChangelogExists = false);

bool shouldCheckPublicPluginRelease(Db* db, const PublicPluginRelease& release);
bool beginPublicReleasePoll(std::chrono::steady_clock::time_point now,
                            std::chrono::steady_clock::time_point& next);

// A baseline never posts. A true result has already persisted an unknown
// attempt, so a crash or missing receipt cannot trigger an automatic resend.
// A definitive not_sent remains eligible on a later polling pass.
bool claimPublicPluginRelease(Db* db, const PublicPluginRelease& release,
                              bool readyToPost);
void recordPublicPluginReleaseReceipt(Db* db,
                                     const PublicPluginRelease& release,
                                     const std::string& outcome,
                                     const std::string& messageId);

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

    void startAnnouncements(DiscordBot* bot);
    void refreshAsync();               // coalesces requests during a refresh
    bool busy() const { return busy_; }
    VersionInfo get(long long projectId);
    void stop();                       // idempotent; joins the owned refresh

private:
    void run();
    void worker(bool announcementPass);
    void observeRelease(const PublicPluginRelease& release);

    Db* db_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopping_{false};
    std::mutex threadMu_; // guards worker_
    std::condition_variable wake_;
    bool requested_ = false;
    bool announcements_ = false;
    DiscordBot* bot_ = nullptr;
    std::thread worker_;
    std::mutex mu_; // guards results_
    std::map<long long, VersionInfo> results_;
};

// Simple blocking HTTPS/HTTP GET. Returns body, or "" on failure.
std::string httpGet(const std::string& url,
                    unsigned long* responseStatus = nullptr);

} // namespace devhub
