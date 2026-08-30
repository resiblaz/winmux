// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <winsqlite/winsqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskCore::details
{
    class SqliteStatement final
    {
    public:
        SqliteStatement(sqlite3* database, const char* sql, std::string& error);
        ~SqliteStatement();

        SqliteStatement(const SqliteStatement&) = delete;
        SqliteStatement& operator=(const SqliteStatement&) = delete;

        [[nodiscard]] sqlite3_stmt* Get() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        sqlite3_stmt* _statement{};
    };

    class SqliteTransaction final
    {
    public:
        SqliteTransaction(sqlite3* database, std::string& error);
        ~SqliteTransaction();

        SqliteTransaction(const SqliteTransaction&) = delete;
        SqliteTransaction& operator=(const SqliteTransaction&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Commit(std::string& error);

    private:
        sqlite3* _database{};
        bool _active{};
    };

    [[nodiscard]] bool ExecuteSql(sqlite3* database, const char* sql, std::string& error);
    [[nodiscard]] std::optional<int64_t> QueryInt64(sqlite3* database, const char* sql, std::string& error);
    [[nodiscard]] std::optional<std::string> QueryText(sqlite3* database, const char* sql, std::string& error);
    [[nodiscard]] std::string SqliteError(sqlite3* database, int result, std::string_view operation) noexcept;
}
