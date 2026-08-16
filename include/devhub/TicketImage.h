#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace devhub {

constexpr std::size_t kMaxTicketAttachments = 10;
constexpr std::size_t kMaxTicketAttachmentBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxTicketAttachmentTotalBytes = 50 * 1024 * 1024;
// Compatibility aliases retained for the existing image worker/tests while
// the durable ticket_attachments owner now accepts non-image files too.
constexpr std::size_t kMaxTicketImages = kMaxTicketAttachments;
constexpr std::size_t kMaxTicketImageBytes = kMaxTicketAttachmentBytes;
constexpr std::size_t kMaxTicketImageTotalBytes =
    kMaxTicketAttachmentTotalBytes;
// A recovery turn may inspect more candidates than can ultimately persist so
// invalid early bodies cannot reserve all ten slots. The turn itself remains
// bounded; deferred rows are rotated fairly by updated_at and resumed later.
constexpr std::size_t kMaxTicketImageCandidatesPerPass =
    kMaxTicketImages * 2;
// The image worker shares its thread with card/manual recovery. One turn may
// make only one bounded Discord request for one item before yielding.
constexpr std::size_t kMaxTicketImageBodiesPerRequest =
    kMaxTicketImageTotalBytes / kMaxTicketImageBytes;
constexpr std::size_t kMaxTicketImageItemsPerTurn = 1;
constexpr bool ticketImageRetryClassForTurn(bool recoverySweep,
                                             bool hasRetry) noexcept {
    return recoverySweep && hasRetry;
}

// Tracks only images that have already passed byte validation and were
// persisted successfully. Rejected candidates never consume a slot or bytes;
// recordPersisted() defensively applies the same bounds as canPersist().
class TicketImageCapacity {
public:
    TicketImageCapacity(std::size_t persistedSavedCount = 0,
                        std::size_t persistedSavedBytes = 0) noexcept
        : persistedSavedCount_(persistedSavedCount),
          persistedSavedBytes_(persistedSavedBytes) {}

    bool canPersist(std::size_t actualBytes) const noexcept {
        if (actualBytes == 0 || actualBytes > kMaxTicketImageBytes ||
            persistedSavedCount_ >= kMaxTicketImages ||
            persistedSavedBytes_ > kMaxTicketImageTotalBytes)
            return false;
        return actualBytes <=
            kMaxTicketImageTotalBytes - persistedSavedBytes_;
    }

    bool recordPersisted(std::size_t actualBytes) noexcept {
        if (!canPersist(actualBytes)) return false;
        ++persistedSavedCount_;
        persistedSavedBytes_ += actualBytes;
        return true;
    }

    std::size_t persistedSavedCount() const noexcept {
        return persistedSavedCount_;
    }
    std::size_t persistedSavedBytes() const noexcept {
        return persistedSavedBytes_;
    }

private:
    std::size_t persistedSavedCount_ = 0;
    std::size_t persistedSavedBytes_ = 0;
};

enum class TicketImageKind {
    Unsupported,
    Png,
    Jpeg,
    Gif,
    Webp,
};

// Discord snowflakes cross the JSON/SQLite boundary as decimal strings. Keep
// them numeric and non-zero before they become filesystem components.
bool isDecimalDiscordSnowflake(std::string_view value);

// Metadata is only a capture-time eligibility hint. Promotion still validates
// the downloaded bytes by signature before any final file is written.
bool isSupportedTicketImageMetadata(std::string_view contentType,
                                    std::string_view filename,
                                    bool ephemeral);

// Every non-ephemeral Discord attachment is eligible for metadata capture.
// Raster-looking files still pass the stronger magic validation at save time;
// other files are stored under an inert generated .bin name.
bool isSupportedTicketAttachmentMetadata(std::string_view contentType,
                                         std::string_view filename,
                                         bool ephemeral);

TicketImageKind detectTicketImageKind(std::string_view bytes);
std::string ticketImageExtension(TicketImageKind kind);
bool ticketImageMimeMatchesKind(std::string_view contentType,
                                TicketImageKind kind);

// Only generated Discord/item IDs and a magic-derived extension participate in
// the local name. The untrusted original filename is display metadata only.
std::string makeTicketImageRelativePath(long long itemId,
                                        std::string_view sourceMessageId,
                                        std::string_view attachmentId,
                                        TicketImageKind kind);

// Non-image bytes never borrow an untrusted filename or executable extension
// for their local path. The original filename remains display metadata.
std::string makeTicketFileRelativePath(long long itemId,
                                       std::string_view sourceMessageId,
                                       std::string_view attachmentId);

// Defense in depth for paths read back from SQLite before packet rendering.
bool isSafeTicketImageRelativePath(std::string_view relativePath);

// Accepts both legacy/generated ticket-images paths and the new inert
// ticket-files paths. Call isStoredTicketImagePath before rendering as raster.
bool isSafeTicketAttachmentRelativePath(std::string_view relativePath);
bool isStoredTicketImagePath(std::string_view relativePath);

} // namespace devhub
