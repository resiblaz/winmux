// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskProviderTypes.h"

#include <chrono>
#include <functional>
#include <memory>

namespace Microsoft::Terminal::TaskProvider
{
    enum class CodexTransportReadKind : uint8_t
    {
        Data,
        EndOfStream,
        Timeout,
        Cancelled,
        OutputLimitExceeded,
        Failed,
    };

    struct CodexTransportReadResult
    {
        CodexTransportReadKind Kind{ CodexTransportReadKind::Failed };
        std::string Bytes;
        DWORD SystemError{};
    };

    class ICodexAppServerTransport
    {
    public:
        virtual ~ICodexAppServerTransport() = default;
        [[nodiscard]] virtual TaskProviderResult Start() noexcept = 0;
        [[nodiscard]] virtual TaskProviderResult Write(
            std::string_view bytes,
            std::chrono::milliseconds timeout,
            HANDLE cancellationEvent) noexcept = 0;
        [[nodiscard]] virtual CodexTransportReadResult Read(
            std::chrono::milliseconds timeout,
            HANDLE cancellationEvent) noexcept = 0;
        [[nodiscard]] virtual bool Running() const noexcept = 0;
        // Wake/cancel in-flight IO without disposing handles it still uses.
        virtual void RequestStop() noexcept { Stop(); }
        virtual void Stop() noexcept = 0;
    };

    using CodexAppServerTransportFactory = std::function<
        std::unique_ptr<ICodexAppServerTransport>()>;

    [[nodiscard]] std::wstring BuildCodexAppServerCommandLine(
        const std::filesystem::path& executable);

    [[nodiscard]] CodexAppServerTransportFactory
    MakeCodexAppServerProcessTransportFactory(
        std::filesystem::path executable);
}
