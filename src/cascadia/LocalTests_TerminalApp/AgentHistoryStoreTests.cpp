// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/AgentHistoryStore.h"
#include "../TerminalApp/WorkspaceAutomation.h"

#include <winrt/Windows.Storage.h>
#include <winsqlite/winsqlite3.h>

#pragma comment(lib, "winsqlite3.lib")

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;

using winrt::TerminalApp::implementation::AgentHistoryRecord;
using winrt::TerminalApp::implementation::AgentHistoryStore;
using winrt::TerminalApp::implementation::WorkspaceAutomationProtocol;

namespace TerminalAppLocalTests
{
    namespace
    {
        constexpr std::wstring_view TestDatabasePrefix{ L"agent-history-store-" };

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
    }

    class AgentHistoryStoreTests
    {
        BEGIN_TEST_CLASS(AgentHistoryStoreTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(InitializesSchemaAndWal);
        TEST_METHOD(UpsertIsIdempotentAndPreservesPrompt);
        TEST_METHOD(FiltersByWorkspaceAndUnicodeSearch);
        TEST_METHOD(PinnedTurnsSortFirst);
        TEST_METHOD(DeleteTurnAndSession);
        TEST_METHOD(PersistsAcrossReopen);
        TEST_METHOD(ConclusionProtocolAcceptsOptionalPrompt);

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
                return _CleanupDatabaseFiles();
            }
            catch (...)
            {
                return false;
            }
        }

        TEST_METHOD_CLEANUP(MethodCleanup)
        {
            return _CleanupDatabaseFiles();
        }

    private:
        bool _CleanupDatabaseFiles() const noexcept
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
                return succeeded;
            }
            catch (...)
            {
                return false;
            }
        }

        std::filesystem::path _temporaryFolderPath;
        std::filesystem::path _databasePath;
    };

    void AgentHistoryStoreTests::InitializesSchemaAndWal()
    {
        AgentHistoryStore store{ _databasePath };
        VERIFY_IS_TRUE(store.IsAvailable());
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(_databasePath));

        const auto database = OpenDatabase(_databasePath);
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "PRAGMA user_version;"));
        VERIFY_IS_TRUE(QueryText(database.get(), "PRAGMA journal_mode;") == "wal");
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='ai_session';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='ai_turn';"));
        VERIFY_ARE_EQUAL(3, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_turn') WHERE name IN ('prompt', 'final_markdown', 'pinned');"));
        VERIFY_ARE_EQUAL(2, QueryInteger(database.get(), "SELECT count(*) FROM pragma_table_info('ai_session') WHERE name IN ('started_at', 'pinned');"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='ai_turn_workspace_order';"));
        VERIFY_ARE_EQUAL(1, QueryInteger(database.get(), "SELECT count(*) FROM sqlite_master WHERE type='index' AND name='ai_turn_global_order';"));
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
}
