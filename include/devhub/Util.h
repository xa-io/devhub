#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace devhub {

std::string trim(const std::string& s);
std::string toLower(const std::string& s);
std::vector<std::string> splitLines(const std::string& text);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool containsCI(const std::string& haystack, const std::string& needle);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);

// Replaces HTTP(S) URLs and D++'s scheme-less HTTP endpoint spellings with a
// fixed marker. Use at logging boundaries where signed or credential-bearing
// URLs must never reach durable output.
std::string redactHttpUrls(std::string s);

// Escapes untrusted Discord embed text. Backslashes are escaped before every
// Markdown delimiter so attacker-controlled odd/even escape runs cannot
// reactivate links, emphasis, code, spoilers, or masked URLs.
std::string escapeDiscordMarkdown(const std::string& value);

// One logical record in, one record out. Remote strings cannot forge, end, or
// visually overwrite a log line; long values are bounded after sanitizing.
std::string logSafe(std::string value, size_t cap = 256);

// "XA HUD Navigator" -> "xa-hud-navigator"
std::string slugify(const std::string& s);

// First line of a block, cut at a word boundary near maxLen.
std::string makeTitle(const std::string& body, size_t maxLen = 120);

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ" and local date as "YYYY-MM-DD".
std::string nowIsoUtc();
// Returns a valid UTC second strictly later than `floor`, even when two
// optimistic-concurrency mutations occur during the same wall-clock second.
std::string nextIsoUtcAfter(const std::string& floor);
std::string todayLocal();
int64_t nowEpochMs();

// `ok` distinguishes a readable empty file from an open/read failure.
std::string readFileUtf8(const std::string& path, bool* ok = nullptr);
bool writeFileUtf8(const std::string& path, const std::string& content);

// Preserve well-formed UTF-8 byte-for-byte. Invalid bytes are interpreted
// individually as Windows-1252 so legacy Windows process/database text can be
// serialized safely without changing the persisted source value.
std::string normalizeUtf8(const std::string& text);

// Largest prefix that does not end inside a potentially valid UTF-8 sequence.
// Streaming callers carry the suffix into the next read and normalize only the
// returned prefix so a split multibyte character is never decoded twice.
size_t utf8SafeSplitPoint(const std::string& text);

// ---------------------------------------------------------------------------
// In-memory debug log shared by every subsystem (GUI, ops, Discord bot
// threads, webhook sender). Thread-safe ring of ~400 lines; each line is
// stamped with the local time. The GUI shows it in the optional bottom
// debug panel.
// ---------------------------------------------------------------------------
void appLog(const std::string& line);
void appLogClear();
// Monotonic counter bumped on every append/clear - cheap dirty check.
uint64_t appLogRevision();
std::vector<std::string> appLogSnapshot();

} // namespace devhub
