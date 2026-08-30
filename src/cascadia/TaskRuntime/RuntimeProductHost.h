// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeBootstrap.h"
#include "RuntimeControlServer.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeProductHostStartStatus
    {
        Started,
        AlreadyRunning,
        BootstrapFailure,
        RuntimeInstanceIdFailure,
        TaskStoreFailure,
        ServerStartFailure,
        ResourceFailure,
    };

    enum class RuntimeProductHostTerminalStatus
    {
        NotStarted,
        Running,
        Stopped,
        TransportFailure,
        InternalFailure,
    };

    enum class RuntimeProductHostWaitStatus
    {
        Stopped,
        Timeout,
        Failed,
    };

    struct RuntimeProductHostWaitResult
    {
        RuntimeProductHostWaitStatus Status{ RuntimeProductHostWaitStatus::Failed };
        DWORD SystemError{};

        [[nodiscard]] bool Stopped() const noexcept
        {
            return Status == RuntimeProductHostWaitStatus::Stopped;
        }
    };

    struct RuntimeProductHostStartResult;

    class RuntimeProductHost final
    {
    public:
        RuntimeProductHost() noexcept;
        ~RuntimeProductHost();

        RuntimeProductHost(const RuntimeProductHost&) = delete;
        RuntimeProductHost& operator=(const RuntimeProductHost&) = delete;
        RuntimeProductHost(RuntimeProductHost&&) noexcept;
        RuntimeProductHost& operator=(RuntimeProductHost&&) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] bool Running() const noexcept;
        [[nodiscard]] RuntimeProductHostTerminalStatus TerminalStatus() const noexcept;
        [[nodiscard]] std::string TerminalError() const;
        [[nodiscard]] const RuntimeIdentity* Identity() const noexcept;
        [[nodiscard]] std::string_view RuntimeInstanceId() const noexcept;

        // The returned handle is borrowed and remains stable until this Host
        // is destroyed or moved from.
        [[nodiscard]] HANDLE StoppedHandle() const noexcept;
        [[nodiscard]] RuntimeProductHostWaitResult Wait(
            uint32_t timeoutMilliseconds = INFINITE) noexcept;
        void Stop() noexcept;

    private:
        struct Impl;
        explicit RuntimeProductHost(std::unique_ptr<Impl> impl) noexcept;
        friend struct RuntimeProductHostStartResult;
        friend RuntimeProductHostStartResult StartRuntimeProductHost(
            std::wstring_view channel,
            const std::filesystem::path& settingsRoot,
            RuntimeControlServerConfiguration configuration);
        std::unique_ptr<Impl> _impl;
    };

    struct RuntimeProductHostStartResult
    {
        RuntimeProductHostStartStatus Status{ RuntimeProductHostStartStatus::BootstrapFailure };
        RuntimeBootstrapStatus BootstrapStatus{ RuntimeBootstrapStatus::InvalidIdentity };
        RuntimeControlServerStatus ServerStatus{ RuntimeControlServerStatus::InvalidConfiguration };
        RuntimeProductHost Host;
        std::string Error;

        [[nodiscard]] bool Started() const noexcept
        {
            return Status == RuntimeProductHostStartStatus::Started;
        }
    };

    [[nodiscard]] RuntimeProductHostStartResult StartRuntimeProductHost(
        std::wstring_view channel,
        const std::filesystem::path& settingsRoot,
        RuntimeControlServerConfiguration configuration = {});
}
