// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeLaunchLockSecurity.h"

#include <aclapi.h>
#include <sddl.h>

namespace Microsoft::Terminal::TaskRuntime::details
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

        RuntimeLaunchLockSecurityResult _failure(
            const DWORD systemError,
            std::string error)
        {
            RuntimeLaunchLockSecurityResult result;
            result.SystemError = systemError == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _validAceMask(const ACCESS_MASK mask) noexcept
        {
            // ConvertStringSecurityDescriptor stores GENERIC_ALL, while the
            // object manager may return the mutex-specific mapped equivalent.
            return mask == GENERIC_ALL || mask == MUTEX_ALL_ACCESS;
        }
    }

    RuntimeLaunchLockSecurityResult ValidateRuntimeLaunchLockSecurity(
        const HANDLE mutex,
        const std::wstring_view currentUserSid)
    {
        try
        {
            if (!mutex || mutex == INVALID_HANDLE_VALUE || currentUserSid.empty())
            {
                return _failure(ERROR_INVALID_PARAMETER, "Runtime launch-lock security input is invalid");
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
                return _failure(
                    status,
                    "Runtime launch-lock owner, DACL, or mandatory label could not be queried");
            }

            SECURITY_DESCRIPTOR_CONTROL control{};
            DWORD revision{};
            if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
                (control & SE_DACL_PRESENT) == 0 || (control & SE_DACL_PROTECTED) == 0)
            {
                return _failure(
                    GetLastError(),
                    "Runtime launch-lock DACL is absent or not protected");
            }
            if ((control & SE_SACL_PRESENT) == 0 || sacl->AceCount != 1)
            {
                return _failure(
                    ERROR_ACCESS_DENIED,
                    "Runtime launch-lock mandatory-label SACL is absent or not singular");
            }

            PSID currentUser{};
            const std::wstring sidText{ currentUserSid };
            if (!ConvertStringSidToSidW(sidText.c_str(), &currentUser) ||
                !currentUser || !IsValidSid(currentUser))
            {
                if (currentUser)
                {
                    LocalFree(currentUser);
                }
                return _failure(ERROR_INVALID_SID, "Runtime launch-lock current-user SID is invalid");
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
                return _failure(GetLastError(), "Local System SID creation failed");
            }
            std::array<uint8_t, SECURITY_MAX_SID_SIZE> mediumSid{};
            DWORD mediumSidBytes = static_cast<DWORD>(mediumSid.size());
            if (!CreateWellKnownSid(
                    WinMediumLabelSid,
                    nullptr,
                    mediumSid.data(),
                    &mediumSidBytes))
            {
                return _failure(GetLastError(), "Medium integrity SID creation failed");
            }
            if (!EqualSid(owner, currentUser) || dacl->AceCount != 2)
            {
                return _failure(
                    ERROR_ACCESS_DENIED,
                    "Runtime launch-lock owner or DACL entry count is invalid");
            }

            size_t currentUserEntries{};
            size_t systemEntries{};
            for (DWORD index = 0; index < dacl->AceCount; ++index)
            {
                void* rawAce{};
                if (!GetAce(dacl, index, &rawAce) || !rawAce)
                {
                    return _failure(GetLastError(), "Runtime launch-lock DACL entry is unreadable");
                }
                const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
                if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
                    ace->Header.AceFlags != 0 || !_validAceMask(ace->Mask))
                {
                    return _failure(
                        ERROR_ACCESS_DENIED,
                        "Runtime launch-lock DACL entry type, flags, or mask is invalid");
                }
                const auto aceSid = &ace->SidStart;
                if (!IsValidSid(aceSid))
                {
                    return _failure(ERROR_INVALID_SID, "Runtime launch-lock DACL SID is invalid");
                }
                currentUserEntries += EqualSid(currentUser, aceSid) ? 1 : 0;
                systemEntries += EqualSid(systemSid.data(), aceSid) ? 1 : 0;
            }
            if (currentUserEntries != 1 || systemEntries != 1)
            {
                return _failure(
                    ERROR_ACCESS_DENIED,
                    "Runtime launch-lock DACL is not restricted to current user and Local System");
            }

            void* rawLabel{};
            if (!GetAce(sacl, 0, &rawLabel) || !rawLabel)
            {
                return _failure(GetLastError(), "Runtime launch-lock mandatory label is unreadable");
            }
            const auto label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(rawLabel);
            if (label->Header.AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE ||
                label->Header.AceFlags != 0 ||
                label->Mask != SYSTEM_MANDATORY_LABEL_NO_WRITE_UP ||
                !IsValidSid(&label->SidStart) ||
                !EqualSid(mediumSid.data(), &label->SidStart))
            {
                return _failure(
                    ERROR_ACCESS_DENIED,
                    "Runtime launch-lock mandatory label is not exact Medium/NoWriteUp");
            }

            RuntimeLaunchLockSecurityResult result;
            result.Succeeded = true;
            return result;
        }
        catch (const std::exception& exception)
        {
            return _failure(ERROR_NOT_ENOUGH_MEMORY, exception.what());
        }
        catch (...)
        {
            return _failure(ERROR_UNHANDLED_EXCEPTION, "Unexpected Runtime launch-lock security failure");
        }
    }
}
