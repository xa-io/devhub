#include "VersionChecker.h"
#include "Db.h"
#include "WinUtil.h"
#include "devhub/Util.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <exception>
#include <cwctype>
#include <thread>
#include <vector>

namespace devhub {

std::string httpGet(const std::string& url) {
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
                if (status == 200) {
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                        std::vector<char> buf(avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(request, buf.data(), avail, &got) || got == 0)
                            break;
                        body.append(buf.data(), got);
                        if (body.size() > 8 * 1024 * 1024) break; // sanity cap
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

void VersionChecker::refreshAsync() {
    bool expected = false;
    if (stopping_) return;
    if (!busy_.compare_exchange_strong(expected, true)) return;
    std::lock_guard<std::mutex> lk(threadMu_);
    if (stopping_) {
        busy_ = false;
        return;
    }
    if (worker_.joinable()) worker_.join();
    try {
        worker_ = std::thread([this]() {
            try {
                worker();
            } catch (const std::exception& e) {
                appLog(std::string("[version] refresh failed: ") + e.what());
            } catch (...) {
                appLog("[version] refresh failed with an unknown exception");
            }
            busy_ = false;
        });
    } catch (const std::exception& e) {
        busy_ = false;
        appLog(std::string("[version] could not start refresh: ") + e.what());
    } catch (...) {
        busy_ = false;
        appLog("[version] could not start refresh: unknown exception");
    }
}

void VersionChecker::stop() {
    stopping_ = true;
    std::thread owned;
    {
        std::lock_guard<std::mutex> lk(threadMu_);
        owned = std::move(worker_);
    }
    if (owned.joinable()) owned.join();
    busy_ = false;
}

void VersionChecker::worker() {
    struct Row {
        long long id;
        std::string localFile, remoteUrl, remoteKey;
    };
    std::vector<Row> rows;
    {
        auto lk = db_->guard();
        SQLite::Statement q(db_->raw(lk.token()),
            "SELECT id, local_version_file, remote_version_url, remote_version_key "
            "FROM projects WHERE archived=0");
        while (!stopping_ && q.executeStep()) {
            rows.push_back({q.getColumn(0).getInt64(),
                            q.getColumn(1).getString(),
                            q.getColumn(2).getString(),
                            q.getColumn(3).getString()});
        }
    }

    // One network fetch per distinct URL per refresh.
    std::map<std::string, std::string> urlCache;
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
            auto it = urlCache.find(r.remoteUrl);
            if (it == urlCache.end())
                it = urlCache.emplace(r.remoteUrl, httpGet(r.remoteUrl)).first;
            if (it->second.empty()) {
                if (info.error.empty()) info.error = "fetch failed: " + r.remoteUrl;
            } else {
                info.remote = extractRemoteVersion(it->second, r.remoteKey);
            }
        }
        info.state = versionState(info.local, info.remote);

        std::lock_guard<std::mutex> lk(mu_);
        results_[r.id] = info;
    }
}

VersionInfo VersionChecker::get(long long projectId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = results_.find(projectId);
    return it != results_.end() ? it->second : VersionInfo{};
}

} // namespace devhub
