// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeSingleInstance.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        bool _validMutexName(const std::wstring_view value) noexcept
        {
            return value.starts_with(L"Global\\WT.TaskRuntime.") &&
                   value.size() <= 240 && value.find(L'\0') == std::wstring_view::npos;
        }
    }

    RuntimeSingleInstance::~RuntimeSingleInstance()
    {
        if (_handle)
        {
            CloseHandle(_handle);
        }
    }

    RuntimeSingleInstance::RuntimeSingleInstance(RuntimeSingleInstance&& other) noexcept :
        _handle{ std::exchange(other._handle, nullptr) }
    {
    }

    RuntimeSingleInstance& RuntimeSingleInstance::operator=(RuntimeSingleInstance&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
            {
                CloseHandle(_handle);
            }
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }

    RuntimeSingleInstance::operator bool() const noexcept
    {
        return _handle != nullptr;
    }

    HANDLE RuntimeSingleInstance::Handle() const noexcept
    {
        return _handle;
    }

    RuntimeSingleInstanceResult AcquireRuntimeSingleInstance(
        const std::wstring_view mutexName,
        const SECURITY_ATTRIBUTES* securityAttributes)
    {
        RuntimeSingleInstanceResult result;
        if (!_validMutexName(mutexName) || !securityAttributes ||
            securityAttributes->nLength != sizeof(SECURITY_ATTRIBUTES) ||
            !securityAttributes->lpSecurityDescriptor || securityAttributes->bInheritHandle)
        {
            result.Status = RuntimeSingleInstanceStatus::InvalidName;
            result.Error = "Runtime single-instance name or security attributes are invalid";
            return result;
        }

        const std::wstring name{ mutexName };
        SetLastError(ERROR_SUCCESS);
        const auto handle = CreateMutexW(
            const_cast<SECURITY_ATTRIBUTES*>(securityAttributes),
            FALSE,
            name.c_str());
        const auto systemError = GetLastError();
        if (!handle)
        {
            result.SystemError = systemError;
            result.Status = systemError == ERROR_ACCESS_DENIED ?
                                RuntimeSingleInstanceStatus::AccessDenied :
                                RuntimeSingleInstanceStatus::SystemError;
            result.Error = "CreateMutex failed: " + std::to_string(systemError);
            return result;
        }
        if (systemError == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(handle);
            result.Status = RuntimeSingleInstanceStatus::AlreadyRunning;
            result.SystemError = systemError;
            result.Error = "Runtime instance already exists";
            return result;
        }

        result.Instance._handle = handle;
        result.Status = RuntimeSingleInstanceStatus::Acquired;
        return result;
    }
}
