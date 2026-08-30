// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimePipe.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        constexpr DWORD PipeBufferBytes{ 64 * 1024 };

        bool _validControlPipeName(const std::wstring_view value) noexcept
        {
            return value.starts_with(L"\\\\.\\pipe\\WT.TaskRuntime.") &&
                   value.ends_with(L".control") && value.size() <= 240 &&
                   value.find(L'\0') == std::wstring_view::npos;
        }

        bool _validSecurityAttributes(
            const SECURITY_ATTRIBUTES* const securityAttributes) noexcept
        {
            return securityAttributes &&
                   securityAttributes->nLength == sizeof(SECURITY_ATTRIBUTES) &&
                   securityAttributes->lpSecurityDescriptor &&
                   !securityAttributes->bInheritHandle;
        }

        struct RawPipeResult
        {
            RuntimePipeStatus Status{ RuntimePipeStatus::SystemError };
            HANDLE Handle{ INVALID_HANDLE_VALUE };
            DWORD SystemError{};
            std::string Error;
        };

        RawPipeResult _createPipe(
            const std::wstring& name,
            const SECURITY_ATTRIBUTES* const securityAttributes,
            const bool firstInstance)
        {
            auto openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
            if (firstInstance)
            {
                openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
            }
            const auto pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                  PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
            const auto handle = CreateNamedPipeW(
                name.c_str(),
                openMode,
                pipeMode,
                PIPE_UNLIMITED_INSTANCES,
                PipeBufferBytes,
                PipeBufferBytes,
                0,
                const_cast<SECURITY_ATTRIBUTES*>(securityAttributes));
            if (handle != INVALID_HANDLE_VALUE)
            {
                return { RuntimePipeStatus::Created, handle, ERROR_SUCCESS, {} };
            }

            RawPipeResult result;
            result.SystemError = GetLastError();
            if (result.SystemError == ERROR_PIPE_BUSY ||
                (result.SystemError == ERROR_ACCESS_DENIED && firstInstance))
            {
                result.Status = RuntimePipeStatus::AlreadyExists;
            }
            else if (result.SystemError == ERROR_ACCESS_DENIED)
            {
                result.Status = RuntimePipeStatus::AccessDenied;
            }
            result.Error = "CreateNamedPipe failed: " +
                           std::to_string(result.SystemError);
            return result;
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

    HANDLE RuntimeNamedPipe::Release() noexcept
    {
        _name.clear();
        return std::exchange(_handle, INVALID_HANDLE_VALUE);
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
        if (!_validControlPipeName(identity.ControlPipeName) ||
            identity.ProtocolIdentity.empty() ||
            !_validSecurityAttributes(securityAttributes))
        {
            result.Status = RuntimePipeStatus::InvalidConfiguration;
            result.Error = "Runtime pipe identity or security attributes are invalid";
            return result;
        }

        const auto created = _createPipe(
            identity.ControlPipeName,
            securityAttributes,
            firstInstance);
        if (created.Status != RuntimePipeStatus::Created)
        {
            result.Status = created.Status;
            result.SystemError = created.SystemError;
            result.Error = created.Error;
            return result;
        }

        result.Pipe._handle = created.Handle;
        result.Pipe._name = identity.ControlPipeName;
        result.Status = RuntimePipeStatus::Created;
        return result;
    }

    RuntimePipeResult CreateRuntimeTerminalPipe(
        const RuntimeTerminalEndpoint& endpoint,
        const SECURITY_ATTRIBUTES* const securityAttributes,
        const bool firstInstance)
    {
        RuntimePipeResult result;
        if (!ValidateRuntimeTerminalEndpoint(endpoint) ||
            !_validSecurityAttributes(securityAttributes))
        {
            result.Status = RuntimePipeStatus::InvalidConfiguration;
            result.Error =
                "Runtime terminal endpoint or security attributes are invalid";
            return result;
        }

        const auto created = _createPipe(
            endpoint.PipeName,
            securityAttributes,
            firstInstance);
        if (created.Status != RuntimePipeStatus::Created)
        {
            result.Status = created.Status;
            result.SystemError = created.SystemError;
            result.Error = created.Error;
            return result;
        }

        result.Pipe._handle = created.Handle;
        result.Pipe._name = endpoint.PipeName;
        result.Status = RuntimePipeStatus::Created;
        return result;
    }
}
