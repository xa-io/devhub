#pragma once

#include "PacketOps.h"

namespace devhub {

class Db;

// Resolves stable project metadata, configured repository skills, and optional
// workflow direction for the shared packet renderer. Callers may request one
// exact workflow or the latest active workflow in the selected scope.
PacketTarget resolvePacketTarget(Db* db, long long projectId,
                                 long long workflowId = 0,
                                 bool selectLatestActive = false);

} // namespace devhub
