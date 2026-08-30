// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"

#include <windows.h>

#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimePipeResult;

    enum class RuntimePipeStatus
    {
        Created,
        AlreadyExists,
        InvalidConfiguration,
        AccessDenied,
        SystemError,
    };

    class RuntimeNamedPipe final
    {
    public:
        RuntimeNamedPipe() noexcept = default;
        ~RuntimeNamedPipe();

        RuntimeNamedPipe(const RuntimeNamedPipe&) = delete;
        RuntimeNamedPipe& operator=(const RuntimeNamedPipe&) = delete;
        RuntimeNamedPipe(RuntimeNamedPipe&& other) noexcept;
        RuntimeNamedPipe& operator=(RuntimeNamedPipe&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] HANDLE Handle() const noexcept;
        [[nodiscard]] const std::wstring& Name() const noexcept;

    private:
        friend struct RuntimePipeResult;
        friend RuntimePipeResult CreateRuntimeControlPipe(
            const RuntimeIdentity& identity,
            const SECURITY_ATTRIBUTES* securityAttributes,
            bool firstInstance);
        HANDLE _handle{ INVALID_HANDLE_VALUE };
        std::wstring _name;
    };

    struct RuntimePipeResult
    {
        RuntimePipeStatus Status{ RuntimePipeStatus::SystemError };
        RuntimeNamedPipe Pipe;
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Created() const noexcept
        {
            return Status == RuntimePipeStatus::Created;
        }
    };

    [[nodiscard]] RuntimePipeResult CreateRuntimeControlPipe(
        const RuntimeIdentity& identity,
        const SECURITY_ATTRIBUTES* securityAttributes,
        bool firstInstance = true);
}
