#include "VersionChecker.h"
#include "Db.h"
#include "DiscordAccess.h"
#include "DiscordBot.h"
#include "WinUtil.h"
#include "devhub/Util.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <cwctype>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace devhub {

namespace {

using json = nlohmann::json;

std::string stringField(const json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string()
        ? found->get<std::string>() : std::string();
}

bool sameVersion(const std::string& lhs, const std::string& rhs) {
    const auto a = parseVersion(lhs), b = parseVersion(rhs);
    return a.valid && b.valid && compareVersions(a, b) == 0;
}

std::string githubRepository(const std::string& url, std::string* tag = nullptr) {
    constexpr const char* prefix = "https://github.com/";
    if (url.rfind(prefix, 0) != 0 || url.find_first_of("\\\r\n\t?#@") !=
            std::string::npos)
        return {};
    const std::string path = url.substr(std::char_traits<char>::length(prefix));
    const size_t ownerEnd = path.find('/');
    if (ownerEnd == std::string::npos || ownerEnd == 0) return {};
    const size_t repoEnd = path.find('/', ownerEnd + 1);
    std::string repository = path.substr(0, repoEnd);
    if (repository.size() >= 4 &&
        repository.compare(repository.size() - 4, 4, ".git") == 0)
        repository.resize(repository.size() - 4);
    if (repository.size() <= ownerEnd + 1 ||
        !std::all_of(repository.begin(), repository.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                   ch == '.' || ch == '/';
        }))
        return {};
    if (tag) {
        if (repoEnd == std::string::npos) return {};
        constexpr const char* releasePrefix = "/releases/download/";
        const std::string rest = path.substr(repoEnd);
        if (rest.rfind(releasePrefix, 0) != 0) return {};
        const size_t tagBegin = std::char_traits<char>::length(releasePrefix);
        const size_t tagEnd = rest.find('/', tagBegin);
        if (tagEnd == std::string::npos || tagEnd + 1 == rest.size()) return {};
        *tag = rest.substr(tagBegin, tagEnd - tagBegin);
        if (!parseVersion(*tag).valid) return {};
    } else if (repoEnd != std::string::npos && repoEnd + 1 != path.size()) {
        return {};
    }
    std::transform(repository.begin(), repository.end(), repository.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return repository;
}

std::string changelogSection(const std::string& changelog,
                             const std::string& version) {
    std::istringstream input(changelog);
    std::string line, notes;
    bool selected = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const bool markdownHeading = line.rfind("## ", 0) == 0;
        std::string heading = trim(markdownHeading ? line.substr(3) : line);
        bool dated = heading.size() > 13 && heading.compare(10, 3, " - ") == 0;
        for (size_t i = 0; dated && i < 10; ++i) {
            const bool separator = i == 4 || i == 7;
            dated = separator ? heading[i] == '-'
                : heading[i] >= '0' && heading[i] <= '9';
        }
        if (dated) heading = trim(heading.substr(13));
        if (heading.rfind("Version ", 0) == 0 || heading.rfind("version ", 0) == 0)
            heading = trim(heading.substr(8));
        if (!heading.empty() && heading.front() == '[') {
            const size_t end = heading.find(']');
            heading = end == std::string::npos ? std::string()
                : heading.substr(1, end - 1);
        } else {
            heading = heading.substr(0, heading.find_first_of(" \t:"));
        }
        const bool versionHeading = parseVersion(heading).valid;
        if (markdownHeading || versionHeading) {
            if (selected) break;
            selected = sameVersion(heading, version);
        } else if (selected) {
            if (!notes.empty()) notes += '\n';
            notes += line;
        }
    }
    return trim(notes);
}

std::string releaseStateKey(const PublicPluginRelease& release) {
    return "release_announcement_state/" + release.repository + "/" + release.plugin;
}

bool validReleaseState(const json& state) {
    if (!state.is_object()) return false;
    for (const char* field : {"baseline", "observed", "attempted", "announced",
                              "outcome", "message_id"}) {
        if (!state.contains(field) || !state[field].is_string()) return false;
    }
    for (const char* field : {"baseline", "observed", "attempted", "announced"}) {
        const std::string version = state[field].get<std::string>();
        if ((!version.empty() || std::string(field) == "baseline" ||
             std::string(field) == "observed") && !parseVersion(version).valid)
            return false;
    }
    const std::string outcome = state["outcome"].get<std::string>();
    const std::string baseline = state["baseline"].get<std::string>();
    const std::string observed = state["observed"].get<std::string>();
    const std::string attempted = state["attempted"].get<std::string>();
    const std::string announced = state["announced"].get<std::string>();
    const std::string messageId = state["message_id"].get<std::string>();
    if (compareVersions(parseVersion(observed), parseVersion(baseline)) < 0 ||
        (!attempted.empty() &&
         (compareVersions(parseVersion(attempted), parseVersion(baseline)) <= 0 ||
          compareVersions(parseVersion(attempted), parseVersion(observed)) > 0)) ||
        (!announced.empty() &&
         (attempted.empty() ||
          compareVersions(parseVersion(announced), parseVersion(baseline)) <= 0 ||
          compareVersions(parseVersion(announced), parseVersion(attempted)) > 0)))
        return false;
    if (outcome == "baseline")
        return attempted.empty() && announced.empty() && messageId.empty();
    if (attempted.empty()) return false;
    if (outcome == "posted") {
        std::uint64_t parsedId = 0;
        return sameVersion(announced, attempted) &&
            parseDiscordAccessSnowflake(messageId, parsedId);
    }
    return (outcome == "not_sent" || outcome == "unknown") && messageId.empty();
}

bool releaseEligible(const json& state, const std::string& version) {
    const auto current = parseVersion(version);
    const std::string attempted = state["attempted"].get<std::string>();
    return compareVersions(current, parseVersion(state["observed"].get<std::string>())) >= 0 &&
        compareVersions(current, parseVersion(state["baseline"].get<std::string>())) > 0 &&
        (attempted.empty() || compareVersions(current, parseVersion(attempted)) > 0 ||
         (sameVersion(version, attempted) && state["outcome"] == "not_sent"));
}

} // namespace

bool selectPublicPluginRelease(const std::string& manifest,
                               const std::string& remoteKey,
                               const std::string& projectName,
                               const std::string& projectGithub,
                               PublicPluginRelease& release) {
    release = {};
    const json document = json::parse(manifest, nullptr, false);
    const json* selected = nullptr;
    if (document.is_array()) {
        if (remoteKey.empty()) return false;
        for (const auto& entry : document) {
            if (!entry.is_object()) continue;
            if (stringField(entry, "InternalName") == remoteKey ||
                stringField(entry, "Name") == remoteKey) {
                if (selected) return false;
                selected = &entry;
            }
        }
    } else if (document.is_object()) {
        if (!remoteKey.empty() && stringField(document, "InternalName") != remoteKey &&
            stringField(document, "Name") != remoteKey)
            return false;
        selected = &document;
    }
    if (!selected) return false;
    for (const char* field : {"IsTestingExclusive", "TestingExclusive"}) {
        if (selected->contains(field) &&
            (!(*selected)[field].is_boolean() || (*selected)[field].get<bool>()))
            return false;
    }
    release.version = trim(stringField(*selected, "AssemblyVersion"));
    if (!parseVersion(release.version).valid) return false;
    release.plugin = trim(stringField(*selected, "InternalName"));
    if (release.plugin.empty()) release.plugin = remoteKey;
    if (release.plugin.empty() || release.plugin.size() > 128) return false;
    release.product = trim(stringField(*selected, "Name"));
    if (release.product.empty()) release.product = projectName;
    const std::string download = stringField(*selected, "DownloadLinkUpdate");
    release.repository = githubRepository(download, &release.tag);
    if (!release.repository.empty()) {
        if (!sameVersion(release.tag, release.version)) return false;
    } else {
        // A GitHub download that fails validation must never select another repo.
        if (download.rfind("https://github.com/", 0) == 0) return false;
        release.tag.clear();
        release.repository = githubRepository(projectGithub);
    }
    return !release.repository.empty();
}

bool qualifyPublicPluginRelease(PublicPluginRelease& release,
                                const std::string& repositoryPayload,
                                const std::string& releasePayload,
                                const std::string& changelog,
                                bool canonicalChangelogExists) {
    release.url.clear();
    release.notes.clear();
    const json repository = json::parse(repositoryPayload, nullptr, false);
    if (!repository.is_object() || !repository.contains("private") ||
        !repository["private"].is_boolean() || repository["private"].get<bool>() ||
        githubRepository(stringField(repository, "html_url")) != release.repository)
        return false;
    const json document = json::parse(releasePayload, nullptr, false);
    const json* selected = nullptr;
    const auto matches = [&](const json& entry) {
        return entry.is_object() && entry.contains("draft") &&
            entry["draft"].is_boolean() && !entry["draft"].get<bool>() &&
            entry.contains("prerelease") && entry["prerelease"].is_boolean() &&
            !entry["prerelease"].get<bool>() &&
            !stringField(entry, "published_at").empty() &&
            sameVersion(stringField(entry, "tag_name"), release.version);
    };
    if (document.is_array()) {
        for (const auto& entry : document) {
            if (!matches(entry)) continue;
            if (selected) return false;
            selected = &entry;
        }
    } else if (matches(document)) {
        selected = &document;
    }
    if (!selected) return false;
    const std::string tag = stringField(*selected, "tag_name");
    if (!release.tag.empty() && release.tag != tag) return false;
    const std::string expectedUrl = "https://github.com/" + release.repository +
                                    "/releases/tag/" + tag;
    release.url = stringField(*selected, "html_url");
    std::string normalizedUrl = release.url;
    std::transform(normalizedUrl.begin(), normalizedUrl.end(), normalizedUrl.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string normalizedExpected = expectedUrl;
    std::transform(normalizedExpected.begin(), normalizedExpected.end(),
                   normalizedExpected.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalizedUrl != normalizedExpected) {
        release.url.clear();
        return false;
    }
    release.tag = tag;
    release.notes = canonicalChangelogExists || !changelog.empty()
        ? changelogSection(changelog, release.version)
        : trim(stringField(*selected, "body"));
    return true;
}

bool shouldCheckPublicPluginRelease(Db* db, const PublicPluginRelease& release) {
    if (release.repository.empty() || release.plugin.empty() ||
        !parseVersion(release.version).valid)
        return false;
    auto guard = db->guard();
    const std::string stored = db->getSetting(guard.token(), releaseStateKey(release));
    if (stored.empty()) return true;
    const json state = json::parse(stored, nullptr, false);
    return validReleaseState(state) && releaseEligible(state, release.version);
}

bool beginPublicReleasePoll(std::chrono::steady_clock::time_point now,
                            std::chrono::steady_clock::time_point& next) {
    if (now < next) return false;
    next = now + std::chrono::minutes(30);
    return true;
}

bool claimPublicPluginRelease(Db* db, const PublicPluginRelease& release,
                              bool readyToPost) {
    if (release.repository.empty() || release.plugin.empty() ||
        !parseVersion(release.version).valid)
        return false;
    const std::string key = releaseStateKey(release);
    auto guard = db->guard();
    const std::string stored = db->getSetting(guard.token(), key);
    if (stored.empty()) {
        db->setSetting(guard.token(), key, json{
            {"baseline", release.version}, {"observed", release.version},
            {"attempted", ""}, {"announced", ""},
            {"outcome", "baseline"}, {"message_id", ""}
        }.dump());
        return false;
    }
    json state = json::parse(stored, nullptr, false);
    if (!validReleaseState(state)) {
        appLog("[release] invalid saved announcement state; preserved without sending");
        return false;
    }
    const auto current = parseVersion(release.version);
    const std::string observed = state["observed"].get<std::string>();
    if (compareVersions(current, parseVersion(observed)) < 0) return false;
    const bool shouldPost = readyToPost && releaseEligible(state, release.version);
    state["observed"] = release.version;
    if (shouldPost) {
        state["attempted"] = release.version;
        state["outcome"] = "unknown";
        state["message_id"] = "";
    }
    if (state.dump() != stored) db->setSetting(guard.token(), key, state.dump());
    return shouldPost;
}

void recordPublicPluginReleaseReceipt(Db* db,
                                     const PublicPluginRelease& release,
                                     const std::string& outcome,
                                     const std::string& messageId) {
    if (outcome != "posted" && outcome != "not_sent" && outcome != "unknown") return;
    auto guard = db->guard();
    const std::string key = releaseStateKey(release);
    json state = json::parse(db->getSetting(guard.token(), key), nullptr, false);
    if (!validReleaseState(state) ||
        !sameVersion(state["attempted"].get<std::string>(), release.version)) return;
    std::uint64_t parsedId = 0;
    const bool posted = outcome == "posted" &&
        parseDiscordAccessSnowflake(messageId, parsedId);
    state["outcome"] = outcome == "posted" && !posted ? "unknown" : outcome;
    state["message_id"] = posted ? messageId : "";
    if (posted) state["announced"] = release.version;
    db->setSetting(guard.token(), key, state.dump());
}

std::string httpGet(const std::string& url, unsigned long* responseStatus) {
    if (responseStatus) *responseStatus = 0;
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};
    if (uc.nScheme != INTERNET_SCHEME_HTTP &&
        uc.nScheme != INTERNET_SCHEME_HTTPS)
        return {};
    std::wstring normalizedHost(host, uc.dwHostNameLength);
    std::transform(normalizedHost.begin(), normalizedHost.end(),
                   normalizedHost.begin(), [](wchar_t ch) {
                       return static_cast<wchar_t>(std::towlower(ch));
                   });
    const bool loopback = normalizedHost == L"127.0.0.1" ||
                          normalizedHost == L"localhost" ||
                          normalizedHost == L"::1";
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && !loopback) return {};
    std::wstring target(path, uc.dwUrlPathLength);
    target.append(extra, uc.dwExtraInfoLength);
    if (const size_t fragment = target.find(L'#');
        fragment != std::wstring::npos)
        target.erase(fragment);

    std::string body;
    HINTERNET session = WinHttpOpen(L"XADevHub/2.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (connect) {
        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (request) {
            DWORD timeout = 10000;
            WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (responseStatus) *responseStatus = status;
                if (status == 200) {
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(request, &avail) ||
                            avail > 8 * 1024 * 1024 - body.size()) {
                            body.clear();
                            if (responseStatus) *responseStatus = 0;
                            break;
                        }
                        if (avail == 0) break;
                        std::vector<char> buf(avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(request, buf.data(), avail, &got) || got == 0) {
                            body.clear();
                            if (responseStatus) *responseStatus = 0;
                            break;
                        }
                        body.append(buf.data(), got);
                    }
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return body;
}

void VersionChecker::startAnnouncements(DiscordBot* bot) {
    {
        std::lock_guard<std::mutex> lock(threadMu_);
        if (stopping_ || announcements_) return;
        bot_ = bot;
        announcements_ = true;
    }
    refreshAsync();
}

void VersionChecker::refreshAsync() {
    std::lock_guard<std::mutex> lock(threadMu_);
    if (stopping_) return;
    requested_ = true;
    busy_ = true;
    if (!worker_.joinable()) {
        try {
            worker_ = std::thread([this]() { run(); });
        } catch (const std::exception& e) {
            requested_ = false;
            busy_ = false;
            appLog(std::string("[version] could not start refresh: ") + e.what());
        }
    }
    wake_.notify_one();
}

void VersionChecker::run() {
    std::unique_lock<std::mutex> lock(threadMu_);
    auto next = std::chrono::steady_clock::now();
    while (!stopping_) {
        if (!requested_) {
            if (announcements_)
                wake_.wait_until(lock, next, [this]() { return stopping_ || requested_; });
            else
                wake_.wait(lock, [this]() { return stopping_ || requested_; });
        }
        if (stopping_) break;
        requested_ = false;
        busy_ = true;
        const bool announcementPass = announcements_ &&
            beginPublicReleasePoll(std::chrono::steady_clock::now(), next);
        lock.unlock();
        try {
            worker(announcementPass);
        } catch (const std::exception& e) {
            appLog(std::string("[version] refresh failed: ") + e.what());
        } catch (...) {
            appLog("[version] refresh failed with an unknown exception");
        }
        lock.lock();
        busy_ = requested_;
    }
    busy_ = false;
}

void VersionChecker::stop() {
    stopping_ = true;
    std::thread owned;
    {
        std::lock_guard<std::mutex> lk(threadMu_);
        owned = std::move(worker_);
    }
    wake_.notify_all();
    if (owned.joinable()) owned.join();
    busy_ = false;
}

void VersionChecker::worker(bool announcementPass) {
    struct Row {
        long long id;
        std::string localFile, remoteUrl, remoteKey, name, github;
    };
    std::vector<Row> rows;
    {
        auto lk = db_->guard();
        SQLite::Statement q(db_->raw(lk.token()),
            "SELECT id, local_version_file, remote_version_url, remote_version_key, name, github_url "
            "FROM projects WHERE archived=0");
        while (!stopping_ && q.executeStep()) {
            rows.push_back({q.getColumn(0).getInt64(),
                            q.getColumn(1).getString(),
                            q.getColumn(2).getString(),
                            q.getColumn(3).getString(),
                            q.getColumn(4).getString(),
                            q.getColumn(5).getString()});
        }
    }

    // One network fetch per distinct URL per refresh.
    std::map<std::string, std::pair<std::string, unsigned long>> urlCache;
    const auto fetch = [&](const std::string& url) -> const auto& {
        auto found = urlCache.find(url);
        if (found == urlCache.end()) {
            unsigned long status = 0;
            std::string body = stopping_ ? std::string() : httpGet(url, &status);
            found = urlCache.emplace(url, std::make_pair(std::move(body), status)).first;
        }
        return found->second;
    };
    bool monitor = false;
    {
        std::lock_guard<std::mutex> lock(threadMu_);
        monitor = announcementPass && bot_ != nullptr;
    }
    std::set<std::string> checkedPlugins;
    for (const auto& r : rows) {
        if (stopping_) break;
        VersionInfo info;
        info.checked = true;

        if (!r.localFile.empty()) {
            bool readable = false;
            std::string content = readFileUtf8(r.localFile, &readable);
            if (!readable) info.error = "cannot read " + r.localFile;
            else if (content.empty())
                info.error = "empty version file: " + r.localFile;
            else info.local = extractLocalVersion(content, r.localFile);
        }
        if (!r.remoteUrl.empty()) {
            const auto& payload = fetch(r.remoteUrl);
            if (payload.first.empty()) {
                if (info.error.empty()) info.error = "fetch failed: " + r.remoteUrl;
            } else {
                info.remote = extractRemoteVersion(payload.first, r.remoteKey);
            }
        }
        info.state = versionState(info.local, info.remote);

        {
            std::lock_guard<std::mutex> lk(mu_);
            results_[r.id] = info;
        }
        if (!monitor || stopping_ || r.remoteUrl.empty()) continue;
        try {
            PublicPluginRelease release;
            if (!selectPublicPluginRelease(fetch(r.remoteUrl).first, r.remoteKey,
                                            r.name, r.github, release)) continue;
            if (!shouldCheckPublicPluginRelease(db_, release) ||
                !checkedPlugins.insert(releaseStateKey(release)).second) continue;
            const std::string api = "https://api.github.com/repos/" + release.repository;
            const auto& repository = fetch(api);
            const std::string releaseRoute = release.tag.empty()
                ? "/releases?per_page=100" : "/releases/tags/" + release.tag;
            const auto& published = fetch(api + releaseRoute);
            if (!qualifyPublicPluginRelease(release, repository.first,
                                             published.first, "")) continue;
            // Read the current public default-branch changelog. Release tags
            // may precede the canonical notes commit made during publication.
            const json metadata = json::parse(repository.first, nullptr, false);
            const std::string branch = stringField(metadata, "default_branch");
            if (branch.empty() || branch.find_first_of("\\?#\r\n\t ") !=
                    std::string::npos) continue;
            const std::string raw = "https://raw.githubusercontent.com/" +
                                     release.repository + "/" + branch + "/";
            const auto* canonical = &fetch(raw + "CHANGELOG.md");
            if (canonical->second == 404) canonical = &fetch(raw + "changelog.txt");
            if (canonical->second != 200 && canonical->second != 404) continue;
            if (!qualifyPublicPluginRelease(release, repository.first,
                                             published.first, canonical->first,
                                             canonical->second == 200)) continue;
            observeRelease(release);
        } catch (const std::exception& e) {
            appLog("[release] " + r.name + ": " + e.what());
        }
    }
}

void VersionChecker::observeRelease(const PublicPluginRelease& release) {
    std::string channelId;
    {
        auto guard = db_->guard();
        channelId = trim(db_->getSetting(guard.token(), "release_announcement_channel_id"));
    }
    PublicReleaseAnnouncement announcement{
        "public", channelId, release.product, release.version, release.url, release.notes
    };
    NotifyCard card;
    std::string error;
    const bool ready = !stopping_ && bot_ && bot_->status().state == "connected" &&
        preparePublicReleaseCard(announcement, card, error);
    if (!claimPublicPluginRelease(db_, release, ready)) return;
    if (stopping_) {
        recordPublicPluginReleaseReceipt(db_, release, "not_sent", "");
        return;
    }
    struct Receipt {
        std::mutex mutex;
        std::condition_variable ready;
        bool complete = false;
        std::string outcome = "unknown", messageId;
    };
    auto receipt = std::make_shared<Receipt>();
    const auto finish = [receipt](const std::string& outcome,
                                  const std::string& messageId) {
        {
            std::lock_guard<std::mutex> lock(receipt->mutex);
            if (receipt->complete) return;
            receipt->outcome = outcome;
            receipt->messageId = messageId;
            receipt->complete = true;
        }
        receipt->ready.notify_one();
    };
    try {
        bot_->postCard(channelId, card,
            [finish](const std::string& id) {
                std::uint64_t parsedId = 0;
                if (parseDiscordAccessSnowflake(id, parsedId)) finish("posted", id);
                else finish("unknown", "");
            }, nullptr,
            [finish](const std::string&, CardPostFailure outcome) {
                finish(outcome == CardPostFailure::NotSent ? "not_sent" : "unknown", "");
            });
    } catch (...) {
        appLog("[release] submission failed after claim; outcome remains unknown");
        return;
    }
    std::unique_lock<std::mutex> lock(receipt->mutex);
    receipt->ready.wait_for(lock, std::chrono::seconds(10),
                            [&receipt]() { return receipt->complete; });
    recordPublicPluginReleaseReceipt(db_, release, receipt->outcome, receipt->messageId);
    appLog("[release] " + release.product + " " + release.version + " " + receipt->outcome);
}

VersionInfo VersionChecker::get(long long projectId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = results_.find(projectId);
    return it != results_.end() ? it->second : VersionInfo{};
}

} // namespace devhub
