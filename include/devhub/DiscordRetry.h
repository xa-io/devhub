#pragma once

#include <cstdint>
#include <string_view>

namespace devhub {

// Discord REST requests can fail after receiving a real HTTP response, in
// which case D++ reports h_success for the transport. Keep transient status
// classification independent of D++ so the worker and focused tests share the
// same contract.
constexpr bool isRetryableDiscordHttpStatus(std::uint16_t status) noexcept {
    return status == 0 || status == 408 || status == 425 || status == 429 ||
           (status >= 500 && status <= 599);
}

// DELETE is idempotently complete when Discord reports that the exact
// callback-generated message no longer exists.
constexpr bool isDiscordDeleteAlreadyAbsent(std::uint16_t status) noexcept {
    return status == 404;
}

// Discord error 30046 is permanent for the stored message target. Some D++
// callback/error persistence paths include the code, the text, or both, so
// every worker, migration, and operator surface must recognize either form.
inline bool isDiscordOldMessageEditLimitText(
    std::string_view message) noexcept {
    return message.find(
               "Maximum number of edits to messages older than 1 hour reached") !=
               std::string_view::npos ||
           message.find("30046") != std::string_view::npos;
}

} // namespace devhub
