// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/base.h>

namespace winrt::TerminalApp::implementation
{
    enum class CodexAppServerErrorKind
    {
        None,
        InvalidArgument,
        CliUnavailable,
        LaunchFailed,
        TransportFailed,
        TransportClosed,
        Timeout,
        Cancelled,
        OutputLimitExceeded,
        MalformedResponse,
        UnexpectedResponse,
        UnsupportedMethod,
        RpcError,
        PaginationLoop,
        InternalError,
    };

    struct CodexAppServerError
    {
        CodexAppServerErrorKind Kind{ CodexAppServerErrorKind::None };
        int64_t RpcCode{};
        winrt::hstring Method;
        winrt::hstring Message;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return Kind != CodexAppServerErrorKind::None;
        }
    };

    struct CodexAppServerThreadSummary
    {
        winrt::hstring Id;
        winrt::hstring Cwd;
        winrt::hstring Name;
        winrt::hstring Preview;
        winrt::hstring ModelProvider;
        winrt::hstring SourceKind;
        int64_t CreatedAtUnixSeconds{};
        int64_t UpdatedAtUnixSeconds{};
    };

    struct CodexAppServerTurn
    {
        winrt::hstring TurnId;
        winrt::hstring Prompt;
        winrt::hstring FinalMarkdown;
        winrt::hstring Model;
        int64_t EventAtUnixMilliseconds{};
    };

    struct CodexAppServerThread
    {
        CodexAppServerThreadSummary Summary;
        std::vector<CodexAppServerTurn> Turns;
    };

    struct CodexAppServerListResult
    {
        CodexAppServerError Error;
        std::vector<CodexAppServerThreadSummary> Threads;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !Error;
        }
    };

    struct CodexAppServerReadResult
    {
        CodexAppServerError Error;
        CodexAppServerThread Thread;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !Error;
        }
    };

    struct CodexAppServerResumeResult
    {
        CodexAppServerError Error;
        CodexAppServerThreadSummary Thread;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !Error;
        }
    };

    enum class CodexAppServerTransportReadKind
    {
        Data,
        EndOfStream,
        Timeout,
        Cancelled,
        OutputLimitExceeded,
        Failed,
    };

    struct CodexAppServerTransportReadResult
    {
        CodexAppServerTransportReadKind Kind{ CodexAppServerTransportReadKind::Failed };
        std::string Bytes;
    };

    // A byte-stream seam rather than a line seam is intentional: protocol tests
    // must cover fragmented and coalesced JSONL frames independently of Win32.
    class ICodexAppServerTransport
    {
    public:
        virtual ~ICodexAppServerTransport() = default;

        [[nodiscard]] virtual CodexAppServerError Start() noexcept = 0;
        [[nodiscard]] virtual CodexAppServerError Write(std::string_view bytes,
                                                        std::chrono::milliseconds timeout,
                                                        HANDLE cancellationEvent) noexcept = 0;
        [[nodiscard]] virtual CodexAppServerTransportReadResult Read(std::chrono::milliseconds timeout,
                                                                     HANDLE cancellationEvent) noexcept = 0;
        virtual void Stop() noexcept = 0;
    };

    struct CodexAppServerTimeouts
    {
        std::chrono::milliseconds Initialize{ std::chrono::seconds{ 5 } };
        std::chrono::milliseconds ListPage{ std::chrono::seconds{ 10 } };
        std::chrono::milliseconds ListTotal{ std::chrono::seconds{ 60 } };
        std::chrono::milliseconds Read{ std::chrono::seconds{ 30 } };
        std::chrono::milliseconds Resume{ std::chrono::seconds{ 30 } };
        std::chrono::milliseconds Unsubscribe{ std::chrono::seconds{ 2 } };
    };

    class CodexAppServerClient final
    {
    public:
        using TransportFactory = std::function<std::unique_ptr<ICodexAppServerTransport>()>;

        static constexpr size_t MaximumLineBytes{ 16 * 1024 * 1024 };
        static constexpr size_t MaximumTotalOutputBytes{ 32 * 1024 * 1024 };
        static constexpr size_t MaximumStderrBytes{ 64 * 1024 };
        static constexpr size_t MaximumRequestBytes{ 64 * 1024 };
        static constexpr size_t MaximumIdentifierCharacters{ 256 };
        static constexpr size_t MaximumCwdCharacters{ 32768 };
        static constexpr size_t MaximumSummaryCharacters{ 8192 };
        static constexpr size_t MaximumTurnTextCharacters{ 1024 * 1024 };
        static constexpr uint32_t MaximumPageLimit{ 100 };
        static constexpr uint32_t MaximumPages{ 40 };
        static constexpr size_t MaximumThreads{ 1000 };
        static constexpr size_t MaximumTurns{ 1000 };
        static constexpr size_t MaximumItems{ 4096 };
        static constexpr size_t MaximumNotificationsPerRequest{ 256 };

        explicit CodexAppServerClient(TransportFactory transportFactory = {},
                                      CodexAppServerTimeouts timeouts = {});

        [[nodiscard]] CodexAppServerListResult ListThreads(std::wstring_view cwd,
                                                            uint32_t pageLimit = 25,
                                                            uint32_t maximumPages = MaximumPages,
                                                            HANDLE cancellationEvent = nullptr) const noexcept;
        [[nodiscard]] CodexAppServerReadResult ReadThread(std::wstring_view threadId,
                                                          bool includeTurns = true,
                                                          HANDLE cancellationEvent = nullptr) const noexcept;
        [[nodiscard]] CodexAppServerResumeResult ProbeResume(std::wstring_view threadId,
                                                              HANDLE cancellationEvent = nullptr) const noexcept;

    private:
        TransportFactory _transportFactory;
        CodexAppServerTimeouts _timeouts;
    };
}
