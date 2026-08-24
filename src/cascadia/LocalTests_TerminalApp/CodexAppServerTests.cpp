// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/CodexAppServerClient.h"

#include <deque>
#include <fstream>
#include <winrt/Windows.Data.Json.h>

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace WEX::Common;
using namespace winrt::Windows::Data::Json;

using winrt::TerminalApp::implementation::CodexAppServerClient;
using winrt::TerminalApp::implementation::CodexAppServerError;
using winrt::TerminalApp::implementation::CodexAppServerErrorKind;
using winrt::TerminalApp::implementation::CodexAppServerTimeouts;
using winrt::TerminalApp::implementation::CodexAppServerTransportReadKind;
using winrt::TerminalApp::implementation::CodexAppServerTransportReadResult;
using winrt::TerminalApp::implementation::CodexAppServerTimeouts;
using winrt::TerminalApp::implementation::ICodexAppServerTransport;

namespace TerminalAppLocalTests
{
    namespace
    {
        struct FakeTransportState
        {
            bool Started{ false };
            bool Stopped{ false };
            std::vector<std::string> Writes;
        };

        class FakeTransport final : public ICodexAppServerTransport
        {
        public:
            FakeTransport(std::shared_ptr<FakeTransportState> state,
                          std::deque<CodexAppServerTransportReadResult> reads) :
                _state{ std::move(state) },
                _reads{ std::move(reads) }
            {
            }

            CodexAppServerError Start() noexcept override
            {
                _state->Started = true;
                return {};
            }

            CodexAppServerError Write(const std::string_view bytes,
                                      std::chrono::milliseconds,
                                      HANDLE) noexcept override
            {
                _state->Writes.emplace_back(bytes);
                return {};
            }

            CodexAppServerTransportReadResult Read(std::chrono::milliseconds, HANDLE) noexcept override
            {
                if (_reads.empty())
                {
                    return { CodexAppServerTransportReadKind::EndOfStream };
                }
                auto result = std::move(_reads.front());
                _reads.pop_front();
                return result;
            }

            void Stop() noexcept override
            {
                _state->Stopped = true;
            }

        private:
            std::shared_ptr<FakeTransportState> _state;
            std::deque<CodexAppServerTransportReadResult> _reads;
        };

        struct ClientFixture
        {
            std::shared_ptr<FakeTransportState> State;
            CodexAppServerClient Client;
        };

        ClientFixture MakeClient(std::deque<CodexAppServerTransportReadResult> reads,
                                 const CodexAppServerTimeouts timeouts = {})
        {
            auto state = std::make_shared<FakeTransportState>();
            auto factory = [state, reads = std::move(reads)]() mutable {
                return std::make_unique<FakeTransport>(
                    state,
                    std::exchange(reads, std::deque<CodexAppServerTransportReadResult>{}));
            };
            return { state, CodexAppServerClient{ std::move(factory), timeouts } };
        }

        CodexAppServerTransportReadResult Data(std::string bytes)
        {
            return { CodexAppServerTransportReadKind::Data, std::move(bytes) };
        }

        std::string InitializeResponse(const int id = 1)
        {
            return fmt::format(FMT_COMPILE(R"({{"id":{},"result":{{"codexHome":"C:\\Codex","platformFamily":"windows","platformOs":"windows","userAgent":"codex-test"}}}})"), id) + "\n";
        }

        std::string ThreadJson(const std::string_view id = "thread-1",
                               const std::string_view turns = "[]",
                               const bool includeCwd = true)
        {
            return fmt::format(
                FMT_COMPILE(R"({{"id":"{}","sessionId":"session-1","projectId":null,"preview":"preview","cliVersion":"0.149.0","modelProvider":"openai","createdAt":10,"updatedAt":20,"ephemeral":false,"source":"cli","status":{{"type":"notLoaded"}},"turns":{},"name":"Thread name","unknownFutureField":true{}}})"),
                id,
                turns,
                includeCwd ? R"(,"cwd":"C:\\repo")" : "");
        }

        std::string ListResponse(const int id,
                                 const std::string_view thread,
                                 const std::string_view nextCursor = "null")
        {
            return fmt::format(FMT_COMPILE(R"({{"id":{},"result":{{"data":[{}],"nextCursor":{}}}}})"), id, thread, nextCursor) + "\n";
        }

        winrt::hstring MethodAt(const std::vector<std::string>& writes, const size_t index)
        {
            VERIFY_IS_LESS_THAN(index, writes.size());
            JsonObject object{ nullptr };
            VERIFY_IS_TRUE(JsonObject::TryParse(winrt::to_hstring(writes[index]), object));
            return object.GetNamedString(L"method");
        }

        void VerifyKind(const CodexAppServerErrorKind expected, const CodexAppServerErrorKind actual)
        {
            VERIFY_ARE_EQUAL(static_cast<int>(expected), static_cast<int>(actual));
        }
    }

    class CodexAppServerTests
    {
        BEGIN_TEST_CLASS(CodexAppServerTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(HandshakePrecedesListAndHandlesFragmentedNotifications);
        TEST_METHOD(RejectsWrongResponseId);
        TEST_METHOD(ClassifiesUnsupportedAndBusinessErrors);
        TEST_METHOD(FailsClosedOnOversizeTimeoutAndCancellation);
        TEST_METHOD(ListTotalBudgetBoundsPagination);
        TEST_METHOD(DetectsPaginationLoop);
        TEST_METHOD(ReadThreadExtractsOnlyPromptAndFinal);
        TEST_METHOD(ProbeResumeUnsubscribesBestEffort);
        TEST_METHOD(Win32TransportBoundsNonReadingChild);
    };

    void CodexAppServerTests::HandshakePrecedesListAndHandlesFragmentedNotifications()
    {
        const auto init = InitializeResponse();
        const auto list = ListResponse(2, ThreadJson());
        auto fixture = MakeClient({
            Data(init.substr(0, 17)),
            Data(init.substr(17)),
            Data(R"({"method":"remoteControl/status/changed","params":{"status":"ready"}})" "\n" + list),
        });

        const auto result = fixture.Client.ListThreads(L"C:\\repo", 25, 1);
        VERIFY_IS_TRUE(static_cast<bool>(result));
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.Threads.size());
        VERIFY_ARE_EQUAL(L"thread-1", result.Threads[0].Id);
        VERIFY_ARE_EQUAL(L"C:\\repo", result.Threads[0].Cwd);
        VERIFY_ARE_EQUAL(L"Thread name", result.Threads[0].Name);
        VERIFY_ARE_EQUAL(L"preview", result.Threads[0].Preview);
        VERIFY_ARE_EQUAL(L"openai", result.Threads[0].ModelProvider);
        VERIFY_ARE_EQUAL(L"cli", result.Threads[0].SourceKind);
        VERIFY_ARE_EQUAL(int64_t{ 10 }, result.Threads[0].CreatedAtUnixSeconds);
        VERIFY_ARE_EQUAL(int64_t{ 20 }, result.Threads[0].UpdatedAtUnixSeconds);

        VERIFY_ARE_EQUAL(size_t{ 3 }, fixture.State->Writes.size());
        VERIFY_ARE_EQUAL(L"initialize", MethodAt(fixture.State->Writes, 0));
        VERIFY_ARE_EQUAL(L"initialized", MethodAt(fixture.State->Writes, 1));
        VERIFY_ARE_EQUAL(L"thread/list", MethodAt(fixture.State->Writes, 2));
        VERIFY_IS_TRUE(fixture.State->Started);
        VERIFY_IS_TRUE(fixture.State->Stopped);
    }

    void CodexAppServerTests::RejectsWrongResponseId()
    {
        auto fixture = MakeClient({ Data(InitializeResponse()), Data(ListResponse(99, ThreadJson())) });
        const auto result = fixture.Client.ListThreads({}, 25, 1);
        VERIFY_IS_FALSE(static_cast<bool>(result));
        VerifyKind(CodexAppServerErrorKind::UnexpectedResponse, result.Error.Kind);
        VERIFY_IS_TRUE(result.Threads.empty());
    }

    void CodexAppServerTests::ClassifiesUnsupportedAndBusinessErrors()
    {
        {
            auto fixture = MakeClient({
                Data(InitializeResponse()),
                Data(R"({"id":2,"error":{"code":-32601,"message":"Method not found"}})" "\n"),
            });
            const auto result = fixture.Client.ListThreads({}, 25, 1);
            VerifyKind(CodexAppServerErrorKind::UnsupportedMethod, result.Error.Kind);
            VERIFY_ARE_EQUAL(int64_t{ -32601 }, result.Error.RpcCode);
            VERIFY_ARE_EQUAL(L"thread/list", result.Error.Method);
        }
        {
            auto fixture = MakeClient({
                Data(InitializeResponse()),
                Data(R"({"id":2,"error":{"code":-32000,"message":"thread unavailable"}})" "\n"),
            });
            const auto result = fixture.Client.ReadThread(L"thread-1");
            VerifyKind(CodexAppServerErrorKind::RpcError, result.Error.Kind);
            VERIFY_ARE_EQUAL(int64_t{ -32000 }, result.Error.RpcCode);
            VERIFY_ARE_EQUAL(L"thread/read", result.Error.Method);
        }
    }

    void CodexAppServerTests::FailsClosedOnOversizeTimeoutAndCancellation()
    {
        {
            auto fixture = MakeClient({
                Data(InitializeResponse()),
                Data(std::string(CodexAppServerClient::MaximumLineBytes + 1, 'x')),
            });
            const auto result = fixture.Client.ListThreads({}, 25, 1);
            VerifyKind(CodexAppServerErrorKind::OutputLimitExceeded, result.Error.Kind);
        }
        {
            auto fixture = MakeClient({
                Data(InitializeResponse()),
                { CodexAppServerTransportReadKind::Timeout },
            });
            const auto result = fixture.Client.ListThreads({}, 25, 1);
            VerifyKind(CodexAppServerErrorKind::Timeout, result.Error.Kind);
        }
        {
            auto fixture = MakeClient({
                Data(InitializeResponse()),
                { CodexAppServerTransportReadKind::OutputLimitExceeded },
            });
            const auto result = fixture.Client.ListThreads({}, 25, 1);
            VerifyKind(CodexAppServerErrorKind::OutputLimitExceeded, result.Error.Kind);
        }
        {
            auto fixture = MakeClient({});
            wil::unique_handle cancelled{ CreateEventW(nullptr, TRUE, TRUE, nullptr) };
            VERIFY_IS_NOT_NULL(cancelled.get());
            const auto result = fixture.Client.ListThreads({}, 25, 1, cancelled.get());
            VerifyKind(CodexAppServerErrorKind::Cancelled, result.Error.Kind);
            VERIFY_IS_FALSE(fixture.State->Started);
            VERIFY_IS_TRUE(fixture.State->Stopped);
        }
    }

    void CodexAppServerTests::DetectsPaginationLoop()
    {
        const auto cursor = R"("same-cursor")";
        auto fixture = MakeClient({
            Data(InitializeResponse()),
            Data(ListResponse(2, ThreadJson("thread-1"), cursor)),
            Data(ListResponse(3, ThreadJson("thread-2"), cursor)),
        });
        const auto result = fixture.Client.ListThreads({}, 25, 3);
        VerifyKind(CodexAppServerErrorKind::PaginationLoop, result.Error.Kind);
        VERIFY_IS_TRUE(result.Threads.empty());
    }

    void CodexAppServerTests::ListTotalBudgetBoundsPagination()
    {
        CodexAppServerTimeouts timeouts;
        timeouts.ListTotal = std::chrono::milliseconds::zero();
        auto fixture = MakeClient({ Data(InitializeResponse()) }, timeouts);

        const auto result = fixture.Client.ListThreads({}, 25, 3);
        VerifyKind(CodexAppServerErrorKind::Timeout, result.Error.Kind);
        VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.State->Writes.size());
        VERIFY_ARE_EQUAL(L"initialize", MethodAt(fixture.State->Writes, 0));
        VERIFY_ARE_EQUAL(L"initialized", MethodAt(fixture.State->Writes, 1));
        VERIFY_IS_TRUE(fixture.State->Stopped);
    }

    void CodexAppServerTests::ReadThreadExtractsOnlyPromptAndFinal()
    {
        constexpr std::string_view turns = R"([{"id":"turn-1","status":"completed","startedAt":11,"completedAt":12,"model":"future-model","items":[{"type":"userMessage","id":"u1","content":[{"type":"text","text":"first"},{"type":"image","url":"ignored"},{"type":"text","text":"second"}]},{"type":"reasoning","id":"r1","content":["secret"]},{"type":"commandExecution","id":"c1","aggregatedOutput":"never retained"},{"type":"agentMessage","id":"a1","phase":"commentary","text":"not final"},{"type":"agentMessage","id":"a2","phase":"final_answer","text":"final answer"}]},{"id":"turn-2","status":"completed","startedAt":13,"completedAt":null,"items":[{"type":"userMessage","id":"u2","content":[{"type":"text","text":"fallback prompt"}]},{"type":"agentMessage","id":"a3","text":"legacy final"}]},{"id":"turn-3","status":"failed","startedAt":null,"completedAt":null,"items":[]}])";
        const auto response = fmt::format(FMT_COMPILE(R"({{"id":2,"result":{{"thread":{}}}}})"), ThreadJson("thread-1", turns)) + "\n";
        auto fixture = MakeClient({ Data(InitializeResponse()), Data(response) });

        const auto result = fixture.Client.ReadThread(L"thread-1");
        if (!result)
        {
            Log::Comment(winrt::hstring{ fmt::format(FMT_COMPILE(L"ReadThread error kind={} message={}"),
                                                      static_cast<int>(result.Error.Kind),
                                                      std::wstring_view{ result.Error.Message }) }
                             .c_str());
        }
        VERIFY_IS_TRUE(static_cast<bool>(result));
        VERIFY_ARE_EQUAL(size_t{ 3 }, result.Thread.Turns.size());
        VERIFY_ARE_EQUAL(L"first\nsecond", result.Thread.Turns[0].Prompt);
        VERIFY_ARE_EQUAL(L"final answer", result.Thread.Turns[0].FinalMarkdown);
        VERIFY_ARE_EQUAL(L"future-model", result.Thread.Turns[0].Model);
        VERIFY_ARE_EQUAL(int64_t{ 12000 }, result.Thread.Turns[0].EventAtUnixMilliseconds);
        VERIFY_ARE_EQUAL(L"legacy final", result.Thread.Turns[1].FinalMarkdown);
        VERIFY_ARE_EQUAL(int64_t{ 13000 }, result.Thread.Turns[1].EventAtUnixMilliseconds);
        VERIFY_ARE_EQUAL(int64_t{ 20000 }, result.Thread.Turns[2].EventAtUnixMilliseconds);
        VERIFY_IS_TRUE(result.Thread.Turns[2].Prompt.empty());
        VERIFY_IS_TRUE(result.Thread.Turns[2].FinalMarkdown.empty());
    }

    void CodexAppServerTests::ProbeResumeUnsubscribesBestEffort()
    {
        const auto resume = fmt::format(FMT_COMPILE(R"({{"id":2,"result":{{"thread":{}}}}})"), ThreadJson()) + "\n";
        auto fixture = MakeClient({
            Data(InitializeResponse()),
            Data(R"({"method":"thread/started","params":{"thread":{"id":"thread-1"}}})" "\n" + resume),
            Data(R"({"id":3,"result":{"status":"unsubscribed"}})" "\n"),
        });

        const auto result = fixture.Client.ProbeResume(L"thread-1");
        VERIFY_IS_TRUE(static_cast<bool>(result));
        VERIFY_ARE_EQUAL(L"thread-1", result.Thread.Id);
        VERIFY_ARE_EQUAL(size_t{ 4 }, fixture.State->Writes.size());
        VERIFY_ARE_EQUAL(L"thread/resume", MethodAt(fixture.State->Writes, 2));
        VERIFY_ARE_EQUAL(L"thread/unsubscribe", MethodAt(fixture.State->Writes, 3));
    }

    void CodexAppServerTests::Win32TransportBoundsNonReadingChild()
    {
        GUID testId{};
        VERIFY_SUCCEEDED(CoCreateGuid(&testId));
        const auto root = std::filesystem::temp_directory_path() /
                          std::filesystem::path{ fmt::format(FMT_COMPILE(L"wt-codex-write-timeout-{}"),
                                                             ::Microsoft::Console::Utils::GuidToPlainString(testId)) };
        std::error_code error;
        std::filesystem::create_directories(root, error);
        VERIFY_IS_FALSE(static_cast<bool>(error));
        const auto cleanup = wil::scope_exit([&]() noexcept {
            std::error_code cleanupError;
            std::filesystem::remove_all(root, cleanupError);
        });

        const auto fakeCodex = root / L"codex.cmd";
        {
            std::ofstream script{ fakeCodex, std::ios::binary };
            VERIFY_IS_TRUE(static_cast<bool>(script));
            script << "@echo off\r\n"
                      "echo {\"id\":1,\"result\":{\"codexHome\":\"C:\\\\Codex\",\"platformFamily\":\"windows\",\"platformOs\":\"windows\",\"userAgent\":\"fake\"}}\r\n"
                      ":wait\r\n"
                      "goto wait\r\n";
        }

        const auto pathLength = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        VERIFY_IS_GREATER_THAN(pathLength, DWORD{ 0 });
        std::wstring originalPath(pathLength, L'\0');
        const auto written = GetEnvironmentVariableW(L"PATH", originalPath.data(), pathLength);
        VERIFY_IS_GREATER_THAN(written, DWORD{ 0 });
        originalPath.resize(written);
        const auto restorePath = wil::scope_exit([&]() noexcept {
            SetEnvironmentVariableW(L"PATH", originalPath.c_str());
        });
        const auto testPath = root.native() + L";" + originalPath;
        VERIFY_WIN32_BOOL_SUCCEEDED(SetEnvironmentVariableW(L"PATH", testPath.c_str()));

        CodexAppServerTimeouts timeouts;
        timeouts.Initialize = std::chrono::seconds{ 2 };
        timeouts.ListPage = std::chrono::milliseconds{ 250 };
        timeouts.ListTotal = std::chrono::seconds{ 2 };
        CodexAppServerClient client{ {}, timeouts };

        const auto started = std::chrono::steady_clock::now();
        const auto result = client.ListThreads(std::wstring(30000, L'a'), 1, 1);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        VerifyKind(CodexAppServerErrorKind::Timeout, result.Error.Kind);
        VERIFY_IS_LESS_THAN(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), int64_t{ 5000 });
    }
}
