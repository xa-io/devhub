#include "Ops.h"
#include "PacketOps.h"
#include "TicketImageStore.h"
#include "devhub/TicketImage.h"
#include "devhub/Util.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace devhub {
namespace {

constexpr std::size_t kMaxImportItems = 200;
constexpr std::size_t kMaxImportTextBytes = 1024 * 1024;
constexpr std::size_t kMaxImportAttachmentBytes = 128 * 1024 * 1024;

struct ImportProject {
    json row;
    long long destinationId = 0;
    bool create = false;
};

struct ImportSource {
    json row;
    long long destinationId = 0;
    bool create = false;
    std::string destinationName;
};

struct ImportItem {
    json row;
    bool duplicate = false;
};

struct ImportMessage {
    json row;
    long long destinationId = 0;
    long long destinationChannelId = 0;
    bool duplicate = false;
};

struct ImportCard {
    json row;
    long long destinationId = 0;
    bool duplicate = false;
};

struct ImportAttachment {
    json row;
    TicketImportFileReadResult sourceFile;
    bool duplicate = false;
};

json importFailure(const std::string& code, const std::string& error) {
    return json{{"ok", false}, {"code", code}, {"error", error}};
}

long long integerField(const json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || found->is_null()) return 0;
    if (!found->is_number_integer())
        throw std::runtime_error(std::string("invalid integer field: ") + key);
    return found->get<long long>();
}

double realField(const json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || found->is_null()) return 0.0;
    if (!found->is_number())
        throw std::runtime_error(std::string("invalid numeric field: ") + key);
    return found->get<double>();
}

std::string stringField(const json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || found->is_null()) return {};
    if (!found->is_string())
        throw std::runtime_error(std::string("invalid text field: ") + key);
    return found->get<std::string>();
}

std::string placeholders(std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) result.push_back(',');
        result.push_back('?');
    }
    return result;
}

json selectedRows(SQLite::Database& database, const std::string& sql,
                  const std::vector<long long>& itemIds) {
    SQLite::Statement query(database, sql);
    for (std::size_t index = 0; index < itemIds.size(); ++index)
        query.bind(static_cast<int>(index + 1), itemIds[index]);
    return Db::rowsToJson(query);
}

json allRows(SQLite::Database& database, const std::string& sql) {
    SQLite::Statement query(database, sql);
    return Db::rowsToJson(query);
}

bool sourceDatabaseHealthy(SQLite::Database& source, std::string& error) {
    SQLite::Statement version(source, "PRAGMA user_version");
    if (!version.executeStep() || version.getColumn(0).getInt() != 14) {
        error = "historical DevHub database must use schema v14";
        return false;
    }
    SQLite::Statement integrity(source, "PRAGMA integrity_check");
    if (!integrity.executeStep() ||
        integrity.getColumn(0).getString() != "ok" ||
        integrity.executeStep()) {
        error = "historical DevHub database failed integrity_check";
        return false;
    }
    SQLite::Statement foreignKeys(source, "PRAGMA foreign_key_check");
    if (foreignKeys.executeStep()) {
        error = "historical DevHub database has foreign-key errors";
        return false;
    }
    return true;
}

bool validStatus(const std::string& status) {
    return status == "open" || status == "in_progress" ||
           status == "blocked" || status == "completed";
}

bool validInactiveCard(const json& row) {
    const std::string post = stringField(row, "post_state");
    const std::string edit = stringField(row, "edit_state");
    return (post == "posted" || post == "failed") &&
           (edit == "idle" || edit == "failed");
}

bool addTextBudget(const json& value, std::size_t& total) {
    if (value.is_string()) {
        const std::size_t bytes = value.get_ref<const std::string&>().size();
        if (bytes > kMaxImportTextBytes || total > kMaxImportTextBytes - bytes)
            return false;
        total += bytes;
        return true;
    }
    if (value.is_array()) {
        for (const json& child : value)
            if (!addTextBudget(child, total)) return false;
    } else if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (total > kMaxImportTextBytes - key.size()) return false;
            total += key.size();
            if (!addTextBudget(child, total)) return false;
        }
    }
    return true;
}

long long canonicalDestinationSource(Db::Held held, Db* db,
                                     long long sourceId) {
    std::set<long long> seen;
    long long current = sourceId;
    for (int depth = 0; depth < 64 && current > 0; ++depth) {
        if (!seen.insert(current).second)
            throw std::runtime_error(
                "destination contributor merge lineage contains a cycle");
        SQLite::Statement exists(db->raw(held),
            "SELECT 1 FROM sources WHERE id=?");
        exists.bind(1, current);
        if (!exists.executeStep()) return 0;
        SQLite::Statement merged(db->raw(held),
            "SELECT target_source_id FROM source_merges WHERE source_id=?");
        merged.bind(1, current);
        if (!merged.executeStep()) return current;
        current = merged.getColumn(0).getInt64();
    }
    throw std::runtime_error(
        "destination contributor merge lineage is invalid or too deep");
}

bool sameText(const json& source, const json& destination,
              std::initializer_list<const char*> fields) {
    for (const char* field : fields)
        if (stringField(source, field) != stringField(destination, field))
            return false;
    return true;
}

bool sameInteger(const json& source, const json& destination,
                 std::initializer_list<const char*> fields) {
    for (const char* field : fields)
        if (integerField(source, field) != integerField(destination, field))
            return false;
    return true;
}

json oneRow(SQLite::Statement& query) {
    if (!query.executeStep()) return nullptr;
    return Db::rowToJson(query);
}

long long destinationCount(Db::Held held, Db* db, const std::string& sql,
                           long long id) {
    SQLite::Statement query(db->raw(held), sql);
    query.bind(1, id);
    if (!query.executeStep()) return -1;
    return query.getColumn(0).getInt64();
}

bool removeCreatedFiles(const std::vector<std::string>& paths,
                        std::string& cleanupError) {
    bool ok = true;
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        std::error_code error;
        const bool removed = std::filesystem::remove(
            std::filesystem::path(*it), error);
        if (error || !removed) {
            ok = false;
            if (cleanupError.empty())
                cleanupError = "could not remove an uncommitted attachment file";
        }
    }
    return ok;
}

} // namespace

json importWorkItems(Db* db, const json& payload) {
    if (!db) return importFailure("invalid_request", "database is unavailable");
    bool destinationPhase = false;
    try {
        if (!payload.is_object())
            return importFailure("invalid_request", "import payload must be an object");
        for (const auto& entry : payload.items()) {
            const std::string& key = entry.key();
            if (key != "source_data_root" && key != "item_ids" &&
                key != "add_projects")
                return importFailure(
                    "invalid_request", "import payload contains an unsupported field: " + key);
        }
        if (!payload.contains("source_data_root") ||
            !payload["source_data_root"].is_string())
            return importFailure(
                "invalid_request", "source_data_root must be a string");
        const std::string sourceDataRoot =
            trim(payload["source_data_root"].get<std::string>());
        if (!payload.contains("item_ids") || !payload["item_ids"].is_array())
            return importFailure("invalid_request", "item_ids must be an array");
        if (payload["item_ids"].empty() ||
            payload["item_ids"].size() > kMaxImportItems)
            return importFailure(
                "invalid_request", "item_ids must contain 1 to 200 IDs");

        std::vector<long long> itemIds;
        itemIds.reserve(payload["item_ids"].size());
        for (const json& value : payload["item_ids"]) {
            if (!value.is_number_integer())
                return importFailure("invalid_request", "every item_id must be an integer");
            const long long id = value.get<long long>();
            if (id <= 0 || id > std::numeric_limits<int>::max())
                return importFailure(
                    "invalid_request", "every item_id must be a positive 32-bit integer");
            itemIds.push_back(id);
        }
        std::sort(itemIds.begin(), itemIds.end());
        if (std::adjacent_find(itemIds.begin(), itemIds.end()) != itemIds.end())
            return importFailure("invalid_request", "item_ids must be unique");

        std::string pathError;
        const std::string sourceDatabasePath =
            ticketImportDatabasePath(sourceDataRoot, pathError);
        if (sourceDatabasePath.empty())
            return importFailure("invalid_request", pathError);
        std::error_code equivalentError;
        const bool sameDatabase = std::filesystem::equivalent(
            std::filesystem::path(sourceDatabasePath),
            std::filesystem::path(db->path()), equivalentError);
        if (equivalentError)
            return importFailure(
                "invalid_request", "could not compare source and destination database paths");
        if (sameDatabase)
            return importFailure(
                "invalid_request", "the running DevHub database cannot be its own import source");

        SQLite::Database source(sourceDatabasePath, SQLite::OPEN_READONLY);
        source.exec("PRAGMA query_only=ON");
        source.exec("PRAGMA foreign_keys=ON");
        source.exec("PRAGMA busy_timeout=5000");
        SQLite::Transaction sourceSnapshot(source);
        std::string sourceError;
        if (!sourceDatabaseHealthy(source, sourceError))
            return importFailure("invalid_request", sourceError);

        const std::string in = "(" + placeholders(itemIds.size()) + ")";
        json itemRows = selectedRows(source,
            "SELECT id,project_id,type,title,body,status,priority,source_id,"
            "credited,origin,due_date,tags,created_at,updated_at,completed_at,"
            "review_date,blocked_reason FROM items WHERE id IN " + in +
            " ORDER BY id", itemIds);
        if (itemRows.size() != itemIds.size())
            return importFailure(
                "not_found", "one or more selected historical work items do not exist");

        json projectRows = allRows(source,
            "SELECT id,name,slug,description,color,archived,sort_order,"
            "created_at,updated_at,discord_tickets FROM projects ORDER BY id");
        SQLite::Statement linkedSources(source,
            "SELECT DISTINCT s.id,s.name,s.platform,s.handle,s.notes,s.created_at "
            "FROM sources s WHERE s.id IN (SELECT source_id FROM item_sources "
            "WHERE item_id IN " + in + ") OR s.id IN (SELECT source_id FROM items "
            "WHERE id IN " + in + ") ORDER BY s.id");
        int sourceBind = 1;
        for (long long id : itemIds) linkedSources.bind(sourceBind++, id);
        for (long long id : itemIds) linkedSources.bind(sourceBind++, id);
        json sourceRows = Db::rowsToJson(linkedSources);
        json itemSourceRows = selectedRows(source,
            "SELECT item_id,source_id,credited,added_at FROM item_sources "
            "WHERE item_id IN " + in + " ORDER BY item_id,source_id", itemIds);
        json eventRows = selectedRows(source,
            "SELECT id,project_id,item_id,title,kind,date,notes,created_at "
            "FROM events WHERE item_id IN " + in + " ORDER BY item_id,id", itemIds);
        json messageRows = selectedRows(source,
            "SELECT id,channel_row_id,message_id,author,author_id,content,posted_at,"
            "ingested_at,kind,score,matched,state,item_id,inferred_project_id,"
            "notify_channel_id,notify_message_id,admin_note,manual_target_message_id,"
            "manual_command_state,manual_attempts,manual_next_retry_at,"
            "manual_last_error,manual_result_kind,manual_result_message_row_id,"
            "manual_result_item_id,manual_result_images_queued,manual_effect_state,"
            "manual_effect_attempts,manual_effect_next_retry_at,manual_effect_error,"
            "manual_effect_updated_at FROM discord_messages WHERE item_id IN " +
            in + " ORDER BY id", itemIds);
        json channelRows = selectedRows(source,
            "SELECT DISTINCT c.id,c.channel_id,c.guild_id,c.guild_name,c.channel_name,"
            "c.project_id,c.enabled,c.created_at FROM discord_channels c WHERE c.id IN "
            "(SELECT channel_row_id FROM discord_messages WHERE item_id IN " + in +
            ") ORDER BY c.id", itemIds);
        json cardRows = selectedRows(source,
            "SELECT id,discord_message_row_id,item_id,notify_channel_id,"
            "notify_message_id,post_state,post_attempts,post_revision,"
            "post_next_retry_at,post_last_error,edit_state,edit_attempts,"
            "edit_revision,edit_next_retry_at,edit_last_error,edit_updated_at,"
            "created_at,updated_at FROM discord_notify_cards WHERE item_id IN " +
            in + " ORDER BY id", itemIds);
        json attachmentRows = selectedRows(source,
            "SELECT id,discord_message_row_id,item_id,source_channel_id,"
            "source_message_id,attachment_id,source_role,original_filename,"
            "content_type,declared_size,width,height,state,relative_path,actual_size,"
            "sha256,error,created_at,updated_at,saved_at FROM ticket_attachments "
            "WHERE item_id IN " + in + " ORDER BY item_id,id", itemIds);
        json dismissalRows = selectedRows(source,
            "SELECT d.card_row_id,d.operation,d.revision,d.dismissed_at,d.reason "
            "FROM discord_notify_failure_dismissals d JOIN discord_notify_cards n "
            "ON n.id=d.card_row_id WHERE n.item_id IN " + in +
            " ORDER BY d.card_row_id,d.operation,d.revision", itemIds);

        std::size_t textBudget = 0;
        for (const json* rows : {&itemRows, &projectRows, &sourceRows,
                                 &itemSourceRows, &eventRows, &messageRows,
                                 &channelRows, &cardRows, &attachmentRows,
                                 &dismissalRows})
            if (!addTextBudget(*rows, textBudget))
                return importFailure(
                    "payload_too_large", "historical ticket text exceeds the 1 MiB import budget");

        std::map<long long, ImportProject> projects;
        for (const json& row : projectRows) {
            const long long id = integerField(row, "id");
            if (id <= 0 || projects.count(id) != 0)
                return importFailure("invalid_request", "historical project identity is invalid");
            projects.emplace(id, ImportProject{row});
        }
        std::map<long long, ImportItem> items;
        std::set<long long> selectedItemIds(itemIds.begin(), itemIds.end());
        std::set<long long> selectedProjectIds;
        std::map<long long, int> completionEvents;
        for (const json& row : itemRows) {
            const long long id = integerField(row, "id");
            const long long projectId = integerField(row, "project_id");
            const std::string title = trim(stringField(row, "title"));
            const std::string status = stringField(row, "status");
            const long long priority = integerField(row, "priority");
            if (id <= 0 || selectedItemIds.count(id) == 0 ||
                projects.count(projectId) == 0 || title.empty() ||
                !validStatus(status) || priority < 1 || priority > 4)
                return importFailure(
                    "invalid_request", "historical work item fields are invalid");
            if (integerField(projects.at(projectId).row, "archived") != 0)
                return importFailure(
                    "conflict", "historical work items cannot import into an archived source project");
            const std::string completedAt = stringField(row, "completed_at");
            if ((status == "completed") != !completedAt.empty())
                return importFailure(
                    "invalid_request", "historical completion status and timestamp disagree");
            if (!items.emplace(id, ImportItem{row}).second)
                return importFailure("invalid_request", "historical item IDs are duplicated");
            selectedProjectIds.insert(projectId);
        }

        std::map<long long, ImportSource> sources;
        for (const json& row : sourceRows) {
            const long long id = integerField(row, "id");
            if (id <= 0 || trim(stringField(row, "name")).empty() ||
                !sources.emplace(id, ImportSource{row}).second)
                return importFailure(
                    "invalid_request", "historical contributor identity is invalid");
        }
        std::map<long long, std::vector<json>> linksByItem;
        for (const json& row : itemSourceRows) {
            const long long itemId = integerField(row, "item_id");
            const long long sourceId = integerField(row, "source_id");
            if (items.count(itemId) == 0 || sources.count(sourceId) == 0 ||
                (integerField(row, "credited") != 0 &&
                 integerField(row, "credited") != 1) ||
                stringField(row, "added_at").empty())
                return importFailure(
                    "invalid_request", "historical contributor link is invalid");
            linksByItem[itemId].push_back(row);
        }
        for (const auto& [itemId, item] : items) {
            const long long primary = integerField(item.row, "source_id");
            if (primary > 0 && sources.count(primary) == 0)
                return importFailure(
                    "invalid_request", "historical primary contributor is missing");
        }

        std::map<long long, std::vector<json>> eventsByItem;
        for (const json& row : eventRows) {
            const long long itemId = integerField(row, "item_id");
            if (items.count(itemId) == 0 ||
                integerField(row, "project_id") !=
                    integerField(items.at(itemId).row, "project_id"))
                return importFailure(
                    "invalid_request", "historical event ownership is invalid");
            eventsByItem[itemId].push_back(row);
            if (stringField(row, "kind") == "completion")
                ++completionEvents[itemId];
        }
        for (const auto& [itemId, item] : items) {
            const int expected = stringField(item.row, "status") == "completed" ? 1 : 0;
            if (completionEvents[itemId] != expected)
                return importFailure(
                    "invalid_request", "historical completion event lineage is incomplete");
        }

        std::map<long long, json> channels;
        for (const json& row : channelRows) {
            const long long id = integerField(row, "id");
            if (id <= 0 || stringField(row, "channel_id").empty() ||
                !channels.emplace(id, row).second)
                return importFailure(
                    "invalid_request", "historical Discord channel identity is invalid");
        }
        std::map<long long, ImportMessage> messages;
        std::map<std::string, long long> sourceMessageIds;
        std::map<long long, std::vector<long long>> messagesByItem;
        for (const json& row : messageRows) {
            const long long id = integerField(row, "id");
            const long long itemId = integerField(row, "item_id");
            const std::string messageId = stringField(row, "message_id");
            if (id <= 0 || items.count(itemId) == 0 || messageId.empty() ||
                channels.count(integerField(row, "channel_row_id")) == 0 ||
                stringField(row, "state") != "promoted" ||
                !messages.emplace(id, ImportMessage{row}).second ||
                !sourceMessageIds.emplace(messageId, id).second)
                return importFailure(
                    "invalid_request", "historical Discord message lineage is invalid");
            const long long inferred = integerField(row, "inferred_project_id");
            if (inferred > 0 && projects.count(inferred) == 0)
                return importFailure(
                    "invalid_request", "historical inferred project is missing");
            const std::string commandState =
                stringField(row, "manual_command_state");
            const std::string effectState =
                stringField(row, "manual_effect_state");
            if ((commandState != "" && commandState != "done" &&
                 commandState != "failed") ||
                (effectState != "" && effectState != "done" &&
                 effectState != "failed"))
                return importFailure(
                    "conflict", "active manual Discord commands cannot be imported");
            messagesByItem[itemId].push_back(id);
        }
        for (const auto& [id, message] : messages) {
            const long long resultMessage =
                integerField(message.row, "manual_result_message_row_id");
            const long long resultItem =
                integerField(message.row, "manual_result_item_id");
            if ((resultMessage > 0 && messages.count(resultMessage) == 0) ||
                (resultItem > 0 && items.count(resultItem) == 0))
                return importFailure(
                    "conflict", "manual Discord result lineage leaves the selected batch");
        }

        std::map<long long, ImportCard> cards;
        std::map<long long, std::vector<long long>> cardsByItem;
        for (const json& row : cardRows) {
            const long long id = integerField(row, "id");
            const long long itemId = integerField(row, "item_id");
            const long long messageId =
                integerField(row, "discord_message_row_id");
            if (id <= 0 || items.count(itemId) == 0 ||
                messages.count(messageId) == 0 || !validInactiveCard(row) ||
                stringField(row, "notify_channel_id").empty() ||
                stringField(row, "notify_message_id").empty() ||
                !cards.emplace(id, ImportCard{row}).second)
                return importFailure(
                    "conflict", "historical Discord card is incomplete or still active");
            cardsByItem[itemId].push_back(id);
        }
        std::map<long long, std::vector<json>> dismissalsByCard;
        for (const json& row : dismissalRows) {
            const long long cardId = integerField(row, "card_row_id");
            const std::string operation = stringField(row, "operation");
            if (cards.count(cardId) == 0 ||
                (operation != "create" && operation != "edit") ||
                integerField(row, "revision") < 0 ||
                stringField(row, "dismissed_at").empty())
                return importFailure(
                    "invalid_request", "historical notification acknowledgement is invalid");
            dismissalsByCard[cardId].push_back(row);
        }

        std::vector<ImportAttachment> attachments;
        std::map<long long, std::vector<std::size_t>> attachmentsByItem;
        std::map<long long, std::size_t> attachmentBytesByItem;
        std::size_t attachmentBytes = 0;
        attachments.reserve(attachmentRows.size());
        for (const json& row : attachmentRows) {
            const long long itemId = integerField(row, "item_id");
            const long long messageId =
                integerField(row, "discord_message_row_id");
            const long long actualSize = integerField(row, "actual_size");
            if (items.count(itemId) == 0 ||
                (messageId > 0 && messages.count(messageId) == 0) ||
                stringField(row, "state") != "saved" || actualSize <= 0 ||
                actualSize > static_cast<long long>(kMaxTicketAttachmentBytes))
                return importFailure(
                    "conflict", "only complete saved historical attachments can be imported");
            if (attachmentsByItem[itemId].size() >= 10 ||
                attachmentBytesByItem[itemId] >
                    50 * 1024 * 1024 - static_cast<std::size_t>(actualSize) ||
                attachmentBytes >
                    kMaxImportAttachmentBytes - static_cast<std::size_t>(actualSize))
                return importFailure(
                    "payload_too_large", "historical attachments exceed ticket or batch limits");
            TicketImportFileReadResult sourceFile =
                readTicketImportAttachmentFile(
                    sourceDataRoot, stringField(row, "relative_path"),
                    static_cast<std::size_t>(actualSize),
                    stringField(row, "sha256"));
            if (!sourceFile.ok)
                return importFailure("conflict", sourceFile.error);
            const std::size_t index = attachments.size();
            attachments.push_back(ImportAttachment{row, std::move(sourceFile)});
            attachmentsByItem[itemId].push_back(index);
            attachmentBytesByItem[itemId] += static_cast<std::size_t>(actualSize);
            attachmentBytes += static_cast<std::size_t>(actualSize);
        }

        SQLite::Statement selectedMerge(source,
            "SELECT COUNT(*) FROM item_merges WHERE source_item_id IN " + in +
            " OR target_item_id IN " + in);
        int mergeBind = 1;
        for (long long id : itemIds) selectedMerge.bind(mergeBind++, id);
        for (long long id : itemIds) selectedMerge.bind(mergeBind++, id);
        if (!selectedMerge.executeStep() ||
            selectedMerge.getColumn(0).getInt64() != 0)
            return importFailure(
                "conflict", "merge-linked historical items cannot be imported independently");
        SQLite::Statement selectedCleanup(source,
            "SELECT COUNT(*) FROM discord_review_control_cleanups c "
            "JOIN discord_notify_cards n ON n.id=c.card_row_id "
            "WHERE n.item_id IN " + in);
        for (std::size_t index = 0; index < itemIds.size(); ++index)
            selectedCleanup.bind(static_cast<int>(index + 1), itemIds[index]);
        if (!selectedCleanup.executeStep() ||
            selectedCleanup.getColumn(0).getInt64() != 0)
            return importFailure(
                "conflict", "active Discord review cleanup cannot be imported safely");
        SQLite::Statement selectedOrphans(source,
            "SELECT COUNT(*) FROM discord_notify_orphans o "
            "WHERE o.card_row_id IN (SELECT id FROM discord_notify_cards "
            "WHERE item_id IN " + in + ")");
        for (std::size_t index = 0; index < itemIds.size(); ++index)
            selectedOrphans.bind(static_cast<int>(index + 1), itemIds[index]);
        if (!selectedOrphans.executeStep() ||
            selectedOrphans.getColumn(0).getInt64() != 0)
            return importFailure(
                "conflict", "active Discord notification orphan cleanup cannot be imported safely");

        if (!sources.empty()) {
            std::string sourceIn = "(" + placeholders(sources.size()) + ")";
            SQLite::Statement sourceAliases(source,
                "SELECT COUNT(*) FROM source_merges WHERE source_id IN " +
                sourceIn);
            int aliasBind = 1;
            for (const auto& [id, ignored] : sources) {
                (void)ignored;
                sourceAliases.bind(aliasBind++, id);
            }
            if (!sourceAliases.executeStep() ||
                sourceAliases.getColumn(0).getInt64() != 0)
                return importFailure(
                    "conflict", "merged-away contributor aliases require a wider lineage import");
        }

        std::map<long long, std::string> explicitlyAddedProjects;
        const json addProjects = payload.value("add_projects", json::array());
        if (!addProjects.is_array() || addProjects.size() > 32)
            return importFailure(
                "invalid_request", "add_projects must be an array of at most 32 entries");
        for (const json& entry : addProjects) {
            if (!entry.is_object() || entry.size() != 2 ||
                !entry.contains("source_project_id") ||
                !entry.contains("name") ||
                !entry["source_project_id"].is_number_integer() ||
                !entry["name"].is_string())
                return importFailure(
                    "invalid_request", "each add_projects entry requires only source_project_id and name");
            const long long sourceProjectId =
                entry["source_project_id"].get<long long>();
            const std::string name = trim(entry["name"].get<std::string>());
            const auto project = projects.find(sourceProjectId);
            if (sourceProjectId <= 0 || project == projects.end() ||
                selectedProjectIds.count(sourceProjectId) == 0 ||
                name != stringField(project->second.row, "name") ||
                !explicitlyAddedProjects.emplace(sourceProjectId, name).second)
                return importFailure(
                    "invalid_request", "add_projects must identify a selected historical project exactly");
        }

        sourceSnapshot.commit();

        destinationPhase = true;
        auto lock = db->guard();
        const Db::Held held = lock.token();
        std::set<long long> referencedProjectIds = selectedProjectIds;
        for (const auto& [id, message] : messages) {
            (void)id;
            const long long inferred =
                integerField(message.row, "inferred_project_id");
            if (inferred > 0) referencedProjectIds.insert(inferred);
        }
        for (long long sourceProjectId : referencedProjectIds) {
            auto found = projects.find(sourceProjectId);
            if (found == projects.end())
                return importFailure(
                    "conflict", "a referenced historical project is missing");
            ImportProject& project = found->second;
            SQLite::Statement existing(db->raw(held),
                "SELECT id,archived FROM projects WHERE name=? COLLATE NOCASE "
                "ORDER BY id LIMIT 1");
            existing.bind(1, stringField(project.row, "name"));
            if (existing.executeStep()) {
                if (existing.getColumn(1).getInt() != 0)
                    return importFailure(
                        "conflict", "destination project is archived: " +
                            stringField(project.row, "name"));
                project.destinationId = existing.getColumn(0).getInt64();
                continue;
            }
            if (explicitlyAddedProjects.count(sourceProjectId) == 0)
                return importFailure(
                    "conflict", "destination project is missing and was not explicitly authorized: " +
                        stringField(project.row, "name"));
            project.create = true;
        }

        std::set<std::string> reservedSourceNames;
        std::map<long long, long long> destinationSourceOwners;
        for (auto& [sourceId, imported] : sources) {
            const std::string name = trim(stringField(imported.row, "name"));
            const std::string platform = trim(stringField(imported.row, "platform"));
            const std::string handle = trim(stringField(imported.row, "handle"));
            long long destinationId = 0;
            if (!platform.empty() && !handle.empty()) {
                SQLite::Statement stable(db->raw(held),
                    "SELECT id FROM sources WHERE platform=? COLLATE NOCASE "
                    "AND handle=? ORDER BY id LIMIT 1");
                stable.bind(1, platform);
                stable.bind(2, handle);
                if (stable.executeStep())
                    destinationId = canonicalDestinationSource(
                        held, db, stable.getColumn(0).getInt64());
            }
            if (destinationId == 0) {
                SQLite::Statement named(db->raw(held),
                    "SELECT id,platform,handle FROM sources WHERE name=? "
                    "COLLATE NOCASE ORDER BY id LIMIT 1");
                named.bind(1, name);
                if (named.executeStep()) {
                    const std::string existingPlatform =
                        named.getColumn(1).getString();
                    const std::string existingHandle =
                        named.getColumn(2).getString();
                    const bool reusable =
                        toLower(existingPlatform) == toLower(platform) &&
                        existingHandle == handle;
                    if (reusable)
                        destinationId = canonicalDestinationSource(
                            held, db, named.getColumn(0).getInt64());
                }
            }
            if (destinationId > 0) {
                if (!destinationSourceOwners.emplace(destinationId, sourceId).second)
                    return importFailure(
                        "conflict", "distinct historical contributors collapse to one destination identity");
                imported.destinationId = destinationId;
                continue;
            }

            std::string destinationName = name;
            auto nameAvailable = [&](const std::string& candidate) {
                if (reservedSourceNames.count(toLower(candidate)) != 0) return false;
                SQLite::Statement collision(db->raw(held),
                    "SELECT 1 FROM sources WHERE name=? COLLATE NOCASE");
                collision.bind(1, candidate);
                return !collision.executeStep();
            };
            if (!nameAvailable(destinationName)) {
                const std::string base = name + " (" +
                    (platform.empty() ? std::string("import") : platform) + ")";
                destinationName = base;
                for (int suffix = 2; !nameAvailable(destinationName); ++suffix)
                    destinationName = base + " #" + std::to_string(suffix);
            }
            reservedSourceNames.insert(toLower(destinationName));
            imported.create = true;
            imported.destinationName = destinationName;
        }

        std::size_t duplicateItems = 0;
        for (auto& [itemId, imported] : items) {
            const long long sourceProjectId =
                integerField(imported.row, "project_id");
            const ImportProject& project = projects.at(sourceProjectId);
            const long long sourcePrimary =
                integerField(imported.row, "source_id");
            const long long destinationPrimary = sourcePrimary > 0
                ? sources.at(sourcePrimary).destinationId : 0;
            SQLite::Statement existing(db->raw(held),
                "SELECT id,project_id,type,title,body,status,priority,"
                "COALESCE(source_id,0) AS source_id,credited,origin,due_date,tags,"
                "created_at,updated_at,completed_at,review_date,blocked_reason "
                "FROM items WHERE id=?");
            existing.bind(1, itemId);
            json destination = oneRow(existing);
            if (!destination.is_null()) {
                if (project.destinationId <= 0 ||
                    (sourcePrimary > 0 && destinationPrimary <= 0) ||
                    integerField(destination, "project_id") != project.destinationId ||
                    integerField(destination, "source_id") != destinationPrimary ||
                    !sameInteger(imported.row, destination,
                                 {"id", "priority", "credited"}) ||
                    !sameText(imported.row, destination,
                              {"type", "title", "body", "status", "origin",
                               "due_date", "tags", "created_at", "updated_at",
                               "completed_at", "review_date", "blocked_reason"}))
                    return importFailure(
                        "conflict", "existing work item ID conflicts with historical item ID " +
                            std::to_string(itemId));
                imported.duplicate = true;
                ++duplicateItems;
            }

            if (project.destinationId > 0) {
                SQLite::Statement identityConflict(db->raw(held),
                    "SELECT id FROM items WHERE project_id=? AND id<>? AND "
                    "(title=? COLLATE NOCASE OR (body!='' AND body=?)) "
                    "ORDER BY id LIMIT 1");
                identityConflict.bind(1, project.destinationId);
                identityConflict.bind(2, itemId);
                identityConflict.bind(3, stringField(imported.row, "title"));
                identityConflict.bind(4, stringField(imported.row, "body"));
                if (identityConflict.executeStep())
                    return importFailure(
                        "conflict", "existing title or body conflicts with historical item ID " +
                            std::to_string(itemId));
            }
        }

        for (const auto& [itemId, imported] : items) {
            if (!imported.duplicate) continue;
            const auto found = linksByItem.find(itemId);
            const std::size_t sourceCount = found == linksByItem.end()
                ? 0 : found->second.size();
            if (destinationCount(
                    held, db,
                    "SELECT COUNT(*) FROM item_sources WHERE item_id=?", itemId) !=
                static_cast<long long>(sourceCount))
                return importFailure(
                    "conflict", "existing contributor count conflicts for item ID " +
                        std::to_string(itemId));
            if (found == linksByItem.end()) continue;
            for (const json& link : found->second) {
                const long long destinationSource =
                    sources.at(integerField(link, "source_id")).destinationId;
                if (destinationSource <= 0)
                    return importFailure(
                        "conflict", "existing item requires a missing contributor");
                SQLite::Statement exact(db->raw(held),
                    "SELECT 1 FROM item_sources WHERE item_id=? AND source_id=? "
                    "AND credited=? AND added_at=?");
                exact.bind(1, itemId);
                exact.bind(2, destinationSource);
                exact.bind(3, integerField(link, "credited"));
                exact.bind(4, stringField(link, "added_at"));
                if (!exact.executeStep())
                    return importFailure(
                        "conflict", "existing contributor lineage conflicts for item ID " +
                            std::to_string(itemId));
            }
        }

        for (const auto& [itemId, imported] : items) {
            if (!imported.duplicate) continue;
            const auto found = eventsByItem.find(itemId);
            const std::size_t sourceCount = found == eventsByItem.end()
                ? 0 : found->second.size();
            if (destinationCount(
                    held, db, "SELECT COUNT(*) FROM events WHERE item_id=?", itemId) !=
                static_cast<long long>(sourceCount))
                return importFailure(
                    "conflict", "existing event count conflicts for item ID " +
                        std::to_string(itemId));
            if (found == eventsByItem.end()) continue;
            for (const json& event : found->second) {
                SQLite::Statement exact(db->raw(held),
                    "SELECT COUNT(*) FROM events WHERE project_id=? AND item_id=? "
                    "AND title=? AND kind=? AND date=? AND notes=? AND created_at=?");
                exact.bind(1, projects.at(integerField(event, "project_id")).destinationId);
                exact.bind(2, itemId);
                exact.bind(3, stringField(event, "title"));
                exact.bind(4, stringField(event, "kind"));
                exact.bind(5, stringField(event, "date"));
                exact.bind(6, stringField(event, "notes"));
                exact.bind(7, stringField(event, "created_at"));
                if (!exact.executeStep() || exact.getColumn(0).getInt64() != 1)
                    return importFailure(
                        "conflict", "existing event lineage conflicts for item ID " +
                            std::to_string(itemId));
            }
        }

        std::map<long long, long long> destinationChannels;
        for (const auto& [sourceChannelId, channel] : channels) {
            SQLite::Statement existing(db->raw(held),
                "SELECT id FROM discord_channels WHERE channel_id=?");
            existing.bind(1, stringField(channel, "channel_id"));
            if (!existing.executeStep())
                return importFailure(
                    "conflict", "required destination Discord channel is missing: " +
                        stringField(channel, "channel_id"));
            destinationChannels[sourceChannelId] =
                existing.getColumn(0).getInt64();
        }

        for (auto& [sourceMessageRowId, message] : messages) {
            const long long itemId = integerField(message.row, "item_id");
            message.destinationChannelId = destinationChannels.at(
                integerField(message.row, "channel_row_id"));
            SQLite::Statement existing(db->raw(held),
                "SELECT id,item_id FROM discord_messages WHERE message_id=?");
            existing.bind(1, stringField(message.row, "message_id"));
            const bool found = existing.executeStep();
            if (items.at(itemId).duplicate) {
                if (!found || existing.getColumn(1).getInt64() != itemId)
                    return importFailure(
                        "conflict", "existing Discord source lineage is incomplete for item ID " +
                            std::to_string(itemId));
                message.destinationId = existing.getColumn(0).getInt64();
                message.duplicate = true;
            } else if (found) {
                return importFailure(
                    "conflict", "Discord message already belongs to existing destination data: " +
                        stringField(message.row, "message_id"));
            }
            (void)sourceMessageRowId;
        }
        for (const auto& [itemId, imported] : items) {
            if (!imported.duplicate) continue;
            const std::size_t expected = messagesByItem.count(itemId) == 0
                ? 0 : messagesByItem.at(itemId).size();
            if (destinationCount(
                    held, db,
                    "SELECT COUNT(*) FROM discord_messages WHERE item_id=?", itemId) !=
                static_cast<long long>(expected))
                return importFailure(
                    "conflict", "existing Discord message count conflicts for item ID " +
                        std::to_string(itemId));
        }
        for (const auto& [sourceMessageRowId, message] : messages) {
            if (!message.duplicate) continue;
            SQLite::Statement existing(db->raw(held),
                "SELECT id,channel_row_id,message_id,author,author_id,content,"
                "posted_at,ingested_at,kind,score,matched,state,item_id,"
                "COALESCE(inferred_project_id,0) AS inferred_project_id,"
                "notify_channel_id,notify_message_id,admin_note,"
                "manual_target_message_id,manual_command_state,manual_attempts,"
                "manual_next_retry_at,manual_last_error,manual_result_kind,"
                "manual_result_message_row_id,manual_result_item_id,"
                "manual_result_images_queued,manual_effect_state,"
                "manual_effect_attempts,manual_effect_next_retry_at,"
                "manual_effect_error,manual_effect_updated_at "
                "FROM discord_messages WHERE id=?");
            existing.bind(1, message.destinationId);
            json destination = oneRow(existing);
            const long long sourceInferred =
                integerField(message.row, "inferred_project_id");
            const long long expectedInferred = sourceInferred > 0
                ? projects.at(sourceInferred).destinationId : 0;
            const long long sourceResult =
                integerField(message.row, "manual_result_message_row_id");
            const long long expectedResult = sourceResult > 0
                ? messages.at(sourceResult).destinationId : 0;
            if (destination.is_null() ||
                integerField(destination, "channel_row_id") !=
                    message.destinationChannelId ||
                integerField(destination, "item_id") !=
                    integerField(message.row, "item_id") ||
                integerField(destination, "inferred_project_id") !=
                    expectedInferred ||
                integerField(destination, "manual_result_message_row_id") !=
                    expectedResult ||
                !sameInteger(message.row, destination,
                             {"manual_attempts", "manual_result_item_id",
                              "manual_result_images_queued",
                              "manual_effect_attempts"}) ||
                realField(message.row, "score") != realField(destination, "score") ||
                !sameText(message.row, destination,
                          {"message_id", "author", "author_id", "content",
                           "posted_at", "ingested_at", "kind", "matched", "state",
                           "notify_channel_id", "notify_message_id", "admin_note",
                           "manual_target_message_id", "manual_command_state",
                           "manual_next_retry_at", "manual_last_error",
                           "manual_result_kind", "manual_effect_state",
                           "manual_effect_next_retry_at", "manual_effect_error",
                           "manual_effect_updated_at"}))
                return importFailure(
                    "conflict", "existing Discord message fields conflict for source " +
                        stringField(message.row, "message_id"));
            (void)sourceMessageRowId;
        }

        for (auto& [sourceCardId, card] : cards) {
            const long long itemId = integerField(card.row, "item_id");
            const long long messageId = messages.at(
                integerField(card.row, "discord_message_row_id")).destinationId;
            long long bySource = 0;
            long long byNotification = 0;
            if (messageId > 0) {
                SQLite::Statement sourceCard(db->raw(held),
                    "SELECT id FROM discord_notify_cards "
                    "WHERE discord_message_row_id=?");
                sourceCard.bind(1, messageId);
                if (sourceCard.executeStep())
                    bySource = sourceCard.getColumn(0).getInt64();
            }
            SQLite::Statement notificationCard(db->raw(held),
                "SELECT id FROM discord_notify_cards WHERE notify_channel_id=? "
                "AND notify_message_id=?");
            notificationCard.bind(1, stringField(card.row, "notify_channel_id"));
            notificationCard.bind(2, stringField(card.row, "notify_message_id"));
            if (notificationCard.executeStep())
                byNotification = notificationCard.getColumn(0).getInt64();
            if (bySource > 0 && byNotification > 0 && bySource != byNotification)
                return importFailure(
                    "conflict", "Discord card identities resolve to different destination rows");
            card.destinationId = bySource > 0 ? bySource : byNotification;
            if (items.at(itemId).duplicate) {
                if (card.destinationId <= 0)
                    return importFailure(
                        "conflict", "existing Discord card lineage is incomplete for item ID " +
                            std::to_string(itemId));
                card.duplicate = true;
            } else if (card.destinationId > 0) {
                return importFailure(
                    "conflict", "Discord notification card already exists in destination data");
            }
            (void)sourceCardId;
        }
        for (const auto& [itemId, imported] : items) {
            if (!imported.duplicate) continue;
            const std::size_t expected = cardsByItem.count(itemId) == 0
                ? 0 : cardsByItem.at(itemId).size();
            if (destinationCount(
                    held, db,
                    "SELECT COUNT(*) FROM discord_notify_cards WHERE item_id=?", itemId) !=
                static_cast<long long>(expected))
                return importFailure(
                    "conflict", "existing Discord card count conflicts for item ID " +
                        std::to_string(itemId));
        }
        for (const auto& [sourceCardId, card] : cards) {
            if (!card.duplicate) continue;
            SQLite::Statement existing(db->raw(held),
                "SELECT id,COALESCE(discord_message_row_id,0) AS discord_message_row_id,"
                "COALESCE(item_id,0) AS item_id,notify_channel_id,notify_message_id,"
                "post_state,post_attempts,post_revision,post_next_retry_at,"
                "post_last_error,edit_state,edit_attempts,edit_revision,"
                "edit_next_retry_at,edit_last_error,edit_updated_at,created_at,updated_at "
                "FROM discord_notify_cards WHERE id=?");
            existing.bind(1, card.destinationId);
            json destination = oneRow(existing);
            const long long expectedMessage = messages.at(
                integerField(card.row, "discord_message_row_id")).destinationId;
            if (destination.is_null() ||
                integerField(destination, "discord_message_row_id") != expectedMessage ||
                !sameInteger(card.row, destination,
                             {"item_id", "post_attempts", "post_revision",
                              "edit_attempts", "edit_revision"}) ||
                !sameText(card.row, destination,
                          {"notify_channel_id", "notify_message_id", "post_state",
                           "post_next_retry_at", "post_last_error", "edit_state",
                           "edit_next_retry_at", "edit_last_error", "edit_updated_at",
                           "created_at", "updated_at"}))
                return importFailure(
                    "conflict", "existing Discord card fields conflict for item ID " +
                        std::to_string(integerField(card.row, "item_id")));
            (void)sourceCardId;
        }

        const std::string destinationDataRoot =
            std::filesystem::path(db->path()).parent_path().string();
        for (ImportAttachment& attachment : attachments) {
            const long long itemId = integerField(attachment.row, "item_id");
            SQLite::Statement existing(db->raw(held),
                "SELECT id,COALESCE(discord_message_row_id,0) AS discord_message_row_id,"
                "item_id,source_channel_id,source_message_id,attachment_id,"
                "source_role,original_filename,content_type,declared_size,width,height,"
                "state,relative_path,actual_size,sha256,error,created_at,updated_at,saved_at "
                "FROM ticket_attachments WHERE source_message_id=? AND attachment_id=?");
            existing.bind(1, stringField(attachment.row, "source_message_id"));
            existing.bind(2, stringField(attachment.row, "attachment_id"));
            json destination = oneRow(existing);
            if (items.at(itemId).duplicate) {
                if (destination.is_null())
                    return importFailure(
                        "conflict", "existing attachment lineage is incomplete for item ID " +
                            std::to_string(itemId));
                const long long sourceMessage =
                    integerField(attachment.row, "discord_message_row_id");
                const long long expectedMessage = sourceMessage > 0
                    ? messages.at(sourceMessage).destinationId : 0;
                if (integerField(destination, "discord_message_row_id") !=
                        expectedMessage ||
                    !sameInteger(attachment.row, destination,
                                 {"item_id", "declared_size", "width", "height",
                                  "actual_size"}) ||
                    !sameText(attachment.row, destination,
                              {"source_channel_id", "source_message_id",
                               "attachment_id", "source_role", "original_filename",
                               "content_type", "state", "relative_path", "sha256",
                               "error", "created_at", "updated_at", "saved_at"}))
                    return importFailure(
                        "conflict", "existing attachment fields conflict for item ID " +
                            std::to_string(itemId));
                TicketImportFileReadResult destinationFile =
                    readTicketImportAttachmentFile(
                        destinationDataRoot,
                        stringField(attachment.row, "relative_path"),
                        static_cast<std::size_t>(
                            integerField(attachment.row, "actual_size")),
                        stringField(attachment.row, "sha256"));
                if (!destinationFile.ok)
                    return importFailure(
                        "conflict", "existing destination attachment bytes conflict: " +
                            destinationFile.error);
                attachment.duplicate = true;
            } else if (!destination.is_null()) {
                return importFailure(
                    "conflict", "attachment identity already exists in destination data");
            }
        }
        for (const auto& [itemId, imported] : items) {
            if (!imported.duplicate) continue;
            const std::size_t expected = attachmentsByItem.count(itemId) == 0
                ? 0 : attachmentsByItem.at(itemId).size();
            if (destinationCount(
                    held, db,
                    "SELECT COUNT(*) FROM ticket_attachments WHERE item_id=?", itemId) !=
                static_cast<long long>(expected))
                return importFailure(
                    "conflict", "existing attachment count conflicts for item ID " +
                        std::to_string(itemId));
        }

        for (const auto& [sourceCardId, card] : cards) {
            if (!card.duplicate) continue;
            const std::size_t expected = dismissalsByCard.count(sourceCardId) == 0
                ? 0 : dismissalsByCard.at(sourceCardId).size();
            if (destinationCount(
                    held, db,
                    "SELECT COUNT(*) FROM discord_notify_failure_dismissals "
                    "WHERE card_row_id=?", card.destinationId) !=
                static_cast<long long>(expected))
                return importFailure(
                    "conflict", "notification acknowledgement count conflicts");
            if (dismissalsByCard.count(sourceCardId) == 0) continue;
            for (const json& dismissal : dismissalsByCard.at(sourceCardId)) {
                SQLite::Statement exact(db->raw(held),
                    "SELECT 1 FROM discord_notify_failure_dismissals "
                    "WHERE card_row_id=? AND operation=? AND revision=? "
                    "AND dismissed_at=? AND reason=?");
                exact.bind(1, card.destinationId);
                exact.bind(2, stringField(dismissal, "operation"));
                exact.bind(3, integerField(dismissal, "revision"));
                exact.bind(4, stringField(dismissal, "dismissed_at"));
                exact.bind(5, stringField(dismissal, "reason"));
                if (!exact.executeStep())
                    return importFailure(
                        "conflict", "notification acknowledgement fields conflict");
            }
        }

        std::vector<std::string> createdFiles;
        std::vector<long long> createdProjectIds;
        std::size_t restoredAttachments = 0;
        try {
            SQLite::Transaction transaction(db->raw(held));
            for (auto& [sourceProjectId, project] : projects) {
                if (!project.create) continue;
                SQLite::Statement insert(db->raw(held),
                    "INSERT INTO projects(name,slug,description,color,archived,"
                    "sort_order,created_at,updated_at,discord_tickets) "
                    "VALUES(?,?,?,?,0,?,?,?,?)");
                insert.bind(1, stringField(project.row, "name"));
                insert.bind(2, stringField(project.row, "slug"));
                insert.bind(3, stringField(project.row, "description"));
                insert.bind(4, stringField(project.row, "color"));
                insert.bind(5, integerField(project.row, "sort_order"));
                insert.bind(6, stringField(project.row, "created_at"));
                insert.bind(7, stringField(project.row, "updated_at"));
                insert.bind(8, integerField(project.row, "discord_tickets"));
                insert.exec();
                project.destinationId = db->raw(held).getLastInsertRowid();
                createdProjectIds.push_back(project.destinationId);
                (void)sourceProjectId;
            }

            for (auto& [sourceId, imported] : sources) {
                if (!imported.create) continue;
                SQLite::Statement insert(db->raw(held),
                    "INSERT INTO sources(name,platform,handle,notes,created_at) "
                    "VALUES(?,?,?,?,?)");
                insert.bind(1, imported.destinationName);
                insert.bind(2, stringField(imported.row, "platform"));
                insert.bind(3, stringField(imported.row, "handle"));
                insert.bind(4, stringField(imported.row, "notes"));
                insert.bind(5, stringField(imported.row, "created_at"));
                insert.exec();
                imported.destinationId = db->raw(held).getLastInsertRowid();
                (void)sourceId;
            }

            for (const auto& [itemId, imported] : items) {
                if (imported.duplicate) continue;
                const long long projectId = projects.at(
                    integerField(imported.row, "project_id")).destinationId;
                const long long sourceId = integerField(imported.row, "source_id");
                const long long destinationSource = sourceId > 0
                    ? sources.at(sourceId).destinationId : 0;
                if (projectId <= 0 || (sourceId > 0 && destinationSource <= 0))
                    throw std::runtime_error(
                        "ticket import destination mapping was not completed");
                SQLite::Statement insert(db->raw(held),
                    "INSERT INTO items(id,project_id,type,title,body,status,priority,"
                    "source_id,credited,origin,due_date,tags,created_at,updated_at,"
                    "completed_at,review_date,blocked_reason) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                insert.bind(1, itemId);
                insert.bind(2, projectId);
                insert.bind(3, stringField(imported.row, "type"));
                insert.bind(4, stringField(imported.row, "title"));
                insert.bind(5, stringField(imported.row, "body"));
                insert.bind(6, stringField(imported.row, "status"));
                insert.bind(7, integerField(imported.row, "priority"));
                if (destinationSource > 0) insert.bind(8, destinationSource);
                else insert.bind(8);
                insert.bind(9, integerField(imported.row, "credited"));
                insert.bind(10, stringField(imported.row, "origin"));
                insert.bind(11, stringField(imported.row, "due_date"));
                insert.bind(12, stringField(imported.row, "tags"));
                insert.bind(13, stringField(imported.row, "created_at"));
                insert.bind(14, stringField(imported.row, "updated_at"));
                insert.bind(15, stringField(imported.row, "completed_at"));
                insert.bind(16, stringField(imported.row, "review_date"));
                insert.bind(17, stringField(imported.row, "blocked_reason"));
                insert.exec();
            }

            for (const auto& [itemId, links] : linksByItem) {
                if (items.at(itemId).duplicate) continue;
                for (const json& link : links) {
                    SQLite::Statement insert(db->raw(held),
                        "INSERT INTO item_sources(item_id,source_id,credited,added_at) "
                        "VALUES(?,?,?,?)");
                    insert.bind(1, itemId);
                    insert.bind(2, sources.at(
                        integerField(link, "source_id")).destinationId);
                    insert.bind(3, integerField(link, "credited"));
                    insert.bind(4, stringField(link, "added_at"));
                    insert.exec();
                }
            }

            for (const auto& [itemId, events] : eventsByItem) {
                if (items.at(itemId).duplicate) continue;
                for (const json& event : events) {
                    SQLite::Statement insert(db->raw(held),
                        "INSERT INTO events(project_id,item_id,title,kind,date,notes,"
                        "created_at) VALUES(?,?,?,?,?,?,?)");
                    insert.bind(1, projects.at(
                        integerField(event, "project_id")).destinationId);
                    insert.bind(2, itemId);
                    insert.bind(3, stringField(event, "title"));
                    insert.bind(4, stringField(event, "kind"));
                    insert.bind(5, stringField(event, "date"));
                    insert.bind(6, stringField(event, "notes"));
                    insert.bind(7, stringField(event, "created_at"));
                    insert.exec();
                }
            }

            for (auto& [sourceMessageRowId, message] : messages) {
                if (message.duplicate) continue;
                const long long inferredSourceProject =
                    integerField(message.row, "inferred_project_id");
                const long long inferredDestinationProject =
                    inferredSourceProject > 0
                    ? projects.at(inferredSourceProject).destinationId : 0;
                SQLite::Statement insert(db->raw(held),
                    "INSERT INTO discord_messages(channel_row_id,message_id,author,"
                    "author_id,content,posted_at,ingested_at,kind,score,matched,state,"
                    "item_id,inferred_project_id,notify_channel_id,notify_message_id,"
                    "admin_note,manual_target_message_id,manual_command_state,"
                    "manual_attempts,manual_next_retry_at,manual_last_error,"
                    "manual_result_kind,manual_result_message_row_id,"
                    "manual_result_item_id,manual_result_images_queued,"
                    "manual_effect_state,manual_effect_attempts,"
                    "manual_effect_next_retry_at,manual_effect_error,"
                    "manual_effect_updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?,?,?,?,?)");
                insert.bind(1, message.destinationChannelId);
                insert.bind(2, stringField(message.row, "message_id"));
                insert.bind(3, stringField(message.row, "author"));
                insert.bind(4, stringField(message.row, "author_id"));
                insert.bind(5, stringField(message.row, "content"));
                insert.bind(6, stringField(message.row, "posted_at"));
                insert.bind(7, stringField(message.row, "ingested_at"));
                insert.bind(8, stringField(message.row, "kind"));
                insert.bind(9, realField(message.row, "score"));
                insert.bind(10, stringField(message.row, "matched"));
                insert.bind(11, stringField(message.row, "state"));
                insert.bind(12, integerField(message.row, "item_id"));
                if (inferredDestinationProject > 0)
                    insert.bind(13, inferredDestinationProject);
                else insert.bind(13);
                insert.bind(14, stringField(message.row, "notify_channel_id"));
                insert.bind(15, stringField(message.row, "notify_message_id"));
                insert.bind(16, stringField(message.row, "admin_note"));
                insert.bind(17, stringField(message.row, "manual_target_message_id"));
                insert.bind(18, stringField(message.row, "manual_command_state"));
                insert.bind(19, integerField(message.row, "manual_attempts"));
                insert.bind(20, stringField(message.row, "manual_next_retry_at"));
                insert.bind(21, stringField(message.row, "manual_last_error"));
                insert.bind(22, stringField(message.row, "manual_result_kind"));
                insert.bind(23, integerField(message.row, "manual_result_item_id"));
                insert.bind(24, integerField(message.row, "manual_result_images_queued"));
                insert.bind(25, stringField(message.row, "manual_effect_state"));
                insert.bind(26, integerField(message.row, "manual_effect_attempts"));
                insert.bind(27, stringField(message.row, "manual_effect_next_retry_at"));
                insert.bind(28, stringField(message.row, "manual_effect_error"));
                insert.bind(29, stringField(message.row, "manual_effect_updated_at"));
                insert.exec();
                message.destinationId = db->raw(held).getLastInsertRowid();
                (void)sourceMessageRowId;
            }
            for (const auto& [sourceMessageRowId, message] : messages) {
                if (message.duplicate) continue;
                const long long result =
                    integerField(message.row, "manual_result_message_row_id");
                if (result <= 0) continue;
                SQLite::Statement update(db->raw(held),
                    "UPDATE discord_messages SET manual_result_message_row_id=? "
                    "WHERE id=?");
                update.bind(1, messages.at(result).destinationId);
                update.bind(2, message.destinationId);
                if (update.exec() != 1)
                    throw std::runtime_error(
                        "manual Discord result lineage changed during import");
                (void)sourceMessageRowId;
            }

            for (const ImportAttachment& attachment : attachments) {
                if (attachment.duplicate) continue;
                const long long sourceMessage =
                    integerField(attachment.row, "discord_message_row_id");
                const long long destinationMessage = sourceMessage > 0
                    ? messages.at(sourceMessage).destinationId : 0;
                TicketImageSaveResult saved = saveTicketAttachmentBytes(
                    db->path(), integerField(attachment.row, "item_id"),
                    stringField(attachment.row, "source_message_id"),
                    stringField(attachment.row, "attachment_id"),
                    stringField(attachment.row, "original_filename"),
                    stringField(attachment.row, "content_type"),
                    attachment.sourceFile.bytes);
                if (saved.created) createdFiles.push_back(saved.absolutePath);
                if (!saved.ok ||
                    saved.relativePath != stringField(attachment.row, "relative_path") ||
                    saved.actualSize != static_cast<std::size_t>(
                        integerField(attachment.row, "actual_size")) ||
                    saved.sha256 != stringField(attachment.row, "sha256") ||
                    saved.contentType != stringField(attachment.row, "content_type"))
                    throw std::runtime_error(saved.error.empty()
                        ? "restored attachment metadata differs from its historical record"
                        : saved.error);
                SQLite::Statement insert(db->raw(held),
                    "INSERT INTO ticket_attachments(discord_message_row_id,item_id,"
                    "source_channel_id,source_message_id,attachment_id,source_role,"
                    "original_filename,content_type,declared_size,width,height,state,"
                    "relative_path,actual_size,sha256,error,created_at,updated_at,saved_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                if (destinationMessage > 0) insert.bind(1, destinationMessage);
                else insert.bind(1);
                insert.bind(2, integerField(attachment.row, "item_id"));
                insert.bind(3, stringField(attachment.row, "source_channel_id"));
                insert.bind(4, stringField(attachment.row, "source_message_id"));
                insert.bind(5, stringField(attachment.row, "attachment_id"));
                insert.bind(6, stringField(attachment.row, "source_role"));
                insert.bind(7, stringField(attachment.row, "original_filename"));
                insert.bind(8, stringField(attachment.row, "content_type"));
                insert.bind(9, integerField(attachment.row, "declared_size"));
                insert.bind(10, integerField(attachment.row, "width"));
                insert.bind(11, integerField(attachment.row, "height"));
                insert.bind(12, stringField(attachment.row, "state"));
                insert.bind(13, saved.relativePath);
                insert.bind(14, static_cast<long long>(saved.actualSize));
                insert.bind(15, saved.sha256);
                insert.bind(16, stringField(attachment.row, "error"));
                insert.bind(17, stringField(attachment.row, "created_at"));
                insert.bind(18, stringField(attachment.row, "updated_at"));
                insert.bind(19, stringField(attachment.row, "saved_at"));
                insert.exec();
                ++restoredAttachments;
            }

            for (auto& [sourceCardId, card] : cards) {
                if (card.duplicate) continue;
                SQLite::Statement insert(db->raw(held),
                    "INSERT INTO discord_notify_cards(discord_message_row_id,item_id,"
                    "notify_channel_id,notify_message_id,post_state,post_attempts,"
                    "post_revision,post_next_retry_at,post_last_error,edit_state,"
                    "edit_attempts,edit_revision,edit_next_retry_at,edit_last_error,"
                    "edit_updated_at,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                insert.bind(1, messages.at(integerField(
                    card.row, "discord_message_row_id")).destinationId);
                insert.bind(2, integerField(card.row, "item_id"));
                insert.bind(3, stringField(card.row, "notify_channel_id"));
                insert.bind(4, stringField(card.row, "notify_message_id"));
                insert.bind(5, stringField(card.row, "post_state"));
                insert.bind(6, integerField(card.row, "post_attempts"));
                insert.bind(7, integerField(card.row, "post_revision"));
                insert.bind(8, stringField(card.row, "post_next_retry_at"));
                insert.bind(9, stringField(card.row, "post_last_error"));
                insert.bind(10, stringField(card.row, "edit_state"));
                insert.bind(11, integerField(card.row, "edit_attempts"));
                insert.bind(12, integerField(card.row, "edit_revision"));
                insert.bind(13, stringField(card.row, "edit_next_retry_at"));
                insert.bind(14, stringField(card.row, "edit_last_error"));
                insert.bind(15, stringField(card.row, "edit_updated_at"));
                insert.bind(16, stringField(card.row, "created_at"));
                insert.bind(17, stringField(card.row, "updated_at"));
                insert.exec();
                card.destinationId = db->raw(held).getLastInsertRowid();
                (void)sourceCardId;
            }

            for (const auto& [sourceCardId, dismissals] : dismissalsByCard) {
                if (cards.at(sourceCardId).duplicate) continue;
                for (const json& dismissal : dismissals) {
                    SQLite::Statement insert(db->raw(held),
                        "INSERT INTO discord_notify_failure_dismissals("
                        "card_row_id,operation,revision,dismissed_at,reason) "
                        "VALUES(?,?,?,?,?)");
                    insert.bind(1, cards.at(sourceCardId).destinationId);
                    insert.bind(2, stringField(dismissal, "operation"));
                    insert.bind(3, integerField(dismissal, "revision"));
                    insert.bind(4, stringField(dismissal, "dismissed_at"));
                    insert.bind(5, stringField(dismissal, "reason"));
                    insert.exec();
                }
            }

            transaction.commit();
            createdFiles.clear();
        } catch (const std::exception& writeError) {
            std::string cleanupError;
            removeCreatedFiles(createdFiles, cleanupError);
            throw std::runtime_error(
                std::string(writeError.what()) +
                (cleanupError.empty() ? std::string() : "; " + cleanupError));
        }

        json imported = json::array();
        json createdIds = json::array();
        json duplicateIds = json::array();
        json attachmentManifest = json::array();
        for (const auto& [itemId, item] : items) {
            imported.push_back(json{{"source_id", itemId},
                                    {"id", itemId},
                                    {"result", item.duplicate
                                        ? "duplicate" : "created"}});
            if (item.duplicate) duplicateIds.push_back(itemId);
            else createdIds.push_back(itemId);
        }
        for (const ImportAttachment& attachment : attachments) {
            attachmentManifest.push_back(json{
                {"item_id", integerField(attachment.row, "item_id")},
                {"relative_path", stringField(attachment.row, "relative_path")},
                {"actual_size", integerField(attachment.row, "actual_size")},
                {"sha256", stringField(attachment.row, "sha256")},
                {"result", attachment.duplicate ? "duplicate" : "restored"}});
        }
        return json{{"ok", true},
                    {"created_count", items.size() - duplicateItems},
                    {"duplicate_count", duplicateItems},
                    {"created_ids", std::move(createdIds)},
                    {"duplicate_ids", std::move(duplicateIds)},
                    {"projects_created", createdProjectIds},
                    {"attachments_restored", restoredAttachments},
                    {"attachments", std::move(attachmentManifest)},
                    {"verified_graph", json{
                        {"contributor_links", itemSourceRows.size()},
                        {"events", eventRows.size()},
                        {"discord_messages", messageRows.size()},
                        {"notification_cards", cardRows.size()},
                        {"notification_acknowledgements", dismissalRows.size()},
                        {"attachments", attachmentRows.size()}}},
                    {"items", std::move(imported)}};
    } catch (const std::exception& error) {
        return importFailure(
            destinationPhase ? "internal_error" : "invalid_request",
            error.what());
    }
}

} // namespace devhub
