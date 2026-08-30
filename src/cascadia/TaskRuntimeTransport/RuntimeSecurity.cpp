// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeSecurity.h"

#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeSecurityAttributes::~RuntimeSecurityAttributes()
    {
        if (_descriptor)
        {
            LocalFree(_descriptor);
        }
    }

    RuntimeSecurityAttributes::RuntimeSecurityAttributes(RuntimeSecurityAttributes&& other) noexcept :
        _descriptor{ std::exchange(other._descriptor, nullptr) },
        _attributes{ other._attributes }
    {
        _attributes.lpSecurityDescriptor = _descriptor;
        other._attributes = {};
    }

    RuntimeSecurityAttributes& RuntimeSecurityAttributes::operator=(RuntimeSecurityAttributes&& other) noexcept
    {
        if (this != &other)
        {
            if (_descriptor)
            {
                LocalFree(_descriptor);
            }
            _descriptor = std::exchange(other._descriptor, nullptr);
            _attributes = other._attributes;
            _attributes.lpSecurityDescriptor = _descriptor;
            other._attributes = {};
        }
        return *this;
    }

    const SECURITY_ATTRIBUTES* RuntimeSecurityAttributes::Attributes() const noexcept
    {
        return _descriptor ? &_attributes : nullptr;
    }

    PSECURITY_DESCRIPTOR RuntimeSecurityAttributes::Descriptor() const noexcept
    {
        return _descriptor;
    }

    RuntimeSecurityAttributes::operator bool() const noexcept
    {
        return _descriptor != nullptr;
    }

    RuntimeSecurityResult CreateRuntimeSecurityAttributes(const std::wstring_view userSid)
    {
        RuntimeSecurityResult result;
        PSID parsedSid{};
        const std::wstring sid{ userSid };
        if (!ConvertStringSidToSidW(sid.c_str(), &parsedSid) || !IsValidSid(parsedSid))
        {
            if (parsedSid)
            {
                LocalFree(parsedSid);
            }
            result.Status = RuntimeSecurityStatus::InvalidUserSid;
            result.Error = "Runtime user SID is invalid";
            return result;
        }
        LocalFree(parsedSid);

        const std::wstring sddl =
            L"O:" + sid +
            L"G:" + sid +
            L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)" +
            L"S:(ML;;NW;;;ME)";
        PSECURITY_DESCRIPTOR descriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(),
                SDDL_REVISION_1,
                &descriptor,
                nullptr))
        {
            result.Error = "Security descriptor creation failed: " + std::to_string(GetLastError());
            return result;
        }

        result.Security._descriptor = descriptor;
        result.Security._attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        result.Security._attributes.lpSecurityDescriptor = descriptor;
        result.Security._attributes.bInheritHandle = FALSE;
        result.Status = RuntimeSecurityStatus::Succeeded;
        return result;
    }
}
