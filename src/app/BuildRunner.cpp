#include "BuildRunner.h"
#include "Db.h"
#include "devhub/Util.h"

#include <windows.h>

#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace devhub {

bool BuildRunner::isRunning(long long projectId) {
    return runningJobId(projectId) != 0;
}

long long BuildRunner::runningJobId(long long projectId) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : jobs_)
        if (kv.second->projectId == projectId && !kv.second->done) return kv.first;
    return 0;
}

long long BuildRunner::start(long long projectId, const std::string& kind,
                             const std::string& command, const std::string& cwd,
                             const std::string& stdinData, std::string& error) {
    if (trim(command).empty()) { error = "no command configured"; return 0; }
    {
        // Reserve the project under the same lock that owns the resident jobs.
        // This closes the GUI/API double-start window without holding the lock
        // across SQLite or thread construction.
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) { error = "shutting down"; return 0; }
        for (const auto& kv : jobs_) {
            if (kv.second->projectId == projectId && !kv.second->done) {
                error = "an action is already running for this project";
                return 0;
            }
        }
        if (!starting_.insert(projectId).second) {
            error = "an action is already starting for this project";
            return 0;
        }
    }
    struct ReservationRelease {
        std::mutex& mu;
        std::set<long long>& starting;
        long long projectId;
        ~ReservationRelease() {
            std::lock_guard<std::mutex> lk(mu);
            starting.erase(projectId);
        }
    } release{mu_, starting_, projectId};

    auto job = std::make_shared<BuildJob>();
    job->projectId = projectId;
    job->kind = kind;
    job->command = command;
    job->cwd = cwd;
    job->stdinData = stdinData;
    job->startedAt = nowIsoUtc();

    {
        auto lk = db_->guard();
        SQLite::Statement ins(db_->raw(lk.token()),
            "INSERT INTO builds(project_id,command,cwd,status,started_at,kind) "
            "VALUES(?,?,?,?,?,?)");
        ins.bind(1, (long long)projectId);
        ins.bind(2, command);
        ins.bind(3, cwd);
        ins.bind(4, "running");
        ins.bind(5, job->startedAt);
        ins.bind(6, kind);
        ins.exec();
        job->id = db_->raw(lk.token()).getLastInsertRowid();
        db_->logActivity(lk.token(), kind + "_started", projectId, command);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_[job->id] = job;
        // jobs_ is ordered by ascending build id. Bound the resident FINISHED
        // set at 20 while retaining every running job (running work is bounded
        // independently to one action per project).
        size_t finished = 0;
        for (const auto& kv : jobs_)
            if (kv.second->done) ++finished;
        for (auto it = jobs_.begin(); finished > 20 && it != jobs_.end();) {
            if (it->second->done) {
                it = jobs_.erase(it);
                --finished;
            } else {
                ++it;
            }
        }
    }

    try {
        std::lock_guard<std::mutex> lk(threadsMu_);
        if (stopping_) throw std::runtime_error("shutting down");

        // Reserve before constructing the OS thread. Once the thread exists,
        // moving it into the vector is noexcept and cannot strand a joinable
        // temporary during an allocation failure.
        threads_.reserve(threads_.size() + 1);
        std::thread worker([this, job]() {
            try {
                runJob(job);
            } catch (const std::exception& e) {
                failJob(job, e.what());
            } catch (...) {
                failJob(job, "unknown worker exception");
            }
        });
        threads_.push_back(std::move(worker));
    } catch (const std::exception& e) {
        failJob(job, std::string("could not start worker thread: ") + e.what());
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.erase(job->id);
        }
        error = std::string("could not start worker thread: ") + e.what();
        return 0;
    } catch (...) {
        failJob(job, "could not start worker thread: unknown exception");
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.erase(job->id);
        }
        error = "could not start worker thread: unknown exception";
        return 0;
    }
    return job->id;
}

void BuildRunner::stop() {
    stopping_ = true;
    std::vector<std::thread> owned;
    {
        std::lock_guard<std::mutex> lk(threadsMu_);
        owned.swap(threads_);
    }
    for (std::thread& worker : owned) {
        if (worker.joinable()) worker.join();
    }
}

void BuildRunner::failJob(std::shared_ptr<BuildJob> job,
                          const std::string& error) noexcept {
    try {
        const std::string safeError = error.empty()
                                          ? "unknown worker exception" : error;
        const std::string finishedAt = nowIsoUtc();
        std::string failureLine;
        {
            std::lock_guard<std::mutex> lk(job->mu);
            failureLine = normalizeUtf8(
                "[devhub] build worker failed: " + safeError + "\n");
            job->output += failureLine;
            constexpr size_t kLiveCap = 2 * 1024 * 1024;
            if (job->output.size() > kLiveCap) {
                size_t drop = job->output.size() - kLiveCap;
                while (drop < job->output.size() &&
                       (static_cast<unsigned char>(job->output[drop]) & 0xc0) ==
                           0x80)
                    ++drop;
                job->output.erase(0, drop);
                job->outputBase += drop;
            }
            job->exitCode = -1;
            job->status = "error";
            job->finishedAt = finishedAt;
        }
        job->done = true;
        appLog("[build] worker failed for build " +
               std::to_string(job->id) + ": " + safeError);

        try {
            auto lk = db_->guard();
            SQLite::Statement up(db_->raw(lk.token()),
                "UPDATE builds SET status='error',exit_code=-1,output=output || ?,"
                "finished_at=? WHERE id=?");
            up.bind(1, failureLine);
            up.bind(2, finishedAt);
            up.bind(3, static_cast<long long>(job->id));
            up.exec();
            db_->logActivity(lk.token(), job->kind + "_finished", job->projectId,
                             "error (worker exception)");
        } catch (const std::exception& e) {
            appLog("[build] couldn't persist worker failure for build " +
                   std::to_string(job->id) + ": " + e.what());
        } catch (...) {
            appLog("[build] couldn't persist worker failure for build " +
                   std::to_string(job->id) + ": unknown exception");
        }
    } catch (...) {
        // Last-resort containment: never let failure reporting escape the
        // worker entry and invoke std::terminate.
        if (job) job->done = true;
    }
}

void BuildRunner::runJob(std::shared_ptr<BuildJob> job) {
    HANDLE outRead = nullptr, outWrite = nullptr;
    HANDLE inRead = nullptr, inWrite = nullptr;
    HANDLE processJob = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    std::string finalStatus = "error";
    int exitCode = -1;
    constexpr size_t kLiveCap = 2 * 1024 * 1024;
    constexpr size_t kPersistBatch = 256 * 1024;
    std::string persistBatch;
    std::string persistenceError;
    auto flushPersistedOutput = [&](bool retry) {
        if (persistBatch.empty()) return true;
        if (!persistenceError.empty() && !retry) return false;
        std::string batch;
        batch.swap(persistBatch);
        try {
            auto lk = db_->guard();
            SQLite::Statement append(db_->raw(lk.token()),
                "UPDATE builds SET output=output || ? WHERE id=?");
            append.bind(1, batch);
            append.bind(2, static_cast<long long>(job->id));
            append.exec();
            persistenceError.clear();
            return true;
        } catch (const std::exception& e) {
            persistBatch.insert(0, batch);
            persistenceError = e.what();
            return false;
        } catch (...) {
            persistBatch.insert(0, batch);
            persistenceError = "unknown database exception";
            return false;
        }
    };
    auto appendNormalized = [&](const std::string& text) {
        if (text.empty()) return;
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->output.append(text);
            if (job->output.size() > kLiveCap) {
                size_t drop = job->output.size() - kLiveCap;
                // The retained window itself must remain valid UTF-8. It is
                // safe to discard up to three extra continuation bytes.
                while (drop < job->output.size() &&
                       (static_cast<unsigned char>(job->output[drop]) & 0xc0) ==
                           0x80)
                    ++drop;
                job->output.erase(0, drop);
                job->outputBase += drop;
            }
        }
        persistBatch.append(text);
        if (persistBatch.size() >= kPersistBatch)
            flushPersistedOutput(false);
    };
    std::string utf8Carry;
    auto flushUtf8Carry = [&]() {
        if (utf8Carry.empty()) return;
        appendNormalized(normalizeUtf8(utf8Carry));
        utf8Carry.clear();
    };
    auto appendChildOutput = [&](const char* data, size_t size) {
        utf8Carry.append(data, size);
        const size_t complete = utf8SafeSplitPoint(utf8Carry);
        if (complete == 0) return;
        appendNormalized(normalizeUtf8(utf8Carry.substr(0, complete)));
        utf8Carry.erase(0, complete);
    };
    auto appendLine = [&](const std::string& text) {
        // A parent-generated diagnostic is a real stream boundary. Flush any
        // dangling child byte before placing the diagnostic after it.
        flushUtf8Carry();
        appendNormalized(normalizeUtf8(text));
    };

    if (CreatePipe(&outRead, &outWrite, &sa, 0) &&
        CreatePipe(&inRead, &inWrite, &sa, 0)) {
        const bool handlesSecured =
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0) != FALSE &&
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0) != FALSE;
        const DWORD handleSecurityError =
            handlesSecured ? ERROR_SUCCESS : GetLastError();

        STARTUPINFOEXA startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = outWrite;
        startup.StartupInfo.hStdError = outWrite;
        startup.StartupInfo.hStdInput = inRead;

        PROCESS_INFORMATION pi{};
        std::string cmdLine = "cmd.exe /C " + job->command;
        std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back('\0');

        const char* cwd = job->cwd.empty() ? nullptr : job->cwd.c_str();
        DWORD processJobError = ERROR_SUCCESS;
        if (!stopping_) {
            processJob = CreateJobObjectA(nullptr, nullptr);
            if (processJob) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags =
                    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(processJob,
                        JobObjectExtendedLimitInformation, &limits,
                        sizeof(limits))) {
                    processJobError = GetLastError();
                    CloseHandle(processJob);
                    processJob = nullptr;
                }
            } else {
                processJobError = GetLastError();
            }
        }

        BOOL ok = FALSE;
        DWORD launchError = stopping_ ? ERROR_CANCELLED : ERROR_SUCCESS;
        SIZE_T attributeBytes = 0;
        std::vector<unsigned char> attributeStorage;
        bool attributesInitialized = false;
        if (!handlesSecured) {
            launchError = handleSecurityError;
            appendLine("[devhub] could not secure pipe handles (error " +
                       std::to_string(launchError) + ")\n");
        } else if (!stopping_ && !processJob) {
            launchError = processJobError == ERROR_SUCCESS
                ? ERROR_INVALID_HANDLE : processJobError;
            appendLine("[devhub] could not create a kill-on-close process job "
                       "(error " + std::to_string(launchError) + ")\n");
        } else if (!stopping_) {
            InitializeProcThreadAttributeList(
                nullptr, 1, 0, &attributeBytes);
            if (attributeBytes == 0) {
                launchError = GetLastError();
            } else {
                attributeStorage.resize(attributeBytes);
                startup.lpAttributeList =
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                        attributeStorage.data());
                attributesInitialized = InitializeProcThreadAttributeList(
                    startup.lpAttributeList, 1, 0, &attributeBytes) != FALSE;
                if (!attributesInitialized) {
                    launchError = GetLastError();
                } else {
                    HANDLE inheritOnly[] = {outWrite, inRead};
                    if (!UpdateProcThreadAttribute(
                            startup.lpAttributeList, 0,
                            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritOnly,
                            sizeof(inheritOnly), nullptr, nullptr)) {
                        launchError = GetLastError();
                    } else {
                        ok = CreateProcessA(
                            nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED |
                                EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, cwd, &startup.StartupInfo, &pi);
                        launchError = ok ? ERROR_SUCCESS : GetLastError();
                    }
                }
            }
        }
        if (attributesInitialized)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(outWrite); outWrite = nullptr; // child owns its copies now
        CloseHandle(inRead);   inRead = nullptr;

        if (ok) {
            bool jobAssigned = false;
            if (processJob) {
                jobAssigned = AssignProcessToJobObject(processJob, pi.hProcess) != FALSE;
                if (!jobAssigned) {
                    const DWORD assignError = GetLastError();
                    TerminateProcess(pi.hProcess, ERROR_CANCELLED);
                    WaitForSingleObject(pi.hProcess, 5000);
                    appendLine("[devhub] child process job assignment failed "
                               "(error " + std::to_string(assignError) + ")\n");
                }
            }
            const DWORD resumeResult = jobAssigned
                ? ResumeThread(pi.hThread) : static_cast<DWORD>(-1);
            CloseHandle(pi.hThread);
            pi.hThread = nullptr;
            if (jobAssigned && resumeResult == static_cast<DWORD>(-1)) {
                const DWORD resumeError = GetLastError();
                TerminateProcess(pi.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(pi.hProcess, 5000);
                appendLine("[devhub] failed to resume process (error " +
                           std::to_string(resumeError) + ")\n");
                CloseHandle(pi.hProcess);
                pi.hProcess = nullptr;
            } else if (!jobAssigned) {
                CloseHandle(pi.hProcess);
                pi.hProcess = nullptr;
            }

            // Feed the scripted answers (if any), then EOF the child's stdin
            // so leftover input() prompts exit instead of hanging forever.
            if (pi.hProcess && !job->stdinData.empty()) {
                size_t sent = 0;
                while (sent < job->stdinData.size()) {
                    const DWORD wanted = static_cast<DWORD>(
                        std::min<size_t>(job->stdinData.size() - sent, 4096));
                    DWORD written = 0;
                    const BOOL wrote = WriteFile(
                        inWrite, job->stdinData.data() + sent, wanted,
                        &written, nullptr);
                    const DWORD inputError = wrote
                        ? (written == 0 ? ERROR_WRITE_FAULT : ERROR_SUCCESS)
                        : GetLastError();
                    if (!wrote || written == 0) {
                        appendLine(
                            "[devhub] scripted input truncated after " +
                            std::to_string(sent) + " of " +
                            std::to_string(job->stdinData.size()) +
                            " bytes (error " +
                            std::to_string(inputError) + ")\n");
                        break;
                    }
                    sent += written;
                }
            }
            CloseHandle(inWrite); inWrite = nullptr;

            bool pipeOpen = true;
            bool outputReadFailed = false;
            DWORD outputReadError = ERROR_SUCCESS;
            size_t childOutputBytes = 0;
            const auto failOutputRead = [&](DWORD error) {
                pipeOpen = false;
                outputReadFailed = true;
                outputReadError = error == ERROR_SUCCESS
                    ? ERROR_READ_FAULT : error;
                appendLine("[devhub] child output read failed (error " +
                           std::to_string(outputReadError) + ")\n");
                // Once the reader fails, a chatty child can block forever on
                // a full pipe. Closing our end wakes it while the process/job
                // lifetime logic below still owns and reaps it.
                if (outRead) {
                    CloseHandle(outRead);
                    outRead = nullptr;
                }
            };
            auto drainAvailable = [&]() {
                while (pipeOpen) {
                    if (childOutputBytes >=
                        testHooks_.failOutputReadAfterBytes) {
                        failOutputRead(
                            static_cast<DWORD>(testHooks_.outputReadError));
                        break;
                    }
                    DWORD available = 0;
                    if (!PeekNamedPipe(outRead, nullptr, 0, nullptr,
                                       &available, nullptr)) {
                        const DWORD pipeError = GetLastError();
                        pipeOpen = false;
                        if (pipeError != ERROR_BROKEN_PIPE)
                            failOutputRead(pipeError);
                        break;
                    }
                    if (available == 0) break;
                    char buf[4096];
                    const DWORD wanted =
                        (available < sizeof(buf)) ? available : sizeof(buf);
                    DWORD n = 0;
                    const BOOL readOk = ReadFile(outRead, buf, wanted, &n, nullptr);
                    if (!readOk || n == 0) {
                        const DWORD pipeError = readOk ? ERROR_BROKEN_PIPE : GetLastError();
                        pipeOpen = false;
                        if (pipeError != ERROR_BROKEN_PIPE)
                            failOutputRead(pipeError);
                        break;
                    }
                    appendChildOutput(buf, n);
                    childOutputBytes += n;
                }
            };

            bool stopRequested = false;
            DWORD waitResult = WAIT_FAILED;
            DWORD waitError = ERROR_SUCCESS;
            if (pi.hProcess) {
                for (;;) {
                    drainAvailable();
                    if (stopping_ && !stopRequested) {
                        stopRequested = true;
                        appendLine("[devhub] action stopped during application shutdown\n");
                        const BOOL terminated = jobAssigned
                            ? TerminateJobObject(processJob, ERROR_CANCELLED)
                            : TerminateProcess(pi.hProcess, ERROR_CANCELLED);
                        const DWORD capturedTerminationError = terminated
                            ? ERROR_SUCCESS : GetLastError();
                        const DWORD terminationError =
                            testHooks_.injectedTerminationError != 0
                                ? static_cast<DWORD>(
                                      testHooks_.injectedTerminationError)
                                : capturedTerminationError;
                        if ((!terminated ||
                             testHooks_.injectedTerminationError != 0) &&
                            terminationError != ERROR_ACCESS_DENIED) {
                            appendLine("[devhub] child termination failed (error " +
                                       std::to_string(terminationError) + ")\n");
                        }
                    }
                    waitResult = WaitForSingleObject(pi.hProcess, 50);
                    if (waitResult == WAIT_FAILED) waitError = GetLastError();
                    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
                        break;
                }
                drainAvailable();
            }

            if (pi.hProcess && waitResult == WAIT_OBJECT_0) {
                DWORD code = 0;
                const BOOL gotExitCode = GetExitCodeProcess(pi.hProcess, &code);
                if (gotExitCode && code != STILL_ACTIVE) {
                    exitCode = static_cast<int>(code);
                    finalStatus = stopRequested || outputReadFailed
                        ? "error" : ((code == 0) ? "success" : "failed");
                } else {
                    const DWORD processError = gotExitCode
                                                   ? ERROR_BUSY : GetLastError();
                    appendLine("[devhub] could not read child exit code (error " +
                               std::to_string(processError) + ")\n");
                }
            } else if (pi.hProcess) {
                appendLine("[devhub] child wait failed (result " +
                           std::to_string(waitResult) + ", error " +
                           std::to_string(waitError) + ")\n");
                if (jobAssigned) TerminateJobObject(processJob, ERROR_CANCELLED);
                else TerminateProcess(pi.hProcess, ERROR_CANCELLED);
                WaitForSingleObject(pi.hProcess, 5000);
            }
            if (pi.hProcess) CloseHandle(pi.hProcess);
        } else {
            appendLine("[devhub] failed to start process (error " +
                       std::to_string(launchError) + ")");
            if (launchError == ERROR_ELEVATION_REQUIRED) {
                appendLine(" - this executable is marked RUNASADMIN; clear its "
                           "compatibility setting instead of elevating DevHub");
            } else if (launchError == ERROR_DIRECTORY ||
                       launchError == ERROR_PATH_NOT_FOUND) {
                appendLine(" - the configured working directory does not exist: " +
                           job->cwd);
            }
            appendLine("\n");
        }
    } else {
        appendLine("[devhub] failed to create pipes\n");
    }
    if (processJob) CloseHandle(processJob);
    if (outRead) CloseHandle(outRead);
    if (outWrite) CloseHandle(outWrite);
    if (inRead) CloseHandle(inRead);
    if (inWrite) CloseHandle(inWrite);
    flushUtf8Carry();
    const bool outputPersisted = flushPersistedOutput(true);
    std::string persistenceDiagnostic;
    if (!outputPersisted) {
        finalStatus = "error";
        exitCode = -1;
        persistenceDiagnostic = normalizeUtf8(
            "[devhub] could not persist the complete build output: " +
            persistenceError + "\n");
        {
            std::lock_guard<std::mutex> lk(job->mu);
            job->output.append(persistenceDiagnostic);
            if (job->output.size() > kLiveCap) {
                size_t drop = job->output.size() - kLiveCap;
                while (drop < job->output.size() &&
                       (static_cast<unsigned char>(job->output[drop]) & 0xc0) ==
                           0x80)
                    ++drop;
                job->output.erase(0, drop);
                job->outputBase += drop;
            }
        }
    }

    const std::string finishedAt = nowIsoUtc();
    {
        std::lock_guard<std::mutex> lk(job->mu);
        // Output is normalized exactly once at append time, so the resident
        // window and the incrementally persisted stream share one byte-offset
        // space even after the resident job is evicted.
        job->exitCode = exitCode;
        job->status = finalStatus;
        job->finishedAt = finishedAt;
    }
    job->done = true;
    {
        auto lk = db_->guard();
        SQLite::Statement up(db_->raw(lk.token()),
            "UPDATE builds SET status=?, exit_code=?, output=output || ?, "
            "finished_at=? WHERE id=?");
        up.bind(1, finalStatus);
        up.bind(2, exitCode);
        up.bind(3, persistenceDiagnostic);
        up.bind(4, finishedAt);
        up.bind(5, (long long)job->id);
        up.exec();
        db_->logActivity(lk.token(), job->kind + "_finished", job->projectId,
                         finalStatus + " (exit " + std::to_string(exitCode) + ")");
    }
}

json BuildRunner::status(long long buildId, size_t offset) {
    std::shared_ptr<BuildJob> job;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = jobs_.find(buildId);
        if (it != jobs_.end()) job = it->second;
    }
    json j;
    if (job) {
        std::lock_guard<std::mutex> lk(job->mu);
        j["id"] = job->id;
        j["project_id"] = job->projectId;
        j["kind"] = job->kind;
        j["command"] = job->command;
        j["status"] = job->status;
        j["exit_code"] = job->exitCode;
        j["started_at"] = job->startedAt;
        j["finished_at"] = job->finishedAt;
        j["done"] = job->done.load();
        const size_t streamEnd = job->outputBase + job->output.size();
        size_t from = offset;
        bool truncated = false;
        if (from < job->outputBase) {
            from = job->outputBase;
            truncated = true;
        } else if (from > streamEnd) {
            from = streamEnd;
            truncated = true;
        }
        size_t relative = from - job->outputBase;
        while (relative < job->output.size() &&
               (static_cast<unsigned char>(job->output[relative]) & 0xc0) ==
                   0x80) {
            ++relative;
            ++from;
            truncated = true;
        }
        j["output"] = from < streamEnd
            ? job->output.substr(relative) : std::string();
        j["next_offset"] = streamEnd;
        j["dropped_before"] = job->outputBase;
        if (truncated) j["truncated"] = true;
        return j;
    }
    // Fall back to the DB for older builds.
    auto lk = db_->guard();
    SQLite::Statement q(db_->raw(lk.token()), "SELECT * FROM builds WHERE id=?");
    q.bind(1, (long long)buildId);
    if (!q.executeStep()) {
        j["error"] = "build not found";
        return j;
    }
    j = Db::rowToJson(q);
    std::string full = j.value("output", "");
    j["output"] = (offset < full.size()) ? full.substr(offset) : std::string();
    j["next_offset"] = full.size();
    j["dropped_before"] = 0;
    if (offset > full.size()) j["truncated"] = true;
    j["done"] = true;
    return j;
}

} // namespace devhub
