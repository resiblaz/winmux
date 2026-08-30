// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SqliteTaskStore.h"
#include "SqliteTaskStoreInternal.h"

#include "TaskStoreSchema.h"
#include "TaskStoreSqlite.h"

#include <filesystem>
#include <winsqlite/winsqlite3.h>

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
