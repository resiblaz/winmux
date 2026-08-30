// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeSingleInstanceResult;

    enum class RuntimeSingleInstanceStatus
    {
        Acquired,
        AlreadyRunning,
        InvalidName,
        AccessDenied,
        SystemError,
    };

    class RuntimeSingleInstance final
    {
    public:
        RuntimeSingleInstance() noexcept = default;
        ~RuntimeSingleInstance();

        RuntimeSingleInstance(const RuntimeSingleInstance&) = delete;
        RuntimeSingleInstance& operator=(const RuntimeSingleInstance&) = delete;
        RuntimeSingleInstance(RuntimeSingleInstance&& other) noexcept;
        RuntimeSingleInstance& operator=(RuntimeSingleInstance&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;

    private:
        friend struct RuntimeSingleInstanceResult;
        friend RuntimeSingleInstanceResult AcquireRuntimeSingleInstance(
            std::wstring_view mutexName,
            const SECURITY_ATTRIBUTES* securityAttributes);
        HANDLE _handle{};
    };

    struct RuntimeSingleInstanceResult
    {
        RuntimeSingleInstanceStatus Status{ RuntimeSingleInstanceStatus::SystemError };
        RuntimeSingleInstance Instance;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Acquired() const noexcept
        {
            return Status == RuntimeSingleInstanceStatus::Acquired;
        }
    };

    [[nodiscard]] RuntimeSingleInstanceResult AcquireRuntimeSingleInstance(
        std::wstring_view mutexName,
        const SECURITY_ATTRIBUTES* securityAttributes);
}
