#pragma once

namespace devhub {

class Db;

// Idempotently initializes a neutral starter project without overwriting
// operator-managed fields or recreating projects that were later deleted.
void seedDefaults(Db& db);

} // namespace devhub
