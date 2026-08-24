// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentHistoryStore.h"

#include "../../types/inc/utils.hpp"

#include <winsqlite/winsqlite3.h>

#pragma comment(lib, "winsqlite3.lib")

namespace winrt::TerminalApp::implementation
{
    namespace
    {
        constexpr int SchemaVersion{ 1 };
        constexpr int BusyTimeoutMilliseconds{ 5000 };

        constexpr size_t MaximumProviderCharacters{ 64 };
        constexpr size_t MaximumIdentifierCharacters{ 128 };
        constexpr size_t MaximumWorkspaceTitleCharacters{ 512 };
        constexpr size_t MaximumCwdCharacters{ 32768 };
        constexpr size_t MaximumTitleCharacters{ 4096 };
        constexpr size_t MaximumBodyCharacters{ 8 * 1024 * 1024 };
        constexpr size_t MaximumModelCharacters{ 256 };
        constexpr size_t MaximumSearchCharacters{ 1024 };
        constexpr uint32_t MaximumListLimit{ 10000 };

        struct DatabaseDeleter
        {
            void operator()(sqlite3* database) const noexcept
            {
                if (database)
                {
                    sqlite3_close_v2(database);
                }
            }
        };

        struct StatementDeleter
        {
            void operator()(sqlite3_stmt* statement) const noexcept
            {
                if (statement)
                {
                    sqlite3_finalize(statement);
                }
            }
        };

        using UniqueDatabase = std::unique_ptr<sqlite3, DatabaseDeleter>;
        using UniqueStatement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

        bool _execute(sqlite3* database, const char* sql) noexcept
        {
            return database && sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
        }

        UniqueStatement _prepare(sqlite3* database, const char* sql) noexcept
        {
            sqlite3_stmt* rawStatement{ nullptr };
            if (!database || sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
            {
                return {};
            }
            return UniqueStatement{ rawStatement };
        }

        class Transaction final
        {
        public:
            explicit Transaction(sqlite3* database) noexcept :
                _database{ database },
                _active{ _execute(database, "BEGIN IMMEDIATE;") }
            {
            }

            ~Transaction()
            {
                if (_active)
                {
                    _execute(_database, "ROLLBACK;");
                }
            }

            explicit operator bool() const noexcept
            {
                return _active;
            }

            bool Commit() noexcept
            {
                if (_active && _execute(_database, "COMMIT;"))
                {
                    _active = false;
                    return true;
                }
                return false;
            }

        private:
            sqlite3* _database{ nullptr };
            bool _active{ false };
        };

        bool _toUtf8(const std::wstring_view value, std::string& result) noexcept
        {
            result.clear();
            return SUCCEEDED(til::u16u8(value, result));
        }

        bool _fromUtf8(const std::string_view value, std::wstring& result) noexcept
        {
            result.clear();
            return SUCCEEDED(til::u8u16(value, result));
        }

        bool _bindText(sqlite3_stmt* statement, const int index, const std::wstring_view value) noexcept
        {
            std::string utf8;
            if (!_toUtf8(value, utf8) || utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }

            return sqlite3_bind_text(statement,
                                     index,
                                     utf8.data(),
                                     static_cast<int>(utf8.size()),
                                     SQLITE_TRANSIENT) == SQLITE_OK;
        }

        bool _bindText(sqlite3_stmt* statement, const int index, const winrt::hstring& value) noexcept
        {
            return _bindText(statement, index, std::wstring_view{ value.c_str(), value.size() });
        }

        std::optional<winrt::hstring> _columnText(sqlite3_stmt* statement, const int index) noexcept
        {
            const auto text = sqlite3_column_text(statement, index);
            const auto byteCount = sqlite3_column_bytes(statement, index);
            if (!text || byteCount < 0)
            {
                return winrt::hstring{};
            }

            std::wstring utf16;
            if (!_fromUtf8({ reinterpret_cast<const char*>(text), static_cast<size_t>(byteCount) }, utf16))
            {
                return std::nullopt;
            }
            return winrt::hstring{ utf16 };
        }

        bool _containsControlCharacter(const std::wstring_view value) noexcept
        {
            return std::ranges::any_of(value, [](const wchar_t character) {
                return character < L' ' || character == 0x7f;
            });
        }

        bool _validKey(const std::wstring_view value, const size_t maximumLength) noexcept
        {
            return !value.empty() && value.size() <= maximumLength && !_containsControlCharacter(value);
        }

        bool _validKey(const winrt::hstring& value, const size_t maximumLength) noexcept
        {
            return _validKey(std::wstring_view{ value.c_str(), value.size() }, maximumLength);
        }

        bool _validRecord(const AgentHistoryRecord& record) noexcept
        {
            return record.WorkspaceId != winrt::guid{} &&
                   _validKey(record.Provider, MaximumProviderCharacters) &&
                   _validKey(record.SessionId, MaximumIdentifierCharacters) &&
                   _validKey(record.TurnId, MaximumIdentifierCharacters) &&
                   record.WorkspaceTitle.size() <= MaximumWorkspaceTitleCharacters &&
                   record.Cwd.size() <= MaximumCwdCharacters &&
                   record.Title.size() <= MaximumTitleCharacters &&
                   record.Prompt.size() <= MaximumBodyCharacters &&
                   record.FinalMarkdown.size() <= MaximumBodyCharacters &&
                   record.Model.size() <= MaximumModelCharacters &&
                   record.EventAt >= 0 &&
                   record.ReceivedAt >= 0;
        }

        bool _guidToUtf8(const winrt::guid& value, std::string& result) noexcept
        {
            if (value == winrt::guid{})
            {
                return false;
            }

            try
            {
                const auto text = ::Microsoft::Console::Utils::GuidToPlainString(value);
                return _toUtf8(text, result);
            }
            catch (...)
            {
                return false;
            }
        }

        std::optional<winrt::guid> _guidFromUtf8(const std::string_view value) noexcept
        {
            try
            {
                std::wstring text;
                if (!_fromUtf8(value, text) || text.size() != 36)
                {
                    return std::nullopt;
                }

                const winrt::guid parsed{ ::Microsoft::Console::Utils::GuidFromPlainString(text.c_str()) };
                if (parsed == winrt::guid{})
                {
                    return std::nullopt;
                }

                const auto canonical = ::Microsoft::Console::Utils::GuidToPlainString(parsed);
                if (CompareStringOrdinal(text.data(),
                                         static_cast<int>(text.size()),
                                         canonical.data(),
                                         static_cast<int>(canonical.size()),
                                         TRUE) != CSTR_EQUAL)
                {
                    return std::nullopt;
                }
                return parsed;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::optional<winrt::guid> _columnGuid(sqlite3_stmt* statement, const int index) noexcept
        {
            const auto text = sqlite3_column_text(statement, index);
            const auto byteCount = sqlite3_column_bytes(statement, index);
            if (!text || byteCount != 36)
            {
                return std::nullopt;
            }
            return _guidFromUtf8({ reinterpret_cast<const char*>(text), static_cast<size_t>(byteCount) });
        }

        void SQLITE_CALLBACK _containsCaseInsensitive(sqlite3_context* context,
                                                       const int argumentCount,
                                                       sqlite3_value** arguments) noexcept
        {
            if (argumentCount != 2 ||
                sqlite3_value_type(arguments[0]) == SQLITE_NULL ||
                sqlite3_value_type(arguments[1]) == SQLITE_NULL)
            {
                sqlite3_result_int(context, 0);
                return;
            }

            try
            {
                const auto sourceBytes = sqlite3_value_bytes(arguments[0]);
                const auto searchBytes = sqlite3_value_bytes(arguments[1]);
                const auto sourceText = sqlite3_value_text(arguments[0]);
                const auto searchText = sqlite3_value_text(arguments[1]);
                if (!sourceText || !searchText || sourceBytes < 0 || searchBytes < 0)
                {
                    sqlite3_result_int(context, 0);
                    return;
                }

                std::wstring source;
                std::wstring search;
                if (!_fromUtf8({ reinterpret_cast<const char*>(sourceText), static_cast<size_t>(sourceBytes) }, source) ||
                    !_fromUtf8({ reinterpret_cast<const char*>(searchText), static_cast<size_t>(searchBytes) }, search) ||
                    source.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                    search.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    sqlite3_result_int(context, 0);
                    return;
                }

                if (search.empty())
                {
                    sqlite3_result_int(context, 1);
                    return;
                }

                const auto match = FindStringOrdinal(FIND_FROMSTART,
                                                     source.data(),
                                                     static_cast<int>(source.size()),
                                                     search.data(),
                                                     static_cast<int>(search.size()),
                                                     TRUE);
                sqlite3_result_int(context, match >= 0 ? 1 : 0);
            }
            catch (const std::bad_alloc&)
            {
                sqlite3_result_error_nomem(context);
            }
            catch (...)
            {
                sqlite3_result_int(context, 0);
            }
        }

        bool _querySingleInteger(sqlite3* database, const char* sql, int& value) noexcept
        {
            const auto statement = _prepare(database, sql);
            if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return false;
            }
            value = sqlite3_column_int(statement.get(), 0);
            return sqlite3_step(statement.get()) == SQLITE_DONE;
        }

        bool _tableHasColumn(sqlite3* database, const char* tableInfoSql, const std::string_view columnName) noexcept
        {
            const auto statement = _prepare(database, tableInfoSql);
            if (!statement)
            {
                return false;
            }

            for (;;)
            {
                const auto result = sqlite3_step(statement.get());
                if (result == SQLITE_DONE)
                {
                    return false;
                }
                if (result != SQLITE_ROW)
                {
                    return false;
                }

                const auto text = sqlite3_column_text(statement.get(), 1);
                const auto byteCount = sqlite3_column_bytes(statement.get(), 1);
                if (text && byteCount >= 0 &&
                    std::string_view{ reinterpret_cast<const char*>(text), static_cast<size_t>(byteCount) } == columnName)
                {
                    return true;
                }
            }
        }

        bool _enableWal(sqlite3* database) noexcept
        {
            const auto statement = _prepare(database, "PRAGMA journal_mode=WAL;");
            if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW)
            {
                return false;
            }

            const auto text = sqlite3_column_text(statement.get(), 0);
            const auto byteCount = sqlite3_column_bytes(statement.get(), 0);
            const std::string_view mode{ reinterpret_cast<const char*>(text), text && byteCount > 0 ? static_cast<size_t>(byteCount) : 0 };
            return mode == "wal" && sqlite3_step(statement.get()) == SQLITE_DONE;
        }

        bool _initializeSchema(sqlite3* database) noexcept
        {
            int version{};
            if (!_querySingleInteger(database, "PRAGMA user_version;", version) || version < 0 || version > SchemaVersion)
            {
                return false;
            }

            Transaction transaction{ database };
            if (!transaction)
            {
                return false;
            }

            constexpr auto schema = R"sql(
                CREATE TABLE IF NOT EXISTS ai_session (
                    provider TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    workspace_id TEXT NOT NULL,
                    workspace_title TEXT NOT NULL DEFAULT '',
                    cwd TEXT NOT NULL DEFAULT '',
                    model TEXT NOT NULL DEFAULT '',
                    started_at INTEGER NOT NULL DEFAULT 0,
                    last_activity_at INTEGER NOT NULL,
                    pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1)),
                    PRIMARY KEY (provider, session_id)
                ) WITHOUT ROWID;

                CREATE TABLE IF NOT EXISTS ai_turn (
                    id INTEGER PRIMARY KEY,
                    provider TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    turn_id TEXT NOT NULL,
                    workspace_id TEXT NOT NULL,
                    workspace_title TEXT NOT NULL DEFAULT '',
                    cwd TEXT NOT NULL DEFAULT '',
                    title TEXT NOT NULL DEFAULT '',
                    prompt TEXT NOT NULL DEFAULT '',
                    final_markdown TEXT NOT NULL DEFAULT '',
                    model TEXT NOT NULL DEFAULT '',
                    event_at INTEGER NOT NULL,
                    received_at INTEGER NOT NULL,
                    pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1)),
                    UNIQUE (provider, session_id, turn_id),
                    FOREIGN KEY (provider, session_id)
                        REFERENCES ai_session (provider, session_id)
                        ON DELETE CASCADE
                );

                CREATE INDEX IF NOT EXISTS ai_turn_workspace_order
                    ON ai_turn (workspace_id, pinned DESC, event_at DESC);
                CREATE INDEX IF NOT EXISTS ai_turn_global_order
                    ON ai_turn (pinned DESC, event_at DESC);
            )sql";

            if (!_execute(database, schema))
            {
                return false;
            }

            // During development, schema version 1 briefly existed without
            // these session fields. Keep opening those databases losslessly.
            if (!_tableHasColumn(database, "PRAGMA table_info(ai_session);", "started_at") &&
                (!_execute(database, "ALTER TABLE ai_session ADD COLUMN started_at INTEGER NOT NULL DEFAULT 0;") ||
                 !_execute(database, "UPDATE ai_session SET started_at=last_activity_at WHERE started_at=0;")))
            {
                return false;
            }
            if (!_tableHasColumn(database, "PRAGMA table_info(ai_session);", "pinned") &&
                !_execute(database, "ALTER TABLE ai_session ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0 CHECK (pinned IN (0, 1));"))
            {
                return false;
            }

            if (!_execute(database, "PRAGMA user_version=1;") ||
                !transaction.Commit())
            {
                return false;
            }

            return _querySingleInteger(database, "PRAGMA user_version;", version) && version == SchemaVersion;
        }

        bool _bindSession(sqlite3_stmt* statement, const AgentHistoryRecord& record, const std::string& workspaceId) noexcept
        {
            return _bindText(statement, 1, record.Provider) &&
                   _bindText(statement, 2, record.SessionId) &&
                   sqlite3_bind_text(statement, 3, workspaceId.data(), static_cast<int>(workspaceId.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
                   _bindText(statement, 4, record.WorkspaceTitle) &&
                   _bindText(statement, 5, record.Cwd) &&
                   _bindText(statement, 6, record.Model) &&
                   sqlite3_bind_int64(statement, 7, record.EventAt) == SQLITE_OK &&
                   sqlite3_bind_int64(statement, 8, record.EventAt) == SQLITE_OK &&
                   sqlite3_bind_int(statement, 9, 0) == SQLITE_OK;
        }

        bool _bindTurn(sqlite3_stmt* statement, const AgentHistoryRecord& record, const std::string& workspaceId) noexcept
        {
            return _bindText(statement, 1, record.Provider) &&
                   _bindText(statement, 2, record.SessionId) &&
                   _bindText(statement, 3, record.TurnId) &&
                   sqlite3_bind_text(statement, 4, workspaceId.data(), static_cast<int>(workspaceId.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
                   _bindText(statement, 5, record.WorkspaceTitle) &&
                   _bindText(statement, 6, record.Cwd) &&
                   _bindText(statement, 7, record.Title) &&
                   _bindText(statement, 8, record.Prompt) &&
                   _bindText(statement, 9, record.FinalMarkdown) &&
                   _bindText(statement, 10, record.Model) &&
                   sqlite3_bind_int64(statement, 11, record.EventAt) == SQLITE_OK &&
                   sqlite3_bind_int64(statement, 12, record.ReceivedAt) == SQLITE_OK &&
                   sqlite3_bind_int(statement, 13, record.Pinned ? 1 : 0) == SQLITE_OK;
        }

        bool _bindIdentity(sqlite3_stmt* statement,
                           const std::wstring_view provider,
                           const std::wstring_view sessionId,
                           const std::optional<std::wstring_view> turnId = std::nullopt) noexcept
        {
            return _bindText(statement, 1, provider) &&
                   _bindText(statement, 2, sessionId) &&
                   (!turnId || _bindText(statement, 3, turnId.value()));
        }
    }

    struct AgentHistoryStore::Impl
    {
        bool Initialize(const std::filesystem::path& path) noexcept
        {
            try
            {
                if (path.empty() || path.native().find(L'\0') != std::wstring::npos)
                {
                    return false;
                }

                if (const auto parent = path.parent_path(); !parent.empty())
                {
                    std::error_code error;
                    std::filesystem::create_directories(parent, error);
                    if (error)
                    {
                        return false;
                    }
                }

                std::string utf8Path;
                if (!_toUtf8(path.native(), utf8Path) || utf8Path.empty())
                {
                    return false;
                }

                sqlite3* rawDatabase{ nullptr };
                const auto openResult = sqlite3_open_v2(utf8Path.c_str(),
                                                        &rawDatabase,
                                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                                        nullptr);
                UniqueDatabase candidate{ rawDatabase };
                if (openResult != SQLITE_OK || !candidate)
                {
                    return false;
                }

                sqlite3_extended_result_codes(candidate.get(), 1);
                if (sqlite3_busy_timeout(candidate.get(), BusyTimeoutMilliseconds) != SQLITE_OK ||
                    !_execute(candidate.get(), "PRAGMA foreign_keys=ON;") ||
                    !_execute(candidate.get(), "PRAGMA synchronous=NORMAL;") ||
                    !_enableWal(candidate.get()) ||
                    sqlite3_create_function_v2(candidate.get(),
                                               "wt_contains_ci",
                                               2,
                                               SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                               nullptr,
                                               &_containsCaseInsensitive,
                                               nullptr,
                                               nullptr,
                                               nullptr) != SQLITE_OK ||
                    !_initializeSchema(candidate.get()))
                {
                    return false;
                }

                int foreignKeysEnabled{};
                if (!_querySingleInteger(candidate.get(), "PRAGMA foreign_keys;", foreignKeysEnabled) || foreignKeysEnabled != 1)
                {
                    return false;
                }

                Database = std::move(candidate);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        mutable std::mutex Mutex;
        UniqueDatabase Database;
    };

    AgentHistoryStore::AgentHistoryStore(const std::filesystem::path& path) noexcept
    {
        try
        {
            auto impl = std::make_unique<Impl>();
            impl->Initialize(path);
            _impl = std::move(impl);
        }
        catch (...)
        {
        }
    }

    AgentHistoryStore::~AgentHistoryStore() = default;

    bool AgentHistoryStore::IsAvailable() const noexcept
    {
        try
        {
            if (!_impl)
            {
                return false;
            }
            const std::scoped_lock lock{ _impl->Mutex };
            return _impl->Database != nullptr;
        }
        catch (...)
        {
            return false;
        }
    }

    bool AgentHistoryStore::UpsertTurn(const AgentHistoryRecord& record) noexcept
    {
        try
        {
            if (!_impl || !_validRecord(record))
            {
                return false;
            }

            std::string workspaceId;
            if (!_guidToUtf8(record.WorkspaceId, workspaceId))
            {
                return false;
            }

            const std::scoped_lock lock{ _impl->Mutex };
            auto* const database = _impl->Database.get();
            if (!database)
            {
                return false;
            }

            Transaction transaction{ database };
            if (!transaction)
            {
                return false;
            }

            constexpr auto upsertSession = R"sql(
                INSERT INTO ai_session (
                    provider, session_id, workspace_id, workspace_title, cwd, model,
                    started_at, last_activity_at, pinned
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT (provider, session_id) DO UPDATE SET
                    workspace_id = CASE
                        WHEN excluded.last_activity_at >= ai_session.last_activity_at THEN excluded.workspace_id
                        ELSE ai_session.workspace_id
                    END,
                    workspace_title = CASE
                        WHEN excluded.workspace_title <> '' AND
                             (excluded.last_activity_at >= ai_session.last_activity_at OR ai_session.workspace_title = '')
                            THEN excluded.workspace_title
                        ELSE ai_session.workspace_title
                    END,
                    cwd = CASE
                        WHEN excluded.cwd <> '' AND
                             (excluded.last_activity_at >= ai_session.last_activity_at OR ai_session.cwd = '')
                            THEN excluded.cwd
                        ELSE ai_session.cwd
                    END,
                    model = CASE
                        WHEN excluded.model <> '' AND
                             (excluded.last_activity_at >= ai_session.last_activity_at OR ai_session.model = '')
                            THEN excluded.model
                        ELSE ai_session.model
                    END,
                    started_at = MIN(ai_session.started_at, excluded.started_at),
                    last_activity_at = MAX(ai_session.last_activity_at, excluded.last_activity_at);
            )sql";

            const auto sessionStatement = _prepare(database, upsertSession);
            if (!sessionStatement ||
                !_bindSession(sessionStatement.get(), record, workspaceId) ||
                sqlite3_step(sessionStatement.get()) != SQLITE_DONE)
            {
                return false;
            }

            constexpr auto upsertTurn = R"sql(
                INSERT INTO ai_turn (
                    provider, session_id, turn_id, workspace_id, workspace_title, cwd,
                    title, prompt, final_markdown, model, event_at, received_at, pinned
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT (provider, session_id, turn_id) DO UPDATE SET
                    workspace_id = excluded.workspace_id,
                    workspace_title = CASE WHEN excluded.workspace_title <> '' THEN excluded.workspace_title ELSE ai_turn.workspace_title END,
                    cwd = CASE WHEN excluded.cwd <> '' THEN excluded.cwd ELSE ai_turn.cwd END,
                    title = CASE WHEN excluded.title <> '' THEN excluded.title ELSE ai_turn.title END,
                    prompt = CASE WHEN excluded.prompt <> '' THEN excluded.prompt ELSE ai_turn.prompt END,
                    final_markdown = CASE WHEN excluded.final_markdown <> '' THEN excluded.final_markdown ELSE ai_turn.final_markdown END,
                    model = CASE WHEN excluded.model <> '' THEN excluded.model ELSE ai_turn.model END,
                    event_at = MAX(ai_turn.event_at, excluded.event_at),
                    received_at = MAX(ai_turn.received_at, excluded.received_at),
                    pinned = MAX(ai_turn.pinned, excluded.pinned);
            )sql";

            const auto turnStatement = _prepare(database, upsertTurn);
            if (!turnStatement ||
                !_bindTurn(turnStatement.get(), record, workspaceId) ||
                sqlite3_step(turnStatement.get()) != SQLITE_DONE)
            {
                return false;
            }

            return transaction.Commit();
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<AgentHistoryRecord> AgentHistoryStore::ListTurns(const std::optional<winrt::guid> workspaceId,
                                                                  const std::wstring_view search,
                                                                  const uint32_t limit) noexcept
    {
        try
        {
            if (!_impl ||
                (workspaceId && workspaceId.value() == winrt::guid{}) ||
                search.size() > MaximumSearchCharacters ||
                limit == 0)
            {
                return {};
            }

            std::string workspaceIdUtf8;
            if (workspaceId && !_guidToUtf8(workspaceId.value(), workspaceIdUtf8))
            {
                return {};
            }

            const std::scoped_lock lock{ _impl->Mutex };
            auto* const database = _impl->Database.get();
            if (!database)
            {
                return {};
            }

            constexpr auto listTurns = R"sql(
                SELECT
                    workspace_id, workspace_title, provider, session_id, turn_id, cwd,
                    title, prompt, final_markdown, model, event_at, received_at, pinned
                FROM ai_turn
                WHERE (?1 IS NULL OR workspace_id = ?1)
                  AND (?2 = '' OR
                       wt_contains_ci(workspace_title, ?2) OR
                       wt_contains_ci(provider, ?2) OR
                       wt_contains_ci(session_id, ?2) OR
                       wt_contains_ci(turn_id, ?2) OR
                       wt_contains_ci(cwd, ?2) OR
                       wt_contains_ci(title, ?2) OR
                       wt_contains_ci(prompt, ?2) OR
                       wt_contains_ci(final_markdown, ?2) OR
                       wt_contains_ci(model, ?2))
                ORDER BY pinned DESC, event_at DESC, received_at DESC, id DESC
                LIMIT ?3;
            )sql";

            const auto statement = _prepare(database, listTurns);
            if (!statement)
            {
                return {};
            }

            if ((workspaceId ? sqlite3_bind_text(statement.get(),
                                                  1,
                                                  workspaceIdUtf8.data(),
                                                  static_cast<int>(workspaceIdUtf8.size()),
                                                  SQLITE_TRANSIENT) :
                               sqlite3_bind_null(statement.get(), 1)) != SQLITE_OK ||
                !_bindText(statement.get(), 2, search) ||
                sqlite3_bind_int64(statement.get(), 3, std::min(limit, MaximumListLimit)) != SQLITE_OK)
            {
                return {};
            }

            std::vector<AgentHistoryRecord> records;
            records.reserve(std::min(limit, MaximumListLimit));
            for (;;)
            {
                const auto result = sqlite3_step(statement.get());
                if (result == SQLITE_DONE)
                {
                    return records;
                }
                if (result != SQLITE_ROW)
                {
                    return {};
                }

                const auto rowWorkspaceId = _columnGuid(statement.get(), 0);
                const auto workspaceTitle = _columnText(statement.get(), 1);
                const auto provider = _columnText(statement.get(), 2);
                const auto sessionId = _columnText(statement.get(), 3);
                const auto turnId = _columnText(statement.get(), 4);
                const auto cwd = _columnText(statement.get(), 5);
                const auto title = _columnText(statement.get(), 6);
                const auto prompt = _columnText(statement.get(), 7);
                const auto finalMarkdown = _columnText(statement.get(), 8);
                const auto model = _columnText(statement.get(), 9);
                if (!rowWorkspaceId || !workspaceTitle || !provider || !sessionId || !turnId ||
                    !cwd || !title || !prompt || !finalMarkdown || !model)
                {
                    continue;
                }

                AgentHistoryRecord record{
                    .WorkspaceId = rowWorkspaceId.value(),
                    .WorkspaceTitle = workspaceTitle.value(),
                    .Provider = provider.value(),
                    .SessionId = sessionId.value(),
                    .TurnId = turnId.value(),
                    .Cwd = cwd.value(),
                    .Title = title.value(),
                    .Prompt = prompt.value(),
                    .FinalMarkdown = finalMarkdown.value(),
                    .Model = model.value(),
                    .EventAt = sqlite3_column_int64(statement.get(), 10),
                    .ReceivedAt = sqlite3_column_int64(statement.get(), 11),
                    .Pinned = sqlite3_column_int(statement.get(), 12) != 0,
                };

                if (_validRecord(record))
                {
                    records.emplace_back(std::move(record));
                }
            }
        }
        catch (...)
        {
            return {};
        }
    }

    bool AgentHistoryStore::SetTurnPinned(const std::wstring_view provider,
                                           const std::wstring_view sessionId,
                                           const std::wstring_view turnId,
                                           const bool pinned) noexcept
    {
        try
        {
            if (!_impl ||
                !_validKey(provider, MaximumProviderCharacters) ||
                !_validKey(sessionId, MaximumIdentifierCharacters) ||
                !_validKey(turnId, MaximumIdentifierCharacters))
            {
                return false;
            }

            const std::scoped_lock lock{ _impl->Mutex };
            auto* const database = _impl->Database.get();
            const auto statement = _prepare(database,
                                            "UPDATE ai_turn SET pinned=?4 "
                                            "WHERE provider=?1 AND session_id=?2 AND turn_id=?3;");
            if (!statement ||
                !_bindIdentity(statement.get(), provider, sessionId, turnId) ||
                sqlite3_bind_int(statement.get(), 4, pinned ? 1 : 0) != SQLITE_OK ||
                sqlite3_step(statement.get()) != SQLITE_DONE)
            {
                return false;
            }
            return sqlite3_changes(database) > 0;
        }
        catch (...)
        {
            return false;
        }
    }

    bool AgentHistoryStore::DeleteTurn(const std::wstring_view provider,
                                        const std::wstring_view sessionId,
                                        const std::wstring_view turnId) noexcept
    {
        try
        {
            if (!_impl ||
                !_validKey(provider, MaximumProviderCharacters) ||
                !_validKey(sessionId, MaximumIdentifierCharacters) ||
                !_validKey(turnId, MaximumIdentifierCharacters))
            {
                return false;
            }

            const std::scoped_lock lock{ _impl->Mutex };
            auto* const database = _impl->Database.get();
            if (!database)
            {
                return false;
            }

            Transaction transaction{ database };
            if (!transaction)
            {
                return false;
            }

            const auto deleteTurn = _prepare(database,
                                             "DELETE FROM ai_turn "
                                             "WHERE provider=?1 AND session_id=?2 AND turn_id=?3;");
            if (!deleteTurn ||
                !_bindIdentity(deleteTurn.get(), provider, sessionId, turnId) ||
                sqlite3_step(deleteTurn.get()) != SQLITE_DONE)
            {
                return false;
            }
            const auto deleted = sqlite3_changes(database) > 0;

            const auto deleteOrphan = _prepare(database,
                                               "DELETE FROM ai_session "
                                               "WHERE provider=?1 AND session_id=?2 "
                                               "AND NOT EXISTS ("
                                               "    SELECT 1 FROM ai_turn "
                                               "    WHERE provider=?1 AND session_id=?2"
                                               ");");
            if (!deleteOrphan ||
                !_bindIdentity(deleteOrphan.get(), provider, sessionId) ||
                sqlite3_step(deleteOrphan.get()) != SQLITE_DONE ||
                !transaction.Commit())
            {
                return false;
            }
            return deleted;
        }
        catch (...)
        {
            return false;
        }
    }

    bool AgentHistoryStore::DeleteSession(const std::wstring_view provider,
                                           const std::wstring_view sessionId) noexcept
    {
        try
        {
            if (!_impl ||
                !_validKey(provider, MaximumProviderCharacters) ||
                !_validKey(sessionId, MaximumIdentifierCharacters))
            {
                return false;
            }

            const std::scoped_lock lock{ _impl->Mutex };
            auto* const database = _impl->Database.get();
            const auto statement = _prepare(database,
                                            "DELETE FROM ai_session "
                                            "WHERE provider=?1 AND session_id=?2;");
            if (!statement ||
                !_bindIdentity(statement.get(), provider, sessionId) ||
                sqlite3_step(statement.get()) != SQLITE_DONE)
            {
                return false;
            }
            return sqlite3_changes(database) > 0;
        }
        catch (...)
        {
            return false;
        }
    }

    std::filesystem::path AgentHistoryStore::DefaultPath()
    {
        try
        {
            const std::filesystem::path settingsPath{
                std::wstring_view{ winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() }
            };
            return settingsPath.parent_path() / L"workspace-history.db";
        }
        catch (...)
        {
            return {};
        }
    }
}
