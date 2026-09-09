#include "devhub/Util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>

namespace devhub {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    lines.push_back(cur);
    return lines;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool containsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static size_t findAsciiInsensitive(std::string_view haystack,
                                   std::string_view needle,
                                   size_t from = 0) {
    if (needle.empty()) return std::min(from, haystack.size());
    if (from > haystack.size() || needle.size() > haystack.size() - from)
        return std::string::npos;
    const auto lower = [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z'
            ? static_cast<unsigned char>(ch + ('a' - 'A')) : ch;
    };
    for (size_t at = from; at + needle.size() <= haystack.size(); ++at) {
        size_t i = 0;
        while (i < needle.size() &&
               lower(static_cast<unsigned char>(haystack[at + i])) ==
                   lower(static_cast<unsigned char>(needle[i])))
            ++i;
        if (i == needle.size()) return at;
    }
    return std::string::npos;
}

std::string redactHttpUrls(std::string s) {
    const bool methodShaped =
        findAsciiInsensitive(s, "GET ") != std::string::npos ||
        findAsciiInsensitive(s, "POST ") != std::string::npos ||
        findAsciiInsensitive(s, "PUT ") != std::string::npos ||
        findAsciiInsensitive(s, "PATCH ") != std::string::npos ||
        findAsciiInsensitive(s, "DELETE ") != std::string::npos;
    if (s.find("://") == std::string::npos &&
        s.find('/') == std::string::npos && !methodShaped &&
        findAsciiInsensitive(s, "discordapp") == std::string::npos)
        return s;
    constexpr char replacement[] = "[redacted URL]";
    constexpr size_t replacementLength = sizeof(replacement) - 1;
    const auto isEndpointEnd = [](unsigned char ch) {
        return ch <= ' ' || ch == '"' || ch == '\'' || ch == '<' || ch == '>' ||
               ch == '(' || ch == ')' || ch == '[' || ch == ']' ||
               ch == '{' || ch == '}';
    };
    const auto replaceEndpointAt = [&](size_t begin) {
        size_t end = begin;
        while (end < s.size() &&
               !isEndpointEnd(static_cast<unsigned char>(s[end])))
            ++end;
        if (end == begin) return begin + 1;
        s.replace(begin, end - begin, replacement);
        return begin + replacementLength;
    };

    // Conventional URLs.
    for (size_t searchFrom = 0; searchFrom < s.size();) {
        const size_t http = findAsciiInsensitive(s, "http://", searchFrom);
        const size_t https = findAsciiInsensitive(s, "https://", searchFrom);
        size_t begin = std::min(http, https);
        if (http == std::string::npos) begin = https;
        if (https == std::string::npos) begin = http;
        if (begin == std::string::npos) break;
        searchFrom = replaceEndpointAt(begin);
    }

    // D++ transport errors render raw requests as, for example,
    // "GET cdn.discordapp.com:443/attachments/...?hm=..." with no scheme.
    static constexpr std::string_view kMethods[] = {
        "get ", "post ", "put ", "patch ", "delete ", "head ", "options "
    };
    for (const std::string_view method : kMethods) {
        for (size_t searchFrom = 0; searchFrom < s.size();) {
            const size_t methodAt =
                findAsciiInsensitive(s, method, searchFrom);
            if (methodAt == std::string::npos) break;
            if (methodAt > 0 &&
                static_cast<unsigned char>(s[methodAt - 1]) > ' ') {
                searchFrom = methodAt + method.size();
                continue;
            }
            const size_t endpointAt = methodAt + method.size();
            size_t endpointEnd = endpointAt;
            while (endpointEnd < s.size() &&
                   !isEndpointEnd(static_cast<unsigned char>(s[endpointEnd])))
                ++endpointEnd;
            const auto containsBeforeEnd = [&](char needle) {
                const size_t found = s.find(needle, endpointAt);
                return found != std::string::npos && found < endpointEnd;
            };
            if (!containsBeforeEnd('/') && !containsBeforeEnd(':') &&
                !containsBeforeEnd('?') && !containsBeforeEnd('.')) {
                searchFrom = endpointEnd;
                continue;
            }
            searchFrom = replaceEndpointAt(endpointAt);
        }
    }

    // Some D++ and persisted legacy diagnostics omit both the scheme and HTTP
    // method. Redact only the two known Discord CDN hosts, and only when they
    // begin a token and are followed by an endpoint delimiter, so ordinary
    // prose containing a similar substring is not consumed.
    static constexpr std::string_view kHosts[] = {
        "cdn.discordapp.com", "media.discordapp.net"
    };
    for (const std::string_view host : kHosts) {
        for (size_t searchFrom = 0; searchFrom < s.size();) {
            const size_t hostAt =
                findAsciiInsensitive(s, host, searchFrom);
            if (hostAt == std::string::npos) break;
            const bool tokenStart = hostAt == 0 ||
                (!std::isalnum(static_cast<unsigned char>(s[hostAt - 1])) &&
                 s[hostAt - 1] != '.' && s[hostAt - 1] != '-' &&
                 s[hostAt - 1] != '_');
            const size_t suffixAt = hostAt + host.size();
            const bool endpointSuffix = suffixAt < s.size() &&
                (s[suffixAt] == '/' || s[suffixAt] == ':' ||
                 s[suffixAt] == '?');
            if (!tokenStart || !endpointSuffix) {
                searchFrom = suffixAt;
                continue;
            }
            searchFrom = replaceEndpointAt(hostAt);
        }
    }

    // D++ rate-limit messages can contain only the endpoint path. Keep the
    // catch narrow to Discord attachment routes so ordinary prose is intact.
    static constexpr std::string_view kPaths[] = {
        "/attachments/", "/ephemeral-attachments/"
    };
    for (const std::string_view path : kPaths) {
        for (size_t searchFrom = 0; searchFrom < s.size();) {
            const size_t pathAt =
                findAsciiInsensitive(s, path, searchFrom);
            if (pathAt == std::string::npos) break;
            searchFrom = replaceEndpointAt(pathAt);
        }
    }
    return s;
}

std::string escapeDiscordMarkdown(const std::string& value) {
    static constexpr std::string_view kEscaped = R"(\[]()`*_~|)";
    std::string out;
    out.reserve(value.size() + 16);
    for (const char ch : value) {
        if (kEscaped.find(ch) != std::string_view::npos)
            out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

std::string slugify(const std::string& s) {
    std::string out;
    bool lastDash = true; // suppress leading dash
    for (unsigned char c : s) {
        if (std::isalnum(c)) { out.push_back((char)std::tolower(c)); lastDash = false; }
        else if (!lastDash) { out.push_back('-'); lastDash = true; }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string makeTitle(const std::string& body, size_t maxLen) {
    // First non-empty line.
    std::string line;
    for (auto& l : splitLines(body)) {
        line = trim(l);
        if (!line.empty()) break;
    }
    if (line.size() <= maxLen) return line;
    size_t cut = line.rfind(' ', maxLen);
    if (cut == std::string::npos || cut < maxLen / 2) cut = maxLen;
    return trim(line.substr(0, cut)) + "...";
}

std::string nowIsoUtc() {
    auto t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string nextIsoUtcAfter(const std::string& floor) {
    const std::string now = nowIsoUtc();
    if (floor.empty() || now > floor) return now;

    std::tm parsed{};
    std::istringstream input(floor);
    input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%SZ");
    if (input.fail()) return now;
    const __time64_t floorEpoch = _mkgmtime64(&parsed);
    if (floorEpoch < 0) return now;

    const __time64_t nextEpoch = floorEpoch + 1;
    std::tm next{};
    if (gmtime_s(&next, &nextEpoch) != 0) return now;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &next);
    return buf;
}

std::string todayLocal() {
    auto t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

bool isUtf8Continuation(unsigned char byte) {
    return byte >= 0x80 && byte <= 0xBF;
}

size_t validUtf8SequenceLength(const std::string& text, size_t offset) {
    const auto byte = [&](size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const size_t remaining = text.size() - offset;
    const unsigned char first = byte(offset);
    if (first <= 0x7F) return 1;
    if (first >= 0xC2 && first <= 0xDF)
        return remaining >= 2 && isUtf8Continuation(byte(offset + 1)) ? 2 : 0;
    if (first == 0xE0)
        return remaining >= 3 && byte(offset + 1) >= 0xA0 &&
                       byte(offset + 1) <= 0xBF &&
                       isUtf8Continuation(byte(offset + 2))
                   ? 3 : 0;
    if ((first >= 0xE1 && first <= 0xEC) ||
        (first >= 0xEE && first <= 0xEF))
        return remaining >= 3 && isUtf8Continuation(byte(offset + 1)) &&
                       isUtf8Continuation(byte(offset + 2))
                   ? 3 : 0;
    if (first == 0xED)
        return remaining >= 3 && byte(offset + 1) >= 0x80 &&
                       byte(offset + 1) <= 0x9F &&
                       isUtf8Continuation(byte(offset + 2))
                   ? 3 : 0;
    if (first == 0xF0)
        return remaining >= 4 && byte(offset + 1) >= 0x90 &&
                       byte(offset + 1) <= 0xBF &&
                       isUtf8Continuation(byte(offset + 2)) &&
                       isUtf8Continuation(byte(offset + 3))
                   ? 4 : 0;
    if (first >= 0xF1 && first <= 0xF3)
        return remaining >= 4 && isUtf8Continuation(byte(offset + 1)) &&
                       isUtf8Continuation(byte(offset + 2)) &&
                       isUtf8Continuation(byte(offset + 3))
                   ? 4 : 0;
    if (first == 0xF4)
        return remaining >= 4 && byte(offset + 1) >= 0x80 &&
                       byte(offset + 1) <= 0x8F &&
                       isUtf8Continuation(byte(offset + 2)) &&
                       isUtf8Continuation(byte(offset + 3))
                   ? 4 : 0;
    return 0;
}

char32_t windows1252CodePoint(unsigned char byte) {
    static constexpr char32_t c1Table[] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    if (byte >= 0x80 && byte <= 0x9F) return c1Table[byte - 0x80];
    return byte;
}

void appendUtf8(std::string& output, char32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

} // namespace

std::string normalizeUtf8(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (size_t offset = 0; offset < text.size();) {
        const size_t sequenceLength = validUtf8SequenceLength(text, offset);
        if (sequenceLength != 0) {
            output.append(text, offset, sequenceLength);
            offset += sequenceLength;
            continue;
        }
        appendUtf8(output, windows1252CodePoint(
                               static_cast<unsigned char>(text[offset])));
        ++offset;
    }
    return output;
}

std::string logSafe(std::string value, size_t cap) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) ch = ' ';
    }
    if (value.size() <= cap) return value;
    if (cap <= 3) return value.substr(0, cap);
    value.resize(cap - 3);
    value += "...";
    return value;
}

size_t utf8SafeSplitPoint(const std::string& text) {
    const size_t end = text.size();
    if (end == 0) return 0;
    auto continuation = [&](size_t index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        return (byte & 0xc0) == 0x80;
    };
    auto sequenceLength = [&](size_t index) -> size_t {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead >= 0xc2 && lead <= 0xdf) return 2;
        if (lead >= 0xe0 && lead <= 0xef) return 3;
        if (lead >= 0xf0 && lead <= 0xf4) return 4;
        return 0;
    };

    size_t suffix = end;
    size_t continuationCount = 0;
    while (suffix > 0 && continuationCount < 3 &&
           continuation(suffix - 1)) {
        --suffix;
        ++continuationCount;
    }
    if (continuationCount == 0) {
        const size_t needed = sequenceLength(end - 1);
        return needed > 1 ? end - 1 : end;
    }
    if (suffix == 0) return 0;
    const size_t lead = suffix - 1;
    const size_t needed = sequenceLength(lead);
    if (needed == 0) return end;
    return end - lead < needed ? lead : end;
}

std::string readFileUtf8(const std::string& path, bool* ok) {
    if (ok) *ok = false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad() || ss.bad()) return {};
    std::string s = ss.str();
    // Strip UTF-8 BOM if present.
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    if (ok) *ok = true;
    return s;
}

bool writeFileUtf8(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), (std::streamsize)content.size());
    return f.good();
}

// ---------------------------------------------------------------------------
// app debug log ring
// ---------------------------------------------------------------------------
namespace {
std::mutex g_logMu;
std::deque<std::string> g_logLines;
std::atomic<uint64_t> g_logRev{0};
constexpr size_t kLogCap = 400;
} // namespace

void appLog(const std::string& line) {
    auto t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    std::lock_guard<std::mutex> lk(g_logMu);
    g_logLines.push_back(std::string(stamp) + "  " + logSafe(line, 4096));
    while (g_logLines.size() > kLogCap) g_logLines.pop_front();
    ++g_logRev;
}

void appLogClear() {
    std::lock_guard<std::mutex> lk(g_logMu);
    g_logLines.clear();
    ++g_logRev;
}

uint64_t appLogRevision() { return g_logRev; }

std::vector<std::string> appLogSnapshot() {
    std::lock_guard<std::mutex> lk(g_logMu);
    return std::vector<std::string>(g_logLines.begin(), g_logLines.end());
}

} // namespace devhub
