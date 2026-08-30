// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimePipe.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        constexpr DWORD PipeBufferBytes{ 64 * 1024 };

        bool _validPipeName(const std::wstring_view value) noexcept
        {
            return value.starts_with(L"\\\\.\\pipe\\WT.TaskRuntime.") &&
                   value.ends_with(L".control") && value.size() <= 240 &&
                   value.find(L'\0') == std::wstring_view::npos;
        }
    }

    RuntimeNamedPipe::~RuntimeNamedPipe()
    {
        if (_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(_handle);
        }
    }

    RuntimeNamedPipe::RuntimeNamedPipe(RuntimeNamedPipe&& other) noexcept :
        _handle{ std::exchange(other._handle, INVALID_HANDLE_VALUE) },
        _name{ std::move(other._name) }
    {
    }

    RuntimeNamedPipe& RuntimeNamedPipe::operator=(RuntimeNamedPipe&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(_handle);
            }
            _handle = std::exchange(other._handle, INVALID_HANDLE_VALUE);
            _name = std::move(other._name);
        }
        return *this;
    }

    RuntimeNamedPipe::operator bool() const noexcept
    {
        return _handle != INVALID_HANDLE_VALUE;
    }

    HANDLE RuntimeNamedPipe::Handle() const noexcept
    {
        return _handle;
    }

    const std::wstring& RuntimeNamedPipe::Name() const noexcept
    {
        return _name;
    }

    RuntimePipeResult CreateRuntimeControlPipe(
        const RuntimeIdentity& identity,
        const SECURITY_ATTRIBUTES* securityAttributes,
        const bool firstInstance)
    {
        RuntimePipeResult result;
        if (!_validPipeName(identity.ControlPipeName) ||
            identity.ProtocolIdentity.empty() || !securityAttributes ||
            securityAttributes->nLength != sizeof(SECURITY_ATTRIBUTES) ||
            !securityAttributes->lpSecurityDescriptor || securityAttributes->bInheritHandle)
        {
            result.Status = RuntimePipeStatus::InvalidConfiguration;
            result.Error = "Runtime pipe identity or security attributes are invalid";
            return result;
        }

        auto openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (firstInstance)
        {
            openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        const auto pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        const auto handle = CreateNamedPipeW(
            identity.ControlPipeName.c_str(),
            openMode,
            pipeMode,
            PIPE_UNLIMITED_INSTANCES,
            PipeBufferBytes,
            PipeBufferBytes,
            0,
            const_cast<SECURITY_ATTRIBUTES*>(securityAttributes));
        if (handle == INVALID_HANDLE_VALUE)
        {
            const auto systemError = GetLastError();
            result.SystemError = systemError;
            if (systemError == ERROR_PIPE_BUSY || (systemError == ERROR_ACCESS_DENIED && firstInstance))
            {
                result.Status = RuntimePipeStatus::AlreadyExists;
            }
            else if (systemError == ERROR_ACCESS_DENIED)
            {
                result.Status = RuntimePipeStatus::AccessDenied;
            }
            else
            {
                result.Status = RuntimePipeStatus::SystemError;
            }
            result.Error = "CreateNamedPipe failed: " + std::to_string(systemError);
            return result;
        }

        result.Pipe._handle = handle;
        result.Pipe._name = identity.ControlPipeName;
        result.Status = RuntimePipeStatus::Created;
        return result;
    }
}
