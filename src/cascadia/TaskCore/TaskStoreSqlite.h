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
    [[nodiscard]] bool BindText(sqlite3* database, sqlite3_stmt* statement, int index, std::string_view value, std::string& error);
    [[nodiscard]] bool BindInt64(sqlite3* database, sqlite3_stmt* statement, int index, int64_t value, std::string& error);
    [[nodiscard]] bool BindOptionalText(sqlite3* database, sqlite3_stmt* statement, int index, const std::optional<std::string>& value, std::string& error);
    [[nodiscard]] bool BindOptionalInt64(sqlite3* database, sqlite3_stmt* statement, int index, const std::optional<int64_t>& value, std::string& error);
    [[nodiscard]] bool StepDone(sqlite3* database, sqlite3_stmt* statement, std::string& error);
    [[nodiscard]] std::optional<int64_t> QueryInt64(sqlite3* database, const char* sql, std::string& error);
    [[nodiscard]] std::optional<std::string> QueryText(sqlite3* database, const char* sql, std::string& error);
    [[nodiscard]] std::string ColumnText(sqlite3_stmt* statement, int column);
    [[nodiscard]] std::optional<std::string> ColumnOptionalText(sqlite3_stmt* statement, int column);
    [[nodiscard]] std::optional<int64_t> ColumnOptionalInt64(sqlite3_stmt* statement, int column) noexcept;
    [[nodiscard]] std::string SqliteError(sqlite3* database, int result, std::string_view operation) noexcept;
}
