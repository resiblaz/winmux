// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreSchema.h"
#include "TaskStoreSqlite.h"

#include <filesystem>
#include <winsqlite/winsqlite3.h>
#include <wil/resource.h>

#pragma comment(lib, "winsqlite3.lib")

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        std::string _utf8Path(const std::filesystem::path& path)
        {
            const auto value = path.u8string();
            return { reinterpret_cast<const char*>(value.data()), value.size() };
        }

        bool _backupForAgentMigration(sqlite3* source, const std::filesystem::path& path, std::string& error)
        {
            const auto version = details::QueryInt64(source, "PRAGMA user_version;", error);
            const auto application = details::QueryInt64(source, "PRAGMA application_id;", error);
            if (!version || !application) { error.clear(); return true; }
            if (*version <= 0 || *version >= details::CurrentTaskDatabaseSchemaVersion || *application != details::TaskDatabaseApplicationId) return true;
            FILETIME time;
            GetSystemTimePreciseAsFileTime(&time);
            const auto ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
            auto destination = path;
            destination += (*version < 5 ? L".pre-agent-profiles-" : L".pre-project-board-") + std::to_wstring(ticks) + L".db";
            auto partial = destination;
            partial += L".partial";
            wil::unique_handle reservation{ CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (!reservation) { error = "Could not reserve the pre-upgrade Task backup"; return false; }
            reservation.reset();
            sqlite3* target{};
            const auto close = wil::scope_exit([&] { if (target) sqlite3_close_v2(target); });
            if (sqlite3_open_v2(_utf8Path(partial).c_str(), &target, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
            { error = "Could not open the pre-upgrade Task backup"; return false; }
            auto backup = sqlite3_backup_init(target, "main", source, "main");
            if (!backup) { error = "Could not begin the pre-upgrade Task backup"; return false; }
            const auto copied = sqlite3_backup_step(backup, -1);
            const auto finished = sqlite3_backup_finish(backup);
            if (copied != SQLITE_DONE || finished != SQLITE_OK)
            { error = "Pre-upgrade Task backup failed; the original database was preserved"; return false; }
            sqlite3_close_v2(target);
            target = nullptr;
            if (!MoveFileExW(partial.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            { error = "Could not publish the pre-upgrade Task backup"; return false; }
            return true;
        }
    }

    SqliteTaskStore::Impl::~Impl()
    {
        if (Database)
        {
            sqlite3_close_v2(Database);
        }
    }

    SqliteTaskStore::SqliteTaskStore(const std::filesystem::path& path) :
        _impl{ std::make_unique<Impl>() }
    {
        try
        {
            if (path.empty() || path.native().find(L'\0') != std::wstring::npos)
            {
                _impl->Error = "Database path is invalid";
                return;
            }
            if (const auto parent = path.parent_path(); !parent.empty())
            {
                std::filesystem::create_directories(parent);
            }

            const auto utf8Path = _utf8Path(path);
            const auto result = sqlite3_open_v2(
                utf8Path.c_str(),
                &_impl->Database,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                nullptr);
            if (result != SQLITE_OK)
            {
                _impl->Error = details::SqliteError(_impl->Database, result, "open database");
                return;
            }
            sqlite3_extended_result_codes(_impl->Database, 1);
            if (!_backupForAgentMigration(_impl->Database, path, _impl->Error)) return;
            _impl->Status = details::InitializeOrValidateTaskSchema(_impl->Database, _impl->Version, _impl->Error);
        }
        catch (const std::exception& exception)
        {
            _impl->Status = TaskStoreOpenStatus::IOError;
            _impl->Error = exception.what();
        }
        catch (...)
        {
            _impl->Status = TaskStoreOpenStatus::IOError;
            _impl->Error = "Unknown database initialization failure";
        }
    }

    SqliteTaskStore::~SqliteTaskStore() = default;

    bool SqliteTaskStore::IsReady() const noexcept
    {
        return _impl->Status == TaskStoreOpenStatus::Ready;
    }

    TaskStoreOpenStatus SqliteTaskStore::OpenStatus() const noexcept
    {
        return _impl->Status;
    }

    int32_t SqliteTaskStore::SchemaVersion() const noexcept
    {
        return _impl->Version;
    }

    const std::string& SqliteTaskStore::LastError() const noexcept
    {
        return _impl->Error;
    }
}
