#pragma once

// XA DevHub native UI: Dear ImGui + DirectX 11 + Win32. No browser anywhere.
#include "Gui.h"
#include "BuildRunner.h"
#include "DataMaintenance.h"
#include "Db.h"
#include "DiscordBot.h"
#include "KnowledgeOps.h"
#include "Ops.h"
#include "TicketImageStore.h"
#include "Version.h"
#include "VersionChecker.h"
#include "WinUtil.h"
#include "resource.h"
#include "devhub/DiscordRetry.h"
#include "devhub/TicketImage.h"
#include "devhub/Util.h"
#include "devhub/VersionUtil.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace devhub {

static constexpr const char* kAppDisplayNameSetting = "app_display_name";

// Segoe MDL2 Assets glyphs (merged into the main font).
#define ICON_FOLDER  "\xEE\xA2\xB7" // U+E8B7 OpenFolderHorizontal
#define ICON_REFRESH "\xEE\x9C\xAC" // U+E72C Refresh
#define ICON_GLOBE   "\xEE\x9D\xB4" // U+E774 Globe

// ---------------------------------------------------------------------------
// palette
// ---------------------------------------------------------------------------
static ImVec4 rgba(unsigned hex, float a = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                  (hex & 0xFF) / 255.0f, a);
}
static const ImVec4 C_BG      = rgba(0x0d1117);
static const ImVec4 C_BG2     = rgba(0x161b22);
static const ImVec4 C_BG3     = rgba(0x1c2330);
static const ImVec4 C_BORDER  = rgba(0x2d333b);
// Sidebar: its own neutral light-gray panel, deliberately not the blue-tinted
// background family, drawn flush against the window edges.
static const ImVec4 C_SIDEBAR  = rgba(0x26292e);
static const ImVec4 C_SIDEHOV  = rgba(0x33373d);
static const ImVec4 C_SIDESEL  = rgba(0x3c4149);
static const ImVec4 C_TEXT    = rgba(0xd7dde5);
static const ImVec4 C_DIM     = rgba(0x8b949e);
static const ImVec4 C_ACCENT  = rgba(0x4f8ff7);
static const ImVec4 C_GREEN   = rgba(0x3fb950);
static const ImVec4 C_RED     = rgba(0xf85149);
static const ImVec4 C_ORANGE  = rgba(0xd29922);
static const ImVec4 C_PURPLE  = rgba(0xbc8cff);
static const ImVec4 C_CYAN    = rgba(0x39c5cf);

inline ImVec4 typeColor(const std::string& t) {
    if (t == "fix") return C_RED;
    if (t == "implementation") return C_ACCENT;
    if (t == "reference") return C_PURPLE;
    return C_CYAN;
}
inline ImVec4 statusColor(const std::string& s) {
    if (s == "success" || s == "completed") return C_GREEN;
    if (s == "failed" || s == "error") return C_RED;
    if (s == "running" || s == "blocked") return C_ORANGE;
    if (s == "in_progress") return C_ACCENT;
    return C_DIM;
}
inline ImVec4 verStateColor(VersionState s) {
    switch (s) {
        case VersionState::DevAhead: return C_ACCENT;
        case VersionState::InSync:   return C_GREEN;
        case VersionState::Older:    return C_RED;
        default:                     return C_DIM;
    }
}

// ---------------------------------------------------------------------------
// cached rows
// ---------------------------------------------------------------------------
struct Project {
    long long id = 0;
    std::string name, desc, path, rules, codexSkills, buildCwd;
    std::string buildCmd, prepCmd, releaseCmd, versionCmd;
    std::string localVerFile, remoteUrl, remoteKey, githubUrl, releaseNotes;
    std::string aliases;
    int archived = 0;
    int discordTickets = 1; // listed in the bot's !tickets menu
    long long openFix = 0, openImpl = 0, openOther = 0, done = 0;
    std::string lastBuildStatus, lastBuildKind, lastBuildAt;
};
struct Item {
    long long id = 0, projectId = 0;
    std::string project, type, title, body, status, source, origin, due, created, tags;
    std::string updated, reviewDate, blockedReason;
    std::string completedAt; // filled by the credits drill-down
    int priority = 2, credited = 0, sourceCount = 0, uncreditedCount = 0;
    int stale = 0, overdue = 0, reviewDue = 0;
};
struct Contributor {
    long long id = 0;
    std::string name;
    int credited = 0;
};
struct Source {
    long long id = 0;
    std::string name, platform;
    long long items = 0, completed = 0, uncredited = 0;
    bool leaderboardVisible = true;
};
struct ReviewChoice {
    long long id = 0;
    std::string type, title, body, status;
    int priority = 0;
    bool selected = true;
};
struct Channel {
    long long id = 0, projectId = 0;
    std::string channelId, guild, name, lastRead, lastScan;
    int enabled = 1;
    long long msgs = 0, pending = 0;
};
struct Msg {
    long long id = 0, channelProjectId = 0, inferredProjectId = 0;
    std::string author, content, posted, kind, matched, channelName;
    std::string guildId, channelId, messageId; // for the Discord jump link
    std::string adminNote; // whitelisted admin's reply-mention note (if any)
    double score = 0;
    int attachmentCount = 0;
    long long selProjectId = 0; // durable selection identity; 0 = none
    int selProject = -1; // display index derived from selProjectId
    int selType = 0;     // 0 impl, 1 fix, 2 note
};
struct Guild {
    long long id = 0;
    std::string guildId, name;
    int enabled = 1;
};
struct Activity {
    std::string ts, kind, project, detail;
    long long msgId = 0; // != 0: a captured Discord message still in the inbox
};
struct CalEntry {
    std::string date, title, kind, project;
    long long eventId = 0;         // deletable when != 0
    std::string entry;             // event | due | build
    long long itemId = 0, buildId = 0;
    std::string notes, created, status, itemType, ts;
    int exitCode = -1;
};
// Lazily fetched detail for a calendar entry's underlying ticket (item).
struct Ticket {
    bool loaded = false, found = false;
    long long projectId = 0;
    std::string title, type, status, created, completed, source, origin;
    std::string body, discordState;
    int priority = 2;
};

enum class Page { Dashboard, Work, Project, Knowledge, Calendar, Discord, Credits, Settings };

struct Console {
    bool open = false, done = false, reloaded = false, autoScroll = true;
    bool truncated = false;
    long long buildId = 0;
    size_t offset = 0;
    std::string title, status, text;
    int exitCode = -1;
};

struct PreservedText {
    std::string original;
    bool truncated = false;
};

struct NotifyFailureRow {
    long long id = 0;
    long long itemId = 0;
    long long messageRowId = 0;
    std::string operation;
    std::string error;
    std::string channelId;
    std::string messageId;
    int attempts = 0;
    int revision = 0;
};

struct OrphanFailureRow {
    long long id = 0;
    long long cardRowId = 0;
    std::string channelId;
    std::string messageId;
    std::string error;
    int attempts = 0;
};

struct ReviewCleanupFailureRow {
    long long id = 0;
    long long cardRowId = 0;
    std::string channelId;
    std::string messageId;
    std::string error;
    int attempts = 0;
};

struct NotifyFailureCache {
    std::vector<NotifyFailureRow> active;
    std::vector<NotifyFailureRow> dismissed;
    int activeTotal = 0;
    int dismissedTotal = 0;
    int oldMessageTotal = 0;
    std::vector<NotifyFailureAlertTarget> oldMessageTargets;
    std::vector<OrphanFailureRow> orphans;
    int orphanTotal = 0;
    std::vector<ReviewCleanupFailureRow> reviewCleanups;
    int reviewCleanupTotal = 0;
};

struct App {
    Db* db = nullptr;
    BuildRunner* builds = nullptr;
    VersionChecker* versions = nullptr;
    DiscordBot* discord = nullptr;
    MaintenanceRunner* maintenance = nullptr;
    HWND hwnd = nullptr;
    uint16_t apiPort = 0;
    double lastAutoRefresh = 0;

    // Presentation-only branding. The internal product identity remains
    // DEVHUB_APP_NAME everywhere outside the sidebar and native caption.
    std::string appDisplayName = DEVHUB_APP_NAME;
    char appDisplayNameInput[64]{};
    bool appDisplayNameInvalid = false;

    Page page = Page::Dashboard;
    long long selProject = 0;
    char dashboardProjectFilter[256]{};
    bool dashboardActiveOnly = false;
    char workSearch[256]{};
    int workProject = 0, workType = 0, workStatus = 0, workPriority = 0;
    int workFocus = 0; // all, overdue, stale, review due, blocked

    // caches + dirty flags
    std::vector<Project> projects;   bool needProjects = true;
    std::vector<Item> workItems;     bool needWork = true;
    std::vector<Item> items;         bool needItems = true;   // for selProject
    std::vector<Source> sources;     bool needSources = true;
    std::vector<LeaderboardEntry> leaderboard;
    bool leaderboardConfigValid = true;
    std::string leaderboardConfigError;
    std::vector<Channel> channels;   bool needChannels = true;
    std::vector<Guild> guilds;       bool needGuilds = true;
    std::vector<Msg> inbox;          bool needInbox = true;
    NotifyFailureCache notifyFailures;
    bool needNotifyCounters = true;
    std::vector<Activity> activity;
    std::map<std::string, std::vector<CalEntry>> calendar; bool needCal = true;
    nlohmann::json knowledgeNodes = nlohmann::json::array();
    nlohmann::json knowledgeConflicts = nlohmann::json::array();
    nlohmann::json knowledgeReports = nlohmann::json::array();
    nlohmann::json knowledgeWorkflows = nlohmann::json::array();
    nlohmann::json knowledgeHealthState = nlohmann::json::object();
    bool needKnowledge = true;
    long long statPending = 0, statUncredited = 0;
    long long statCompletedWindow = 0, statOverdue = 0;
    std::map<long long, std::vector<Item>> sourceItems; // lazy per source

    int calYear = 0, calMonth = 0; // month: 0-11
    std::string dayPopupDate;
    std::set<int> dayExpanded;                // expanded rows in the Day popup
    std::map<long long, Ticket> ticketCache;  // per-item detail, popup-lifetime
    long long focusItemId = 0;                // jump target on the project page
    int pendingTab = -1;                      // tab to force-select (0-3, 4=done)

    Console console;

    // bottom debug log panel (optional)
    bool showLog = false, logAutoScroll = true;
    uint64_t logRevSeen = (uint64_t)-1;
    std::vector<std::string> logLines;

    std::set<long long> creditExpanded; // expanded item bodies on Credits

    // Collapsible-section open states, persisted in settings (ui_sec_*).
    std::map<std::string, bool> secOpen;

    ImFont* fontBig = nullptr;

    // toast
    std::string toastMsg;
    double toastUntil = 0;
    bool toastErr = false;

    // modal state + buffers
    long long releaseFor = 0;      char releaseBuf[16]{};
    long long versionFor = 0;      int versionChoice = 0; char versionBuf[32]{};
    long long reviewFor = 0;
    char reviewSearch[512]{};
    std::vector<ReviewChoice> reviewChoices;
    long long mergeSourceId = 0;
    long long mergeTargetSourceId = 0;
    long long deleteItemId = 0;
    long long recreateNotifyCardId = 0;
    std::string recreateNotifyCardOperation;
    int recreateNotifyCardRevision = -1;
    std::vector<NotifyFailureAlertTarget> dismissOldNotifyAlertTargets;
    int dismissOldNotifyAlertTotal = 0;
    long long editPendingMessageId = 0;
    int editPendingProject = -1;
    long long editPendingProjectId = 0;
    int editPendingType = 0, editPendingPrio = 1;
    char editPendingTitle[512]{}, editPendingContent[16384]{};
    char editPendingAdminNote[4096]{};
    long long editItemId = 0, editItemProjectId = 0, eiProjectId = 0;
    char eiTitle[512]{}, eiBody[16384]{}, eiDue[32]{}, eiSource[128]{};
    PreservedText eiBodyValue;
    char eiReview[32]{}, eiBlocked[512]{}, eiAddSource[128]{};
    std::vector<Contributor> editContributors;
    int eiMergeTarget = -1;
    int eiType = 0, eiPrio = 1, eiStatus = 0;
    bool editProject = false;
    char epName[256]{}, epDesc[512]{}, epPath[512]{}, epRules[512]{};
    char epSkills[512]{};
    char epBuild[512]{}, epPrep[512]{}, epRelease[512]{}, epVersion[512]{};
    char epCwd[512]{}, epVerFile[512]{}, epRemoteUrl[512]{}, epRemoteKey[128]{};
    char epGithub[512]{}, epAliases[512]{};
    PreservedText epPathValue, epBuildValue, epPrepValue, epReleaseValue;
    PreservedText epVersionValue, epCwdValue;
    bool epTickets = true; // listed in the bot's !tickets menu
    char npName[256]{};
    // quick add
    char qaTitle[512]{}, qaSource[128]{}, projectItemSearch[256]{};
    int qaType = 1, qaPrio = 1;
    int projectItemSort = 0, projectItemPriority = 0;
    // discord page buffers
    char dcToken[512]{}; bool dcShowToken = false; bool dcLoaded = false;
    char dcAdminIds[2048]{};
    PreservedText dcAdminIdsValue;
    char dcServerId[64]{}, dcChanId[64]{}, dcMonName[128]{};
    char dcPatterns[32768]{}; bool dcPatternsLoaded = false;
    char dcNotifyGuild[64]{}, dcNotifyChan[64]{}; bool dcNotifyLoaded = false;
    // event popup
    char evTitle[256]{}, evNotes[512]{};
    int evKind = 0, evProject = 0;

    // Knowledge & Processing workspace. Project selectors use 0=all/global,
    // then project cache index + 1.
    char kbSearch[256]{};
    int kbProject = 0, kbKindFilter = 0, kbContextLevel = 2;
    char kbTitle[512]{}, kbPreamble[1024]{}, kbSummary[4096]{}, kbBody[16384]{};
    char kbTags[512]{}, kbSourceUri[1024]{};
    int kbNewKind = 0, kbFreshness = 0, kbConfidence = 0, kbVolatility = 0;
    char kbReportTitle[512]{}, kbReportBody[16384]{};
    int kbReportType = 0;
    char kbWorkflowTitle[512]{}, kbWorkflowObjective[4096]{};
    char kbWorkflowSuccess[2048]{}, kbWorkflowValidation[4096]{};
    char kbWorkflowNext[1024]{};
    int kbWorkflowContext = 0, kbWorkflowPhase = 0;

    int staleDays = 30, releaseDraftDays = 30, backupRetention = 14;
    int dashboardCompletedDays = 30;
    std::string maintenanceStatus;
};

static const char* kTypeNames[] = {"fix", "implementation", "reference", "note"};
static const char* kPromoteTypeNames[] = {"implementation", "fix", "note"};
static const char* kPrioNames[] = {"low", "normal", "high", "critical"};
static const char* kStatusNames[] = {"open", "in_progress", "blocked", "completed", "wont_do"};
static const char* kEvKinds[] = {"event", "release", "deadline"};
static const char* kKnowledgeKinds[] = {
    "reference", "research", "decision", "design", "learning", "status",
    "handoff", "synthesis", "distillation", "architecture", "project", "idea"
};
static const char* kFreshnessNames[] = {"timeless", "snapshot", "pointer"};
static const char* kConfidenceNames[] = {"stated", "high", "medium", "speculation"};
static const char* kVolatilityNames[] = {"stable", "slow", "fast"};
static const char* kReportTypes[] = {
    "project", "daily", "weekly", "monthly", "feedback", "release",
    "health", "validation", "handoff"
};
static const char* kWorkflowContexts[] = {"dev", "knowledge", "mixed"};
static const char* kWorkflowPhases[] = {"discover", "define", "develop", "deliver"};
static constexpr int kDashboardCompletedPresetCount = 6;
static constexpr int kDashboardCompletedPresetDays[kDashboardCompletedPresetCount] = {
    7, 30, 60, 90, 180, 365
};
static const char* kDashboardCompletedPresetLabels[kDashboardCompletedPresetCount] = {
    "7 days", "30 days", "60 days", "90 days", "6 months", "1 year"
};
static const char* kDashboardCompletedPresetCompact[kDashboardCompletedPresetCount] = {
    "7d", "30d", "60d", "90d", "6mo", "1y"
};

// ---------------------------------------------------------------------------
// small helpers

// Cross-file native UI helpers. These are internal to the devhub_app target;
// Gui.h remains the public application boundary.
void toast(App& a, const std::string& msg, bool err = false);
bool copyPacket(App& a, const std::string& packet,
                const std::string& successMessage);
nlohmann::json nonEmptyLines(const char* value);
void openUrl(const std::string& url);
void drawBodyWithLinks(const std::string& text, const ImVec4& color);
void drawItemAttachments(App& a, long long itemId);
void copyBuf(char* dst, size_t cap, const std::string& src);
void loadPreservedText(char* dst, size_t cap, const std::string& src,
                       PreservedText& value);
std::string preservedText(const char* buffer, const PreservedText& value);
std::string shortTs(const std::string& iso);
std::string dateAfterDays(int days);
bool activeItemStatus(const std::string& status);
Project* findProject(App& a, long long id);
int projectIndexForId(const App& a, long long id);
void openReviewSelection(App& a, long long projectId);
void openEditPendingSuggestion(App& a, const Msg& message);
Ticket& ticketInfo(App& a, long long itemId);
void jumpToItem(App& a, long long projectId, long long itemId);
void chip(const char* text, const ImVec4& color);
void launchAction(App& a, const Project& p, const std::string& kind,
                  const std::string& command, const std::string& stdinData);
bool sectionHeader(App& a, const char* label, const std::string& key,
                   bool defaultOpen, ImGuiTreeNodeFlags flags = 0);
bool projectCombo(App& a, const char* label, int* index,
                  float width = 220.0f);
bool projectIdCombo(App& a, const char* label, long long* projectId,
                    float width = 320.0f);
long long knowledgeProjectId(const App& a, int selector);
bool knowledgeProjectCombo(App& a, const char* label, int* selector,
                           float width = 220.0f);

void drawDiscord(App& a);
void drawKnowledge(App& a);
void drawModals(App& a);

} // namespace devhub
