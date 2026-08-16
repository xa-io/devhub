#pragma once
#include <string>
#include <vector>

namespace devhub {

// Comparison outcome between the local (in-development) version and the
// published (uploaded) version of a project.
enum class VersionState {
    Unknown,  // one or both sides missing/unparseable
    DevAhead, // local > published: normal while developing
    InSync,   // local == published
    Older,    // local < published: error likely (stale checkout?)
};

struct ParsedVersion {
    std::vector<int> parts;
    bool valid = false;
};

// Accepts "1.2.3.4", "0.4.2", "v1.2.3.4" (2-4 numeric parts).
ParsedVersion parseVersion(const std::string& s);

// -1 / 0 / +1; missing trailing parts compare as 0 (1.2 == 1.2.0.0).
int compareVersions(const ParsedVersion& a, const ParsedVersion& b);

// Pull a version string out of a local project file.
//   *.csproj -> first <Version>x.y.z.w</Version>
//   *.json   -> "AssemblyVersion" then "version"/"Version" keys
//   anything -> first x.y.z[.w] looking token
// Returns "" when nothing found.
std::string extractLocalVersion(const std::string& fileContent,
                                const std::string& fileName);

// Pull a version out of a fetched payload.
//   JSON array of plugin manifests + key -> entry with InternalName==key,
//     its AssemblyVersion (Dalamud pluginmaster / aethertek x.json format).
//   JSON object + key -> dotted path lookup ("app.latest").
//   JSON object, no key -> "AssemblyVersion" / "version" / "Version" / "latest".
//   Fallback -> first x.y.z[.w] token in the payload.
std::string extractRemoteVersion(const std::string& payload,
                                 const std::string& key);

VersionState versionState(const std::string& local, const std::string& remote);
const char* versionStateLabel(VersionState s);

} // namespace devhub
