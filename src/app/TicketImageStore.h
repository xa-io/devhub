#pragma once

#include <cstddef>
#include <string>

namespace devhub {

struct TicketImageSaveResult {
    bool ok = false;
    bool created = false;
    bool isImage = false;
    std::string relativePath;
    std::string absolutePath;
    std::string sha256;
    std::size_t actualSize = 0;
    std::string contentType;
    std::string error;
};

struct TicketImportFileReadResult {
    bool ok = false;
    std::string bytes;
    std::string sha256;
    std::size_t actualSize = 0;
    std::string error;
};

// Resolves a generated relative path against parent_path(devhub.db). Invalid or
// escaping paths return an empty string.
std::string ticketImageAbsolutePath(const std::string& dbPath,
                                    const std::string& relativePath);

// Resolves either a generated ticket-images path or an inert ticket-files
// path. Invalid or escaping paths return an empty string.
std::string ticketAttachmentAbsolutePath(const std::string& dbPath,
                                         const std::string& relativePath);

// Resolves an explicitly selected historical DevHub data root to its
// devhub.db. The root and database must already exist as ordinary,
// non-reparse filesystem objects. An empty result is returned on rejection.
std::string ticketImportDatabasePath(const std::string& sourceDataRoot,
                                     std::string& error);

// Reads one saved ticket attachment from an explicitly selected historical
// DevHub data root. Every path component is opened without following reparse
// points and held without delete sharing while the file is bounded, read, and
// checked against its recorded size and lowercase SHA-256.
TicketImportFileReadResult readTicketImportAttachmentFile(
    const std::string& sourceDataRoot, const std::string& relativePath,
    std::size_t expectedSize, const std::string& expectedSha256);

// Validates raster magic, derives the stored MIME/extension from those bytes,
// hashes, writes a same-directory staging file, and atomically renames it to
// the generated final path. Discord and HTTP MIME values are advisory only.
TicketImageSaveResult saveTicketImageBytes(const std::string& dbPath,
                                           long long itemId,
                                           const std::string& sourceMessageId,
                                           const std::string& attachmentId,
                                           const std::string& bytes);

// Preserves raster magic validation and normalized image extensions. Every
// other file is hashed and saved with an opaque generated .bin filename so
// untrusted Discord names/extensions are never executable local paths.
TicketImageSaveResult saveTicketAttachmentBytes(
    const std::string& dbPath, long long itemId,
    const std::string& sourceMessageId, const std::string& attachmentId,
    const std::string& originalFilename, const std::string& contentType,
    const std::string& bytes);

} // namespace devhub
