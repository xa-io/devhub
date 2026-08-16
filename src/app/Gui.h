#pragma once
#include <cstdint>
#include <string>

namespace devhub {

class Db;
class BuildRunner;
class VersionChecker;
class DiscordBot;
class MaintenanceRunner;

// Normalizes the optional presentation-only name. Blank is a valid request for
// the canonical default. Invalid input is rejected so values written through
// the generic settings API cannot become unsafe window/UI text.
bool normalizeAppDisplayName(const std::string& configured,
                             std::string& normalized,
                             std::string* error = nullptr);
std::string resolveAppDisplayName(const std::string& configured);

// Runs the native ImGui/DX11 application window. Blocks until the window is
// closed. Returns the process exit code.
int runGui(Db* db, BuildRunner* builds, VersionChecker* versions,
           DiscordBot* discord, MaintenanceRunner* maintenance,
           uint16_t apiPort);

} // namespace devhub
