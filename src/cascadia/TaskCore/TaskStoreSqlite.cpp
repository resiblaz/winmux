// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreSqlite.h"

namespace Microsoft::Terminal::TaskCore::details
{
    std::string SqliteError(sqlite3* database, const int result, const std::string_view operation) noexcept
    {
        try
        {
            std::string message{ operation };
            message.append(": ");
            message.append(database ? sqlite3_errmsg(database) : sqlite3_errstr(result));
            message.append(" (");
            message.append(std::to_string(result));
            message.push_back(')');
            return message;
        }
        catch (...)
        {
            return "SQLite operation failed";
        }
    }

    SqliteStatement::SqliteStatement(sqlite3* database, const char* sql, std::string& error)
    {
        const auto result = sqlite3_prepare_v2(database, sql, -1, &_statement, nullptr);
        if (result != SQLITE_OK)
        {
            error = SqliteError(database, result, "prepare");
            _statement = nullptr;
        }
    }

    SqliteStatement::~SqliteStatement()
    {
        if (_statement)
        {
            sqlite3_finalize(_statement);
        }
    }

    sqlite3_stmt* SqliteStatement::Get() const noexcept
    {
        return _statement;
    }

    SqliteStatement::operator bool() const noexcept
    {
        return _statement != nullptr;
    }

    bool ExecuteSql(sqlite3* database, const char* sql, std::string& error)
    {
        char* rawError{};
        const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &rawError);
        if (result == SQLITE_OK)
        {
            return true;
        }

        try
        {
            error = rawError ? rawError : SqliteError(database, result, "execute");
        }
        catch (...)
        {
            error = "SQLite execute failed";
        }
        sqlite3_free(rawError);
        return false;
    }

    SqliteTransaction::SqliteTransaction(sqlite3* database, std::string& error) :
        _database{ database },
        _active{ ExecuteSql(database, "BEGIN IMMEDIATE;", error) }
    {
    }

    SqliteTransaction::~SqliteTransaction()
    {
        if (_active)
        {
            try
            {
                std::string ignored;
                (void)ExecuteSql(_database, "ROLLBACK;", ignored);
            }
            catch (...)
            {
            }
        }
    }

    SqliteTransaction::operator bool() const noexcept
    {
        return _active;
    }

    bool SqliteTransaction::Commit(std::string& error)
    {
        if (!_active)
        {
            return false;
        }
        if (!ExecuteSql(_database, "COMMIT;", error))
        {
            return false;
        }
        _active = false;
        return true;
    }

    bool BindText(sqlite3* database, sqlite3_stmt* statement, const int index, const std::string_view value, std::string& error)
    {
        const auto result = sqlite3_bind_text64(
            statement,
            index,
            value.data(),
            static_cast<sqlite3_uint64>(value.size()),
            SQLITE_TRANSIENT,
            SQLITE_UTF8);
        if (result == SQLITE_OK)
        {
            return true;
        }
        error = SqliteError(database, result, "bind text");
        return false;
    }

    bool BindInt64(sqlite3* database, sqlite3_stmt* statement, const int index, const int64_t value, std::string& error)
    {
        const auto result = sqlite3_bind_int64(statement, index, value);
        if (result == SQLITE_OK)
        {
            return true;
        }
        error = SqliteError(database, result, "bind integer");
        return false;
    }

    bool BindOptionalText(sqlite3* database, sqlite3_stmt* statement, const int index, const std::optional<std::string>& value, std::string& error)
    {
        if (value)
        {
            return BindText(database, statement, index, value.value(), error);
        }
        const auto result = sqlite3_bind_null(statement, index);
        if (result == SQLITE_OK)
        {
            return true;
        }
        error = SqliteError(database, result, "bind null text");
        return false;
    }

    bool BindOptionalInt64(sqlite3* database, sqlite3_stmt* statement, const int index, const std::optional<int64_t>& value, std::string& error)
    {
        if (value)
        {
            return BindInt64(database, statement, index, value.value(), error);
        }
        const auto result = sqlite3_bind_null(statement, index);
        if (result == SQLITE_OK)
        {
            return true;
        }
        error = SqliteError(database, result, "bind null integer");
        return false;
    }

    bool StepDone(sqlite3* database, sqlite3_stmt* statement, std::string& error)
    {
        const auto result = sqlite3_step(statement);
        if (result == SQLITE_DONE)
        {
            return true;
        }
        error = SqliteError(database, result, "execute statement");
        return false;
    }

    std::optional<int64_t> QueryInt64(sqlite3* database, const char* sql, std::string& error)
    {
        SqliteStatement statement{ database, sql, error };
        if (!statement)
        {
            return std::nullopt;
        }
        const auto result = sqlite3_step(statement.Get());
        if (result != SQLITE_ROW)
        {
            error = SqliteError(database, result, "query integer");
            return std::nullopt;
        }
        return sqlite3_column_int64(statement.Get(), 0);
    }

    std::optional<std::string> QueryText(sqlite3* database, const char* sql, std::string& error)
    {
        SqliteStatement statement{ database, sql, error };
        if (!statement)
        {
            return std::nullopt;
        }
        const auto result = sqlite3_step(statement.Get());
        if (result != SQLITE_ROW)
        {
            error = SqliteError(database, result, "query text");
            return std::nullopt;
        }
        const auto text = sqlite3_column_text(statement.Get(), 0);
        const auto size = sqlite3_column_bytes(statement.Get(), 0);
        return text ? std::optional<std::string>{ std::string{ reinterpret_cast<const char*>(text), static_cast<size_t>(size) } } : std::optional<std::string>{ std::string{} };
    }

    std::string ColumnText(sqlite3_stmt* statement, const int column)
    {
        const auto text = sqlite3_column_text(statement, column);
        const auto size = sqlite3_column_bytes(statement, column);
        return text ? std::string{ reinterpret_cast<const char*>(text), static_cast<size_t>(size) } : std::string{};
    }

    std::optional<std::string> ColumnOptionalText(sqlite3_stmt* statement, const int column)
    {
        return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::nullopt : std::optional<std::string>{ ColumnText(statement, column) };
    }

    std::optional<int64_t> ColumnOptionalInt64(sqlite3_stmt* statement, const int column) noexcept
    {
        return sqlite3_column_type(statement, column) == SQLITE_NULL ? std::nullopt : std::optional<int64_t>{ sqlite3_column_int64(statement, column) };
    }
}
