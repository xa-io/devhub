#include "AppBootstrap.h"

#include "Db.h"
#include "devhub/Util.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <string>

namespace devhub {

// Give a genuinely new installation one neutral editable project. The marker
// prevents a user who later deletes every project from having it recreated.
// Existing databases are marked initialized without changing their projects.
void seedDefaults(Db& db) {
    auto lk = db.guard();
    {
        SQLite::Statement marker(db.raw(lk.token()),
            "SELECT 1 FROM settings WHERE key='starter_project_initialized'");
        if (marker.executeStep()) return;
    }

    bool freshDatabase = false;
    {
        SQLite::Statement existing(db.raw(lk.token()), "SELECT COUNT(*) FROM projects");
        existing.executeStep();
        freshDatabase = existing.getColumn(0).getInt64() == 0;
    }

    SQLite::Transaction tx(db.raw(lk.token()));
    if (freshDatabase) {
        const std::string now = nowIsoUtc();
        SQLite::Statement ins(db.raw(lk.token()),
            "INSERT INTO projects(name,slug,description,created_at,updated_at) "
            "VALUES(?,?,?,?,?)");
        ins.bind(1, "My Project");
        ins.bind(2, "my-project");
        ins.bind(3, "Edit this starter project or add your own.");
        ins.bind(4, now);
        ins.bind(5, now);
        ins.exec();
        db.logActivity(lk.token(), "seed", 0, "Added the neutral starter project");
    }
    SQLite::Statement initialized(db.raw(lk.token()),
        "INSERT INTO settings(key,value) VALUES('starter_project_initialized','1')");
    initialized.exec();
    tx.commit();
}

} // namespace devhub
