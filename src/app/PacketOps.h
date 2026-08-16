#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace devhub {

// PacketOps is deliberately independent from Db, the native UI, and the HTTP
// server. Callers collect a consistent payload snapshot, then use this module
// once to add the shared routing, provenance, authority, and safety envelope.
enum class PacketKind {
    Review,
    Release,
    Knowledge,
    Report,
    Resume,
};

struct PacketTarget {
    long long projectId = 0;
    std::string projectName;
    std::string workspace;
    std::string rulesPath;

    long long workflowId = 0;
    std::string workflowTitle;
    std::string workflowContext;
    std::string workflowPhase;
    std::string workflowStatus;

    // Durable direction copied from the selected workflow. These fields add
    // context; they never expand the authority granted by the live request.
    std::string objective;
    std::string minimumSuccess;
    std::string validation;
    std::string currentStep;
    std::string nextAction;
    std::string blockers;

    // Repository-specific skills supplement the invariant DevHub routing.
    // Tokens may be supplied with or without a leading '$'.
    std::string repositorySkill;
    std::vector<std::string> companionSkills;
};

// Renderer-owned report approval metadata. These values are emitted outside
// both untrusted recorded metadata and the stored payload. input_hash is not a
// caller field: renderContextPacket derives it from the packet source SHA-256.
// evidenceAt must equal generatedAt and is intentionally excluded from packet
// identity so repeated copies of unchanged evidence keep a stable packet ID.
struct PacketReportRegistryKeys {
    bool enabled = false;
    std::string reportType;
    std::string periodStart;
    std::string periodEnd;
    std::string evidenceAt;
};

struct PacketRenderRequest {
    PacketKind kind = PacketKind::Review;
    PacketTarget target;

    // Packet-specific Markdown only. Do not pass another complete packet here;
    // this renderer owns the single outer envelope and untrusted-data boundary.
    std::string payloadMarkdown;

    // Canonical evidence used for sourceHash. When omitted, payloadMarkdown is
    // used. generatedAt is intentionally excluded from hashes and packet IDs.
    std::string sourceMaterial;
    std::string generatedAt;

    // Required for report packets and rejected for every other packet kind.
    // The renderer validates these as bounded single-line trusted scalars.
    PacketReportRegistryKeys reportRegistry;

    // Trusted, concise notes supplied by the caller. Untrusted records belong
    // only in payloadMarkdown.
    std::vector<std::string> scopeNotes;
    std::vector<std::string> continuationNotes;
};

struct PacketHashResult {
    bool ok = false;
    std::string hex;
    std::string error;
};

struct PacketRenderResult {
    bool ok = false;
    std::string markdown;
    std::string packetId;
    std::string packetHash;
    std::string sourceHash;
    std::vector<std::string> requiredSkills;
    std::vector<std::string> warnings;
    std::string error;
};

// Returns a canonical token such as "$ffxiv-dalamud-plugin-builder". Invalid values return
// an empty string; renderContextPacket converts unresolved repository skills
// into a visible deterministic fallback rather than guessing.
std::string normalizeSkillToken(std::string_view value);

// Report types are exact trust-boundary identifiers. Callers must supply a
// value matching [a-z0-9][a-z0-9_-]{0,63}; this function never trims, folds
// case, or otherwise turns caller input into a different trusted value.
bool isValidReportType(std::string_view value);

// Windows SHA-256 backed by BCrypt. No exception escapes this function.
PacketHashResult sha256Hex(std::string_view value);

const char* packetKindName(PacketKind kind);
const char* packetOperationName(PacketKind kind);

// Renders exactly one self-contained context packet. The result is deterministic
// for the same non-volatile request fields; generatedAt never changes identity.
PacketRenderResult renderContextPacket(const PacketRenderRequest& request);

} // namespace devhub
