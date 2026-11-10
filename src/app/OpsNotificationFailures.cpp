#include "Ingest.h"
#include "Db.h"
#include "DiscordBot.h"
#include "devhub/Util.h"

#include <exception>

namespace devhub {

NotifyFailureDismissResult clearAcknowledgedNotifyCardAlerts(
    Db* db, const std::vector<NotifyFailureAlertTarget>& targets) {
    if (!db || targets.empty()) return {};
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        int changed = 0;
        SQLite::Statement clear(db->raw(lk.token()), R"sql(
UPDATE discord_notify_failure_dismissals
   SET reason='cleared:' || reason
 WHERE card_row_id=? AND operation=? AND revision=?
   AND reason NOT LIKE 'cleared:%'
   AND EXISTS(
       SELECT 1 FROM discord_notify_cards n
        WHERE n.id=discord_notify_failure_dismissals.card_row_id
          AND ((operation='create' AND n.post_state='failed'
                AND n.post_revision=revision)
            OR (operation='edit' AND n.edit_state='failed'
                AND n.edit_revision=revision))))sql");
        for (const NotifyFailureAlertTarget& target : targets) {
            if (target.cardRowId <= 0 || target.revision < 0 ||
                (target.operation != "create" && target.operation != "edit"))
                return {}; // Roll back the entire snapshot on invalid input.
            clear.bind(1, target.cardRowId);
            clear.bind(2, target.operation);
            clear.bind(3, target.revision);
            changed += clear.exec();
            clear.reset();
        }
        tx.commit();
        return {true, changed};
    } catch (const std::exception& e) {
        appLog("[notify] acknowledged failure history could not be cleared: " +
               redactHttpUrls(e.what()));
        return {};
    }
}

namespace {

NotifyFailureDismissResult changeFailedReviewControlCleanups(
    Db* db, const std::vector<ReviewCleanupFailureTarget>& targets, bool retry) {
    if (!db || targets.empty()) return {};
    try {
        auto lk = db->guard();
        SQLite::Transaction tx(db->raw(lk.token()));
        const std::string now = nowIsoUtc();
        SQLite::Statement change(db->raw(lk.token()), retry ? R"sql(
UPDATE discord_review_control_cleanups
   SET cleanup_state='pending',cleanup_attempts=0,
       cleanup_revision=cleanup_revision+1,cleanup_next_retry_at='',
       cleanup_last_error='',updated_at=?
 WHERE id=? AND cleanup_revision=? AND cleanup_state='failed')sql" : R"sql(
UPDATE discord_review_control_cleanups
   SET cleanup_last_error='cleared:' || cleanup_last_error,
       cleanup_revision=cleanup_revision+1,updated_at=?
 WHERE id=? AND cleanup_revision=? AND cleanup_state='failed'
   AND cleanup_last_error NOT LIKE 'cleared:%')sql");
        int changed = 0;
        for (const auto& target : targets) {
            if (target.rowId <= 0 || target.revision < 0) return {};
            change.bind(1, now);
            change.bind(2, target.rowId);
            change.bind(3, target.revision);
            changed += change.exec();
            change.reset();
        }
        tx.commit();
        return {true, changed};
    } catch (const std::exception& e) {
        appLog("[review] bulk reaction cleanup action failed: " +
               redactHttpUrls(e.what()));
        return {};
    }
}

} // namespace

NotifyFailureDismissResult clearFailedReviewControlCleanupAlerts(
    Db* db, const std::vector<ReviewCleanupFailureTarget>& targets) {
    return changeFailedReviewControlCleanups(db, targets, false);
}

NotifyFailureDismissResult retryFailedReviewControlCleanups(
    Db* db, DiscordBot* bot,
    const std::vector<ReviewCleanupFailureTarget>& targets) {
    const auto result = changeFailedReviewControlCleanups(db, targets, true);
    if (result.ok && result.changed > 0 && bot) bot->requestNotifyCardPost();
    return result;
}

} // namespace devhub
