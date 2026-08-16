#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace devhub {

// Canonical fail-closed HTTP disclosure policy for persisted settings. Public
// settings remain usable values; every unlisted key is redacted by the API.
bool isPublicSettingKey(const std::string& key);

// Thin wrapper around the SQLite connection. The connection is shared by the
// HTTP worker threads and the build runner, so every access must hold guard().
// The mutex is recursive so packet builders can hold one outer snapshot guard
// while calling existing guarded read helpers without allowing interleaved
// in-process writes.
class Db {
public:
    explicit Db(const std::string& path);

    class Guard;
    class Held {
        friend class Db;
        friend class Guard;
        explicit Held(const Db* owner) : owner_(owner) {}
        const Db* owner_ = nullptr;
    };
    class Guard {
    public:
        Guard(Guard&&) noexcept = default;
        Guard& operator=(Guard&&) noexcept = default;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        Held token() const {
            if (!lock_.owns_lock())
                throw std::logic_error("database guard token requested after unlock");
            return Held(owner_);
        }
        void unlock() { lock_.unlock(); }
        bool owns_lock() const noexcept { return lock_.owns_lock(); }

    private:
        friend class Db;
        explicit Guard(Db* owner)
            : owner_(owner), lock_(owner->mu_) {}
        Db* owner_ = nullptr;
        std::unique_lock<std::recursive_mutex> lock_;
    };

    Guard guard() { return Guard(this); }
    SQLite::Database& raw(Held held) {
        if (held.owner_ != this)
            throw std::logic_error("database guard token belongs to another connection");
        return *db_;
    }

    const std::string& path() const { return path_; }

    // The Held parameter makes the shared-connection lock part of the API.
    std::string getSetting(Held, const std::string& key,
                           const std::string& def = "");
    void setSetting(Held, const std::string& key, const std::string& value);
    void logActivity(Held, const std::string& kind, long long projectId,
                     const std::string& detail);

    // Runs the remaining SELECT rows into a JSON array of objects.
    static nlohmann::json rowsToJson(SQLite::Statement& q);
    static nlohmann::json rowToJson(SQLite::Statement& q); // current row only

private:
    std::string getSettingUnlocked(const std::string& key,
                                   const std::string& def);
    void setSettingUnlocked(const std::string& key, const std::string& value);
    void logActivityUnlocked(const std::string& kind, long long projectId,
                             const std::string& detail);
    void migrate();
    void seedKnownProjectDefaults();
    std::string path_;
    std::unique_ptr<SQLite::Database> db_;
    std::recursive_mutex mu_;
};

} // namespace devhub
