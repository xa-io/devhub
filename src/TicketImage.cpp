#include "devhub/TicketImage.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>

namespace devhub {
namespace {

std::string lowerMime(std::string_view value) {
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string_view::npos) value = value.substr(0, semicolon);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string lowerExtension(std::string_view filename) {
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(filename.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

bool decimalDigits(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return ch >= '0' && ch <= '9';
           });
}

bool supportedExtension(std::string_view ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".gif" || ext == ".webp";
}

std::string ticketRelativeFilename(std::string_view relativePath,
                                   std::string_view root) {
    if (relativePath.empty() || relativePath.front() == '/' ||
        relativePath.front() == '\\' ||
        relativePath.find(':') != std::string_view::npos ||
        relativePath.find("..") != std::string_view::npos)
        return {};
    std::string normalized(relativePath);
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    const std::size_t first = normalized.find('\\');
    const std::size_t second = first == std::string::npos
        ? std::string::npos : normalized.find('\\', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        normalized.find('\\', second + 1) != std::string::npos ||
        std::string_view(normalized).substr(0, first) != root)
        return {};
    const std::string item =
        normalized.substr(first + 1, second - first - 1);
    if (item.size() < 2 || item.front() != 'I' ||
        !decimalDigits(std::string_view(item).substr(1)))
        return {};
    return normalized.substr(second + 1);
}

bool snowflakePairStem(std::string_view stem) {
    const std::size_t dash = stem.find('-');
    return dash != std::string_view::npos && dash > 0 &&
           dash + 1 < stem.size() && decimalDigits(stem.substr(0, dash)) &&
           decimalDigits(stem.substr(dash + 1));
}

} // namespace

bool isDecimalDiscordSnowflake(std::string_view value) {
    if (value.empty() || value.size() > 20 || !decimalDigits(value)) return false;
    std::uint64_t parsed = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto result = std::from_chars(first, last, parsed, 10);
    return result.ec == std::errc{} && result.ptr == last && parsed != 0;
}

bool isSupportedTicketImageMetadata(std::string_view contentType,
                                    std::string_view filename,
                                    bool ephemeral) {
    if (ephemeral) return false;
    const std::string mime = lowerMime(contentType);
    if (mime == "image/png" || mime == "image/jpeg" || mime == "image/jpg" ||
        mime == "image/gif" || mime == "image/webp")
        return true;
    if (!mime.empty() && mime != "application/octet-stream") return false;
    return supportedExtension(lowerExtension(filename));
}

bool isSupportedTicketAttachmentMetadata(std::string_view,
                                         std::string_view,
                                         bool ephemeral) {
    return !ephemeral;
}

TicketImageKind detectTicketImageKind(std::string_view bytes) {
    const auto b = [&](std::size_t i) {
        return static_cast<unsigned char>(bytes[i]);
    };
    if (bytes.size() >= 8 && b(0) == 0x89 && bytes.substr(1, 3) == "PNG" &&
        b(4) == 0x0d && b(5) == 0x0a && b(6) == 0x1a && b(7) == 0x0a)
        return TicketImageKind::Png;
    if (bytes.size() >= 3 && b(0) == 0xff && b(1) == 0xd8 && b(2) == 0xff)
        return TicketImageKind::Jpeg;
    if (bytes.size() >= 6 &&
        (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a"))
        return TicketImageKind::Gif;
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" &&
        bytes.substr(8, 4) == "WEBP")
        return TicketImageKind::Webp;
    return TicketImageKind::Unsupported;
}

std::string ticketImageExtension(TicketImageKind kind) {
    switch (kind) {
    case TicketImageKind::Png: return ".png";
    case TicketImageKind::Jpeg: return ".jpg";
    case TicketImageKind::Gif: return ".gif";
    case TicketImageKind::Webp: return ".webp";
    default: return {};
    }
}

bool ticketImageMimeMatchesKind(std::string_view contentType,
                                TicketImageKind kind) {
    const std::string mime = lowerMime(contentType);
    if (mime.empty() || mime == "application/octet-stream") return true;
    switch (kind) {
    case TicketImageKind::Png: return mime == "image/png";
    case TicketImageKind::Jpeg:
        return mime == "image/jpeg" || mime == "image/jpg";
    case TicketImageKind::Gif: return mime == "image/gif";
    case TicketImageKind::Webp: return mime == "image/webp";
    default: return false;
    }
}

std::string makeTicketImageRelativePath(long long itemId,
                                        std::string_view sourceMessageId,
                                        std::string_view attachmentId,
                                        TicketImageKind kind) {
    const std::string ext = ticketImageExtension(kind);
    if (itemId <= 0 || ext.empty() ||
        !isDecimalDiscordSnowflake(sourceMessageId) ||
        !isDecimalDiscordSnowflake(attachmentId))
        return {};
    return "ticket-images\\I" + std::to_string(itemId) + "\\" +
           std::string(sourceMessageId) + "-" + std::string(attachmentId) + ext;
}

std::string makeTicketFileRelativePath(long long itemId,
                                       std::string_view sourceMessageId,
                                       std::string_view attachmentId) {
    if (itemId <= 0 || !isDecimalDiscordSnowflake(sourceMessageId) ||
        !isDecimalDiscordSnowflake(attachmentId))
        return {};
    return "ticket-files\\I" + std::to_string(itemId) + "\\" +
           std::string(sourceMessageId) + "-" + std::string(attachmentId) +
           ".bin";
}

bool isSafeTicketImageRelativePath(std::string_view relativePath) {
    const std::string file =
        ticketRelativeFilename(relativePath, "ticket-images");
    if (file.empty()) return false;
    const std::size_t dot = file.find_last_of('.');
    return dot != std::string::npos && dot > 0 &&
           snowflakePairStem(std::string_view(file).substr(0, dot)) &&
           supportedExtension(lowerExtension(file));
}

bool isStoredTicketImagePath(std::string_view relativePath) {
    return isSafeTicketImageRelativePath(relativePath);
}

bool isSafeTicketAttachmentRelativePath(std::string_view relativePath) {
    if (isSafeTicketImageRelativePath(relativePath)) return true;
    const std::string file =
        ticketRelativeFilename(relativePath, "ticket-files");
    if (file.empty()) return false;
    constexpr std::string_view suffix = ".bin";
    if (file.size() <= suffix.size() ||
        file.compare(file.size() - suffix.size(), suffix.size(), ".bin") != 0)
        return false;
    return snowflakePairStem(
        std::string_view(file.data(), file.size() - suffix.size()));
}

} // namespace devhub
