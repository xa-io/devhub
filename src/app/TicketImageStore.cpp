#include "TicketImageStore.h"
#include "PacketOps.h"
#include "devhub/TicketImage.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>

namespace devhub {
namespace fs = std::filesystem;

namespace {

struct LockedDirectory {
    HANDLE handle = INVALID_HANDLE_VALUE;
    LockedDirectory() = default;
    explicit LockedDirectory(HANDLE value) : handle(value) {}
    LockedDirectory(const LockedDirectory&) = delete;
    LockedDirectory& operator=(const LockedDirectory&) = delete;
    LockedDirectory(LockedDirectory&& other) noexcept
        : handle(other.handle) {
        other.handle = INVALID_HANDLE_VALUE;
    }
    LockedDirectory& operator=(LockedDirectory&& other) noexcept {
        if (this == &other) return *this;
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = other.handle;
        other.handle = INVALID_HANDLE_VALUE;
        return *this;
    }
    ~LockedDirectory() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

bool reparsePoint(const fs::path& path, bool& isReparse) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    isReparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return true;
}

bool openLockedDirectory(const fs::path& path, bool rejectReparse,
                         LockedDirectory& locked, std::string& error) {
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "could not lock ticket attachment directory (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &info, sizeof(info))) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        error = "could not inspect ticket attachment directory (error " +
                std::to_string(code) + ")";
        return false;
    }
    if ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (rejectReparse &&
         (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
        CloseHandle(handle);
        error = rejectReparse
            ? "ticket attachment directory is a reparse point"
            : "ticket attachment path component is not a directory";
        return false;
    }
    locked = LockedDirectory(handle);
    return true;
}

bool finalPathForHandle(HANDLE handle, fs::path& path, std::string& error) {
    const DWORD needed = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) {
        error = "could not resolve locked ticket attachment directory";
        return false;
    }
    std::wstring buffer(static_cast<size_t>(needed), L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), needed,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= needed) {
        error = "could not read locked ticket attachment directory";
        return false;
    }
    buffer.resize(written);
    path = fs::path(buffer);
    return true;
}

bool lockAttachmentParent(const std::string& dbPath,
                          const std::string& relativePath,
                          fs::path& finalPath,
                          std::vector<LockedDirectory>& locks,
                          std::string& error) {
    if (dbPath.empty() || !isSafeTicketAttachmentRelativePath(relativePath)) {
        error = "generated ticket attachment path was rejected";
        return false;
    }
    std::error_code ec;
    const fs::path db = fs::absolute(fs::path(dbPath), ec);
    if (ec) {
        error = "could not resolve the ticket attachment data directory";
        return false;
    }
    const fs::path dataRoot = db.parent_path().lexically_normal();
    LockedDirectory dataLock;
    if (!openLockedDirectory(dataRoot, /*rejectReparse=*/false, dataLock,
                             error))
        return false;
    locks.push_back(std::move(dataLock));

    fs::path current = dataRoot;
    const fs::path relative(relativePath);
    for (const fs::path& part : relative.parent_path()) {
        current /= part;
        const bool created = fs::create_directory(current, ec);
        (void)created;
        if (ec) {
            error = "could not create ticket attachment directory: " +
                    ec.message();
            return false;
        }
        LockedDirectory component;
        // This includes ticket-images/ticket-files themselves. Holding each
        // handle without FILE_SHARE_DELETE closes the validation-to-write
        // junction swap window for every traversed directory.
        if (!openLockedDirectory(current, /*rejectReparse=*/true, component,
                                 error))
            return false;
        locks.push_back(std::move(component));
    }

    fs::path lockedParent;
    if (!finalPathForHandle(locks.back().handle, lockedParent, error))
        return false;
    finalPath = lockedParent / relative.filename();
    return true;
}

std::string mimeFor(TicketImageKind kind) {
    switch (kind) {
    case TicketImageKind::Png: return "image/png";
    case TicketImageKind::Jpeg: return "image/jpeg";
    case TicketImageKind::Gif: return "image/gif";
    case TicketImageKind::Webp: return "image/webp";
    default: return {};
    }
}

bool readExisting(const fs::path& path, std::string& bytes) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO info{};
    LARGE_INTEGER size{};
    const bool usable = GetFileInformationByHandleEx(
                            file, FileAttributeTagInfo, &info, sizeof(info)) &&
                        (info.FileAttributes &
                         (FILE_ATTRIBUTE_DIRECTORY |
                          FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                        GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
                        static_cast<unsigned long long>(size.QuadPart) <=
                            kMaxTicketAttachmentBytes;
    if (!usable) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - offset, 64 * 1024));
        if (!ReadFile(file, bytes.data() + offset, wanted, &read, nullptr) ||
            read == 0) {
            CloseHandle(file);
            bytes.clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool pathBelow(const fs::path& child, const fs::path& parent) {
    const fs::path relative = child.lexically_relative(parent);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const fs::path& part : relative)
        if (part == "..") return false;
    return true;
}

bool openImportRoot(const std::string& sourceDataRoot, fs::path& root,
                    fs::path& lockedRoot,
                    std::vector<LockedDirectory>& locks,
                    std::string& error) {
    if (sourceDataRoot.empty()) {
        error = "source_data_root is required";
        return false;
    }
    std::error_code ec;
    root = fs::absolute(fs::path(sourceDataRoot), ec).lexically_normal();
    if (ec || root.empty() || !fs::exists(root, ec) || ec ||
        !fs::is_directory(root, ec) || ec) {
        error = "source_data_root must be an existing directory";
        return false;
    }
    LockedDirectory rootLock;
    if (!openLockedDirectory(root, /*rejectReparse=*/true, rootLock, error))
        return false;
    if (!finalPathForHandle(rootLock.handle, lockedRoot, error)) return false;
    locks.push_back(std::move(rootLock));
    return true;
}

bool readLockedImportFile(const fs::path& sourceRoot,
                          const fs::path& lockedRoot,
                          const fs::path& relativePath,
                          std::size_t maximumBytes,
                          std::vector<LockedDirectory>& locks,
                          std::string& bytes, std::string& error) {
    fs::path current = sourceRoot;
    for (const fs::path& part : relativePath.parent_path()) {
        if (part.empty() || part == "." || part == "..") {
            error = "historical attachment path was rejected";
            return false;
        }
        current /= part;
        LockedDirectory component;
        if (!openLockedDirectory(current, /*rejectReparse=*/true, component,
                                 error))
            return false;
        locks.push_back(std::move(component));
    }

    const fs::path filePath = sourceRoot / relativePath;
    HANDLE file = CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "could not open historical ticket attachment (error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    LARGE_INTEGER size{};
    fs::path lockedFile;
    const bool usable = GetFileInformationByHandleEx(
                            file, FileAttributeTagInfo, &info, sizeof(info)) &&
                        (info.FileAttributes &
                         (FILE_ATTRIBUTE_DIRECTORY |
                          FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                        GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
                        static_cast<unsigned long long>(size.QuadPart) <=
                            maximumBytes &&
                        finalPathForHandle(file, lockedFile, error) &&
                        pathBelow(lockedFile, lockedRoot);
    if (!usable) {
        if (error.empty())
            error = "historical ticket attachment is unsafe or exceeds its size limit";
        CloseHandle(file);
        return false;
    }

    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - offset, 64 * 1024));
        if (!ReadFile(file, bytes.data() + offset, wanted, &read, nullptr) ||
            read == 0) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            bytes.clear();
            error = "historical ticket attachment read failed (error " +
                    std::to_string(code) + ")";
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

std::string boundedContentType(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) ch = ' ';
    }
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    if (value.size() > 127) value.resize(127);
    return value.empty() ? "application/octet-stream" : value;
}

TicketImageSaveResult saveStoredAttachment(
    const std::string& dbPath, long long itemId,
    const std::string& sourceMessageId, const std::string& attachmentId,
    const std::string& originalFilename, const std::string& contentType,
    const std::string& bytes, bool requireImage) {
    TicketImageSaveResult out;
    if (bytes.empty()) {
        out.error = "download returned an empty file";
        return out;
    }
    if (bytes.size() > kMaxTicketAttachmentBytes) {
        out.error = "attachment exceeds the 10 MiB per-file limit";
        return out;
    }
    const TicketImageKind kind = detectTicketImageKind(bytes);
    const bool rasterMetadata = isSupportedTicketImageMetadata(
        contentType, originalFilename, false);
    if (kind == TicketImageKind::Unsupported &&
        (requireImage || rasterMetadata)) {
        out.error = "download is not a supported PNG, JPEG, GIF, or WebP image";
        return out;
    }
    out.isImage = kind != TicketImageKind::Unsupported;
    out.contentType = out.isImage ? mimeFor(kind)
                                  : boundedContentType(contentType);
    out.relativePath = out.isImage
        ? makeTicketImageRelativePath(itemId, sourceMessageId, attachmentId,
                                      kind)
        : makeTicketFileRelativePath(itemId, sourceMessageId, attachmentId);
    out.absolutePath = ticketAttachmentAbsolutePath(dbPath, out.relativePath);
    if (out.relativePath.empty() || out.absolutePath.empty()) {
        out.error = "generated ticket attachment path was rejected";
        return out;
    }
    PacketHashResult hash = sha256Hex(bytes);
    if (!hash.ok) {
        out.error = "attachment SHA-256 failed: " + hash.error;
        return out;
    }
    out.sha256 = hash.hex;
    out.actualSize = bytes.size();

    fs::path finalPath;
    std::vector<LockedDirectory> directoryLocks;
    if (!lockAttachmentParent(dbPath, out.relativePath, finalPath,
                              directoryLocks, out.error))
        return out;
    out.absolutePath = finalPath.string();
    std::error_code ec;
    if (fs::exists(finalPath, ec) && !ec) {
        std::string existing;
        PacketHashResult existingHash;
        if (readExisting(finalPath, existing)) existingHash = sha256Hex(existing);
        if (existingHash.ok && existingHash.hex == out.sha256) {
            out.ok = true;
            return out;
        }
        out.error =
            "ticket attachment destination already exists with different content";
        return out;
    }

    static std::atomic<unsigned long long> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path staging = finalPath;
    staging += ".part-" + std::to_string(ticks) + "-" +
               std::to_string(sequence.fetch_add(1));
    HANDLE file = CreateFileW(
        staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        out.error = "could not open ticket attachment staging file (error " +
                    std::to_string(GetLastError()) + ")";
        return out;
    }
    size_t writtenTotal = 0;
    bool writeOk = true;
    DWORD writeError = ERROR_SUCCESS;
    while (writtenTotal < bytes.size()) {
        DWORD written = 0;
        const DWORD wanted = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - writtenTotal, 64 * 1024));
        if (!WriteFile(file, bytes.data() + writtenTotal, wanted, &written,
                       nullptr) || written == 0) {
            writeError = GetLastError();
            writeOk = false;
            break;
        }
        writtenTotal += written;
    }
    if (writeOk && !FlushFileBuffers(file)) {
        writeError = GetLastError();
        writeOk = false;
    }
    CloseHandle(file);
    if (!writeOk) {
        fs::remove(staging, ec);
        out.error = "ticket attachment staging write failed (error " +
                    std::to_string(writeError) + ")";
        return out;
    }
    fs::rename(staging, finalPath, ec);
    if (ec) {
        std::error_code cleanup;
        fs::remove(staging, cleanup);
        out.error = "could not finalize ticket attachment: " + ec.message();
        return out;
    }
    out.created = true;
    out.ok = true;
    return out;
}

} // namespace

std::string ticketImageAbsolutePath(const std::string& dbPath,
                                    const std::string& relativePath) {
    if (!isSafeTicketImageRelativePath(relativePath)) return {};
    return ticketAttachmentAbsolutePath(dbPath, relativePath);
}

std::string ticketAttachmentAbsolutePath(const std::string& dbPath,
                                         const std::string& relativePath) {
    if (dbPath.empty() ||
        !isSafeTicketAttachmentRelativePath(relativePath))
        return {};
    std::error_code ec;
    fs::path db = fs::absolute(fs::path(dbPath), ec);
    if (ec) return {};
    const fs::path dataRoot = db.parent_path().lexically_normal();
    const fs::path candidate = (dataRoot / fs::path(relativePath)).lexically_normal();
    const fs::path attachmentRoot =
        (dataRoot / (isStoredTicketImagePath(relativePath)
                         ? "ticket-images" : "ticket-files"))
            .lexically_normal();
    fs::path underRoot = candidate.lexically_relative(attachmentRoot);
    if (underRoot.empty() || underRoot.is_absolute()) return {};
    for (const auto& part : underRoot) {
        if (part == "..") return {};
    }

    // Lexical containment cannot see NTFS junctions. Resolve the deepest
    // existing candidate ancestor and require its real path to remain below
    // the real attachment root before any directory or file is created.
    fs::path probe = candidate;
    std::error_code probeEc;
    while (!probe.empty() && !fs::exists(probe, probeEc)) {
        if (probeEc) return {};
        const fs::path parent = probe.parent_path();
        if (parent == probe) break;
        probe = parent;
    }
    if (probe.empty() || probeEc) return {};

    std::error_code rootEc;
    if (fs::exists(attachmentRoot, rootEc)) {
        if (rootEc) return {};
        bool rootIsReparse = false;
        if (!reparsePoint(attachmentRoot, rootIsReparse) || rootIsReparse)
            return {};
        const fs::path realRoot = fs::canonical(attachmentRoot, rootEc);
        if (rootEc) return {};
        const fs::path realProbe = fs::canonical(probe, probeEc);
        if (probeEc) return {};
        const fs::path realRelative =
            realProbe.lexically_relative(realRoot);
        if (realRelative.empty() || realRelative.is_absolute()) return {};
        for (const auto& part : realRelative)
            if (part == "..") return {};
    } else if (rootEc) {
        return {};
    }
    return candidate.string();
}

std::string ticketImportDatabasePath(const std::string& sourceDataRoot,
                                     std::string& error) {
    error.clear();
    fs::path root, lockedRoot;
    std::vector<LockedDirectory> locks;
    if (!openImportRoot(sourceDataRoot, root, lockedRoot, locks, error))
        return {};

    const fs::path database = root / "devhub.db";
    HANDLE file = CreateFileW(
        database.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "source_data_root does not contain a readable devhub.db";
        return {};
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    fs::path lockedDatabase;
    const bool usable = GetFileInformationByHandleEx(
                            file, FileAttributeTagInfo, &info, sizeof(info)) &&
                        (info.FileAttributes &
                         (FILE_ATTRIBUTE_DIRECTORY |
                          FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                        finalPathForHandle(file, lockedDatabase, error) &&
                        pathBelow(lockedDatabase, lockedRoot);
    CloseHandle(file);
    if (!usable) {
        if (error.empty()) error = "historical devhub.db path was rejected";
        return {};
    }
    return database.string();
}

TicketImportFileReadResult readTicketImportAttachmentFile(
    const std::string& sourceDataRoot, const std::string& relativePath,
    std::size_t expectedSize, const std::string& expectedSha256) {
    TicketImportFileReadResult out;
    if (!isSafeTicketAttachmentRelativePath(relativePath)) {
        out.error = "historical attachment relative path was rejected";
        return out;
    }
    if (expectedSize == 0 || expectedSize > kMaxTicketAttachmentBytes) {
        out.error = "historical attachment size is outside the 1-byte to 10-MiB limit";
        return out;
    }
    if (expectedSha256.size() != 64 ||
        !std::all_of(expectedSha256.begin(), expectedSha256.end(),
                     [](unsigned char ch) {
                         return (ch >= '0' && ch <= '9') ||
                                (ch >= 'a' && ch <= 'f');
                     })) {
        out.error = "historical attachment SHA-256 must be lowercase hexadecimal";
        return out;
    }

    fs::path root, lockedRoot;
    std::vector<LockedDirectory> locks;
    if (!openImportRoot(sourceDataRoot, root, lockedRoot, locks, out.error))
        return out;
    if (!readLockedImportFile(root, lockedRoot, fs::path(relativePath),
                              kMaxTicketAttachmentBytes, locks, out.bytes,
                              out.error))
        return out;
    out.actualSize = out.bytes.size();
    if (out.actualSize != expectedSize) {
        out.bytes.clear();
        out.error = "historical attachment size does not match its database record";
        return out;
    }
    const PacketHashResult hash = sha256Hex(out.bytes);
    if (!hash.ok || hash.hex != expectedSha256) {
        out.bytes.clear();
        out.error = "historical attachment SHA-256 does not match its database record";
        return out;
    }
    out.sha256 = hash.hex;
    out.ok = true;
    return out;
}

TicketImageSaveResult saveTicketImageBytes(const std::string& dbPath,
                                           long long itemId,
                                           const std::string& sourceMessageId,
                                           const std::string& attachmentId,
                                           const std::string& bytes) {
    return saveStoredAttachment(dbPath, itemId, sourceMessageId, attachmentId,
                                {}, "image/unknown", bytes,
                                /*requireImage=*/true);
}

TicketImageSaveResult saveTicketAttachmentBytes(
    const std::string& dbPath, long long itemId,
    const std::string& sourceMessageId, const std::string& attachmentId,
    const std::string& originalFilename, const std::string& contentType,
    const std::string& bytes) {
    return saveStoredAttachment(dbPath, itemId, sourceMessageId, attachmentId,
                                originalFilename, contentType, bytes,
                                /*requireImage=*/false);
}

} // namespace devhub
