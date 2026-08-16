#pragma once
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace devhub {

class Db;

// One action execution (build / prep / release / version bump / custom):
// `cmd /C <command>` in a working directory, stdout and stderr merged and
// captured incrementally so the UI can show live output. Optional stdin data
// is written to the child then closed, which drives the interactive prompts
// in the release scripts ("yes" confirms, version menu picks, final
// press-enter) and cleanly EOFs anything unexpected.
struct BuildJob {
    long long id = 0;
    long long projectId = 0;
    std::string kind = "build";
    std::string command;
    std::string cwd;
    std::string stdinData;
    std::string startedAt;
    std::string finishedAt;
    std::atomic<bool> done{false};
    int exitCode = -1;
    std::string status = "running"; // running | success | failed | error

    std::mutex mu;      // guards output and outputBase
    std::string output; // normalized UTF-8 retained live window
    size_t outputBase = 0; // absolute stream offset represented by output[0]
};

// Deterministic component-test seam for failures that the operating system
// cannot be asked to produce reliably. Defaults are inert in production.
struct BuildRunnerTestHooks {
    size_t failOutputReadAfterBytes = (std::numeric_limits<size_t>::max)();
    std::uint32_t outputReadError = 30; // ERROR_READ_FAULT
    std::uint32_t injectedTerminationError = 0;
};

class BuildRunner {
public:
    explicit BuildRunner(Db* db, BuildRunnerTestHooks testHooks = {})
        : db_(db), testHooks_(testHooks) {}
    ~BuildRunner() { stop(); }
    BuildRunner(const BuildRunner&) = delete;
    BuildRunner& operator=(const BuildRunner&) = delete;

    // Returns build id, or 0 with `error` set (an action is already running
    // for this project, or the command is empty).
    long long start(long long projectId, const std::string& kind,
                    const std::string& command, const std::string& cwd,
                    const std::string& stdinData, std::string& error);

    // Live status + output from `offset`; falls back to the DB for finished
    // builds that are no longer resident.
    nlohmann::json status(long long buildId, size_t offset);

    bool isRunning(long long projectId);
    long long runningJobId(long long projectId); // 0 when idle

    // Prevents new actions, asks running children to terminate, and joins all
    // owned workers. Safe to call more than once.
    void stop();
    bool stopping() const { return stopping_.load(); }

private:
    void runJob(std::shared_ptr<BuildJob> job);
    void failJob(std::shared_ptr<BuildJob> job,
                 const std::string& error) noexcept;

    Db* db_;
    BuildRunnerTestHooks testHooks_;
    std::atomic<bool> stopping_{false};
    std::mutex mu_; // guards jobs_ and starting_
    std::map<long long, std::shared_ptr<BuildJob>> jobs_;
    std::set<long long> starting_; // project reservations not yet published
    std::mutex threadsMu_;
    std::vector<std::thread> threads_; // owned and joined by stop()
};

} // namespace devhub
