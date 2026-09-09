#pragma once

#include "Db.h"

#include <string>

namespace devhub {

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
