#include "PacketData.h"

#include "Db.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace devhub {
namespace {

std::vector<std::string> splitSkills(const std::string& value) {
    static constexpr size_t kMaxSkillTextBytes = 64 * 1024;
    static constexpr size_t kMaxSkillTokens = 65; // repository + 64 companions
    static constexpr size_t kMaxSkillTokenBytes = 256;
    std::vector<std::string> skills;
    std::string token;
    auto flush = [&]() {
        const size_t begin = token.find_first_not_of(" \t");
        if (begin != std::string::npos) {
            const size_t end = token.find_last_not_of(" \t");
            skills.push_back(
                token.substr(begin, (std::min)(end - begin + 1,
                                               kMaxSkillTokenBytes)));
        }
        token.clear();
    };
    const size_t bytes = (std::min)(value.size(), kMaxSkillTextBytes);
    for (size_t i = 0; i < bytes && skills.size() < kMaxSkillTokens; ++i) {
        const char ch = value[i];
        if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r') flush();
        else token.push_back(ch);
    }
    flush();
    return skills;
}

void loadProjectLocked(Db::Held held, Db* db, long long projectId, PacketTarget& target) {
    if (projectId <= 0) return;
    SQLite::Statement project(db->raw(held),
        "SELECT id,name,path,rules_path,codex_skills FROM projects WHERE id=?");
    project.bind(1, projectId);
    if (!project.executeStep()) return;
    target.projectId = project.getColumn(0).getInt64();
    target.projectName = project.getColumn(1).getString();
    target.workspace = project.getColumn(2).getString();
    target.rulesPath = project.getColumn(3).getString();
    std::vector<std::string> skills = splitSkills(project.getColumn(4).getString());
    if (!skills.empty()) {
        target.repositorySkill = skills.front();
        target.companionSkills.assign(skills.begin() + 1, skills.end());
    }
}

} // namespace

PacketTarget resolvePacketTarget(Db* db, long long projectId,
                                 long long workflowId,
                                 bool selectLatestActive) {
    PacketTarget target;
    auto lock = db->guard();
    loadProjectLocked(lock.token(), db, projectId, target);

    std::string sql = R"sql(
SELECT id,project_id,title,context,phase,status,objective,minimum_success,
 validation_json,current_step,next_action,blockers_json,workspace
FROM workflow_runs)sql";
    if (workflowId > 0) {
        sql += " WHERE id=?";
    } else if (selectLatestActive) {
        sql += " WHERE status IN ('active','blocked','verification')";
        if (projectId > 0) sql += " AND project_id=?";
        sql += " ORDER BY CASE status WHEN 'blocked' THEN 0 "
               "WHEN 'verification' THEN 1 ELSE 2 END,updated_at DESC,id DESC LIMIT 1";
    } else {
        return target;
    }

    SQLite::Statement workflow(db->raw(lock.token()), sql);
    if (workflowId > 0) workflow.bind(1, workflowId);
    else if (projectId > 0) workflow.bind(1, projectId);
    if (!workflow.executeStep()) return target;

    const long long resolvedWorkflowId = workflow.getColumn(0).getInt64();
    const long long workflowProjectId = workflow.getColumn(1).isNull()
        ? 0 : workflow.getColumn(1).getInt64();
    if (projectId > 0 && workflowProjectId != projectId) return target;
    target.workflowId = resolvedWorkflowId;
    target.workflowTitle = workflow.getColumn(2).getString();
    target.workflowContext = workflow.getColumn(3).getString();
    target.workflowPhase = workflow.getColumn(4).getString();
    target.workflowStatus = workflow.getColumn(5).getString();
    target.objective = workflow.getColumn(6).getString();
    target.minimumSuccess = workflow.getColumn(7).getString();
    target.validation = workflow.getColumn(8).getString();
    target.currentStep = workflow.getColumn(9).getString();
    target.nextAction = workflow.getColumn(10).getString();
    target.blockers = workflow.getColumn(11).getString();
    const std::string workflowWorkspace = workflow.getColumn(12).getString();

    if (target.projectId <= 0 && workflowProjectId > 0)
        loadProjectLocked(lock.token(), db, workflowProjectId, target);
    if (!workflowWorkspace.empty()) target.workspace = workflowWorkspace;
    return target;
}

} // namespace devhub
