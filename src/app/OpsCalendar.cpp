#include "Ops.h"
#include "Db.h"

using json = nlohmann::json;

namespace devhub {

json calendarEntries(Db* db, const std::string& from, const std::string& to) {
    auto lk = db->guard();
    json out = json::array();
    {
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT e.*, p.name AS project_name FROM events e "
            "LEFT JOIN projects p ON p.id=e.project_id "
            "WHERE e.date>=? AND e.date<=? ORDER BY e.date");
        q.bind(1, from); q.bind(2, to);
        for (auto& e : Db::rowsToJson(q)) {
            e["entry"] = "event";
            out.push_back(e);
        }
    }
    {
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT i.id AS item_id, i.title, i.due_date AS date, i.status, i.type, "
            "i.project_id, p.name AS project_name FROM items i "
            "JOIN projects p ON p.id=i.project_id "
            "WHERE i.due_date!='' AND i.due_date>=? AND i.due_date<=? "
            "AND i.status IN ('open','in_progress','blocked')");
        q.bind(1, from); q.bind(2, to);
        for (auto& e : Db::rowsToJson(q)) {
            e["entry"] = "due";
            e["kind"] = "deadline";
            out.push_back(e);
        }
    }
    {
        SQLite::Statement q(db->raw(lk.token()),
            "SELECT b.id AS build_id, b.status, b.command, b.kind AS build_kind, "
            "b.exit_code, substr(b.started_at,1,10) AS date, b.started_at, "
            "b.finished_at, b.project_id, p.name AS project_name "
            "FROM builds b JOIN projects p ON p.id=b.project_id "
            "WHERE substr(b.started_at,1,10)>=? AND substr(b.started_at,1,10)<=?");
        q.bind(1, from); q.bind(2, to);
        for (auto& e : Db::rowsToJson(q)) {
            e["entry"] = "build";
            e["kind"] = "build";
            e["title"] = e.value("build_kind", "build") + " " +
                         e.value("status", "");
            out.push_back(e);
        }
    }
    return out;
}

} // namespace devhub
