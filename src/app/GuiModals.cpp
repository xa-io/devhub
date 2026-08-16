#include "GuiInternal.h"

namespace devhub {

static std::string reviewBodyPreview(const std::string& body) {
    constexpr std::size_t kPreviewBytes = 900;
    if (body.size() <= kPreviewBytes) return body;

    // Back up from the byte bound to the start of the current UTF-8 code
    // point so the preview never leaves ImGui with a split character.
    std::size_t cut = kPreviewBytes;
    while (cut > 0 &&
           (static_cast<unsigned char>(body[cut]) & 0xc0u) == 0x80u)
        --cut;
    return body.substr(0, cut) + "\n\n[preview clipped after 900 bytes]";
}

static void drawReviewChoiceTooltip(const ReviewChoice& choice) {
    if (!ImGui::IsItemHovered()) return;

    constexpr float kTooltipWidth = 520.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f),
                                        ImVec2(kTooltipWidth, 560.0f));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kTooltipWidth - 36.0f);

    const std::string metadata = "I" + std::to_string(choice.id) +
        "  P" + std::to_string(choice.priority) + "  " + choice.status;
    ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
    ImGui::TextUnformatted(metadata.c_str());
    ImGui::PopStyleColor();

    ImGui::TextUnformatted(choice.title.empty() ? "(untitled)"
                                                : choice.title.c_str());
    ImGui::Separator();
    const std::string preview = reviewBodyPreview(choice.body);
    ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
    ImGui::TextUnformatted(preview.empty() ? "(no details)" : preview.c_str());
    ImGui::PopStyleColor();

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void drawModals(App& a) {
    if (a.reviewFor != 0 && !ImGui::IsPopupOpen("Choose AI review tickets"))
        ImGui::OpenPopup("Choose AI review tickets");
    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(
            "Choose AI review tickets", nullptr,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        Project* project = findProject(a, a.reviewFor);
        if (!project) {
            a.reviewFor = 0;
            a.reviewChoices.clear();
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("%s - active work", project->name.c_str());
            ImGui::TextColored(C_DIM,
                "Choose the whole project, a ticket type, or individual tickets. "
                "Completed work is never included. The standard review scope is "
                "bounded to the newest 50 active tickets per type.");

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##reviewSearch",
                "search active ticket titles and details...",
                a.reviewSearch, sizeof(a.reviewSearch));
            const std::string reviewNeedle = toLower(trim(a.reviewSearch));
            const auto matchesReviewSearch = [&](const ReviewChoice& choice) {
                if (reviewNeedle.empty()) return true;
                return toLower(choice.title).find(reviewNeedle) !=
                           std::string::npos ||
                       toLower(choice.body).find(reviewNeedle) !=
                           std::string::npos;
            };
            const std::size_t matchingCount = static_cast<std::size_t>(
                std::count_if(a.reviewChoices.begin(), a.reviewChoices.end(),
                              matchesReviewSearch));
            ImGui::TextColored(C_DIM,
                "%zu of %zu shown; search only filters this list, so hidden "
                "selections stay selected.",
                matchingCount, a.reviewChoices.size());

            auto selectedCount = [&]() {
                return static_cast<int>(std::count_if(
                    a.reviewChoices.begin(), a.reviewChoices.end(),
                    [](const ReviewChoice& choice) { return choice.selected; }));
            };
            bool allSelected = !a.reviewChoices.empty() &&
                selectedCount() == static_cast<int>(a.reviewChoices.size());
            bool wholeProject = allSelected;
            if (ImGui::Checkbox(
                    ("Entire project (" +
                     std::to_string(a.reviewChoices.size()) + ")").c_str(),
                    &wholeProject)) {
                const bool select = !allSelected;
                for (auto& choice : a.reviewChoices) choice.selected = select;
            }

            ImGui::SeparatorText("Ticket types");
            static const char* labels[] = {
                "Fixes", "Implementations", "References", "Notes"};
            for (int typeIndex = 0; typeIndex < 4; ++typeIndex) {
                int total = 0, checked = 0;
                for (const auto& choice : a.reviewChoices) {
                    if (choice.type != kTypeNames[typeIndex]) continue;
                    ++total;
                    if (choice.selected) ++checked;
                }
                if (total == 0) continue;
                ImGui::PushID(typeIndex + 9000);
                bool groupSelected = checked == total;
                const std::string label = std::string(labels[typeIndex]) +
                    " (" + std::to_string(checked) + "/" +
                    std::to_string(total) + ")";
                if (ImGui::Checkbox(label.c_str(), &groupSelected)) {
                    const bool select = checked != total;
                    for (auto& choice : a.reviewChoices)
                        if (choice.type == kTypeNames[typeIndex])
                            choice.selected = select;
                }
                ImGui::PopID();
                if (typeIndex < 3) ImGui::SameLine(0, 22);
            }

            ImGui::SeparatorText("Individual tickets");
            // Keep the selected-count line and Copy/Cancel row inside the
            // modal. The ticket list owns any overflow scrolling instead of
            // forcing the whole modal to scroll just to reach its actions.
            const float reviewFooterReserve =
                ImGui::GetTextLineHeightWithSpacing() +
                ImGui::GetFrameHeightWithSpacing() +
                ImGui::GetStyle().ItemSpacing.y * 2.0f;
            ImGui::BeginChild("##reviewChoices",
                              ImVec2(0, -reviewFooterReserve),
                              ImGuiChildFlags_Borders);
            if (a.reviewChoices.empty()) {
                ImGui::TextColored(C_DIM, "No active tickets are available.");
            } else if (matchingCount == 0) {
                ImGui::TextColored(C_DIM, "No tickets match this search.");
            }
            std::string previousType;
            for (auto& choice : a.reviewChoices) {
                if (!matchesReviewSearch(choice)) continue;
                if (choice.type != previousType) {
                    if (!previousType.empty()) ImGui::Separator();
                    ImGui::TextColored(typeColor(choice.type), "%s",
                                       choice.type.c_str());
                    previousType = choice.type;
                }
                ImGui::PushID(static_cast<int>(choice.id));
                ImGui::Checkbox("##selected", &choice.selected);
                ImGui::SameLine();
                ImGui::TextColored(C_DIM, "I%lld  P%d  %s", choice.id,
                                   choice.priority, choice.status.c_str());
                ImGui::SameLine(235);
                ImGui::TextUnformatted(choice.title.c_str());
                drawReviewChoiceTooltip(choice);
                ImGui::PopID();
            }
            ImGui::EndChild();

            const int count = selectedCount();
            ImGui::TextColored(count > 0 ? C_GREEN : C_ORANGE,
                               "%d of %zu selected", count,
                               a.reviewChoices.size());
            if (count == 0) ImGui::BeginDisabled();
            if (ImGui::Button("Copy selected review bundle")) {
                std::vector<long long> ids;
                ids.reserve(static_cast<std::size_t>(count));
                for (const auto& choice : a.reviewChoices)
                    if (choice.selected) ids.push_back(choice.id);
                const bool all = ids.size() == a.reviewChoices.size();
                std::string markdown = all
                    ? buildReviewMarkdown(a.db, a.reviewFor)
                    : buildReviewMarkdown(a.db, a.reviewFor, ids);
                if (markdown.empty()) {
                    const long long projectId = a.reviewFor;
                    toast(a,
                        "review selection changed or is no longer active; refreshed choices",
                        true);
                    openReviewSelection(a, projectId);
                } else {
                    copyPacket(a, markdown,
                        all ? "review bundle copied - all active project work"
                            : "review bundle copied - selected active tickets only");
                    a.reviewFor = 0;
                    a.reviewChoices.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            if (count == 0) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                a.reviewFor = 0;
                a.reviewChoices.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // Bulk acknowledgement is deliberately limited to Discord's exact
    // old-message edit-limit failures and has no remote side effect.
    if (!a.dismissOldNotifyAlertTargets.empty() &&
        !ImGui::IsPopupOpen("Dismiss old-message alerts?"))
        ImGui::OpenPopup("Dismiss old-message alerts?");
    if (ImGui::BeginPopupModal("Dismiss old-message alerts?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const int confirmedCount = static_cast<int>(
            a.dismissOldNotifyAlertTargets.size());
        ImGui::Text("Dismiss %d old-message edit alert(s)?",
                    confirmedCount);
        if (confirmedCount < a.dismissOldNotifyAlertTotal)
            ImGui::TextColored(C_ORANGE,
                "This bounded batch contains %d of %d active alerts.",
                confirmedCount, a.dismissOldNotifyAlertTotal);
        ImGui::TextWrapped(
            "Discord will not accept another edit for these older messages. "
            "This only acknowledges each current failed revision inside "
            "DevHub. It does not delete a notification card, edit Discord, "
            "remove a reaction, or erase its delivery result. The confirmed "
            "card/revision snapshot cannot include a newer failure.");
        if (ImGui::Button("Dismiss alerts")) {
            const NotifyFailureDismissResult result =
                dismissOldMessageNotifyCardAlerts(
                    a.db, a.dismissOldNotifyAlertTargets);
            if (!result.ok)
                toast(a, "old-message alerts could not be saved", true);
            else if (result.changed == confirmedCount)
                toast(a, std::to_string(result.changed) +
                          " old-message alert(s) dismissed");
            else
                toast(a, std::to_string(result.changed) + " of " +
                          std::to_string(confirmedCount) +
                          " alert(s) dismissed; changed revisions remain active",
                      true);
            a.needNotifyCounters = true;
            a.dismissOldNotifyAlertTargets.clear();
            a.dismissOldNotifyAlertTotal = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.dismissOldNotifyAlertTargets.clear();
            a.dismissOldNotifyAlertTotal = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Replacing a terminal card can intentionally create a new Discord
    // message. Confirm separately from a same-target delivery retry.
    if (a.recreateNotifyCardId != 0 &&
        !ImGui::IsPopupOpen("Recreate notification card?"))
        ImGui::OpenPopup("Recreate notification card?");
    if (ImGui::BeginPopupModal("Recreate notification card?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string oldChannelId;
        std::string oldMessageId;
        std::string newChannelId;
        {
            auto lk = a.db->guard();
            newChannelId = trim(a.db->getSetting(lk.token(), "notify_channel_id"));
            SQLite::Statement target(a.db->raw(lk.token()),
                "SELECT notify_channel_id,notify_message_id "
                "FROM discord_notify_cards WHERE id=?");
            target.bind(1, a.recreateNotifyCardId);
            if (target.executeStep()) {
                oldChannelId = target.getColumn(0).getString();
                oldMessageId = target.getColumn(1).getString();
            }
        }
        ImGui::Text("Recreate notification card N%lld?",
                    a.recreateNotifyCardId);
        ImGui::TextColored(C_DIM, "confirmed failure: %s revision %d",
            a.recreateNotifyCardOperation.c_str(),
            a.recreateNotifyCardRevision);
        ImGui::TextWrapped(
            "This stops managing the stored target (channel %s%s%s), retargets "
            "the row to saved channel %s, and creates a replacement message. "
            "If the old card still exists, remove it in Discord first; DevHub "
            "cannot prove or delete a persisted remote target here.",
            oldChannelId.empty() ? "(none)" : oldChannelId.c_str(),
            oldMessageId.empty() ? "" : ", message ",
            oldMessageId.empty() ? "" : oldMessageId.c_str(),
            newChannelId.empty() ? "(none)" : newChannelId.c_str());
        const bool validTarget = isDecimalDiscordSnowflake(newChannelId);
        if (!validTarget)
            ImGui::TextColored(C_RED,
                "Save a valid Discord notification channel before recreating.");
        if (!validTarget) ImGui::BeginDisabled();
        if (ImGui::Button("Recreate card")) {
            const long long cardId = a.recreateNotifyCardId;
            if (recreateFailedNotifyCard(
                    a.db, a.discord, cardId,
                    a.recreateNotifyCardOperation,
                    a.recreateNotifyCardRevision))
                toast(a, "notification card N" + std::to_string(cardId) +
                          " queued in the saved channel");
            else
                toast(a, "notification card could not be recreated; its "
                          "failure may have changed, so review it again",
                      true);
            a.needNotifyCounters = true;
            a.recreateNotifyCardId = 0;
            a.recreateNotifyCardOperation.clear();
            a.recreateNotifyCardRevision = -1;
            ImGui::CloseCurrentPopup();
        }
        if (!validTarget) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.recreateNotifyCardId = 0;
            a.recreateNotifyCardOperation.clear();
            a.recreateNotifyCardRevision = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // release confirm
    if (a.releaseFor != 0 && !ImGui::IsPopupOpen("Confirm release"))
        ImGui::OpenPopup("Confirm release");
    if (ImGui::BeginPopupModal("Confirm release", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        Project* p = findProject(a, a.releaseFor);
        if (!p) { a.releaseFor = 0; ImGui::CloseCurrentPopup(); }
        else {
            ImGui::Text("Send release for %s?", p->name.c_str());
            ImGui::TextColored(C_DIM, "%s", p->releaseCmd.c_str());
            ImGui::TextColored(C_ORANGE,
                "This pushes the release publicly (the script's double-'yes' "
                "confirmation will be answered for you).");
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##yes", "type yes to confirm", a.releaseBuf,
                                     sizeof(a.releaseBuf));
            bool yes = toLower(trim(a.releaseBuf)) == "yes";
            if (!yes) ImGui::BeginDisabled();
            if (ImGui::Button("Send release")) {
                launchAction(a, *p, "release", p->releaseCmd, "yes\nyes\n\n");
                a.releaseFor = 0;
                ImGui::CloseCurrentPopup();
            }
            if (!yes) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                a.releaseFor = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // version bump
    if (a.versionFor != 0 && !ImGui::IsPopupOpen("Update version"))
        ImGui::OpenPopup("Update version");
    if (ImGui::BeginPopupModal("Update version", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        Project* p = findProject(a, a.versionFor);
        if (!p) { a.versionFor = 0; ImGui::CloseCurrentPopup(); }
        else {
            VersionInfo vi = a.versions->get(p->id);
            ImGui::Text("%s - current local version: %s", p->name.c_str(),
                        vi.local.empty() ? "?" : vi.local.c_str());
            ImGui::TextColored(C_DIM, "%s", p->versionCmd.c_str());
            ImGui::TextWrapped(
                "Automatic changes affect only the final version number by exactly one "
                "within 0..999. They never carry into or borrow from earlier numbers.");
            ImGui::RadioButton(
                "Increase final version number by 1 (maximum 999)",
                &a.versionChoice, 0);
            ImGui::RadioButton(
                "Decrease final version number by 1 (minimum 0)",
                &a.versionChoice, 1);
            ImGui::RadioButton("Set the complete version manually:",
                               &a.versionChoice, 2);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##vv", a.versionBuf, sizeof(a.versionBuf));
            ImGui::TextColored(C_DIM,
                               "Only manual entry may change earlier version numbers.");
            if (ImGui::Button("Apply")) {
                std::string data = a.versionChoice == 0 ? "1\n\n"
                                   : a.versionChoice == 1 ? "2\n\n"
                                   : ("3\n" + std::string(a.versionBuf) + "\n\n");
                launchAction(a, *p, "version", p->versionCmd, data);
                a.versionFor = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                a.versionFor = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // delete item
    if (a.deleteItemId != 0 && !ImGui::IsPopupOpen("Delete item?"))
        ImGui::OpenPopup("Delete item?");
    if (ImGui::BeginPopupModal("Delete item?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete item #%lld permanently?", a.deleteItemId);
        if (ImGui::Button("Delete")) {
            bool deleted = false;
            bool hasNotifyCard = false;
            {
                auto lk = a.db->guard();
                SQLite::Statement card(a.db->raw(lk.token()),
                    "SELECT EXISTS(SELECT 1 FROM discord_notify_cards "
                    "WHERE item_id=?)");
                card.bind(1, a.deleteItemId);
                card.executeStep();
                hasNotifyCard = card.getColumn(0).getInt() != 0;
                if (!hasNotifyCard) {
                    SQLite::Statement del(a.db->raw(lk.token()),
                        "DELETE FROM items WHERE id=?");
                    del.bind(1, a.deleteItemId);
                    deleted = del.exec() == 1;
                }
            }
            if (hasNotifyCard) {
                toast(a,
                    "item has a Discord notification card; use won't do instead",
                    true);
            } else if (!deleted) {
                toast(a, "item was not deleted", true);
            } else {
                a.deleteItemId = 0;
                a.needItems = true;
                a.needProjects = true;
                a.needWork = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.deleteItemId = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // edit a pending Discord ticket in place. An explicit project makes this
    // save the canonical promotion; a cleared project saves notes only.
    if (a.editPendingMessageId != 0 &&
        !ImGui::IsPopupOpen("Edit pending ticket"))
        ImGui::OpenPopup("Edit pending ticket");
    ImGui::SetNextWindowSize(ImVec2(660, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Edit pending ticket", nullptr)) {
        ImGui::TextColored(C_DIM, "Discord source D%lld",
                           a.editPendingMessageId);
        projectCombo(a, "project", &a.editPendingProject, 360.0f);
        a.editPendingProjectId =
            (a.editPendingProject >= 0 &&
             a.editPendingProject < static_cast<int>(a.projects.size()))
                ? a.projects[a.editPendingProject].id : 0;
        ImGui::SameLine();
        if (ImGui::SmallButton("clear - keep pending")) {
            a.editPendingProject = -1;
            a.editPendingProjectId = 0;
        }

        Project* pendingProject = findProject(a, a.editPendingProjectId);
        const bool willPromote = pendingProject && !pendingProject->archived;
        if (willPromote) {
            ImGui::InputText("title", a.editPendingTitle,
                             sizeof(a.editPendingTitle));
            ImGui::Combo("type", &a.editPendingType, kPromoteTypeNames, 3);
            ImGui::Combo("priority", &a.editPendingPrio, kPrioNames, 4);
            ImGui::TextColored(
                C_ORANGE,
                "Save creates one credited ticket in the selected project.");
        } else {
            ImGui::TextColored(
                C_ORANGE,
                "No project selected: Save keeps this ticket pending and unassigned.");
        }

        ImGui::TextColored(C_DIM,
            "Ticket notes replace the pending source text used by the ticket and bot card.");
        ImGui::InputTextMultiline("ticket notes", a.editPendingContent,
                                  sizeof(a.editPendingContent),
                                  ImVec2(-1, 190));
        ImGui::InputTextMultiline("admin notes", a.editPendingAdminNote,
                                  sizeof(a.editPendingAdminNote),
                                  ImVec2(-1, 100));

        if (ImGui::Button(willPromote ? "Save as ticket" : "Save pending")) {
            PendingSuggestionEdit edit;
            edit.projectId = willPromote ? a.editPendingProjectId : 0;
            edit.title = a.editPendingTitle;
            edit.type = kPromoteTypeNames[a.editPendingType];
            edit.priority = a.editPendingPrio + 1;
            edit.content = a.editPendingContent;
            edit.adminNote = a.editPendingAdminNote;
            nlohmann::json result = savePendingSuggestionEdit(
                a.db, a.discord, a.editPendingMessageId, edit);
            if (!result.value("ok", false)) {
                toast(a, result.value("error", "pending ticket save failed"),
                      true);
            } else {
                const bool pending = result.value("pending", false);
                const long long itemId = result.value("item_id", 0LL);
                a.editPendingMessageId = 0;
                a.editPendingProject = -1;
                a.editPendingProjectId = 0;
                a.needInbox = true;
                a.needChannels = true;
                a.needProjects = true;
                a.needItems = true;
                a.needSources = true;
                a.needWork = true;
                a.ticketCache.clear();
                toast(a, pending
                    ? "pending ticket notes saved; project still required"
                    : "saved as ticket I" + std::to_string(itemId));
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.editPendingMessageId = 0;
            a.editPendingProject = -1;
            a.editPendingProjectId = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // edit item
    if (a.editItemId != 0 && !ImGui::IsPopupOpen("Edit item"))
        ImGui::OpenPopup("Edit item");
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Edit item", nullptr)) {
        ImGui::InputText("title", a.eiTitle, sizeof(a.eiTitle));
        ImGui::Combo("type", &a.eiType, kTypeNames, 4);
        ImGui::Combo("priority", &a.eiPrio, kPrioNames, 4);
        ImGui::Combo("status", &a.eiStatus, kStatusNames, 5);
        projectIdCombo(a, "project", &a.eiProjectId);
        if (a.eiProjectId != a.editItemProjectId)
            ImGui::TextColored(
                C_ORANGE,
                "Saving moves I%lld and all linked evidence to this project.",
                a.editItemId);
        ImGui::InputTextWithHint("due date", "YYYY-MM-DD", a.eiDue, sizeof(a.eiDue));
        ImGui::InputTextWithHint("review again", "YYYY-MM-DD", a.eiReview,
                                 sizeof(a.eiReview));
        ImGui::SameLine();
        if (ImGui::SmallButton("+7d"))
            copyBuf(a.eiReview, sizeof(a.eiReview), dateAfterDays(7));
        ImGui::SameLine();
        if (ImGui::SmallButton("+30d"))
            copyBuf(a.eiReview, sizeof(a.eiReview), dateAfterDays(30));
        ImGui::SameLine();
        if (ImGui::SmallButton("clear##review")) a.eiReview[0] = 0;
        if (a.eiStatus == 2 || a.eiBlocked[0])
            ImGui::InputTextWithHint("blocked reason", "what must happen before this can continue",
                                     a.eiBlocked, sizeof(a.eiBlocked));

        ImGui::SeparatorText("Contributors");
        for (size_t i = 0; i < a.editContributors.size();) {
            Contributor& contributor = a.editContributors[i];
            ImGui::PushID((int)contributor.id);
            ImGui::TextUnformatted(contributor.name.c_str());
            ImGui::SameLine(250);
            bool credited = contributor.credited != 0;
            if (ImGui::Checkbox("credited", &credited)) {
                auto lk = a.db->guard();
                setItemSourceCreditedLocked(lk.token(), a.db, a.editItemId, contributor.id, credited);
                contributor.credited = credited ? 1 : 0;
                a.needItems = a.needSources = a.needProjects = a.needWork = true;
            }
            ImGui::SameLine();
            bool remove = ImGui::SmallButton("remove");
            ImGui::PopID();
            if (remove) {
                auto lk = a.db->guard();
                removeItemSourceLocked(lk.token(), a.db, a.editItemId, contributor.id);
                a.editContributors.erase(a.editContributors.begin() + i);
                a.needSources = a.needProjects = a.needWork = true;
            } else ++i;
        }
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##addContributor", "add contributor name...",
                                 a.eiAddSource, sizeof(a.eiAddSource));
        ImGui::SameLine();
        if (ImGui::SmallButton("Add contributor") && a.eiAddSource[0]) {
            auto lk = a.db->guard();
            long long sid = ensureSourceLocked(lk.token(), a.db, a.eiAddSource, "", "");
            addItemSourceLocked(lk.token(), a.db, a.editItemId, sid);
            bool present = false;
            for (auto& c : a.editContributors) if (c.id == sid) present = true;
            if (!present) a.editContributors.push_back({sid, trim(a.eiAddSource), 0});
            a.eiAddSource[0] = 0;
            a.needSources = a.needProjects = a.needWork = true;
        }
        ImGui::TextColored(C_DIM,
            "Contributor add/remove/credit actions are saved immediately.");
        if (a.eiBodyValue.truncated) {
            ImGui::TextColored(
                C_ORANGE,
                "This body is %zu bytes, longer than the editor can hold. "
                "The preview is truncated and Save will preserve the complete "
                "original body unchanged.",
                a.eiBodyValue.original.size());
        }
        // InputTextMultiline can't word-wrap, so the body reads on a wrapped
        // View tab with clickable links; the raw editor is one tab away.
        if (ImGui::BeginTabBar("##eibody")) {
            if (ImGui::BeginTabItem("details")) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG2);
                ImGui::BeginChild("##eiview", ImVec2(0, 220),
                                  ImGuiChildFlags_Borders);
                drawBodyWithLinks(a.eiBody, C_TEXT);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("edit text")) {
                // ImGui text inputs cannot word-wrap (upstream limitation),
                // so the editor gets a live wrapped preview underneath -
                // the full text stays readable while typing.
                if (a.eiBodyValue.truncated) ImGui::BeginDisabled();
                ImGui::InputTextMultiline("##eiedit", a.eiBody,
                                          sizeof(a.eiBody), ImVec2(-1, 150));
                if (a.eiBodyValue.truncated) ImGui::EndDisabled();
                ImGui::TextColored(C_DIM, "live preview:");
                ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG2);
                ImGui::BeginChild("##eipreview", ImVec2(0, 150),
                                  ImGuiChildFlags_Borders);
                drawBodyWithLinks(a.eiBody, C_TEXT);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        drawItemAttachments(a, a.editItemId);
        if (ImGui::Button("Save")) {
            const long long savedItemId = a.editItemId;
            const long long destinationProjectId = a.eiProjectId;
            std::string destinationName =
                "P" + std::to_string(destinationProjectId);
            if (Project* destination = findProject(a, destinationProjectId))
                destinationName = destination->name;
            ItemEdit edit;
            edit.projectId = destinationProjectId;
            edit.title = a.eiTitle;
            edit.type = kTypeNames[a.eiType];
            edit.status = kStatusNames[a.eiStatus];
            edit.priority = a.eiPrio + 1;
            edit.dueDate = a.eiDue;
            edit.reviewDate = a.eiReview;
            edit.blockedReason = a.eiBlocked;
            edit.body = preservedText(a.eiBody, a.eiBodyValue);
            bool moved = false;
            bool saved = false;
            std::string error;
            {
                auto lk = a.db->guard();
                saved = saveItemEditLocked(lk.token(),
                    a.db, savedItemId, edit, &moved, error, a.discord);
            }
            if (!saved) {
                toast(a, error.empty() ? "item save failed" : error, true);
            } else {
                a.editItemId = 0;
                a.editItemProjectId = 0;
                a.eiProjectId = 0;
                a.needItems = true;
                a.needProjects = true;
                a.needWork = true;
                a.needCal = true;
                a.needSources = true;
                a.ticketCache.clear();
                toast(a, moved
                    ? "moved I" + std::to_string(savedItemId) + " to " +
                          destinationName
                    : "saved");
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.editItemId = 0;
            a.editItemProjectId = 0;
            a.eiProjectId = 0;
            ImGui::CloseCurrentPopup();
        }

        std::vector<Item*> mergeTargets;
        for (auto& candidate : a.items)
            if (candidate.id != a.editItemId && activeItemStatus(candidate.status))
                mergeTargets.push_back(&candidate);
        if (!mergeTargets.empty()) {
            ImGui::SeparatorText("Merge duplicate feedback");
            const char* preview = a.eiMergeTarget >= 0 &&
                                  a.eiMergeTarget < (int)mergeTargets.size()
                                      ? mergeTargets[a.eiMergeTarget]->title.c_str()
                                      : "choose the item to keep";
            ImGui::SetNextItemWidth(410);
            if (ImGui::BeginCombo("##mergeTarget", preview)) {
                for (int i = 0; i < (int)mergeTargets.size(); ++i)
                    if (ImGui::Selectable(mergeTargets[i]->title.c_str(),
                                          a.eiMergeTarget == i))
                        a.eiMergeTarget = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Merge into selected") && a.eiMergeTarget >= 0) {
                std::string error;
                bool ok;
                const long long mergeTargetId =
                    mergeTargets[a.eiMergeTarget]->id;
                {
                    auto lk = a.db->guard();
                    ok = mergeItemsLocked(lk.token(), a.db, a.editItemId,
                                          mergeTargetId, error);
                }
                if (ok) {
                    // Card rows move inside the merge transaction; compose the
                    // combined image counts/status after releasing the DB guard.
                    refreshTicketNotifyCard(a.db, a.discord, mergeTargetId);
                    a.editItemId = 0;
                    a.needItems = a.needProjects = a.needSources = a.needWork = true;
                    toast(a, "feedback merged; contributors preserved");
                    ImGui::CloseCurrentPopup();
                } else toast(a, error, true);
            }
        }
        ImGui::EndPopup();
    }

    // edit project
    if (a.editProject && !ImGui::IsPopupOpen("Edit project"))
        ImGui::OpenPopup("Edit project");
    ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Edit project", nullptr)) {
        auto preservedInput = [&](const char* label, const char* hint,
                                  char* buffer, size_t capacity,
                                  PreservedText& value) {
            if (value.truncated) {
                ImGui::TextColored(
                    C_ORANGE,
                    "%s is %zu bytes and cannot fit in this editor; Save "
                    "will preserve its complete original value.",
                    label, value.original.size());
                ImGui::BeginDisabled();
            }
            if (hint)
                ImGui::InputTextWithHint(label, hint, buffer, capacity);
            else
                ImGui::InputText(label, buffer, capacity);
            if (value.truncated) ImGui::EndDisabled();
        };
        ImGui::InputText("name", a.epName, sizeof(a.epName));
        ImGui::InputText("description", a.epDesc, sizeof(a.epDesc));
        preservedInput("working path", nullptr, a.epPath, sizeof(a.epPath),
                       a.epPathValue);
        ImGui::InputText("rules path", a.epRules, sizeof(a.epRules));
        ImGui::InputTextWithHint("Codex skills", "$repository-skill, $second-skill",
                                 a.epSkills, sizeof(a.epSkills));
        ImGui::TextColored(C_DIM, "DevHub adds the packet-specific workflow skill automatically.");
        ImGui::SeparatorText("Pipeline commands (run in the working path)");
        preservedInput("build", nullptr, a.epBuild, sizeof(a.epBuild),
                       a.epBuildValue);
        preservedInput("prep release", nullptr, a.epPrep, sizeof(a.epPrep),
                       a.epPrepValue);
        preservedInput("send release", nullptr, a.epRelease,
                       sizeof(a.epRelease), a.epReleaseValue);
        preservedInput("update version", nullptr, a.epVersion,
                       sizeof(a.epVersion), a.epVersionValue);
        preservedInput("working dir override", "blank = working path",
                       a.epCwd, sizeof(a.epCwd), a.epCwdValue);
        ImGui::SeparatorText("Version tracking");
        ImGui::InputTextWithHint("local version file", "csproj / json with the dev version",
                                 a.epVerFile, sizeof(a.epVerFile));
        ImGui::InputTextWithHint("published version url", "e.g. https://aethertek.io/x.json",
                                 a.epRemoteUrl, sizeof(a.epRemoteUrl));
        ImGui::InputTextWithHint("published version key", "InternalName or dotted json path",
                                 a.epRemoteKey, sizeof(a.epRemoteKey));
        ImGui::InputText("github url", a.epGithub, sizeof(a.epGithub));
        ImGui::InputTextWithHint("aliases", "comma separated, for Discord matching",
                                 a.epAliases, sizeof(a.epAliases));
        ImGui::Checkbox("list in the Discord !tickets menu", &a.epTickets);
        if (ImGui::Button("Save")) {
            bool saved = false;
            std::string saveError;
            const std::string path = preservedText(a.epPath, a.epPathValue);
            const std::string build = preservedText(a.epBuild, a.epBuildValue);
            const std::string prep = preservedText(a.epPrep, a.epPrepValue);
            const std::string release =
                preservedText(a.epRelease, a.epReleaseValue);
            const std::string version =
                preservedText(a.epVersion, a.epVersionValue);
            const std::string cwd = preservedText(a.epCwd, a.epCwdValue);
            {
                auto lk = a.db->guard();
                try {
                    SQLite::Statement up(a.db->raw(lk.token()),
                        "UPDATE projects SET name=?, description=?, path=?, rules_path=?, codex_skills=?, "
                        "build_command=?, prep_command=?, release_command=?, "
                        "version_command=?, build_cwd=?, local_version_file=?, "
                        "remote_version_url=?, remote_version_key=?, github_url=?, "
                        "aliases=?, discord_tickets=?, updated_at=? WHERE id=?");
                    up.bind(1, a.epName);
                    up.bind(2, a.epDesc);
                    up.bind(3, path);
                    up.bind(4, a.epRules);
                    up.bind(5, a.epSkills);
                    up.bind(6, build);
                    up.bind(7, prep);
                    up.bind(8, release);
                    up.bind(9, version);
                    up.bind(10, cwd);
                    up.bind(11, a.epVerFile);
                    up.bind(12, a.epRemoteUrl);
                    up.bind(13, a.epRemoteKey);
                    up.bind(14, a.epGithub);
                    up.bind(15, a.epAliases);
                    up.bind(16, a.epTickets ? 1 : 0);
                    up.bind(17, nowIsoUtc());
                    up.bind(18, a.selProject);
                    up.exec();
                    saved = true;
                } catch (const std::exception& e) {
                    saveError = e.what();
                }
            }
            if (!saved) {
                toast(a, saveError.find("UNIQUE") != std::string::npos
                             ? "another project already uses that name"
                             : "could not save project: " + saveError,
                      true);
            } else {
                a.editProject = false;
                a.needProjects = true;
                a.needWork = true;
                a.versions->refreshAsync();
                toast(a, "project saved");
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            a.editProject = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 40);
        ImGui::PushStyleColor(ImGuiCol_Text, C_RED);
        if (ImGui::Button("Delete project")) {
            bool deleted = false;
            bool hasNotifyCard = false;
            {
                auto lk = a.db->guard();
                SQLite::Statement card(a.db->raw(lk.token()),
                    "SELECT EXISTS(SELECT 1 FROM discord_notify_cards n "
                    "JOIN items i ON i.id=n.item_id WHERE i.project_id=?)");
                card.bind(1, a.selProject);
                card.executeStep();
                hasNotifyCard = card.getColumn(0).getInt() != 0;
                if (!hasNotifyCard) {
                    SQLite::Statement del(a.db->raw(lk.token()),
                        "DELETE FROM projects WHERE id=?");
                    del.bind(1, a.selProject);
                    deleted = del.exec() == 1;
                }
            }
            if (hasNotifyCard) {
                toast(a,
                    "project contains Discord notification cards; close items instead",
                    true);
            } else if (!deleted) {
                toast(a, "project was not deleted", true);
            } else {
                a.editProject = false;
                a.needProjects = true;
                a.needWork = true;
                a.page = Page::Dashboard;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    // day popup (calendar)
    if (!a.dayPopupDate.empty() && !ImGui::IsPopupOpen("Day"))
        ImGui::OpenPopup("Day");
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(640, 0), ImVec2(760, 720));
    if (ImGui::BeginPopupModal("Day", nullptr)) {
        ImGui::PushFont(a.fontBig);
        ImGui::TextUnformatted(a.dayPopupDate.c_str());
        ImGui::PopFont();
        ImGui::TextColored(C_DIM, "click an entry for its full timestamped details");
        auto it = a.calendar.find(a.dayPopupDate);
        if (it != a.calendar.end()) {
            for (size_t i = 0; i < it->second.size(); ++i) {
                auto& e = it->second[i];
                ImGui::PushID((int)i);
                ImVec4 kcol = e.kind == "completion" ? C_GREEN
                              : e.kind == "release"  ? C_PURPLE
                              : e.kind == "deadline" ? C_ORANGE
                              : e.kind == "build"    ? C_CYAN : C_ACCENT;
                chip(e.kind.c_str(), kcol);
                ImGui::SameLine();
                bool open = a.dayExpanded.count((int)i) > 0;
                std::string lbl = e.title;
                if (!e.project.empty()) lbl += "  (" + e.project + ")";
                if (ImGui::Selectable((lbl + "###entry").c_str(), open)) {
                    if (open) a.dayExpanded.erase((int)i);
                    else a.dayExpanded.insert((int)i);
                    open = !open;
                }
                if (e.eventId) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("del")) {
                        auto lk = a.db->guard();
                        SQLite::Statement del(a.db->raw(lk.token()),
                            "DELETE FROM events WHERE id=?");
                        del.bind(1, e.eventId);
                        del.exec();
                        lk.unlock();
                        a.needCal = true;
                    }
                }
                if (open) {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_BG3);
                    ImGui::BeginChild("detail", ImVec2(0, 0),
                                      ImGuiChildFlags_AutoResizeY |
                                          ImGuiChildFlags_Borders);
                    if (e.entry == "build") {
                        ImGui::TextColored(C_DIM, "started:");
                        ImGui::SameLine();
                        ImGui::TextUnformatted(shortTs(e.ts).c_str());
                        ImGui::SameLine(0, 18);
                        ImGui::TextColored(C_DIM, "result:");
                        ImGui::SameLine();
                        ImGui::TextColored(statusColor(e.status), "%s%s",
                                           e.status.c_str(),
                                           e.exitCode >= 0
                                               ? (" (exit " + std::to_string(e.exitCode) + ")").c_str()
                                               : "");
                    } else if (e.itemId != 0) {
                        // ticket-backed entry (completion or due date)
                        Ticket& t = ticketInfo(a, e.itemId);
                        if (!t.found) {
                            ImGui::TextColored(C_DIM,
                                "ticket #%lld no longer exists (deleted)",
                                e.itemId);
                        } else {
                            chip(t.type.c_str(), typeColor(t.type));
                            ImGui::SameLine();
                            chip(t.status.c_str(), statusColor(t.status));
                            ImGui::SameLine();
                            if (t.origin == "discord") {
                                chip(t.discordState == "promoted"
                                         ? "promoted from Discord"
                                         : "from Discord", C_PURPLE);
                                ImGui::SameLine();
                            }
                            if (!t.source.empty()) {
                                chip(("by " + t.source).c_str(), C_PURPLE);
                                ImGui::SameLine();
                            }
                            ImGui::NewLine();
                            ImGui::TextColored(C_DIM, "created:");
                            ImGui::SameLine();
                            ImGui::TextUnformatted(shortTs(t.created).c_str());
                            if (!t.completed.empty()) {
                                ImGui::SameLine(0, 18);
                                ImGui::TextColored(C_DIM, "completed:");
                                ImGui::SameLine();
                                ImGui::TextColored(C_GREEN, "%s",
                                                   shortTs(t.completed).c_str());
                            }
                            if (!t.body.empty()) {
                                std::string body = t.body.size() > 900
                                                       ? t.body.substr(0, 900) + " [...]"
                                                       : t.body;
                                drawBodyWithLinks(body, C_DIM);
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
                            if (ImGui::SmallButton("open ticket ->")) {
                                jumpToItem(a, t.projectId, e.itemId);
                                a.dayPopupDate.clear();
                                ImGui::PopStyleColor();
                                ImGui::EndChild();
                                ImGui::PopStyleColor();
                                ImGui::PopID();
                                ImGui::CloseCurrentPopup();
                                ImGui::EndPopup();
                                return;
                            }
                            ImGui::PopStyleColor();
                        }
                    } else {
                        // plain calendar event
                        if (!e.created.empty()) {
                            ImGui::TextColored(C_DIM, "logged:");
                            ImGui::SameLine();
                            ImGui::TextUnformatted(shortTs(e.created).c_str());
                        }
                        if (!e.notes.empty()) {
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextColored(C_DIM, "%s", e.notes.c_str());
                            ImGui::PopTextWrapPos();
                        }
                        if (e.notes.empty() && e.created.empty())
                            ImGui::TextColored(C_DIM, "(no further details)");
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored(C_DIM, "nothing on this day");
        }
        ImGui::SeparatorText("Add event");
        ImGui::InputText("title", a.evTitle, sizeof(a.evTitle));
        ImGui::Combo("kind", &a.evKind, kEvKinds, 3);
        int projIdx = a.evProject - 1;
        ImGui::SetNextItemWidth(220);
        std::string preview = projIdx >= 0 && projIdx < (int)a.projects.size()
                                  ? a.projects[projIdx].name : "no project";
        if (ImGui::BeginCombo("project", preview.c_str())) {
            if (ImGui::Selectable("no project", a.evProject == 0)) a.evProject = 0;
            for (int i = 0; i < (int)a.projects.size(); ++i)
                if (!a.projects[i].archived &&
                    ImGui::Selectable(a.projects[i].name.c_str(), a.evProject == i + 1))
                    a.evProject = i + 1;
            ImGui::EndCombo();
        }
        ImGui::InputText("notes", a.evNotes, sizeof(a.evNotes));
        if (ImGui::Button("Add") && a.evTitle[0]) {
            if (a.evProject < 0 ||
                a.evProject > static_cast<int>(a.projects.size()))
                a.evProject = 0;
            const long long eventProjectId =
                a.evProject > 0 ? a.projects[a.evProject - 1].id : 0;
            {
                auto lk = a.db->guard();
                SQLite::Statement ins(a.db->raw(lk.token()),
                    "INSERT INTO events(project_id,title,kind,date,notes,created_at) "
                    "VALUES(?,?,?,?,?,?)");
                if (eventProjectId > 0) ins.bind(1, eventProjectId);
                else ins.bind(1);
                ins.bind(2, a.evTitle);
                ins.bind(3, kEvKinds[a.evKind]);
                ins.bind(4, a.dayPopupDate);
                ins.bind(5, a.evNotes);
                ins.bind(6, nowIsoUtc());
                ins.exec();
                a.db->logActivity(lk.token(), "event_added", eventProjectId,
                                  std::string(a.evTitle) + " (" + a.dayPopupDate + ")");
            }
            a.needCal = true;
            a.evTitle[0] = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            a.dayPopupDate.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace devhub
