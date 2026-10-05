#pragma once

#include "Db.h"
#include "DiscordAttachment.h"

#include <string>
#include <vector>

namespace devhub {

struct TicketAttachmentStageResult {
    int total = 0;
    int changed = 0;
};
TicketAttachmentStageResult stageTicketAttachmentsLocked(
    Db::Held held, Db* db, long long messageRowId,
    const std::vector<DiscordAttachmentMeta>& attachments, long long itemId = 0);

struct ManualCaptureClassification {
    std::string kind;
    std::string matched;
};

// Explicit manual intake always remains a candidate, while the configured
// detector decides whether its persisted kind is bug or suggestion.
ManualCaptureClassification classifyManualCaptureLocked(
    Db::Held held, Db* db, const std::string& content);

// Follow only completed manual-command result lineage while the caller holds
// the database proof. Used by command staging and restart-safe queue selection.
std::string resolvedManualCaptureTargetLocked(
    Db::Held held, Db* db, const std::string& targetMessageId);

} // namespace devhub
