// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "SqliteTaskStore.h"
#include "TaskStoreSchema.h"

#include <filesystem>
#include <fstream>
#include <winsqlite/winsqlite3.h>

#pragma comment(lib, "winsqlite3.lib")

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    namespace
    {
        class TemporaryDatabasePath final
        {
        public:
            TemporaryDatabasePath()
            {
                std::array<wchar_t, MAX_PATH> temporaryDirectory{};
                VERIFY_IS_GREATER_THAN(GetTempPathW(static_cast<DWORD>(temporaryDirectory.size()), temporaryDirectory.data()), DWORD{ 0 });
                std::array<wchar_t, MAX_PATH> temporaryFile{};
                VERIFY_IS_TRUE(GetTempFileNameW(temporaryDirectory.data(), L"tsk", 0, temporaryFile.data()) != 0);
                _path = temporaryFile.data();
                std::filesystem::remove(_path);
            }

            ~TemporaryDatabasePath()
            {
                std::error_code ignored;
                std::filesystem::remove(_path, ignored);
                std::filesystem::remove(_path.wstring() + L"-wal", ignored);
                std::filesystem::remove(_path.wstring() + L"-shm", ignored);
            }

            [[nodiscard]] const std::filesystem::path& Path() const noexcept
            {
                return _path;
            }

        private:
            std::filesystem::path _path;
        };

        std::string _utf8Path(const std::filesystem::path& path)
        {
            const auto value = path.u8string();
            return { reinterpret_cast<const char*>(value.data()), value.size() };
        }

        sqlite3* _openRaw(const std::filesystem::path& path)
        {
            sqlite3* database{};
            const auto utf8 = _utf8Path(path);
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_open_v2(utf8.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr));
            return database;
        }

        int64_t _queryInteger(sqlite3* database, const char* sql)
        {
            sqlite3_stmt* statement{};
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_prepare_v2(database, sql, -1, &statement, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_ROW, sqlite3_step(statement));
            const auto value = sqlite3_column_int64(statement, 0);
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_finalize(statement));
            return value;
        }

        std::string _queryText(sqlite3* database, const char* sql)
        {
            sqlite3_stmt* statement{};
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_prepare_v2(database, sql, -1, &statement, nullptr));
            VERIFY_ARE_EQUAL(SQLITE_ROW, sqlite3_step(statement));
            const auto text = sqlite3_column_text(statement, 0);
            const auto length = sqlite3_column_bytes(statement, 0);
            std::string value{ reinterpret_cast<const char*>(text), static_cast<size_t>(length) };
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_finalize(statement));
            return value;
        }

        void _downgradeToVersionOne(sqlite3* database)
        {
            constexpr auto sql = R"SQL(
PRAGMA foreign_keys=OFF;
BEGIN IMMEDIATE;
DROP TABLE task_intent;
ALTER TABLE delete_tombstone RENAME TO delete_tombstone_v2;
CREATE TABLE delete_tombstone (
    task_id TEXT PRIMARY KEY NOT NULL,
    deleted_at INTEGER NOT NULL CHECK(deleted_at >= 0),
    expires_at INTEGER NOT NULL CHECK(expires_at > deleted_at),
    replay_digest TEXT NOT NULL
);
INSERT INTO delete_tombstone(task_id, deleted_at, expires_at, replay_digest)
SELECT task_id, deleted_at, expires_at, replay_digest FROM delete_tombstone_v2;
DROP TABLE delete_tombstone_v2;
PRAGMA user_version=1;
COMMIT;
)SQL";
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, sql, nullptr, nullptr, nullptr));
        }
    }

    class TaskCoreSchemaTests
    {
        TEST_CLASS(TaskCoreSchemaTests);

        TEST_METHOD(InitializesCompleteVersionOneSchemaAndPragmas);
        TEST_METHOD(ReopenIsIdempotent);
        TEST_METHOD(RejectsFutureSchemaWithoutRewritingIt);
        TEST_METHOD(RejectsForeignAndUnversionedDatabases);
        TEST_METHOD(PreservesCorruptDatabaseForRecovery);
        TEST_METHOD(MigratesVersionOneSchemaAndPreservesTombstones);
        TEST_METHOD(FailedMigrationRollsBackWithoutVersionAdvance);
    };

    void TaskCoreSchemaTests::InitializesCompleteVersionOneSchemaAndPragmas()
    {
        TemporaryDatabasePath path;
        SqliteTaskStore store{ path.Path() };
        VERIFY_IS_TRUE(store.IsReady());
        VERIFY_IS_TRUE(store.OpenStatus() == TaskStoreOpenStatus::Ready);
        VERIFY_ARE_EQUAL(details::CurrentTaskDatabaseSchemaVersion, store.SchemaVersion());

        auto database = _openRaw(path.Path());
        VERIFY_ARE_EQUAL(int64_t{ details::TaskDatabaseApplicationId }, _queryInteger(database, "PRAGMA application_id;"));
        VERIFY_ARE_EQUAL(int64_t{ 2 }, _queryInteger(database, "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(std::string{ "wal" }, _queryText(database, "PRAGMA journal_mode;"));
        VERIFY_ARE_EQUAL(int64_t{ 13 }, _queryInteger(database, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('task','task_acceptance','execution_context','task_terminal','agent_session','task_run','task_message','task_run_message','task_event','git_checkpoint','command_receipt','delete_tombstone','task_intent');"));
        VERIFY_ARE_EQUAL(int64_t{ 1 }, _queryInteger(database, "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='task_run_one_active' AND sql LIKE '%WHERE state IN (0, 1, 2, 3, 4)%';"));
        VERIFY_ARE_EQUAL(int64_t{ 4 }, _queryInteger(database, "SELECT count(*) FROM pragma_table_info('delete_tombstone') WHERE name IN ('command_id','expected_version','committed_version','last_event_sequence');"));
        VERIFY_ARE_EQUAL(int64_t{ 1 }, _queryInteger(database, "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='task_intent_recovery';"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
    }

    void TaskCoreSchemaTests::ReopenIsIdempotent()
    {
        TemporaryDatabasePath path;
        {
            SqliteTaskStore first{ path.Path() };
            VERIFY_IS_TRUE(first.IsReady());
        }
        const auto sizeBefore = std::filesystem::file_size(path.Path());
        {
            SqliteTaskStore reopened{ path.Path() };
            VERIFY_IS_TRUE(reopened.IsReady());
            VERIFY_ARE_EQUAL(2, reopened.SchemaVersion());
        }
        VERIFY_IS_TRUE(std::filesystem::file_size(path.Path()) >= sizeBefore);
    }

    void TaskCoreSchemaTests::RejectsFutureSchemaWithoutRewritingIt()
    {
        TemporaryDatabasePath path;
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.IsReady());
        }
        auto database = _openRaw(path.Path());
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, "PRAGMA user_version=99;", nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        SqliteTaskStore future{ path.Path() };
        VERIFY_IS_FALSE(future.IsReady());
        VERIFY_IS_TRUE(future.OpenStatus() == TaskStoreOpenStatus::UnsupportedSchema);
        VERIFY_ARE_EQUAL(99, future.SchemaVersion());

        database = _openRaw(path.Path());
        VERIFY_ARE_EQUAL(int64_t{ 99 }, _queryInteger(database, "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
    }

    void TaskCoreSchemaTests::RejectsForeignAndUnversionedDatabases()
    {
        TemporaryDatabasePath foreignPath;
        auto database = _openRaw(foreignPath.Path());
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, "PRAGMA application_id=1234;", nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
        SqliteTaskStore foreign{ foreignPath.Path() };
        VERIFY_IS_TRUE(foreign.OpenStatus() == TaskStoreOpenStatus::ForeignDatabase);
        database = _openRaw(foreignPath.Path());
        VERIFY_ARE_EQUAL(std::string{ "delete" }, _queryText(database, "PRAGMA journal_mode;"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        TemporaryDatabasePath unversionedPath;
        database = _openRaw(unversionedPath.Path());
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, "CREATE TABLE existing(value TEXT);", nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
        SqliteTaskStore unversioned{ unversionedPath.Path() };
        VERIFY_IS_TRUE(unversioned.OpenStatus() == TaskStoreOpenStatus::ForeignDatabase);
        database = _openRaw(unversionedPath.Path());
        VERIFY_ARE_EQUAL(std::string{ "delete" }, _queryText(database, "PRAGMA journal_mode;"));
        VERIFY_ARE_EQUAL(int64_t{ 0 }, _queryInteger(database, "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
    }

    void TaskCoreSchemaTests::PreservesCorruptDatabaseForRecovery()
    {
        TemporaryDatabasePath path;
        constexpr std::string_view corruptContent{ "not a sqlite database" };
        {
            std::ofstream stream{ path.Path(), std::ios::binary };
            stream.write(corruptContent.data(), static_cast<std::streamsize>(corruptContent.size()));
        }

        SqliteTaskStore corrupt{ path.Path() };
        VERIFY_IS_FALSE(corrupt.IsReady());
        VERIFY_IS_TRUE(corrupt.OpenStatus() == TaskStoreOpenStatus::RecoveryRequired);

        std::ifstream stream{ path.Path(), std::ios::binary };
        const std::string retained{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
        VERIFY_ARE_EQUAL(std::string{ corruptContent }, retained);
    }

    void TaskCoreSchemaTests::MigratesVersionOneSchemaAndPreservesTombstones()
    {
        TemporaryDatabasePath path;
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.IsReady());
        }

        constexpr auto taskId = "00000000-0000-4000-8000-000000000111";
        auto database = _openRaw(path.Path());
        VERIFY_ARE_EQUAL(
            SQLITE_OK,
            sqlite3_exec(
                database,
                "INSERT INTO delete_tombstone(task_id,deleted_at,expires_at,replay_digest) VALUES('00000000-0000-4000-8000-000000000111',10,20,'legacy');",
                nullptr,
                nullptr,
                nullptr));
        _downgradeToVersionOne(database);
        VERIFY_ARE_EQUAL(int64_t{ 1 }, _queryInteger(database, "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        SqliteTaskStore migrated{ path.Path() };
        VERIFY_IS_TRUE(migrated.IsReady());
        VERIFY_ARE_EQUAL(2, migrated.SchemaVersion());
        const auto tombstone = migrated.LoadDeleteTombstone(TaskId{ taskId });
        VERIFY_IS_TRUE(tombstone.has_value());
        VERIFY_ARE_EQUAL(std::string{ "legacy" }, tombstone->ReplayDigest);
        VERIFY_IS_FALSE(tombstone->Command.has_value());

        database = _openRaw(path.Path());
        VERIFY_ARE_EQUAL(int64_t{ 2 }, _queryInteger(database, "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(int64_t{ 1 }, _queryInteger(database, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='task_intent';"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
    }

    void TaskCoreSchemaTests::FailedMigrationRollsBackWithoutVersionAdvance()
    {
        TemporaryDatabasePath path;
        {
            SqliteTaskStore store{ path.Path() };
            VERIFY_IS_TRUE(store.IsReady());
        }

        auto database = _openRaw(path.Path());
        _downgradeToVersionOne(database);
        VERIFY_ARE_EQUAL(
            SQLITE_OK,
            sqlite3_exec(database, "ALTER TABLE delete_tombstone ADD COLUMN command_id TEXT;", nullptr, nullptr, nullptr));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));

        SqliteTaskStore failed{ path.Path() };
        VERIFY_IS_FALSE(failed.IsReady());
        VERIFY_IS_TRUE(failed.OpenStatus() == TaskStoreOpenStatus::IOError);

        database = _openRaw(path.Path());
        VERIFY_ARE_EQUAL(int64_t{ 1 }, _queryInteger(database, "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(int64_t{ 0 }, _queryInteger(database, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='task_intent';"));
        VERIFY_ARE_EQUAL(int64_t{ 1 }, _queryInteger(database, "SELECT count(*) FROM pragma_table_info('delete_tombstone') WHERE name='command_id';"));
        VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_close(database));
    }
}
