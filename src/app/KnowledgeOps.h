#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace devhub {

class Db;

// Advanced knowledge operations shared by the native UI and localhost API.
// All functions acquire Db::guard(). Packet builders may intentionally hold an
// outer recursive guard to keep their multi-query snapshot internally coherent.
nlohmann::json saveKnowledgeSource(Db* db, const nlohmann::json& body);
nlohmann::json listKnowledgeNodes(Db* db, long long projectId,
                                  const std::string& query,
                                  const std::string& kind,
                                  const std::string& status, int limit = 50);
nlohmann::json getKnowledgeNode(Db* db, long long nodeId);
nlohmann::json saveKnowledgeNode(Db* db, long long nodeId,
                                 const nlohmann::json& body);
nlohmann::json addKnowledgeClaim(Db* db, long long nodeId,
                                 const nlohmann::json& body);
nlohmann::json linkKnowledgeNodes(Db* db, const nlohmann::json& body);

nlohmann::json listKnowledgeConflicts(Db* db, long long projectId,
                                      const std::string& status, int limit = 100);
nlohmann::json createKnowledgeConflict(Db* db, const nlohmann::json& body);
nlohmann::json resolveKnowledgeConflict(Db* db, long long conflictId,
                                        const nlohmann::json& body);
nlohmann::json knowledgeHealth(Db* db, long long projectId = 0);
std::string buildKnowledgeContext(Db* db, long long projectId,
                                  const std::string& query, int level = 2,
                                  int limit = 30);

nlohmann::json listReports(Db* db, long long projectId,
                           const std::string& type,
                           const std::string& status, int limit = 100);
nlohmann::json getReport(Db* db, long long reportId);
nlohmann::json saveReport(Db* db, const nlohmann::json& body);
nlohmann::json setReportStatus(Db* db, long long reportId,
                               const std::string& status);
std::string buildReportContext(Db* db, long long projectId,
                               const std::string& type, int days,
                               std::string* sourceHash = nullptr,
                               std::string* evidenceAt = nullptr,
                               std::string* periodStart = nullptr,
                               std::string* periodEnd = nullptr,
                               bool registerSnapshot = false);

nlohmann::json listWorkflows(Db* db, long long projectId,
                             const std::string& status, int limit = 100);
nlohmann::json getWorkflow(Db* db, long long workflowId);
nlohmann::json saveWorkflow(Db* db, const nlohmann::json& body);
nlohmann::json setWorkflowStatus(Db* db, long long workflowId,
                                 const std::string& status);
nlohmann::json addWorkflowArtifact(Db* db, long long workflowId,
                                   const nlohmann::json& body);
nlohmann::json addVerificationEvidence(Db* db, long long workflowId,
                                       const nlohmann::json& body);
std::string buildWorkflowResume(Db* db, long long projectId,
                                long long exactWorkflowId = 0);

nlohmann::json listLearnings(Db* db, long long projectId,
                             const std::string& query, int limit = 20);
nlohmann::json saveLearning(Db* db, const nlohmann::json& body);
nlohmann::json listProcessingRuns(Db* db, const std::string& kind,
                                  const std::string& status, int limit = 100);
nlohmann::json saveProcessingRun(Db* db, const nlohmann::json& body);
nlohmann::json listRetrievalEvaluations(Db* db, const std::string& engine,
                                        int limit = 100);
nlohmann::json saveRetrievalEvaluation(Db* db, const nlohmann::json& body);

} // namespace devhub
