// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskExecutionSecurity.h"

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace Microsoft::Terminal::TaskExecution::details
{
    namespace
    {
        struct LocalOwner
        {
            void* Value{};

            ~LocalOwner()
            {
                if (Value)
                {
                    LocalFree(Value);
                }
            }
        };

        struct HandleOwner
        {
            HANDLE Value{};

            ~HandleOwner()
            {
                if (Value && Value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(Value);
                }
            }
        };

        TaskExecutionMutexSecurityValidationResult _validationFailure(
            const DWORD systemError,
            std::string error)
        {
            TaskExecutionMutexSecurityValidationResult result;
            result.SystemError = systemError == ERROR_SUCCESS ?
                                     ERROR_ACCESS_DENIED :
                                     systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _validAceMask(const ACCESS_MASK mask) noexcept
        {
            return mask == GENERIC_ALL || mask == MUTEX_ALL_ACCESS;
        }
    }

    TaskExecutionSecurityAttributes::~TaskExecutionSecurityAttributes()
    {
        if (_descriptor)
        {
            LocalFree(_descriptor);
        }
    }

    TaskExecutionSecurityAttributes::TaskExecutionSecurityAttributes(
        TaskExecutionSecurityAttributes&& other) noexcept :
        _descriptor{ std::exchange(other._descriptor, nullptr) },
        _attributes{ other._attributes }
    {
        _attributes.lpSecurityDescriptor = _descriptor;
        other._attributes = {};
    }

    TaskExecutionSecurityAttributes& TaskExecutionSecurityAttributes::operator=(
        TaskExecutionSecurityAttributes&& other) noexcept
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

    const SECURITY_ATTRIBUTES* TaskExecutionSecurityAttributes::Attributes() const noexcept
    {
        return _descriptor ? &_attributes : nullptr;
    }

    TaskExecutionSecurityAttributes::operator bool() const noexcept
    {
        return _descriptor != nullptr;
    }

    std::optional<std::wstring> CurrentTaskExecutionUserSid(
        std::string& error) noexcept
    {
        try
        {
            HANDLE tokenHandle{};
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle))
            {
                error = "Task execution user token could not be opened";
                return std::nullopt;
            }
            HandleOwner token{ tokenHandle };

            DWORD required{};
            GetTokenInformation(token.Value, TokenUser, nullptr, 0, &required);
            if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            {
                error = "Task execution user token size is unavailable";
                return std::nullopt;
            }
            std::vector<uint8_t> buffer(required);
            if (!GetTokenInformation(
                    token.Value,
                    TokenUser,
                    buffer.data(),
                    required,
                    &required))
            {
                error = "Task execution user token is unavailable";
                return std::nullopt;
            }
            const auto user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            LPWSTR sidText{};
            if (!ConvertSidToStringSidW(user->User.Sid, &sidText))
            {
                error = "Task execution user SID could not be encoded";
                return std::nullopt;
            }
            LocalOwner sidOwner{ sidText };
            return std::wstring{ sidText };
        }
        catch (const std::exception&)
        {
            error = "Task execution user identity exhausted local resources";
            return std::nullopt;
        }
        catch (...)
        {
            error = "Unexpected Task execution user identity failure";
            return std::nullopt;
        }
    }

    TaskExecutionSecurityResult CreateTaskExecutionSecurityAttributes(
        const std::wstring_view userSid) noexcept
    {
        TaskExecutionSecurityResult result;
        try
        {
            PSID parsedSid{};
            const std::wstring sid{ userSid };
            if (!ConvertStringSidToSidW(sid.c_str(), &parsedSid) ||
                !parsedSid || !IsValidSid(parsedSid))
            {
                if (parsedSid)
                {
                    LocalFree(parsedSid);
                }
                result.SystemError = ERROR_INVALID_SID;
                result.Error = "Task execution user SID is invalid";
                return result;
            }
            LocalOwner parsedOwner{ parsedSid };

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
                result.SystemError = GetLastError();
                result.Error = "Task execution security descriptor creation failed";
                return result;
            }

            result.Security._descriptor = descriptor;
            result.Security._attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
            result.Security._attributes.lpSecurityDescriptor = descriptor;
            result.Security._attributes.bInheritHandle = FALSE;
            result.Succeeded = true;
            return result;
        }
        catch (const std::exception&)
        {
            result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
            result.Error = "Task execution security descriptor exhausted local resources";
            return result;
        }
        catch (...)
        {
            result.SystemError = ERROR_UNHANDLED_EXCEPTION;
            result.Error = "Unexpected Task execution security descriptor failure";
            return result;
        }
    }

    TaskExecutionMutexSecurityValidationResult ValidateTaskExecutionMutexSecurity(
        const HANDLE mutex,
        const std::wstring_view currentUserSid) noexcept
    {
        try
        {
            if (!mutex || mutex == INVALID_HANDLE_VALUE || currentUserSid.empty())
            {
                return _validationFailure(
                    ERROR_INVALID_PARAMETER,
                    "Task directory Mutex security input is invalid");
            }

            PSID owner{};
            PACL dacl{};
            PACL sacl{};
            PSECURITY_DESCRIPTOR descriptor{};
            const auto status = GetSecurityInfo(
                mutex,
                SE_KERNEL_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                    LABEL_SECURITY_INFORMATION,
                &owner,
                nullptr,
                &dacl,
                &sacl,
                &descriptor);
            LocalOwner descriptorOwner{ descriptor };
            if (status != ERROR_SUCCESS || !descriptor || !owner || !dacl || !sacl)
            {
                return _validationFailure(
                    status,
                    "Task directory Mutex owner, DACL, or label is unavailable");
            }

            SECURITY_DESCRIPTOR_CONTROL control{};
            DWORD revision{};
            if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
                (control & SE_DACL_PRESENT) == 0 ||
                (control & SE_DACL_PROTECTED) == 0)
            {
                return _validationFailure(
                    GetLastError(),
                    "Task directory Mutex DACL is absent or unprotected");
            }
            if ((control & SE_SACL_PRESENT) == 0 || sacl->AceCount != 1)
            {
                return _validationFailure(
                    ERROR_ACCESS_DENIED,
                    "Task directory Mutex label is absent or not singular");
            }

            PSID currentUser{};
            const std::wstring sid{ currentUserSid };
            if (!ConvertStringSidToSidW(sid.c_str(), &currentUser) ||
                !currentUser || !IsValidSid(currentUser))
            {
                if (currentUser)
                {
                    LocalFree(currentUser);
                }
                return _validationFailure(
                    ERROR_INVALID_SID,
                    "Task directory Mutex current-user SID is invalid");
            }
            LocalOwner currentUserOwner{ currentUser };

            std::array<uint8_t, SECURITY_MAX_SID_SIZE> systemSid{};
            DWORD systemSidBytes = static_cast<DWORD>(systemSid.size());
            if (!CreateWellKnownSid(
                    WinLocalSystemSid,
                    nullptr,
                    systemSid.data(),
                    &systemSidBytes))
            {
                return _validationFailure(
                    GetLastError(),
                    "Local System SID creation failed");
            }
            std::array<uint8_t, SECURITY_MAX_SID_SIZE> mediumSid{};
            DWORD mediumSidBytes = static_cast<DWORD>(mediumSid.size());
            if (!CreateWellKnownSid(
                    WinMediumLabelSid,
                    nullptr,
                    mediumSid.data(),
                    &mediumSidBytes))
            {
                return _validationFailure(
                    GetLastError(),
                    "Medium integrity SID creation failed");
            }
            if (!EqualSid(owner, currentUser) || dacl->AceCount != 2)
            {
                return _validationFailure(
                    ERROR_ACCESS_DENIED,
                    "Task directory Mutex owner or DACL count is invalid");
            }

            size_t currentUserEntries{};
            size_t systemEntries{};
            for (DWORD index = 0; index < dacl->AceCount; ++index)
            {
                void* rawAce{};
                if (!GetAce(dacl, index, &rawAce) || !rawAce)
                {
                    return _validationFailure(
                        GetLastError(),
                        "Task directory Mutex DACL entry is unreadable");
                }
                const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
                if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
                    ace->Header.AceFlags != 0 || !_validAceMask(ace->Mask))
                {
                    return _validationFailure(
                        ERROR_ACCESS_DENIED,
                        "Task directory Mutex DACL entry is invalid");
                }
                const auto aceSid = &ace->SidStart;
                if (!IsValidSid(aceSid))
                {
                    return _validationFailure(
                        ERROR_INVALID_SID,
                        "Task directory Mutex DACL SID is invalid");
                }
                currentUserEntries += EqualSid(currentUser, aceSid) ? 1 : 0;
                systemEntries += EqualSid(systemSid.data(), aceSid) ? 1 : 0;
            }
            if (currentUserEntries != 1 || systemEntries != 1)
            {
                return _validationFailure(
                    ERROR_ACCESS_DENIED,
                    "Task directory Mutex DACL is not current-user/System only");
            }

            void* rawLabel{};
            if (!GetAce(sacl, 0, &rawLabel) || !rawLabel)
            {
                return _validationFailure(
                    GetLastError(),
                    "Task directory Mutex label is unreadable");
            }
            const auto label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(rawLabel);
            if (label->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE ||
                label->Header.AceFlags != 0 ||
                label->Mask != SYSTEM_MANDATORY_LABEL_NO_WRITE_UP ||
                !IsValidSid(&label->SidStart) ||
                !EqualSid(mediumSid.data(), &label->SidStart))
            {
                return _validationFailure(
                    ERROR_ACCESS_DENIED,
                    "Task directory Mutex label is not exact Medium/NoWriteUp");
            }

            TaskExecutionMutexSecurityValidationResult result;
            result.Succeeded = true;
            return result;
        }
        catch (const std::exception&)
        {
            return _validationFailure(
                ERROR_NOT_ENOUGH_MEMORY,
                "Task directory Mutex security validation exhausted local resources");
        }
        catch (...)
        {
            return _validationFailure(
                ERROR_UNHANDLED_EXCEPTION,
                "Unexpected Task directory Mutex security failure");
        }
    }
}
