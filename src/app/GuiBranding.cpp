#include "Gui.h"
#include "Version.h"

#include <cstddef>

namespace devhub {
namespace {

constexpr std::size_t kMaxAppDisplayNameLength = 48;
constexpr std::size_t kMaxRawAppDisplayNameLength = 256;

} // namespace

bool normalizeAppDisplayName(const std::string& configured,
                             std::string& normalized,
                             std::string* error) {
    normalized.clear();
    if (error) error->clear();
    if (configured.size() > kMaxRawAppDisplayNameLength) {
        if (error) *error = "use 48 or fewer characters";
        return false;
    }

    const std::size_t first = configured.find_first_not_of(' ');
    if (first == std::string::npos) return true;
    const std::size_t last = configured.find_last_not_of(' ');
    const std::size_t length = last - first + 1;
    if (length > kMaxAppDisplayNameLength) {
        if (error) *error = "use 48 or fewer characters";
        return false;
    }
    for (std::size_t i = first; i <= last; ++i) {
        const unsigned char ch = static_cast<unsigned char>(configured[i]);
        if (ch < 0x20 || ch > 0x7e) {
            if (error) *error = "use printable standard characters only";
            return false;
        }
    }
    normalized.assign(configured, first, length);
    return true;
}

std::string resolveAppDisplayName(const std::string& configured) {
    std::string normalized;
    return normalizeAppDisplayName(configured, normalized) && !normalized.empty()
        ? normalized : DEVHUB_APP_NAME;
}

} // namespace devhub
