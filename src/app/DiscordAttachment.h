#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace devhub {

// D++-free attachment data passed across DiscordBot.cpp's nlohmann ABI
// boundary. CDN URLs are deliberately absent: promotion refetches the source
// message and downloads by exact attachment ID.
struct DiscordAttachmentMeta {
    std::string attachmentId;
    std::string sourceChannelId;
    std::string sourceMessageId;
    std::string sourceRole; // suggestion | admin_reply | direct_ping
    std::string filename;
    std::string contentType;
    std::uint64_t sizeBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool ephemeral = false;
};

struct DiscordAttachmentDownload {
    DiscordAttachmentMeta meta;
    std::string bytes;
    // Advisory response metadata captured from the original attachment URL.
    // The supported raster signature remains the save-time authority because
    // Discord metadata and transformed representations can disagree.
    std::string responseContentType;
    std::string error;
    // Transport/rate-limit failures remain queued for reconnect or the next
    // bounded rescan. Missing/unsupported attachments are terminal failures.
    bool retryable = false;
};

} // namespace devhub
