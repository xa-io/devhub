#include "devhub/VersionUtil.h"
#include "devhub/Util.h"

#include <nlohmann/json.hpp>

#include <cctype>

using json = nlohmann::json;

namespace devhub {

ParsedVersion parseVersion(const std::string& s) {
    ParsedVersion v;
    std::string t = trim(s);
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t.erase(0, 1);
    if (t.empty()) return v;

    int cur = 0;
    bool inNum = false;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i < t.size() && std::isdigit((unsigned char)t[i])) {
            cur = cur * 10 + (t[i] - '0');
            inNum = true;
            if (cur > 1000000) return {}; // nonsense guard
        } else if (i == t.size() || t[i] == '.') {
            if (!inNum) return {};
            v.parts.push_back(cur);
            cur = 0;
            inNum = false;
        } else {
            return {}; // stray character => not a version
        }
    }
    v.valid = v.parts.size() >= 2 && v.parts.size() <= 4;
    if (!v.valid) v.parts.clear();
    return v;
}

int compareVersions(const ParsedVersion& a, const ParsedVersion& b) {
    size_t n = (std::max)(a.parts.size(), b.parts.size());
    for (size_t i = 0; i < n; ++i) {
        int av = i < a.parts.size() ? a.parts[i] : 0;
        int bv = i < b.parts.size() ? b.parts[i] : 0;
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}

static bool versionTokenAt(const std::string& text, size_t start,
                           size_t& end) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (start >= text.size() || !digit(text[start])) return false;

    size_t cursor = start;
    size_t groups = 0;
    while (cursor < text.size()) {
        const size_t groupStart = cursor;
        while (cursor < text.size() && digit(text[cursor])) ++cursor;
        if (cursor == groupStart) break;
        ++groups;
        if (groups == 4 || cursor >= text.size() || text[cursor] != '.' ||
            cursor + 1 >= text.size() || !digit(text[cursor + 1]))
            break;
        ++cursor;
    }
    if (groups < 2) return false;
    end = cursor;
    return true;
}

// Linear scan for x.y[.z[.w]]. It performs no allocation until a match and
// avoids MSVC's recursive std::regex engine on project-selected whole files.
static std::string firstVersionToken(const std::string& text) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 0; i < text.size(); ++i) {
        if (!digit(text[i])) continue;
        if (i > 0 && (digit(text[i - 1]) || text[i - 1] == '.')) continue;
        size_t end = i;
        if (versionTokenAt(text, i, end)) return text.substr(i, end - i);
        while (i + 1 < text.size() && digit(text[i + 1])) ++i;
    }
    return {};
}

static bool asciiIcaseAt(const std::string& text, size_t at,
                         const char* needle) {
    for (size_t i = 0; needle[i] != '\0'; ++i) {
        if (at + i >= text.size()) return false;
        const unsigned char lhs = static_cast<unsigned char>(text[at + i]);
        const unsigned char rhs = static_cast<unsigned char>(needle[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) return false;
    }
    return true;
}

static std::string firstVPrefixedVersion(const std::string& text) {
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] != 'v' && text[i] != 'V') continue;
        size_t end = i + 1;
        if (versionTokenAt(text, i + 1, end))
            return text.substr(i + 1, end - (i + 1));
    }
    return {};
}

static std::string xmlVersion(const std::string& text) {
    static constexpr char kOpen[] = "<Version>";
    static constexpr char kClose[] = "</Version>";
    const size_t open = text.find(kOpen);
    if (open == std::string::npos) return {};
    const size_t valueStart = open + sizeof(kOpen) - 1;
    const size_t close = text.find(kClose, valueStart);
    if (close == std::string::npos) return {};
    return firstVersionToken(text.substr(valueStart, close - valueStart));
}

static std::string cmakeProjectVersion(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!asciiIcaseAt(text, i, "project")) continue;
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) ||
                      text[i - 1] == '_'))
            continue;
        size_t cursor = i + 7;
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])))
            ++cursor;
        if (cursor >= text.size() || text[cursor] != '(') continue;
        const size_t close = text.find(')', ++cursor);
        if (close == std::string::npos) return {};
        for (; cursor < close; ++cursor) {
            if (!asciiIcaseAt(text, cursor, "version")) continue;
            const size_t wordEnd = cursor + 7;
            if (wordEnd >= close ||
                !std::isspace(static_cast<unsigned char>(text[wordEnd])))
                continue;
            size_t value = wordEnd;
            while (value < close &&
                   std::isspace(static_cast<unsigned char>(text[value])))
                ++value;
            size_t end = value;
            if (versionTokenAt(text, value, end))
                return text.substr(value, end - value);
        }
        i = close;
    }
    return {};
}

// Generic source scan, most reliable signal first:
//   1) a line containing "version" that also carries a number token
//      (APP_VERSION = "1.0.1", VERSION = '2.3', <Version>...),
//   2) a v-prefixed token anywhere (banner lines like "Bridge v1.31.10"),
//   3) the first bare x.y[.z[.w]] token in the file.
static std::string scanVersionText(const std::string& text) {
    for (const auto& rawLine : splitLines(text)) {
        if (toLower(rawLine).find("version") == std::string::npos) continue;
        std::string tok = firstVersionToken(rawLine);
        if (!tok.empty()) return tok;
    }
    std::string prefixed = firstVPrefixedVersion(text);
    if (!prefixed.empty()) return prefixed;
    return firstVersionToken(text);
}

std::string extractLocalVersion(const std::string& fileContent,
                                const std::string& fileName) {
    if (fileContent.empty()) return {};
    std::string lower = toLower(fileName);

    if (endsWith(lower, ".csproj") || endsWith(lower, ".props") ||
        endsWith(lower, ".xml")) {
        std::string version = xmlVersion(fileContent);
        if (!version.empty()) return version;
        return scanVersionText(fileContent);
    }
    if (endsWith(lower, "cmakelists.txt") || endsWith(lower, ".cmake")) {
        // project(name VERSION x.y.z) - must not match
        // cmake_minimum_required(VERSION 3.24).
        return cmakeProjectVersion(fileContent);
    }
    if (endsWith(lower, ".json")) {
        json j = json::parse(fileContent, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            for (const char* k : {"AssemblyVersion", "version", "Version"})
                if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
        }
        return scanVersionText(fileContent);
    }
    return scanVersionText(fileContent);
}

static std::string dottedLookup(const json& j, const std::string& key) {
    const json* cur = &j;
    size_t start = 0;
    while (true) {
        size_t dot = key.find('.', start);
        std::string part = key.substr(start, dot == std::string::npos
                                                  ? std::string::npos
                                                  : dot - start);
        if (!cur->is_object() || !cur->contains(part)) return {};
        cur = &(*cur)[part];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (cur->is_string()) return cur->get<std::string>();
    return {};
}

std::string extractRemoteVersion(const std::string& payload,
                                 const std::string& key) {
    if (payload.empty()) return {};
    json j = json::parse(payload, nullptr, false);

    if (!j.is_discarded()) {
        if (j.is_array()) {
            // Plugin-master style: match InternalName (or Name) == key.
            for (const auto& e : j) {
                if (!e.is_object()) continue;
                std::string name;
                if (e.contains("InternalName") && e["InternalName"].is_string())
                    name = e["InternalName"].get<std::string>();
                else if (e.contains("Name") && e["Name"].is_string())
                    name = e["Name"].get<std::string>();
                if (key.empty() || toLower(name) == toLower(key)) {
                    for (const char* k : {"AssemblyVersion", "TestingAssemblyVersion",
                                          "version", "Version"})
                        if (e.contains(k) && e[k].is_string())
                            return e[k].get<std::string>();
                }
            }
            return {};
        }
        if (j.is_object()) {
            if (!key.empty()) {
                std::string v = dottedLookup(j, key);
                if (!v.empty()) return v;
            }
            for (const char* k : {"AssemblyVersion", "version", "Version", "latest"})
                if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
        }
    }
    return firstVersionToken(payload);
}

VersionState versionState(const std::string& local, const std::string& remote) {
    ParsedVersion l = parseVersion(local);
    ParsedVersion r = parseVersion(remote);
    if (!l.valid || !r.valid) return VersionState::Unknown;
    int c = compareVersions(l, r);
    if (c > 0) return VersionState::DevAhead;
    if (c < 0) return VersionState::Older;
    return VersionState::InSync;
}

const char* versionStateLabel(VersionState s) {
    switch (s) {
        case VersionState::DevAhead: return "dev ahead";
        case VersionState::InSync:   return "in sync";
        case VersionState::Older:    return "OLDER than published";
        default:                     return "unknown";
    }
}

} // namespace devhub
