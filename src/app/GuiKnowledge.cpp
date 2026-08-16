#include "GuiInternal.h"

namespace devhub {

void drawKnowledge(App& a) {
    ImGui::PushFont(a.fontBig);
    ImGui::TextUnformatted("Knowledge & Processing");
    ImGui::PopFont();
    ImGui::TextColored(C_DIM,
        "Evidence-backed project memory, reconciliation, Codex reports, and resumable development state.");

    long long projectId = knowledgeProjectId(a, a.kbProject);
    if (ImGui::Button(ICON_REFRESH " refresh")) a.needKnowledge = true;
    ImGui::SameLine();
    if (ImGui::Button("Copy Codex context")) {
        std::string md = buildKnowledgeContext(a.db, projectId, a.kbSearch,
                                               a.kbContextLevel, 30);
        copyPacket(a, md, "progressive Codex context copied");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy report packet")) {
        std::string md = buildReportContext(
            a.db, projectId, "weekly", 30,
            nullptr, nullptr, nullptr, nullptr, true);
        copyPacket(a, md, "grounded report packet copied and registered");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies and registers the exact approval evidence snapshot");
    ImGui::SameLine();
    if (ImGui::Button("Copy resume packet")) {
        std::string md = buildWorkflowResume(a.db, projectId);
        copyPacket(a, md, "workflow resume packet copied");
    }

    ImGui::Spacing();
    bool filterChanged = false;
    filterChanged |= knowledgeProjectCombo(a, "Project", &a.kbProject, 230.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    const char* filterPreview = a.kbKindFilter > 0
        ? kKnowledgeKinds[a.kbKindFilter - 1] : "all kinds";
    if (ImGui::BeginCombo("Kind", filterPreview)) {
        if (ImGui::Selectable("all kinds", a.kbKindFilter == 0)) {
            a.kbKindFilter = 0; filterChanged = true;
        }
        for (int i = 0; i < IM_ARRAYSIZE(kKnowledgeKinds); ++i) {
            if (ImGui::Selectable(kKnowledgeKinds[i], a.kbKindFilter == i + 1)) {
                a.kbKindFilter = i + 1; filterChanged = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::InputTextWithHint("##kb_search", "search titles, summaries, bodies, claims...",
                                 a.kbSearch, sizeof(a.kbSearch),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
        filterChanged = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("Context L", &a.kbContextLevel, 0, 3)) {}
    if (filterChanged) a.needKnowledge = true;
    projectId = knowledgeProjectId(a, a.kbProject);

    const auto& health = a.knowledgeHealthState;
    if (!health.empty()) {
        ImGui::TextColored(health.value("critical", 0LL) > 0 ? C_RED :
                           health.value("warnings", 0LL) > 0 ? C_ORANGE : C_GREEN,
            "Health: %s | active %lld | claims %lld | stale %lld | conflicts %lld | hygiene %lld/100",
            health.value("status", "unknown").c_str(),
            health.value("active_nodes", 0LL), health.value("claims", 0LL),
            health.value("stale_nodes", 0LL) + health.value("stale_claims", 0LL),
            health.value("open_conflicts", 0LL), health.value("hygiene_score", 0LL));
    }
    ImGui::Separator();

    if (!ImGui::BeginTabBar("knowledge_tabs")) return;

    if (ImGui::BeginTabItem("Library")) {
        if (ImGui::CollapsingHeader("Quick capture", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextColored(C_DIM,
                "For claim-level source blocks and reconciliation, use the DevHub Knowledge Codex skill.");
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("Title", "self-contained knowledge title",
                                     a.kbTitle, sizeof(a.kbTitle));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo("Kind##new", &a.kbNewKind, kKnowledgeKinds,
                         IM_ARRAYSIZE(kKnowledgeKinds));
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("Future-reader context", "why a future Codex session needs this",
                                     a.kbPreamble, sizeof(a.kbPreamble));
            ImGui::TextUnformatted("Summary");
            ImGui::InputTextMultiline("##kb_summary", a.kbSummary, sizeof(a.kbSummary),
                                      ImVec2(-1, 70));
            ImGui::TextUnformatted("Body");
            ImGui::InputTextMultiline("##kb_body", a.kbBody, sizeof(a.kbBody),
                                      ImVec2(-1, 120));
            ImGui::SetNextItemWidth(350.0f);
            ImGui::InputTextWithHint("Tags", "comma-separated", a.kbTags, sizeof(a.kbTags));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(430.0f);
            ImGui::InputTextWithHint("Source URI", "URL or local path (recommended)",
                                     a.kbSourceUri, sizeof(a.kbSourceUri));
            ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("Freshness", &a.kbFreshness, kFreshnessNames,
                         IM_ARRAYSIZE(kFreshnessNames));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("Confidence", &a.kbConfidence, kConfidenceNames,
                         IM_ARRAYSIZE(kConfidenceNames));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("Volatility", &a.kbVolatility, kVolatilityNames,
                         IM_ARRAYSIZE(kVolatilityNames));
            ImGui::SameLine();
            if (ImGui::Button("Save knowledge")) {
                nlohmann::json payload = {
                    {"project_id", projectId}, {"kind", kKnowledgeKinds[a.kbNewKind]},
                    {"title", a.kbTitle}, {"preamble", a.kbPreamble},
                    {"summary", a.kbSummary}, {"body", a.kbBody}, {"tags", a.kbTags},
                    {"freshness", kFreshnessNames[a.kbFreshness]},
                    {"confidence", kConfidenceNames[a.kbConfidence]},
                    {"volatility", kVolatilityNames[a.kbVolatility]}
                };
                if (a.kbSourceUri[0]) payload["source"] = {
                    {"uri", a.kbSourceUri}, {"title", a.kbTitle}, {"source_type", "manual"}
                };
                nlohmann::json result = saveKnowledgeNode(a.db, 0, payload);
                if (result.value("ok", false)) {
                    a.kbTitle[0] = a.kbPreamble[0] = a.kbSummary[0] = a.kbBody[0] = 0;
                    a.kbTags[0] = a.kbSourceUri[0] = 0;
                    a.needKnowledge = true;
                    toast(a, result.value("duplicate", false)
                        ? "existing knowledge reused" : "knowledge saved");
                } else toast(a, result.value("error", "knowledge save failed"), true);
            }
        }

        ImGui::SeparatorText("Active knowledge");
        if (a.knowledgeNodes.empty()) {
            ImGui::TextColored(C_DIM, "No active knowledge matches this scope.");
        }
        for (const auto& node : a.knowledgeNodes) {
            const long long id = node.value("id", 0LL);
            ImGui::PushID(static_cast<int>(id));
            std::string label = "K" + std::to_string(id) + "  " + node.value("title", "");
            if (ImGui::CollapsingHeader(label.c_str())) {
                chip(node.value("kind", "reference").c_str(), C_PURPLE);
                ImGui::SameLine(); chip(node.value("freshness", "timeless").c_str(), C_CYAN);
                ImGui::SameLine(); chip(node.value("confidence", "stated").c_str(), C_ACCENT);
                if (node.value("conflict_count", 0LL) > 0) {
                    ImGui::SameLine(); chip("conflict", C_RED);
                }
                std::string nodeProject = node.value("project_name", "");
                if (nodeProject.empty()) nodeProject = "cross-project";
                ImGui::TextColored(C_DIM,
                    "%s | updated %s | %lld claims | %lld sources | %lld links",
                    nodeProject.c_str(),
                    shortTs(node.value("updated_at", "")).c_str(),
                    node.value("claim_count", 0LL), node.value("source_count", 0LL),
                    node.value("link_count", 0LL));
                if (!node.value("preamble", "").empty())
                    ImGui::TextWrapped("%s", node.value("preamble", "").c_str());
                if (!node.value("summary", "").empty()) {
                    ImGui::Spacing();
                    drawBodyWithLinks(node.value("summary", ""), C_TEXT);
                }
                if (!node.value("body_excerpt", "").empty()) {
                    ImGui::Spacing();
                    drawBodyWithLinks(node.value("body_excerpt", ""), C_DIM);
                }
                if (ImGui::Button("Copy raw record data")) {
                    std::string text = getKnowledgeNode(a.db, id).dump(2);
                    ImGui::SetClipboardText(text.c_str());
                    toast(a, "raw knowledge data copied - no packet safeguards");
                }
                ImGui::SameLine();
                if (ImGui::Button("Archive")) {
                    nlohmann::json result = saveKnowledgeNode(a.db, id, {{"status", "archived"}});
                    if (result.value("ok", false)) { a.needKnowledge = true; toast(a, "knowledge archived"); }
                    else toast(a, result.value("error", "archive failed"), true);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Conflicts")) {
        ImGui::TextColored(C_DIM,
            "Reconciliation is explicit: clear winner, evolution, or ambiguous. History is never silently erased.");
        if (a.knowledgeConflicts.empty())
            ImGui::TextColored(C_GREEN, "No open conflicts in this scope.");
        for (const auto& conflict : a.knowledgeConflicts) {
            long long id = conflict.value("id", 0LL);
            ImGui::PushID(static_cast<int>(id));
            std::string label = "X" + std::to_string(id) + "  " +
                conflict.value("left_title", "") + "  <>  " + conflict.value("right_title", "");
            if (ImGui::CollapsingHeader(label.c_str())) {
                chip(conflict.value("classification", "ambiguous").c_str(), C_ORANGE);
                ImGui::TextWrapped("%s", conflict.value("reason", "").c_str());
                if (ImGui::Button("Copy raw reconciliation data")) {
                    nlohmann::json packet = {
                        {"conflict", conflict},
                        {"left", getKnowledgeNode(a.db, conflict.value("left_node_id", 0LL))},
                        {"right", getKnowledgeNode(a.db, conflict.value("right_node_id", 0LL))}
                    };
                    std::string text = packet.dump(2);
                    ImGui::SetClipboardText(text.c_str());
                    toast(a, "raw reconciliation data copied - no packet safeguards");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Reports")) {
        if (ImGui::CollapsingHeader("New grounded report", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("Report title", "weekly XA status",
                                     a.kbReportTitle, sizeof(a.kbReportTitle));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            ImGui::Combo("Type##report", &a.kbReportType, kReportTypes,
                         IM_ARRAYSIZE(kReportTypes));
            ImGui::TextUnformatted("Report content");
            ImGui::InputTextMultiline("##kb_report_body", a.kbReportBody,
                                      sizeof(a.kbReportBody), ImVec2(-1, 140));
            if (ImGui::Button("Save draft")) {
                std::string sourceHash, evidenceAt, periodStart, periodEnd;
                std::string packet = buildReportContext(
                    a.db, projectId, kReportTypes[a.kbReportType], 30,
                    &sourceHash, &evidenceAt, &periodStart, &periodEnd, true);
                if (packet.empty()) {
                    toast(a, "unable to capture current report evidence", true);
                } else {
                nlohmann::json result = saveReport(a.db, {
                    {"project_id", projectId}, {"report_type", kReportTypes[a.kbReportType]},
                    {"title", a.kbReportTitle}, {"content", a.kbReportBody},
                    {"status", "draft"}, {"generated_by", "native-ui"},
                    {"period_start", periodStart}, {"period_end", periodEnd},
                    {"evidence_at", evidenceAt}, {"input_hash", sourceHash}
                });
                if (result.value("ok", false)) {
                    a.kbReportTitle[0] = a.kbReportBody[0] = 0;
                    a.needKnowledge = true; toast(a, "report draft saved");
                } else toast(a, result.value("error", "report save failed"), true);
                }
            }
        }
        ImGui::SeparatorText("Saved reports");
        for (const auto& report : a.knowledgeReports) {
            long long id = report.value("id", 0LL);
            ImGui::PushID(static_cast<int>(id));
            std::string label = "R" + std::to_string(id) + "  " + report.value("title", "");
            if (ImGui::CollapsingHeader(label.c_str())) {
                chip(report.value("report_type", "project").c_str(), C_PURPLE);
                ImGui::SameLine(); chip(report.value("status", "draft").c_str(),
                                       report.value("status", "") == "approved" ? C_GREEN : C_ORANGE);
                ImGui::TextColored(C_DIM, "Evidence %s | input %s | updated %s",
                    report.value("evidence_at", "").c_str(),
                    report.value("input_hash", "").c_str(),
                    shortTs(report.value("updated_at", "")).c_str());
                drawBodyWithLinks(report.value("content_excerpt", ""), C_TEXT);
                if (ImGui::Button("Copy raw report text")) {
                    nlohmann::json full = getReport(a.db, id);
                    ImGui::SetClipboardText(full.value("content", "").c_str());
                    toast(a, "raw report text copied - no packet safeguards");
                }
                ImGui::SameLine();
                if (report.value("status", "") != "approved" && ImGui::Button("Approve")) {
                    nlohmann::json result = setReportStatus(a.db, id, "approved");
                    if (result.value("ok", false)) { a.needKnowledge = true; toast(a, "report approved"); }
                    else toast(a, result.value("error", "approval failed"), true);
                }
                ImGui::SameLine();
                if (report.value("status", "") != "archived" && ImGui::Button("Archive")) {
                    nlohmann::json result = setReportStatus(a.db, id, "archived");
                    if (result.value("ok", false)) { a.needKnowledge = true; toast(a, "report archived"); }
                    else toast(a, result.value("error", "archive failed"), true);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Development")) {
        if (ImGui::CollapsingHeader("Start intent-driven workflow", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("Workflow title", "implementation or investigation",
                                     a.kbWorkflowTitle, sizeof(a.kbWorkflowTitle));
            ImGui::SameLine(); ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("Context", &a.kbWorkflowContext, kWorkflowContexts,
                         IM_ARRAYSIZE(kWorkflowContexts));
            ImGui::SameLine(); ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("Phase", &a.kbWorkflowPhase, kWorkflowPhases,
                         IM_ARRAYSIZE(kWorkflowPhases));
            ImGui::TextUnformatted("Objective");
            ImGui::InputTextMultiline("##kb_workflow_objective", a.kbWorkflowObjective,
                                      sizeof(a.kbWorkflowObjective), ImVec2(-1, 70));
            ImGui::TextUnformatted("Minimum success");
            ImGui::InputTextMultiline("##kb_workflow_success", a.kbWorkflowSuccess,
                                      sizeof(a.kbWorkflowSuccess), ImVec2(-1, 60));
            ImGui::TextUnformatted(
                "Validation criteria (one independently provable criterion per line)");
            ImGui::InputTextMultiline(
                "##kb_workflow_validation", a.kbWorkflowValidation,
                sizeof(a.kbWorkflowValidation), ImVec2(-1, 90));
            ImGui::InputTextWithHint("Next action", "one precise next action",
                                     a.kbWorkflowNext, sizeof(a.kbWorkflowNext));
            if (ImGui::Button("Start workflow")) {
                nlohmann::json validation = nonEmptyLines(a.kbWorkflowValidation);
                if (a.kbWorkflowPhase >= 1 && validation.empty()) {
                    toast(a,
                        "define/develop/deliver workflows require validation criteria",
                        true);
                } else {
                nlohmann::json result = saveWorkflow(a.db, {
                    {"project_id", projectId}, {"title", a.kbWorkflowTitle},
                    {"context", kWorkflowContexts[a.kbWorkflowContext]},
                    {"phase", kWorkflowPhases[a.kbWorkflowPhase]},
                    {"objective", a.kbWorkflowObjective},
                    {"minimum_success", a.kbWorkflowSuccess},
                    {"validation", validation},
                    {"next_action", a.kbWorkflowNext}, {"status", "active"}
                });
                if (result.value("ok", false)) {
                    a.kbWorkflowTitle[0] = a.kbWorkflowObjective[0] = 0;
                    a.kbWorkflowSuccess[0] = a.kbWorkflowValidation[0] = 0;
                    a.kbWorkflowNext[0] = 0;
                    a.needKnowledge = true; toast(a, "workflow started");
                } else toast(a, result.value("error", "workflow save failed"), true);
                }
            }
        }
        ImGui::SeparatorText("Workflow status");
        for (const auto& workflow : a.knowledgeWorkflows) {
            long long id = workflow.value("id", 0LL);
            ImGui::PushID(static_cast<int>(id));
            std::string label = "W" + std::to_string(id) + "  " + workflow.value("title", "");
            if (ImGui::CollapsingHeader(label.c_str())) {
                chip(workflow.value("context", "dev").c_str(), C_CYAN);
                ImGui::SameLine(); chip(workflow.value("phase", "discover").c_str(), C_PURPLE);
                ImGui::SameLine(); chip(workflow.value("status", "active").c_str(),
                    workflow.value("status", "") == "blocked" ? C_RED : C_ACCENT);
                ImGui::TextWrapped("Objective: %s", workflow.value("objective", "").c_str());
                ImGui::Text("Current: %s", workflow.value("current_step", "").c_str());
                ImGui::Text("Next: %s", workflow.value("next_action", "").c_str());
                ImGui::TextColored(C_DIM, "%lld artifacts | %lld evidence | %lld failed",
                    workflow.value("artifact_count", 0LL), workflow.value("evidence_count", 0LL),
                    workflow.value("failed_evidence", 0LL));
                if (ImGui::Button("Copy resume packet")) {
                    std::string text = buildWorkflowResume(a.db, 0, id);
                    copyPacket(a, text, "exact workflow resume packet copied");
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy raw data")) {
                    std::string text = getWorkflow(a.db, id).dump(2);
                    ImGui::SetClipboardText(text.c_str());
                    toast(a, "raw workflow data copied - no packet safeguards");
                }
                ImGui::SameLine();
                const bool blocked = workflow.value("status", "") == "blocked";
                if (ImGui::Button(blocked ? "Resume" : "Mark blocked")) {
                    nlohmann::json result = setWorkflowStatus(a.db, id, blocked ? "active" : "blocked");
                    if (result.value("ok", false)) { a.needKnowledge = true; toast(a, blocked ? "workflow resumed" : "workflow blocked"); }
                    else toast(a, result.value("error", "status change failed"), true);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("Health")) {
        ImGui::TextUnformatted("Deterministic checks (not AI quality scores)");
        if (!health.empty()) {
            struct Metric { const char* label; const char* key; ImVec4 color; } metrics[] = {
                {"Stale nodes", "stale_nodes", C_ORANGE},
                {"Stale claims", "stale_claims", C_ORANGE},
                {"Undated snapshots", "undated_snapshots", C_RED},
                {"Undated fast claims", "undated_fast_claims", C_RED},
                {"Broken pointers", "broken_pointers", C_RED},
                {"Missing claim provenance", "claims_missing_provenance", C_ORANGE},
                {"Orphan nodes", "orphan_nodes", C_ORANGE},
                {"Duplicate groups", "duplicate_groups", C_ORANGE},
                {"Open conflicts", "open_conflicts", C_ORANGE},
                {"Draft reports", "draft_reports", C_DIM},
                {"Blocked workflows", "blocked_workflows", C_RED},
            };
            if (ImGui::BeginTable("health_metrics", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (const auto& metric : metrics) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(metric.label); ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(health.value(metric.key, 0LL) > 0 ? metric.color : C_GREEN,
                                       "%lld", health.value(metric.key, 0LL));
                }
                ImGui::EndTable();
            }
            if (health.contains("recommendations") && !health["recommendations"].empty()) {
                ImGui::SeparatorText("Recommended repairs");
                for (const auto& recommendation : health["recommendations"])
                    ImGui::BulletText("%s", recommendation.get<std::string>().c_str());
            }
        }
        ImGui::Spacing();
        ImGui::TextColored(C_DIM,
            "Retrieval remains lexical until saved recall@k and reciprocal-rank evaluations prove a hybrid index is better.");
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}


} // namespace devhub
