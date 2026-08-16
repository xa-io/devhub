#pragma once

#include <string_view>

namespace devhub {

// Disposable-database/application acceptance suite. The aggregate remains the
// default for the product --selftest switch; the dedicated CTest executable
// can select an independently reported domain while retaining the canonical
// shared integration fixture.
int runAppSelftest(std::string_view domain = "all");

} // namespace devhub
