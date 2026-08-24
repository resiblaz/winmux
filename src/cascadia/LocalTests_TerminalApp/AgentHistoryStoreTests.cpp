// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/AgentHistoryInbox.h"
#include "../TerminalApp/AgentHistoryStore.h"
#include "../TerminalApp/WorkspaceAutomation.h"

#include <barrier>
#include <fstream>
#include <thread>
#include <winrt/Windows.Storage.h>
#include <winsqlite/winsqlite3.h>

#pragma comment(lib, "winsqlite3.lib")

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

using winrt::TerminalApp::implementation::AgentHistoryEventKind;
using winrt::TerminalApp::implementation::AgentHistoryEventReceipt;
using winrt::TerminalApp::implementation::AgentHistoryInbox;
using winrt::TerminalApp::implementation::AgentHistoryInboxWorkspace;
using winrt::TerminalApp::implementation::AgentHistoryRecord;
using winrt::TerminalApp::implementation::AgentHistorySessionRecord;
using winrt::TerminalApp::implementation::AgentHistoryStore;
using winrt::TerminalApp::implementation::WorkspaceAutomationMethod;
using winrt::TerminalApp::implementation::WorkspaceAutomationProtocol;

namespace TerminalAppLocalTests
{
    namespace
    {
        constexpr std::wstring_view TestDatabasePrefix{ L"agent-history-store-" };
        constexpr std::wstring_view TestInboxPrefix{ L"agent-history-inbox-" };

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

        UniqueDatabase OpenDatabase(const std::filesystem::path& path)
        {
            sqlite3* rawDatabase{ nullptr };
            const auto result = sqlite3_open16(path.c_str(), &rawDatabase);
            UniqueDatabase database{ rawDatabase };
            VERIFY_ARE_EQUAL(SQLITE_OK, result);
            VERIFY_IS_NOT_NULL(database.get());
            return database;
        }

        UniqueStatement Prepare(sqlite3* database, const char* sql)
        {
            sqlite3_stmt* rawStatement{ nullptr };
            const auto result = sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr);
            UniqueStatement statement{ rawStatement };
            VERIFY_ARE_EQUAL(SQLITE_OK, result);
            VERIFY_IS_NOT_NULL(statement.get());
            return statement;
        }

        int QueryInteger(sqlite3* database, const char* sql)
        {
            const auto statement = Prepare(database, sql);
            VERIFY_ARE_EQUAL(SQLITE_ROW, sqlite3_step(statement.get()));
            const auto value = sqlite3_column_int(statement.get(), 0);
            VERIFY_ARE_EQUAL(SQLITE_DONE, sqlite3_step(statement.get()));
            return value;
        }

        std::string QueryText(sqlite3* database, const char* sql)
        {
            const auto statement = Prepare(database, sql);
            VERIFY_ARE_EQUAL(SQLITE_ROW, sqlite3_step(statement.get()));

            const auto text = sqlite3_column_text(statement.get(), 0);
            const auto length = sqlite3_column_bytes(statement.get(), 0);
            VERIFY_IS_NOT_NULL(text);
            VERIFY_IS_GREATER_THAN_OR_EQUAL(length, 0);

            const std::string value{ reinterpret_cast<const char*>(text), static_cast<size_t>(length) };
            VERIFY_ARE_EQUAL(SQLITE_DONE, sqlite3_step(statement.get()));
            return value;
        }

        void Execute(sqlite3* database, const char* sql)
        {
            VERIFY_ARE_EQUAL(SQLITE_OK, sqlite3_exec(database, sql, nullptr, nullptr, nullptr));
        }

        winrt::guid Guid(const wchar_t* value)
        {
            return ::Microsoft::Console::Utils::GuidFromPlainString(value);
        }

        AgentHistoryRecord MakeRecord(const winrt::guid& workspaceId,
                                      const std::wstring_view sessionId,
                                      const std::wstring_view turnId,
                                      const int64_t eventAt)
        {
            AgentHistoryRecord record;
            record.WorkspaceId = workspaceId;
            record.WorkspaceTitle = L"Workspace";
            record.Provider = L"codex";
            record.SessionId = winrt::hstring{ sessionId };
            record.TurnId = winrt::hstring{ turnId };
            record.Cwd = L"C:\\work";
            record.Title = L"Completed turn";
            record.Prompt = L"Original prompt";
            record.FinalMarkdown = L"Original final answer";
            record.Model = L"gpt-test";
            record.EventAt = eventAt;
            record.ReceivedAt = eventAt + 1;
            return record;
        }

        AgentHistoryEventReceipt MakeEvent(const winrt::guid& eventId,
                                           const AgentHistoryRecord& record)
        {
            return {
                .EventId = eventId,
                .Cwd = record.Cwd,
                .Title = record.Title,
                .Prompt = record.Prompt,
                .FinalMarkdown = record.FinalMarkdown,
                .Model = record.Model,
                .EventAt = record.EventAt,
            };
        }

        AgentHistorySessionRecord MakeSessionSummary(const winrt::guid& workspaceId,
                                                      const std::wstring_view sessionId,
                                                      const int64_t createdAt,
                                                      const int64_t updatedAt,
                                                      const int64_t seenAt)
        {
            AgentHistorySessionRecord record;
            record.WorkspaceId = workspaceId;
            record.WorkspaceTitle = L"Workspace summary";
            record.Provider = L"codex";
            record.SessionId = winrt::hstring{ sessionId };
            record.Cwd = L"C:\\summary";
            record.DisplayName = L"App Server session";
            record.Preview = L"Privacy-bounded preview";
            record.SourceKind = L"cli";
            record.CreatedAt = createdAt;
            record.UpdatedAt = updatedAt;
            record.SeenAt = seenAt;
            return record;
        }

        std::string HistoryEnvelope(const std::string_view eventId,
                                    const std::string_view workspaceId,
                                    const std::string_view event,
                                    const std::string_view fields)
        {
            return "{\"version\":1,\"id\":\"test-request\",\"method\":\"history.ingest\",\"params\":{"
                   "\"eventId\":\"" + std::string{ eventId } +
                   "\",\"workspaceId\":\"" + std::string{ workspaceId } +
                   "\",\"sessionId\":\"session-1\",\"turnId\":\"turn-1\",\"event\":\"" +
                   std::string{ event } + "\"" + std::string{ fields } + "}}";
        }

        void WriteEventFile(const std::filesystem::path& path, const std::string_view contents)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                VERIFY_IS_FALSE(static_cast<bool>(error));
                return;
            }

            std::ofstream stream{ path, std::ios::binary | std::ios::trunc };
            if (!stream)
            {
                VERIFY_IS_TRUE(static_cast<bool>(stream));
                return;
            }
            stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            stream.close();
            if (!stream)
            {
                VERIFY_IS_TRUE(static_cast<bool>(stream));
            }
        }

        bool CreateEmptyFile(const std::filesystem::path& path) noexcept
        {
            const wil::unique_hfile file{ CreateFileW(path.c_str(),
                                                      GENERIC_WRITE,
                                                      0,
                                                      nullptr,
                                                      CREATE_NEW,
                                                      FILE_ATTRIBUTE_NORMAL,
                                                      nullptr) };
            return static_cast<bool>(file);
        }

        size_t CountJsonFiles(const std::filesystem::path& path)
        {
            size_t count{};
            std::error_code error;
            for (std::filesystem::directory_iterator iterator{ path, error }, end; !error && iterator != end; iterator.increment(error))
            {
                if (iterator->path().extension() == L".json")
                {
                    ++count;
                }
            }
            VERIFY_IS_FALSE(static_cast<bool>(error));
            return count;
        }

        struct DirectoryUsage
        {
            size_t Entries{};
            uintmax_t Bytes{};
        };

        DirectoryUsage GetDirectoryUsage(const std::filesystem::path& path)
        {
            DirectoryUsage usage;
            std::error_code error;
            for (std::filesystem::directory_iterator iterator{ path, error }, end; !error && iterator != end; iterator.increment(error))
            {
                ++usage.Entries;
                const auto status = iterator->symlink_status(error);
                if (!error && std::filesystem::is_regular_file(status))
                {
                    usage.Bytes += iterator->file_size(error);
                }
            }
            VERIFY_IS_FALSE(static_cast<bool>(error));
            return usage;
        }
    }

    class AgentHistoryStoreTests
    {
        BEGIN_TEST_CLASS(AgentHistoryStoreTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(InitializesSchemaAndWal);
        TEST_METHOD(MigratesVersionOneSchema);
        TEST_METHOD(MigratesVersionTwoSchema);
        TEST_METHOD(SessionSummariesUpsertListSortAndFilter);
        TEST_METHOD(SessionSummaryMergePreservesHookAuthority);
        TEST_METHOD(SessionSummaryDeletionAndValidationBoundaries);
        TEST_METHOD(UpsertIsIdempotentAndPreservesPrompt);
        TEST_METHOD(StagedUpsertHidesIncompleteTurns);
        TEST_METHOD(PromptThenFinalUsesFinalMetadata);
        TEST_METHOD(FinalThenPromptPreservesFinalMetadata);
        TEST_METHOD(EventReceiptsAckReplayAndRejectConflicts);
        TEST_METHOD(FiltersByWorkspaceAndUnicodeSearch);
        TEST_METHOD(PinnedTurnsSortFirst);
        TEST_METHOD(DeleteTurnAndSession);
        TEST_METHOD(PersistsAcrossReopen);
        TEST_METHOD(ConclusionProtocolAcceptsOptionalPrompt);
        TEST_METHOD(HistoryIngestProtocolIsStrict);
        TEST_METHOD(InboxImportsStagesAndDeletesCommittedFiles);
        TEST_METHOD(InboxImportsAndReplaysEmergencySlots);
        TEST_METHOD(InboxRetainsEmergencySlotWhileProducerWrites);
        TEST_METHOD(InboxPrioritizesEmergencySlotsWithinFileLimit);
        TEST_METHOD(InboxDefersOtherWorkspacesAndRetainsCommitFailures);
        TEST_METHOD(InboxQuarantinesInvalidAndProhibitedPayloads);
        TEST_METHOD(InboxBoundsFileSizeAndCount);
        TEST_METHOD(InboxBoundsAllDirectoryEntries);
        TEST_METHOD(InboxRejectsReparsePointRoot);
        TEST_METHOD(ConcurrentInboxesShareQuarantineBudget);
        TEST_METHOD(InboxPathBuildersAreDeterministic);

        TEST_METHOD_SETUP(MethodSetup)
        {
            try
            {
                const auto temporaryFolder = winrt::Windows::Storage::ApplicationData::Current().TemporaryFolder();
                _temporaryFolderPath = std::filesystem::path{ std::wstring_view{ temporaryFolder.Path() } }.lexically_normal();

                GUID id{};
                if (FAILED(CoCreateGuid(&id)))
                {
                    return false;
                }

                const auto filename = fmt::format(FMT_COMPILE(L"{}{}.db"),
                                                  TestDatabasePrefix,
                                                  ::Microsoft::Console::Utils::GuidToPlainString(id));
                _databasePath = (_temporaryFolderPath / filename).lexically_normal();
                const auto inboxName = fmt::format(FMT_COMPILE(L"{}{}"),
                                                   TestInboxPrefix,
                                                   ::Microsoft::Console::Utils::GuidToPlainString(id));
                _inboxPath = (_temporaryFolderPath / inboxName).lexically_normal();
                return _CleanupTestFiles();
            }
            catch (...)
            {
                return false;
            }
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
            return _CleanupTestFiles();
        }

    private:
        bool _CleanupTestFiles() const noexcept
        {
            try
            {
                if (_databasePath.empty() ||
                    _databasePath.parent_path() != _temporaryFolderPath ||
                    _databasePath.extension() != L".db" ||
                    _databasePath.filename().native().rfind(TestDatabasePrefix, 0) != 0)
                {
                    return _databasePath.empty();
                }

                bool succeeded = true;
                for (const auto suffix : { std::wstring_view{}, std::wstring_view{ L"-wal" }, std::wstring_view{ L"-shm" } })
                {
                    auto candidate = _databasePath;
                    if (!suffix.empty())
                    {
                        candidate = std::filesystem::path{ _databasePath.native() + std::wstring{ suffix } };
                    }

                    std::error_code error;
                    std::filesystem::remove(candidate, error);
                    succeeded = succeeded && !error;
                }

                if (_inboxPath.empty() ||
                    _inboxPath.parent_path() != _temporaryFolderPath ||
                    _inboxPath.filename().native().rfind(TestInboxPrefix, 0) != 0)
                {
                    return false;
                }

                std::error_code error;
                std::filesystem::remove_all(_inboxPath, error);
                succeeded = succeeded && !error;
                return succeeded;
            }
            catch (...)
            {
                return false;
            }
        }

        std::filesystem::path _temporaryFolderPath;
        std::filesystem::path _databasePath;
        std::filesystem::path _inboxPath;
    };

    void AgentHistoryStoreTests::InitializesSchemaAndWal()
    {
        AgentHistoryStore store{ _databasePath };
        VERIFY_IS_TRUE(store.IsAvailable());
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(_databasePath));

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(3, QueryInteger(database.get(), "PRAGMA user_version;"));
        VERIFY_IS_TRUE(QueryText(database.get(), "PRAGMA journal_mode;") == "wal");
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='ai_session';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='ai_turn';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='ai_history_event';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_history_event') WHERE name='payload_digest' AND type='BLOB';"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_history_event') WHERE name IN ('cwd', 'title', 'prompt', 'final_markdown', 'model', 'event_at');"));
        VERIFY_ARE_EQUAL(3, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_turn') WHERE name IN ('prompt', 'final_markdown', 'pinned');"));
        VERIFY_ARE_EQUAL(8, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_session') WHERE name IN ('started_at', 'pinned', 'summary_display_name', 'summary_preview', 'summary_source_kind', 'summary_created_at', 'summary_updated_at', 'summary_seen_at');"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_session') WHERE name IN ('raw_json', 'tool_output', 'transcript_path', 'rollout_path', 'turns_json');"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='ai_turn_workspace_order';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='ai_turn_global_order';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='ai_session_workspace_order';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='ai_session_global_order';"));
    }

    void AgentHistoryStoreTests::MigratesVersionOneSchema()
    {
        {
            const auto database = OpenDatabase(_databasePath);
            Execute(database.get(), R"sql(
                PRAGMA foreign_keys=ON;
                CREATE TABLE ai_session (
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
                CREATE TABLE ai_turn (
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
                INSERT INTO ai_session (
                    provider, session_id, workspace_id, workspace_title, cwd, model,
                    started_at, last_activity_at, pinned
                ) VALUES (
                    'codex', 'm1-session', '12121212-1212-4212-8212-121212121212',
                    'M1 workspace', 'C:\m1', 'm1-model', 50, 50, 0
                );
                INSERT INTO ai_turn (
                    provider, session_id, turn_id, workspace_id, workspace_title, cwd,
                    title, prompt, final_markdown, model, event_at, received_at, pinned
                ) VALUES (
                    'codex', 'm1-session', 'm1-turn', '12121212-1212-4212-8212-121212121212',
                    'M1 workspace', 'C:\m1', 'M1 title', 'M1 prompt', 'M1 final',
                    'm1-model', 50, 51, 0
                );
                PRAGMA user_version=1;
            )sql");
        }

        AgentHistoryStore store{ _databasePath };
        VERIFY_IS_TRUE(store.IsAvailable());

        const auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_ARE_EQUAL(L"M1 prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"M1 final", records[0].FinalMarkdown);

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(3, QueryInteger(database.get(), "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='ai_history_event';"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM ai_history_event;"));
        VERIFY_ARE_EQUAL(6, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_session') WHERE name LIKE 'summary_%';"));
    }

    void AgentHistoryStoreTests::MigratesVersionTwoSchema()
    {
        {
            const auto database = OpenDatabase(_databasePath);
            Execute(database.get(), R"sql(
                PRAGMA foreign_keys=ON;
                CREATE TABLE ai_session (
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
                CREATE TABLE ai_turn (
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
                CREATE TABLE ai_history_event (
                    event_id TEXT PRIMARY KEY,
                    workspace_id TEXT NOT NULL,
                    provider TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    turn_id TEXT NOT NULL,
                    event_kind INTEGER NOT NULL CHECK (event_kind IN (0, 1)),
                    payload_digest BLOB NOT NULL CHECK (
                        typeof(payload_digest) = 'blob' AND length(payload_digest) = 32
                    )
                ) WITHOUT ROWID;
                INSERT INTO ai_session (
                    provider, session_id, workspace_id, workspace_title, cwd, model,
                    started_at, last_activity_at, pinned
                ) VALUES (
                    'codex', 'm2-session', '13131313-1313-4313-8313-131313131313',
                    'M2 workspace', 'C:\m2', 'm2-model', 75, 75, 0
                );
                INSERT INTO ai_turn (
                    provider, session_id, turn_id, workspace_id, workspace_title, cwd,
                    title, prompt, final_markdown, model, event_at, received_at, pinned
                ) VALUES (
                    'codex', 'm2-session', 'm2-turn', '13131313-1313-4313-8313-131313131313',
                    'M2 workspace', 'C:\m2', 'M2 title', 'M2 prompt', 'M2 final',
                    'm2-model', 75, 76, 0
                );
                INSERT INTO ai_history_event (
                    event_id, workspace_id, provider, session_id, turn_id, event_kind, payload_digest
                ) VALUES (
                    '93000000-0000-4000-8000-000000000001',
                    '13131313-1313-4313-8313-131313131313',
                    'codex', 'm2-session', 'm2-turn', 1, zeroblob(32)
                );
                PRAGMA user_version=2;
            )sql");
        }

        AgentHistoryStore store{ _databasePath };
        VERIFY_IS_TRUE(store.IsAvailable());

        const auto turns = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, turns.size());
        VERIFY_ARE_EQUAL(L"M2 prompt", turns[0].Prompt);
        VERIFY_ARE_EQUAL(L"M2 final", turns[0].FinalMarkdown);

        const auto sessions = store.ListSessionSummaries(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, sessions.size());
        VERIFY_ARE_EQUAL(L"m2-session", sessions[0].SessionId);
        VERIFY_IS_TRUE(sessions[0].HasLocalTurns);
        VERIFY_ARE_EQUAL(int64_t{ 75 }, sessions[0].CreatedAt);
        VERIFY_ARE_EQUAL(int64_t{ 75 }, sessions[0].UpdatedAt);
        VERIFY_ARE_EQUAL(int64_t{ 0 }, sessions[0].SeenAt);

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(3, QueryInteger(database.get(), "PRAGMA user_version;"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_history_event WHERE length(payload_digest)=32;"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_turn;"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_session;"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT summary_seen_at FROM ai_session WHERE session_id='m2-session';"));
    }

    void AgentHistoryStoreTests::SessionSummariesUpsertListSortAndFilter()
    {
        const auto workspaceOne = Guid(L"14141414-1414-4414-8414-141414141414");
        const auto workspaceTwo = Guid(L"15151515-1515-4515-8515-151515151515");
        AgentHistoryStore store{ _databasePath };

        auto first = MakeSessionSummary(workspaceOne, L"summary-a", 10, 100, 1000);
        first.DisplayName = L"Architecture notes";
        first.Preview = L"Privacy-bounded App Server preview";
        VERIFY_IS_TRUE(store.UpsertSessionSummary(first));
        VERIFY_IS_TRUE(store.UpsertSessionSummary(first));

        auto second = MakeSessionSummary(workspaceTwo, L"summary-b", 20, 300, 900);
        second.WorkspaceTitle = L"研发工作区";
        second.DisplayName = L"研发会话";
        second.Preview = L"修复 Unicode 搜索";
        second.SourceKind = L"vscode";
        VERIFY_IS_TRUE(store.UpsertSessionSummary(second));

        auto localTurn = MakeRecord(workspaceOne, L"hook-session", L"hook-turn", 200);
        localTurn.WorkspaceTitle = L"Hook workspace";
        VERIFY_IS_TRUE(store.UpsertTurn(localTurn));

        const auto all = store.ListSessionSummaries(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 3 }, all.size());
        VERIFY_ARE_EQUAL(L"summary-b", all[0].SessionId);
        VERIFY_ARE_EQUAL(L"hook-session", all[1].SessionId);
        VERIFY_ARE_EQUAL(L"summary-a", all[2].SessionId);
        VERIFY_IS_FALSE(all[0].HasLocalTurns);
        VERIFY_IS_TRUE(all[1].HasLocalTurns);
        VERIFY_ARE_EQUAL(L"gpt-test", all[1].Model);
        VERIFY_ARE_EQUAL(L"", all[2].Model);

        const auto workspaceMatches = store.ListSessionSummaries(std::optional{ workspaceTwo }, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, workspaceMatches.size());
        VERIFY_ARE_EQUAL(L"summary-b", workspaceMatches[0].SessionId);

        const auto unicodeMatches = store.ListSessionSummaries(std::nullopt, L"unicode 搜索");
        VERIFY_ARE_EQUAL(size_t{ 1 }, unicodeMatches.size());
        VERIFY_ARE_EQUAL(L"summary-b", unicodeMatches[0].SessionId);

        const auto caseInsensitiveMatches = store.ListSessionSummaries(std::nullopt, L"PRIVACY-BOUNDED");
        VERIFY_ARE_EQUAL(size_t{ 1 }, caseInsensitiveMatches.size());
        VERIFY_ARE_EQUAL(L"summary-a", caseInsensitiveMatches[0].SessionId);

        const auto limited = store.ListSessionSummaries(std::nullopt, L"", 1);
        VERIFY_ARE_EQUAL(size_t{ 1 }, limited.size());
        VERIFY_ARE_EQUAL(L"summary-b", limited[0].SessionId);
        VERIFY_IS_TRUE(store.ListSessionSummaries(std::nullopt, std::wstring(1025, L'x')).empty());
        VERIFY_IS_TRUE(store.ListSessionSummaries(std::optional{ winrt::guid{} }, L"").empty());
        VERIFY_IS_TRUE(store.ListSessionSummaries(std::nullopt, L"", 0).empty());

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(3, QueryInteger(database.get(), "SELECT count(*) FROM ai_session;"));
        VERIFY_ARE_EQUAL(2, QueryInteger(database.get(), "SELECT count(*) FROM ai_session WHERE summary_seen_at > 0;"));
    }

    void AgentHistoryStoreTests::SessionSummaryMergePreservesHookAuthority()
    {
        const auto summaryWorkspace = Guid(L"16161616-1616-4616-8616-161616161616");
        const auto hookWorkspace = Guid(L"17171717-1717-4717-8717-171717171717");
        const auto newerHookWorkspace = Guid(L"18181818-1818-4818-8818-181818181818");
        AgentHistoryStore store{ _databasePath };

        auto summary = MakeSessionSummary(summaryWorkspace, L"merged-session", 100, 200, 1000);
        summary.Model = L"summary-model";
        VERIFY_IS_TRUE(store.UpsertSessionSummary(summary));

        auto hook = MakeRecord(hookWorkspace, L"merged-session", L"hook-turn-1", 500);
        hook.WorkspaceTitle = L"Authoritative Hook workspace";
        hook.Cwd = L"C:\\hook-authoritative";
        hook.Model = L"gpt-hook";
        VERIFY_IS_TRUE(store.UpsertTurn(hook));

        auto staleSummary = MakeSessionSummary(summaryWorkspace, L"merged-session", 50, 100, 2000);
        staleSummary.WorkspaceTitle = L"Must not replace Hook workspace";
        staleSummary.Cwd = L"C:\\must-not-replace";
        staleSummary.Model = L"must-not-replace-model";
        staleSummary.DisplayName = L"Fresh display from an old thread";
        staleSummary.Preview = L"Display metadata may refresh independently";
        VERIFY_IS_TRUE(store.UpsertSessionSummary(staleSummary));

        auto olderObservation = staleSummary;
        olderObservation.SeenAt = 1500;
        olderObservation.DisplayName = L"Must not roll back display";
        VERIFY_IS_TRUE(store.UpsertSessionSummary(olderObservation));

        auto newerSummary = staleSummary;
        newerSummary.CreatedAt = 80;
        newerSummary.UpdatedAt = 900;
        newerSummary.SeenAt = 3000;
        newerSummary.DisplayName = L"Latest display name";
        newerSummary.Preview = L"Latest preview";
        newerSummary.Cwd = L"C:\\newer-summary-still-supplemental";
        VERIFY_IS_TRUE(store.UpsertSessionSummary(newerSummary));

        auto newerHook = MakeRecord(newerHookWorkspace, L"merged-session", L"hook-turn-2", 1000);
        newerHook.WorkspaceTitle = L"Newest Hook workspace";
        newerHook.Cwd = L"C:\\newest-hook";
        newerHook.Model = L"gpt-newest-hook";
        VERIFY_IS_TRUE(store.UpsertTurn(newerHook));

        const auto sessions = store.ListSessionSummaries(std::nullopt, L"latest display");
        VERIFY_ARE_EQUAL(size_t{ 1 }, sessions.size());
        VERIFY_IS_TRUE(sessions[0].WorkspaceId == newerHookWorkspace);
        VERIFY_ARE_EQUAL(L"Newest Hook workspace", sessions[0].WorkspaceTitle);
        VERIFY_ARE_EQUAL(L"C:\\newest-hook", sessions[0].Cwd);
        VERIFY_ARE_EQUAL(L"gpt-newest-hook", sessions[0].Model);
        VERIFY_ARE_EQUAL(L"Latest display name", sessions[0].DisplayName);
        VERIFY_ARE_EQUAL(L"Latest preview", sessions[0].Preview);
        VERIFY_ARE_EQUAL(int64_t{ 80 }, sessions[0].CreatedAt);
        VERIFY_ARE_EQUAL(int64_t{ 900 }, sessions[0].UpdatedAt);
        VERIFY_ARE_EQUAL(int64_t{ 3000 }, sessions[0].SeenAt);
        VERIFY_IS_TRUE(sessions[0].HasLocalTurns);

        const auto database = OpenDatabase(_databasePath);
        VERIFY_IS_TRUE(QueryText(database.get(), "SELECT workspace_id FROM ai_session WHERE session_id='merged-session';") ==
                       "18181818-1818-4818-8818-181818181818");
        VERIFY_IS_TRUE(QueryText(database.get(), "SELECT cwd FROM ai_session WHERE session_id='merged-session';") == "C:\\newest-hook");
        VERIFY_IS_TRUE(QueryText(database.get(), "SELECT model FROM ai_session WHERE session_id='merged-session';") == "gpt-newest-hook");
        VERIFY_ARE_EQUAL(500, QueryInteger(database.get(), "SELECT started_at FROM ai_session WHERE session_id='merged-session';"));
        VERIFY_ARE_EQUAL(1000, QueryInteger(database.get(), "SELECT last_activity_at FROM ai_session WHERE session_id='merged-session';"));
    }

    void AgentHistoryStoreTests::SessionSummaryDeletionAndValidationBoundaries()
    {
        const auto workspaceId = Guid(L"19191919-1919-4919-8919-191919191919");
        AgentHistoryStore store{ _databasePath };

        const auto summary = MakeSessionSummary(workspaceId, L"retained-summary", 100, 200, 300);
        VERIFY_IS_TRUE(store.UpsertSessionSummary(summary));
        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"retained-summary", L"retained-turn", 400)));
        VERIFY_IS_TRUE(store.DeleteTurn(L"codex", L"retained-summary", L"retained-turn"));
        VERIFY_IS_TRUE(store.ListTurns(std::nullopt, L"").empty());

        auto sessions = store.ListSessionSummaries(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, sessions.size());
        VERIFY_ARE_EQUAL(L"retained-summary", sessions[0].SessionId);
        VERIFY_IS_FALSE(sessions[0].HasLocalTurns);
        VERIFY_IS_TRUE(store.DeleteSession(L"codex", L"retained-summary"));
        VERIFY_IS_TRUE(store.ListSessionSummaries(std::nullopt, L"").empty());

        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"hook-only", L"hook-only-turn", 500)));
        VERIFY_IS_TRUE(store.DeleteTurn(L"codex", L"hook-only", L"hook-only-turn"));
        VERIFY_IS_TRUE(store.ListSessionSummaries(std::nullopt, L"").empty());

        auto invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.WorkspaceId = {};
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.Provider = L"";
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.SessionId = L"";
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.SourceKind = L"";
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.DisplayName = winrt::hstring{ std::wstring(513, L'n') };
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.Preview = winrt::hstring{ std::wstring(8193, L'p') };
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.SourceKind = winrt::hstring{ std::wstring(65, L's') };
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.DisplayName = L"bad\nname";
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", -1, 20, 30);
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 20, 10, 30);
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 0);
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 253402300800000);
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));
        invalid = MakeSessionSummary(workspaceId, L"invalid", 10, 20, 30);
        invalid.HasLocalTurns = true;
        VERIFY_IS_FALSE(store.UpsertSessionSummary(invalid));

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM ai_session;"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM ai_turn;"));
    }

    void AgentHistoryStoreTests::UpsertIsIdempotentAndPreservesPrompt()
    {
        const auto workspaceId = Guid(L"11111111-1111-4111-8111-111111111111");
        AgentHistoryStore store{ _databasePath };

        auto original = MakeRecord(workspaceId, L"session-1", L"turn-1", 100);
        VERIFY_IS_TRUE(store.UpsertTurn(original));
        VERIFY_IS_TRUE(store.UpsertTurn(original));

        auto replay = original;
        replay.Prompt = L"";
        replay.FinalMarkdown = L"Updated final answer";
        replay.EventAt = 200;
        replay.ReceivedAt = 201;
        VERIFY_IS_TRUE(store.UpsertTurn(replay));
        VERIFY_IS_TRUE(store.UpsertTurn(replay));

        const auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_ARE_EQUAL(L"Original prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"Updated final answer", records[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(int64_t{ 200 }, records[0].EventAt);
        VERIFY_ARE_EQUAL(int64_t{ 201 }, records[0].ReceivedAt);
    }

    void AgentHistoryStoreTests::StagedUpsertHidesIncompleteTurns()
    {
        const auto workspaceId = Guid(L"18181818-1818-4818-8818-181818181818");
        AgentHistoryStore store{ _databasePath };

        auto prompt = MakeRecord(workspaceId, L"session-staged", L"turn-staged", 100);
        prompt.Title = L"";
        prompt.FinalMarkdown = L"";
        VERIFY_IS_TRUE(store.UpsertPrompt(prompt));
        VERIFY_IS_TRUE(store.UpsertPrompt(prompt));
        VERIFY_IS_TRUE(store.ListTurns(std::nullopt, L"").empty());

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_turn;"));
        VERIFY_IS_TRUE(QueryText(database.get(), "SELECT prompt FROM ai_turn;") == "Original prompt");

        auto invalidPrompt = prompt;
        invalidPrompt.FinalMarkdown = L"Must not be present";
        VERIFY_IS_FALSE(store.UpsertPrompt(invalidPrompt));

        auto final = prompt;
        final.Prompt = L"";
        final.Title = L"Completed staged turn";
        final.FinalMarkdown = L"Staged final answer";
        final.EventAt = 200;
        final.ReceivedAt = 201;
        VERIFY_IS_TRUE(store.UpsertFinal(final));
        VERIFY_IS_TRUE(store.UpsertFinal(final));

        const auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_ARE_EQUAL(L"Original prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"Staged final answer", records[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(L"Completed staged turn", records[0].Title);

        final.FinalMarkdown = L"";
        VERIFY_IS_FALSE(store.UpsertFinal(final));
        VERIFY_IS_FALSE(store.UpsertTurn(final));
    }

    void AgentHistoryStoreTests::PromptThenFinalUsesFinalMetadata()
    {
        const auto promptWorkspace = Guid(L"19191919-1919-4919-8919-191919191919");
        const auto finalWorkspace = Guid(L"20202020-2020-4020-8020-202020202020");
        const auto promptEventId = Guid(L"90000000-0000-4000-8000-000000000001");
        const auto finalEventId = Guid(L"90000000-0000-4000-8000-000000000002");
        AgentHistoryStore store{ _databasePath };

        auto prompt = MakeRecord(promptWorkspace, L"ordered-session", L"ordered-turn", 500);
        prompt.WorkspaceTitle = L"Prompt workspace";
        prompt.Cwd = L"C:\\prompt";
        prompt.Title = L"";
        prompt.FinalMarkdown = L"";
        prompt.Model = L"prompt-model";
        VERIFY_IS_TRUE(store.UpsertPrompt(prompt, MakeEvent(promptEventId, prompt)));

        auto final = MakeRecord(finalWorkspace, L"ordered-session", L"ordered-turn", 100);
        final.WorkspaceTitle = L"Final workspace";
        final.Cwd = L"C:\\final";
        final.Title = L"Final title";
        final.Prompt = L"";
        final.FinalMarkdown = L"Final answer";
        final.Model = L"final-model";
        final.ReceivedAt = 101;
        VERIFY_IS_TRUE(store.UpsertFinal(final, MakeEvent(finalEventId, final)));

        const auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_IS_TRUE(records[0].WorkspaceId == finalWorkspace);
        VERIFY_ARE_EQUAL(L"Final workspace", records[0].WorkspaceTitle);
        VERIFY_ARE_EQUAL(L"C:\\final", records[0].Cwd);
        VERIFY_ARE_EQUAL(L"Final title", records[0].Title);
        VERIFY_ARE_EQUAL(L"Original prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"Final answer", records[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(L"final-model", records[0].Model);
        VERIFY_ARE_EQUAL(int64_t{ 100 }, records[0].EventAt);
    }

    void AgentHistoryStoreTests::FinalThenPromptPreservesFinalMetadata()
    {
        const auto promptWorkspace = Guid(L"21212121-2121-4121-8121-212121212121");
        const auto finalWorkspace = Guid(L"22222222-2222-4222-8222-222222222220");
        const auto promptEventId = Guid(L"91000000-0000-4000-8000-000000000001");
        const auto finalEventId = Guid(L"91000000-0000-4000-8000-000000000002");
        const auto staleFinalEventId = Guid(L"91000000-0000-4000-8000-000000000003");
        AgentHistoryStore store{ _databasePath };

        auto final = MakeRecord(finalWorkspace, L"reverse-session", L"reverse-turn", 200);
        final.WorkspaceTitle = L"Authoritative workspace";
        final.Cwd = L"C:\\authoritative";
        final.Title = L"Authoritative title";
        final.Prompt = L"";
        final.FinalMarkdown = L"Authoritative final";
        final.Model = L"authoritative-model";
        final.ReceivedAt = 201;
        VERIFY_IS_TRUE(store.UpsertFinal(final, MakeEvent(finalEventId, final)));

        auto staleFinal = MakeRecord(promptWorkspace, L"reverse-session", L"reverse-turn", 100);
        staleFinal.WorkspaceTitle = L"Stale final workspace";
        staleFinal.Cwd = L"C:\\stale-final";
        staleFinal.Title = L"Stale final title";
        staleFinal.Prompt = L"";
        staleFinal.FinalMarkdown = L"Stale final answer";
        staleFinal.Model = L"stale-final-model";
        staleFinal.ReceivedAt = 1000;
        VERIFY_IS_TRUE(store.UpsertFinal(staleFinal, MakeEvent(staleFinalEventId, staleFinal)));

        auto delayedPrompt = MakeRecord(promptWorkspace, L"reverse-session", L"reverse-turn", 900);
        delayedPrompt.WorkspaceTitle = L"Stale workspace";
        delayedPrompt.Cwd = L"C:\\stale";
        delayedPrompt.Title = L"";
        delayedPrompt.Prompt = L"Delayed prompt";
        delayedPrompt.FinalMarkdown = L"";
        delayedPrompt.Model = L"stale-model";
        delayedPrompt.ReceivedAt = 901;
        VERIFY_IS_TRUE(store.UpsertPrompt(delayedPrompt, MakeEvent(promptEventId, delayedPrompt)));

        const auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_IS_TRUE(records[0].WorkspaceId == finalWorkspace);
        VERIFY_ARE_EQUAL(L"Authoritative workspace", records[0].WorkspaceTitle);
        VERIFY_ARE_EQUAL(L"C:\\authoritative", records[0].Cwd);
        VERIFY_ARE_EQUAL(L"Authoritative title", records[0].Title);
        VERIFY_ARE_EQUAL(L"Delayed prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"Authoritative final", records[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(L"authoritative-model", records[0].Model);
        VERIFY_ARE_EQUAL(int64_t{ 200 }, records[0].EventAt);
        VERIFY_ARE_EQUAL(int64_t{ 201 }, records[0].ReceivedAt);
    }

    void AgentHistoryStoreTests::EventReceiptsAckReplayAndRejectConflicts()
    {
        const auto workspaceId = Guid(L"23232323-2323-4323-8323-232323232323");
        const auto otherWorkspaceId = Guid(L"24242424-2424-4424-8424-242424242424");
        const auto eventId = Guid(L"92000000-0000-4000-8000-000000000001");
        auto original = MakeRecord(workspaceId, L"receipt-session", L"receipt-turn", 100);

        {
            AgentHistoryStore store{ _databasePath };
            VERIFY_IS_TRUE(store.UpsertFinal(original, MakeEvent(eventId, original)));
        }

        AgentHistoryStore reopened{ _databasePath };
        auto arrivalReplay = original;
        arrivalReplay.ReceivedAt = 1000;
        VERIFY_IS_TRUE(reopened.UpsertFinal(arrivalReplay, MakeEvent(eventId, arrivalReplay)));

        auto changedReplay = original;
        changedReplay.WorkspaceTitle = L"Must not replace workspace";
        changedReplay.Cwd = L"C:\\must-not-replace";
        changedReplay.Title = L"Must not replace title";
        changedReplay.Prompt = L"Must not replace prompt";
        changedReplay.FinalMarkdown = L"Must not replace final";
        changedReplay.Model = L"must-not-replace-model";
        changedReplay.EventAt = 999;
        changedReplay.ReceivedAt = 1000;
        VERIFY_IS_FALSE(reopened.UpsertFinal(changedReplay, MakeEvent(eventId, changedReplay)));

        auto conflictingEvent = original;
        conflictingEvent.Title = L"";
        conflictingEvent.FinalMarkdown = L"";
        VERIFY_IS_FALSE(reopened.UpsertPrompt(conflictingEvent, MakeEvent(eventId, conflictingEvent)));

        auto conflictingIdentity = original;
        conflictingIdentity.TurnId = L"different-turn";
        VERIFY_IS_FALSE(reopened.UpsertFinal(conflictingIdentity, MakeEvent(eventId, conflictingIdentity)));

        auto conflictingWorkspace = original;
        conflictingWorkspace.WorkspaceId = otherWorkspaceId;
        VERIFY_IS_FALSE(reopened.UpsertFinal(conflictingWorkspace, MakeEvent(eventId, conflictingWorkspace)));

        const auto records = reopened.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_IS_TRUE(records[0].WorkspaceId == workspaceId);
        VERIFY_ARE_EQUAL(L"Workspace", records[0].WorkspaceTitle);
        VERIFY_ARE_EQUAL(L"C:\\work", records[0].Cwd);
        VERIFY_ARE_EQUAL(L"Completed turn", records[0].Title);
        VERIFY_ARE_EQUAL(L"Original prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"Original final answer", records[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(L"gpt-test", records[0].Model);
        VERIFY_ARE_EQUAL(int64_t{ 100 }, records[0].EventAt);
        VERIFY_ARE_EQUAL(int64_t{ 101 }, records[0].ReceivedAt);

        VERIFY_IS_TRUE(reopened.DeleteTurn(L"codex", L"receipt-session", L"receipt-turn"));
        VERIFY_IS_TRUE(reopened.UpsertFinal(original, MakeEvent(eventId, original)));
        VERIFY_IS_FALSE(reopened.UpsertFinal(conflictingIdentity, MakeEvent(eventId, conflictingIdentity)));
        VERIFY_IS_TRUE(reopened.ListTurns(std::nullopt, L"").empty());

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_history_event WHERE length(payload_digest)=32;"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM ai_turn;"));
    }

    void AgentHistoryStoreTests::FiltersByWorkspaceAndUnicodeSearch()
    {
        const auto workspaceOne = Guid(L"22222222-2222-4222-8222-222222222222");
        const auto workspaceTwo = Guid(L"33333333-3333-4333-8333-333333333333");
        AgentHistoryStore store{ _databasePath };

        auto unicodeRecord = MakeRecord(workspaceOne, L"session-unicode", L"turn-unicode", 100);
        unicodeRecord.WorkspaceTitle = L"研发工作区";
        unicodeRecord.Title = L"修复渲染错误";
        unicodeRecord.Prompt = L"请检查终端输出";
        unicodeRecord.FinalMarkdown = L"已完成 Unicode Search 验证";
        VERIFY_IS_TRUE(store.UpsertTurn(unicodeRecord));

        auto otherRecord = MakeRecord(workspaceTwo, L"session-other", L"turn-other", 200);
        otherRecord.WorkspaceTitle = L"Documentation";
        otherRecord.Title = L"Update docs";
        otherRecord.FinalMarkdown = L"Documentation updated";
        VERIFY_IS_TRUE(store.UpsertTurn(otherRecord));

        const auto workspaceRecords = store.ListTurns(std::optional{ workspaceOne }, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, workspaceRecords.size());
        VERIFY_ARE_EQUAL(L"turn-unicode", workspaceRecords[0].TurnId);

        const auto unicodeMatches = store.ListTurns(std::nullopt, L"渲染");
        VERIFY_ARE_EQUAL(size_t{ 1 }, unicodeMatches.size());
        VERIFY_ARE_EQUAL(L"turn-unicode", unicodeMatches[0].TurnId);

        const auto caseInsensitiveMatches = store.ListTurns(std::nullopt, L"unicode search");
        VERIFY_ARE_EQUAL(size_t{ 1 }, caseInsensitiveMatches.size());
        VERIFY_ARE_EQUAL(L"turn-unicode", caseInsensitiveMatches[0].TurnId);

        const auto excludedByWorkspace = store.ListTurns(std::optional{ workspaceTwo }, L"渲染");
        VERIFY_IS_TRUE(excludedByWorkspace.empty());
    }

    void AgentHistoryStoreTests::PinnedTurnsSortFirst()
    {
        const auto workspaceId = Guid(L"44444444-4444-4444-8444-444444444444");
        AgentHistoryStore store{ _databasePath };

        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"session-sort", L"turn-old", 100)));
        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"session-sort", L"turn-new", 200)));

        auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 2 }, records.size());
        VERIFY_ARE_EQUAL(L"turn-new", records[0].TurnId);

        VERIFY_IS_TRUE(store.SetTurnPinned(L"codex", L"session-sort", L"turn-old", true));
        records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 2 }, records.size());
        VERIFY_ARE_EQUAL(L"turn-old", records[0].TurnId);
        VERIFY_IS_TRUE(records[0].Pinned);

        VERIFY_IS_TRUE(store.SetTurnPinned(L"codex", L"session-sort", L"turn-old", false));
        records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(L"turn-new", records[0].TurnId);
        VERIFY_IS_FALSE(records[1].Pinned);
    }

    void AgentHistoryStoreTests::DeleteTurnAndSession()
    {
        const auto workspaceId = Guid(L"55555555-5555-4555-8555-555555555555");
        AgentHistoryStore store{ _databasePath };

        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"session-a", L"turn-a1", 100)));
        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"session-a", L"turn-a2", 200)));
        VERIFY_IS_TRUE(store.UpsertTurn(MakeRecord(workspaceId, L"session-b", L"turn-b1", 300)));

        VERIFY_IS_TRUE(store.DeleteTurn(L"codex", L"session-a", L"turn-a1"));
        auto records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 2 }, records.size());

        VERIFY_IS_TRUE(store.DeleteSession(L"codex", L"session-a"));
        records = store.ListTurns(std::nullopt, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_ARE_EQUAL(L"session-b", records[0].SessionId);

        VERIFY_IS_TRUE(store.DeleteTurn(L"codex", L"session-b", L"turn-b1"));
        VERIFY_IS_TRUE(store.ListTurns(std::nullopt, L"").empty());
        VERIFY_IS_FALSE(store.DeleteTurn(L"codex", L"session-b", L"turn-b1"));

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM ai_session;"));
        VERIFY_ARE_EQUAL(0, QueryInteger(database.get(), "SELECT count(*) FROM ai_turn;"));
    }

    void AgentHistoryStoreTests::PersistsAcrossReopen()
    {
        const auto workspaceId = Guid(L"66666666-6666-4666-8666-666666666666");
        {
            AgentHistoryStore store{ _databasePath };
            auto record = MakeRecord(workspaceId, L"session-persist", L"turn-persist", 1234);
            record.Prompt = L"Persist this prompt";
            record.FinalMarkdown = L"Persist this answer";
            VERIFY_IS_TRUE(store.UpsertTurn(record));
            VERIFY_IS_TRUE(store.SetTurnPinned(L"codex", L"session-persist", L"turn-persist", true));
        }

        VERIFY_IS_TRUE(std::filesystem::is_regular_file(_databasePath));

        {
            AgentHistoryStore reopened{ _databasePath };
            VERIFY_IS_TRUE(reopened.IsAvailable());
            const auto records = reopened.ListTurns(std::optional{ workspaceId }, L"");
            VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
            VERIFY_ARE_EQUAL(L"session-persist", records[0].SessionId);
            VERIFY_ARE_EQUAL(L"turn-persist", records[0].TurnId);
            VERIFY_ARE_EQUAL(L"Persist this prompt", records[0].Prompt);
            VERIFY_ARE_EQUAL(L"Persist this answer", records[0].FinalMarkdown);
            VERIFY_ARE_EQUAL(int64_t{ 1234 }, records[0].EventAt);
            VERIFY_IS_TRUE(records[0].Pinned);
        }
    }

    void AgentHistoryStoreTests::ConclusionProtocolAcceptsOptionalPrompt()
    {
        constexpr std::string_view withPrompt = R"json({
            "version": 1,
            "id": "history-with-prompt",
            "method": "conclusion.create",
            "params": {
                "workspaceId": "77777777-7777-4777-8777-777777777777",
                "sessionId": "session-1",
                "turnId": "turn-1",
                "title": "Completed",
                "prompt": "Explain the change",
                "body": "Done",
                "model": "gpt-test"
            }
        })json";
        const auto parsed = WorkspaceAutomationProtocol::ParseRequest(withPrompt);
        VERIFY_IS_TRUE(static_cast<bool>(parsed));
        VERIFY_ARE_EQUAL(L"Explain the change", parsed.Request->Prompt);

        constexpr std::string_view legacy = R"json({
            "version": 1,
            "id": "history-without-prompt",
            "method": "conclusion.create",
            "params": {
                "workspaceId": "77777777-7777-4777-8777-777777777777",
                "sessionId": "session-1",
                "turnId": "turn-2",
                "title": "Completed",
                "body": "Done"
            }
        })json";
        const auto legacyParsed = WorkspaceAutomationProtocol::ParseRequest(legacy);
        VERIFY_IS_TRUE(static_cast<bool>(legacyParsed));
        VERIFY_IS_TRUE(legacyParsed.Request->Prompt.empty());

        constexpr std::string_view invalid = R"json({
            "version": 1,
            "id": "history-invalid-prompt",
            "method": "conclusion.create",
            "params": {
                "workspaceId": "77777777-7777-4777-8777-777777777777",
                "sessionId": "session-1",
                "turnId": "turn-3",
                "title": "Completed",
                "prompt": 42,
                "body": "Done"
            }
        })json";
        const auto invalidParsed = WorkspaceAutomationProtocol::ParseRequest(invalid);
        VERIFY_IS_FALSE(static_cast<bool>(invalidParsed));
        VERIFY_IS_TRUE(invalidParsed.ErrorCode == "invalid_params");
    }

    void AgentHistoryStoreTests::HistoryIngestProtocolIsStrict()
    {
        constexpr std::string_view eventId{ "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" };
        constexpr std::string_view workspaceId{ "77777777-7777-4777-8777-777777777777" };

        const auto prompt = WorkspaceAutomationProtocol::ParseRequest(HistoryEnvelope(
            eventId,
            workspaceId,
            "prompt",
            R"json(,"cwd":"C:\\work","prompt":"Explain the change","model":"gpt-test","eventAt":0)json"));
        VERIFY_IS_TRUE(static_cast<bool>(prompt));
        VERIFY_IS_TRUE(prompt.Request->Method == WorkspaceAutomationMethod::HistoryIngest);
        VERIFY_IS_TRUE(prompt.Request->HistoryEvent == AgentHistoryEventKind::Prompt);
        VERIFY_ARE_EQUAL(L"Explain the change", prompt.Request->Prompt);
        VERIFY_IS_TRUE(prompt.Request->EventAt.has_value());
        VERIFY_ARE_EQUAL(int64_t{ 0 }, prompt.Request->EventAt.value());

        const auto final = WorkspaceAutomationProtocol::ParseRequest(HistoryEnvelope(
            eventId,
            workspaceId,
            "final",
            R"json(,"prompt":"Explain the change","finalMarkdown":"Done","title":"Completed","eventAt":253402300799999)json"));
        VERIFY_IS_TRUE(static_cast<bool>(final));
        VERIFY_IS_TRUE(final.Request->HistoryEvent == AgentHistoryEventKind::Final);
        VERIFY_ARE_EQUAL(L"Done", final.Request->Body);
        VERIFY_ARE_EQUAL(int64_t{ 253402300799999LL }, final.Request->EventAt.value());

        const auto verifyInvalid = [&](const std::string& value) {
            const auto parsed = WorkspaceAutomationProtocol::ParseRequest(value);
            VERIFY_IS_FALSE(static_cast<bool>(parsed));
            VERIFY_IS_TRUE(parsed.ErrorCode == "invalid_params");
        };

        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":1,"transcript":"forbidden")json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":1,"tokens":42)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":1,"env":{})json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":1,"toolOutput":"forbidden")json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "prompt", R"json(,"prompt":42,"eventAt":1)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "prompt", R"json(,"prompt":"","eventAt":1)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "prompt", R"json(,"prompt":"Valid","title":"Not allowed yet","eventAt":1)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"","title":"Completed","eventAt":1)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","eventAt":1)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":"1")json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":1.5)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":253402300800000)json"));
        verifyInvalid(HistoryEnvelope(eventId, workspaceId, "final", R"json(,"finalMarkdown":"Done","title":"Completed")json"));
        verifyInvalid(HistoryEnvelope("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA", workspaceId, "prompt", R"json(,"prompt":"Valid","eventAt":1)json"));
        verifyInvalid(HistoryEnvelope(eventId, "77777777-7777-4777-8777-77777777777A", "prompt", R"json(,"prompt":"Valid","eventAt":1)json"));
    }

    void AgentHistoryStoreTests::InboxImportsStagesAndDeletesCommittedFiles()
    {
        constexpr std::string_view promptEventId{ "81000000-0000-4000-8000-000000000001" };
        constexpr std::string_view finalEventId{ "81000000-0000-4000-8000-000000000002" };
        constexpr std::string_view workspaceIdText{ "81818181-8181-4818-8818-818181818181" };
        const auto workspaceId = Guid(L"81818181-8181-4818-8818-818181818181");

        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ workspaceId, L"Inbox workspace" } };

        const auto promptPath = _inboxPath / L"81000000-0000-4000-8000-000000000001.json";
        WriteEventFile(promptPath,
                       HistoryEnvelope(promptEventId,
                                       workspaceIdText,
                                       "prompt",
                                       R"json(,"cwd":"C:\\repo","prompt":"Original inbox prompt","model":"gpt-test","eventAt":100)json"));

        const auto promptResult = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, promptResult.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 1 }, promptResult.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, promptResult.Deleted);
        VERIFY_IS_FALSE(std::filesystem::exists(promptPath));
        VERIFY_IS_TRUE(store.ListTurns(std::nullopt, L"").empty());

        {
            const auto database = OpenDatabase(_databasePath);
            VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_turn;"));
            VERIFY_IS_TRUE(QueryText(database.get(), "SELECT prompt FROM ai_turn;") == "Original inbox prompt");
        }

        const auto finalPath = _inboxPath / L"81000000-0000-4000-8000-000000000002.json";
        WriteEventFile(finalPath,
                       HistoryEnvelope(finalEventId,
                                       workspaceIdText,
                                       "final",
                                       R"json(,"finalMarkdown":"Inbox final answer","title":"Inbox completed","eventAt":200)json"));

        const auto finalResult = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, finalResult.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, finalResult.Deleted);
        VERIFY_IS_FALSE(std::filesystem::exists(finalPath));

        const auto records = store.ListTurns(std::optional{ workspaceId }, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_ARE_EQUAL(L"Inbox workspace", records[0].WorkspaceTitle);
        VERIFY_ARE_EQUAL(L"Original inbox prompt", records[0].Prompt);
        VERIFY_ARE_EQUAL(L"Inbox final answer", records[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(L"Inbox completed", records[0].Title);
        VERIFY_ARE_EQUAL(L"codex", records[0].Provider);
        VERIFY_ARE_EQUAL(int64_t{ 200 }, records[0].EventAt);
    }

    void AgentHistoryStoreTests::InboxImportsAndReplaysEmergencySlots()
    {
        constexpr std::string_view eventId{ "81111111-1111-4111-8111-111111111111" };
        constexpr std::string_view workspaceIdText{ "81818181-8181-4818-8818-818181818182" };
        const auto workspaceId = Guid(L"81818181-8181-4818-8818-818181818182");
        const auto payload = HistoryEnvelope(eventId,
                                             workspaceIdText,
                                             "final",
                                             R"json(,"finalMarkdown":"Emergency answer","title":"Emergency completed","eventAt":210)json");

        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ workspaceId, L"Emergency workspace" } };

        const auto firstSlot = _inboxPath / L"emergency-00.json";
        WriteEventFile(firstSlot, payload);
        const auto first = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, first.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 1 }, first.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, first.Deleted);
        VERIFY_ARE_EQUAL(size_t{ 0 }, first.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(firstSlot));

        // The receipt key remains the canonical payload eventId, not the
        // reusable slot name, so an exact replay is acknowledged and removed.
        const auto replaySlot = _inboxPath / L"emergency-31.json";
        WriteEventFile(replaySlot, payload);
        const auto replay = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, replay.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, replay.Deleted);
        VERIFY_ARE_EQUAL(size_t{ 0 }, replay.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(replaySlot));

        const auto records = store.ListTurns(std::optional{ workspaceId }, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, records.size());
        VERIFY_ARE_EQUAL(L"Emergency answer", records[0].FinalMarkdown);
        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM ai_history_event;"));

        // Similar-looking names outside the fixed 00-31 reservation retain the
        // normal filename validation rules.
        const auto invalidSlot = _inboxPath / L"emergency-32.json";
        WriteEventFile(invalidSlot,
                       HistoryEnvelope("81111111-1111-4111-8111-111111111112",
                                       workspaceIdText,
                                       "final",
                                       R"json(,"finalMarkdown":"Must not import","title":"Invalid slot","eventAt":211)json"));
        const auto invalid = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, invalid.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 0 }, invalid.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, invalid.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(invalidSlot));

        // A reserved filename relaxes only the filename/payload equality rule;
        // the payload eventId itself must still be canonical.
        const auto noncanonicalPayloadSlot = _inboxPath / L"emergency-01.json";
        WriteEventFile(noncanonicalPayloadSlot,
                       HistoryEnvelope("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA",
                                       workspaceIdText,
                                       "final",
                                       R"json(,"finalMarkdown":"Must not import","title":"Invalid payload id","eventAt":212)json"));
        const auto noncanonical = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, noncanonical.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 0 }, noncanonical.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, noncanonical.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(noncanonicalPayloadSlot));
    }

    void AgentHistoryStoreTests::InboxRetainsEmergencySlotWhileProducerWrites()
    {
        constexpr std::string_view eventId{ "81222222-2222-4222-8222-222222222222" };
        constexpr std::string_view workspaceIdText{ "81818181-8181-4818-8818-818181818183" };
        const auto workspaceId = Guid(L"81818181-8181-4818-8818-818181818183");
        const auto payload = HistoryEnvelope(eventId,
                                             workspaceIdText,
                                             "prompt",
                                             R"json(,"prompt":"Emergency prompt","eventAt":220)json");

        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ workspaceId, L"Exclusive producer" } };
        std::error_code error;
        std::filesystem::create_directories(_inboxPath, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));

        const auto slot = _inboxPath / L"emergency-00.json";
        {
            const wil::unique_hfile producer{ CreateFileW(slot.c_str(),
                                                          GENERIC_WRITE,
                                                          0,
                                                          nullptr,
                                                          CREATE_NEW,
                                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                                          nullptr) };
            VERIFY_IS_TRUE(static_cast<bool>(producer));

            const auto active = inbox.Import(workspaces);
            VERIFY_ARE_EQUAL(size_t{ 1 }, active.FilesVisited);
            VERIFY_ARE_EQUAL(size_t{ 0 }, active.Committed);
            VERIFY_ARE_EQUAL(size_t{ 0 }, active.Quarantined);
            VERIFY_ARE_EQUAL(size_t{ 1 }, active.Retained);
            VERIFY_IS_TRUE(std::filesystem::is_regular_file(slot));
            VERIFY_IS_FALSE(std::filesystem::exists(AgentHistoryInbox::QuarantinePath(_inboxPath)));

            DWORD bytesWritten{};
            VERIFY_IS_TRUE(WriteFile(producer.get(),
                                     payload.data(),
                                     static_cast<DWORD>(payload.size()),
                                     &bytesWritten,
                                     nullptr));
            VERIFY_ARE_EQUAL(static_cast<DWORD>(payload.size()), bytesWritten);
            VERIFY_IS_TRUE(FlushFileBuffers(producer.get()));
        }

        const auto completed = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, completed.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, completed.Deleted);
        VERIFY_ARE_EQUAL(size_t{ 0 }, completed.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(slot));

        WriteEventFile(slot, {});
        const auto abandonedEmpty = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, abandonedEmpty.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 1 }, abandonedEmpty.Quarantined);
        VERIFY_ARE_EQUAL(size_t{ 0 }, abandonedEmpty.Retained);
        VERIFY_IS_FALSE(std::filesystem::exists(slot));
    }

    void AgentHistoryStoreTests::InboxPrioritizesEmergencySlotsWithinFileLimit()
    {
        constexpr std::string_view localWorkspaceIdText{ "81818181-8181-4818-8818-818181818184" };
        constexpr std::string_view otherWorkspaceIdText{ "81818181-8181-4818-8818-818181818185" };
        const auto localWorkspaceId = Guid(L"81818181-8181-4818-8818-818181818184");

        for (size_t index = 0; index < AgentHistoryInbox::MaximumFilesPerDirectory; ++index)
        {
            const auto eventId = fmt::format(FMT_COMPILE("81333333-3333-4333-8333-{:012x}"), index);
            const auto filename = winrt::to_hstring(eventId + ".json");
            WriteEventFile(_inboxPath / std::filesystem::path{ std::wstring_view{ filename } },
                           HistoryEnvelope(eventId,
                                           otherWorkspaceIdText,
                                           "prompt",
                                           R"json(,"prompt":"Deferred capacity event","eventAt":230)json"));
        }

        const auto emergencySlot = _inboxPath / L"emergency-00.json";
        WriteEventFile(emergencySlot,
                       HistoryEnvelope("81333333-3333-4333-8333-ffffffffffff",
                                       localWorkspaceIdText,
                                       "prompt",
                                       R"json(,"prompt":"Prioritized emergency event","eventAt":231)json"));

        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ localWorkspaceId, L"Priority workspace" } };
        const auto result = inbox.Import(workspaces);

        VERIFY_IS_TRUE(result.LimitReached);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumFilesPerDirectory, result.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Deleted);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumFilesPerDirectory - 1, result.Deferred);
        VERIFY_IS_FALSE(std::filesystem::exists(emergencySlot));
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumFilesPerDirectory, CountJsonFiles(_inboxPath));
    }

    void AgentHistoryStoreTests::InboxDefersOtherWorkspacesAndRetainsCommitFailures()
    {
        constexpr std::string_view eventId{ "82000000-0000-4000-8000-000000000001" };
        constexpr std::string_view otherWorkspaceText{ "82828282-8282-4828-8828-828282828282" };
        const auto localWorkspace = Guid(L"83838383-8383-4838-8838-838383838383");
        const auto otherWorkspace = Guid(L"82828282-8282-4828-8828-828282828282");

        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const auto eventPath = _inboxPath / L"82000000-0000-4000-8000-000000000001.json";
        WriteEventFile(eventPath,
                       HistoryEnvelope(eventId,
                                       otherWorkspaceText,
                                       "final",
                                       R"json(,"finalMarkdown":"Other answer","title":"Other completed","eventAt":300)json"));

        const std::array localOnly{ AgentHistoryInboxWorkspace{ localWorkspace, L"Local" } };
        const auto deferred = inbox.Import(localOnly);
        VERIFY_ARE_EQUAL(size_t{ 1 }, deferred.Deferred);
        VERIFY_ARE_EQUAL(size_t{ 0 }, deferred.Committed);
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(eventPath));
        VERIFY_IS_TRUE(store.ListTurns(std::nullopt, L"").empty());

        const std::array targetAvailable{ AgentHistoryInboxWorkspace{ otherWorkspace, L"Other", L"C:\\other" } };
        const auto accepted = inbox.Import(targetAvailable);
        VERIFY_ARE_EQUAL(size_t{ 1 }, accepted.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, accepted.Deleted);
        VERIFY_IS_FALSE(std::filesystem::exists(eventPath));
        const auto acceptedRecords = store.ListTurns(std::optional{ otherWorkspace }, L"");
        VERIFY_ARE_EQUAL(size_t{ 1 }, acceptedRecords.size());
        VERIFY_ARE_EQUAL(L"C:\\other", acceptedRecords[0].Cwd);

        constexpr std::string_view failedEventId{ "82000000-0000-4000-8000-000000000002" };
        const auto failedPath = _inboxPath / L"82000000-0000-4000-8000-000000000002.json";
        WriteEventFile(failedPath,
                       HistoryEnvelope(failedEventId,
                                       otherWorkspaceText,
                                       "final",
                                       R"json(,"finalMarkdown":"Retain me","title":"Persistence failed","eventAt":400)json"));

        AgentHistoryStore unavailableStore{ _inboxPath };
        VERIFY_IS_FALSE(unavailableStore.IsAvailable());
        AgentHistoryInbox unavailableInbox{ unavailableStore, { _inboxPath } };
        const auto failed = unavailableInbox.Import(targetAvailable);
        VERIFY_ARE_EQUAL(size_t{ 0 }, failed.Committed);
        VERIFY_ARE_EQUAL(size_t{ 1 }, failed.Retained);
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(failedPath));
    }

    void AgentHistoryStoreTests::InboxQuarantinesInvalidAndProhibitedPayloads()
    {
        constexpr std::string_view workspaceId{ "84848484-8484-4848-8848-848484848484" };
        const auto workspaceGuid = Guid(L"84848484-8484-4848-8848-848484848484");
        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ workspaceGuid, L"Restricted" } };

        const std::array prohibitedFields{
            std::string_view{ R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":500,"transcript":"raw")json" },
            std::string_view{ R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":500,"tokens":123)json" },
            std::string_view{ R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":500,"env":{"SECRET":"value"})json" },
            std::string_view{ R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":500,"toolOutput":"raw")json" },
        };

        for (size_t index = 0; index < prohibitedFields.size(); ++index)
        {
            const auto id = fmt::format(FMT_COMPILE("85000000-0000-4000-8000-{:012x}"), index + 1);
            const auto filename = winrt::to_hstring(id + ".json");
            WriteEventFile(_inboxPath / std::filesystem::path{ std::wstring_view{ filename } },
                           HistoryEnvelope(id, workspaceId, "final", prohibitedFields[index]));
        }

        constexpr std::string_view payloadEventId{ "85000000-0000-4000-8000-000000000010" };
        WriteEventFile(_inboxPath / L"not-a-guid.json",
                       HistoryEnvelope(payloadEventId,
                                       workspaceId,
                                       "final",
                                       R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":500)json"));

        WriteEventFile(_inboxPath / L"85000000-0000-4000-8000-000000000011.json",
                       HistoryEnvelope("85000000-0000-4000-8000-000000000012",
                                       workspaceId,
                                       "final",
                                       R"json(,"finalMarkdown":"Done","title":"Completed","eventAt":500)json"));

        const auto result = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 6 }, result.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 6 }, result.Quarantined);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.Committed);
        VERIFY_ARE_EQUAL(size_t{ 0 }, CountJsonFiles(_inboxPath));
        VERIFY_IS_TRUE(std::filesystem::is_directory(AgentHistoryInbox::QuarantinePath(_inboxPath)));
        VERIFY_IS_TRUE(store.ListTurns(std::nullopt, L"").empty());
    }

    void AgentHistoryStoreTests::InboxBoundsFileSizeAndCount()
    {
        const auto workspaceGuid = Guid(L"86868686-8686-4868-8868-868686868686");
        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ workspaceGuid, L"Bounds" } };

        const auto oversizedPath = _inboxPath / L"86000000-0000-4000-8000-000000000001.json";
        WriteEventFile(oversizedPath, std::string(AgentHistoryInbox::MaximumEventBytes + 1, 'x'));
        const auto oversized = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 1 }, oversized.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 1 }, oversized.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(oversizedPath));

        const auto quarantinePath = AgentHistoryInbox::QuarantinePath(_inboxPath);
        std::error_code error;
        std::filesystem::remove_all(quarantinePath, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));

        const auto exceedsQuarantineBudgetPath = _inboxPath / L"86000000-0000-4000-8000-000000000002.json";
        WriteEventFile(exceedsQuarantineBudgetPath, {});
        std::filesystem::resize_file(exceedsQuarantineBudgetPath, AgentHistoryInbox::MaximumQuarantineBytes + 1, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));
        const auto exceedsQuarantineBudget = inbox.Import(workspaces);
        VERIFY_ARE_EQUAL(size_t{ 0 }, exceedsQuarantineBudget.Quarantined);
        VERIFY_ARE_EQUAL(size_t{ 1 }, exceedsQuarantineBudget.Retained);
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(exceedsQuarantineBudgetPath));
        std::filesystem::remove(exceedsQuarantineBudgetPath, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));

        const auto saturatedQuarantinePath = quarantinePath / L"saturated.bin";
        WriteEventFile(saturatedQuarantinePath, {});
        std::filesystem::resize_file(saturatedQuarantinePath, AgentHistoryInbox::MaximumQuarantineBytes, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));

        bool createdAllFiles = true;
        for (size_t index = 0; index < AgentHistoryInbox::MaximumFilesPerDirectory + 1; ++index)
        {
            const auto filename = fmt::format(FMT_COMPILE(L"87000000-0000-4000-8000-{:012x}.json"), index + 1);
            createdAllFiles = CreateEmptyFile(_inboxPath / filename) && createdAllFiles;
        }
        VERIFY_IS_TRUE(createdAllFiles);

        const auto bounded = inbox.Import(workspaces);
        VERIFY_IS_TRUE(bounded.LimitReached);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumFilesPerDirectory, bounded.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 0 }, bounded.Quarantined);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumFilesPerDirectory, bounded.Retained);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumFilesPerDirectory + 1, CountJsonFiles(_inboxPath));
    }

    void AgentHistoryStoreTests::InboxBoundsAllDirectoryEntries()
    {
        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { _inboxPath } };

        constexpr auto noiseEntries = AgentHistoryInbox::MaximumEntriesPerDirectory + 32;
        for (size_t index = 0; index < noiseEntries; ++index)
        {
            if (index % 3 == 2)
            {
                std::error_code error;
                std::filesystem::create_directories(_inboxPath / fmt::format(FMT_COMPILE(L"noise-{:04x}.dir"), index), error);
                if (error)
                {
                    VERIFY_IS_FALSE(static_cast<bool>(error));
                    return;
                }
            }
            else
            {
                const auto extension = index % 3 == 0 ? L".tmp" : L".txt";
                WriteEventFile(_inboxPath / fmt::format(FMT_COMPILE(L"noise-{:04x}{}"), index, extension), "noise");
            }
        }

        const auto bounded = inbox.Import({});
        VERIFY_IS_TRUE(bounded.LimitReached);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumEntriesPerDirectory, bounded.EntriesInspected);
        VERIFY_ARE_EQUAL(size_t{ 0 }, bounded.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 0 }, bounded.Quarantined);
        VERIFY_IS_FALSE(std::filesystem::exists(AgentHistoryInbox::QuarantinePath(_inboxPath)));
    }

    void AgentHistoryStoreTests::InboxRejectsReparsePointRoot()
    {
        const auto targetPath = _inboxPath / L"target";
        const auto linkPath = _inboxPath / L"link";
        const auto eventPath = targetPath / L"not-a-guid.json";
        WriteEventFile(eventPath, "invalid");

        std::error_code error;
        std::filesystem::create_directory_symlink(targetPath, linkPath, error);
        if (error)
        {
            Log::Comment(L"Directory symlink creation is unavailable; reparse-point integration check skipped.");
            return;
        }

        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox inbox{ store, { linkPath } };
        const auto result = inbox.Import({});
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.EntriesInspected);
        VERIFY_ARE_EQUAL(size_t{ 0 }, result.FilesVisited);
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Retained);
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(eventPath));
        VERIFY_IS_FALSE(std::filesystem::exists(AgentHistoryInbox::QuarantinePath(targetPath)));
    }

    void AgentHistoryStoreTests::ConcurrentInboxesShareQuarantineBudget()
    {
        const auto workspaceGuid = Guid(L"88888888-8888-4888-8888-888888888888");
        AgentHistoryStore store{ _databasePath };
        AgentHistoryInbox firstInbox{ store, { _inboxPath } };
        AgentHistoryInbox secondInbox{ store, { _inboxPath } };
        const std::array workspaces{ AgentHistoryInboxWorkspace{ workspaceGuid, L"Concurrent" } };

        const auto quarantinePath = AgentHistoryInbox::QuarantinePath(_inboxPath);
        constexpr size_t smallQuarantineFiles = AgentHistoryInbox::MaximumQuarantineFiles - 2;
        constexpr size_t eventBytes = AgentHistoryInbox::MaximumEventBytes;
        constexpr size_t largeQuarantineBytes = static_cast<size_t>(AgentHistoryInbox::MaximumQuarantineBytes) - eventBytes - smallQuarantineFiles;
        const auto largeQuarantinePath = quarantinePath / L"existing-large.json";
        WriteEventFile(largeQuarantinePath, {});
        std::error_code error;
        std::filesystem::resize_file(largeQuarantinePath, largeQuarantineBytes, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));
        for (size_t index = 0; index < smallQuarantineFiles; ++index)
        {
            WriteEventFile(quarantinePath / fmt::format(FMT_COMPILE(L"existing-{:04x}.json"), index), "q");
        }

        const std::string invalidEvent(eventBytes, 'x');
        for (size_t index = 0; index < 8; ++index)
        {
            WriteEventFile(_inboxPath / fmt::format(FMT_COMPILE(L"89000000-0000-4000-8000-{:012x}.json"), index + 1), invalidEvent);
        }

        std::array<winrt::TerminalApp::implementation::AgentHistoryInboxImportResult, 2> results;
        std::barrier startLine{ 3 };
        std::thread firstThread{ [&] {
            startLine.arrive_and_wait();
            results[0] = firstInbox.Import(workspaces);
        } };
        std::thread secondThread{ [&] {
            startLine.arrive_and_wait();
            results[1] = secondInbox.Import(workspaces);
        } };
        startLine.arrive_and_wait();
        firstThread.join();
        secondThread.join();

        const auto usage = GetDirectoryUsage(quarantinePath);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumQuarantineFiles, usage.Entries);
        VERIFY_ARE_EQUAL(AgentHistoryInbox::MaximumQuarantineBytes, usage.Bytes);
        VERIFY_ARE_EQUAL(size_t{ 1 }, results[0].Quarantined + results[1].Quarantined);
    }

    void AgentHistoryStoreTests::InboxPathBuildersAreDeterministic()
    {
        const auto localAppData = _temporaryFolderPath / L"local-app-data";
        const auto shared = AgentHistoryInbox::SharedPathForLocalAppData(localAppData);
        VERIFY_IS_TRUE(shared == localAppData / L"Microsoft" / L"Windows Terminal" / L"workspace-history-inbox");

        const auto settingsFile = _temporaryFolderPath / L"profile" / L"settings.json";
        const auto privatePath = AgentHistoryInbox::PrivatePathForSettingsFile(settingsFile);
        VERIFY_IS_TRUE(privatePath == settingsFile.parent_path() / L"workspace-history-inbox");
        VERIFY_IS_TRUE(AgentHistoryInbox::QuarantinePath(privatePath) == privatePath / L".invalid");

        VERIFY_IS_TRUE(AgentHistoryInbox::SharedPathForLocalAppData({}).empty());
        VERIFY_IS_TRUE(AgentHistoryInbox::PrivatePathForSettingsFile({}).empty());
        VERIFY_IS_TRUE(AgentHistoryInbox::QuarantinePath({}).empty());
    }
}
