// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ActivationTestSupport.h"
#include "RuntimeLaunchLock.h"
#include "RuntimeSecurity.h"

#include <aclapi.h>
#include <sddl.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    namespace
    {
        bool _daclContainsSid(const ACL* const dacl, const PSID sid) noexcept
        {
            if (!dacl || !sid)
            {
                return false;
            }
            for (DWORD index = 0; index < dacl->AceCount; ++index)
            {
                void* value{};
                if (!GetAce(const_cast<ACL*>(dacl), index, &value) || !value)
                {
                    continue;
                }
                const auto header = static_cast<ACE_HEADER*>(value);
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
                {
                    continue;
                }
                const auto allowed = static_cast<ACCESS_ALLOWED_ACE*>(value);
                if (EqualSid(sid, &allowed->SidStart))
                {
                    return true;
                }
            }
            return false;
        }

        HANDLE _createMutexWithSddl(
            const std::wstring& name,
            const std::wstring& sddl)
        {
            PSECURITY_DESCRIPTOR descriptor{};
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(),
                    SDDL_REVISION_1,
                    &descriptor,
                    nullptr))
            {
                return nullptr;
            }
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.lpSecurityDescriptor = descriptor;
            attributes.bInheritHandle = FALSE;
            const auto mutex = CreateMutexW(&attributes, FALSE, name.c_str());
            LocalFree(descriptor);
            return mutex;
        }
    }

    class RuntimeLaunchLockOwnershipTests
    {
        TEST_CLASS(RuntimeLaunchLockOwnershipTests);

        TEST_METHOD(HandleIsNonInheritableAndAclContainsCurrentUserAndSystem);
        TEST_METHOD(OwnerThreadScopeExitReleasesMutex);
        TEST_METHOD(SameThreadMovesPreserveOwnership);
        TEST_METHOD(CrossThreadReleaseIsRejectedWithoutLosingOwnership);
        TEST_METHOD(AbandonedMutexIsAcquiredAndReported);
        TEST_METHOD(RejectsPrecreatedMutexWithBroadAcl);
        TEST_METHOD(RejectsPrecreatedMutexThatDeniesCurrentUser);
    };

    void RuntimeLaunchLockOwnershipTests::HandleIsNonInheritableAndAclContainsCurrentUserAndSystem()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto acquired = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(acquired.Acquired());

        DWORD flags{};
        VERIFY_IS_TRUE(GetHandleInformation(acquired.Lock.Handle(), &flags));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, flags & HANDLE_FLAG_INHERIT);

        PACL dacl{};
        PSECURITY_DESCRIPTOR descriptor{};
        const auto securityStatus = GetSecurityInfo(
            acquired.Lock.Handle(),
            SE_KERNEL_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            &dacl,
            nullptr,
            &descriptor);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_SUCCESS }, securityStatus);
        VERIFY_IS_NOT_NULL(dacl);

        PSID currentUser{};
        const auto converted = ConvertStringSidToSidW(
            identity.Identity().UserSid.c_str(),
            &currentUser);
        VERIFY_IS_TRUE(converted);
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
        PSID localSystem{};
        const auto allocated = AllocateAndInitializeSid(
            &ntAuthority,
            1,
            SECURITY_LOCAL_SYSTEM_RID,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            &localSystem);
        VERIFY_IS_TRUE(allocated);
        if (dacl && currentUser)
        {
            VERIFY_IS_TRUE(_daclContainsSid(dacl, currentUser));
        }
        if (dacl && localSystem)
        {
            VERIFY_IS_TRUE(_daclContainsSid(dacl, localSystem));
        }

        if (localSystem)
        {
            FreeSid(localSystem);
        }
        if (currentUser)
        {
            LocalFree(currentUser);
        }
        if (descriptor)
        {
            LocalFree(descriptor);
        }
        VERIFY_IS_TRUE(acquired.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockOwnershipTests::OwnerThreadScopeExitReleasesMutex()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        {
            auto first = AcquireRuntimeLaunchLock(
                identity.Identity(),
                DeadlineAfter(*services),
                nullptr,
                *services);
            VERIFY_IS_TRUE(first.Acquired());
        }

        auto replacement = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(replacement.Acquired());
        VERIFY_IS_TRUE(
            replacement.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockOwnershipTests::SameThreadMovesPreserveOwnership()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto acquired = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(acquired.Acquired());
        const auto ownerThread = acquired.Lock.OwnerThreadId();

        RuntimeLaunchLock moved{ std::move(acquired.Lock) };
        VERIFY_IS_FALSE(static_cast<bool>(acquired.Lock));
        VERIFY_IS_TRUE(static_cast<bool>(moved));
        RuntimeLaunchLock assigned;
        assigned = std::move(moved);
        VERIFY_IS_FALSE(static_cast<bool>(moved));
        VERIFY_IS_TRUE(static_cast<bool>(assigned));
        VERIFY_ARE_EQUAL(ownerThread, assigned.OwnerThreadId());
        VERIFY_IS_TRUE(assigned.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockOwnershipTests::CrossThreadReleaseIsRejectedWithoutLosingOwnership()
    {
        UniqueActivationIdentity identity;
        const auto services = MakeRuntimeActivationWindowsServices();
        auto acquired = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(acquired.Acquired());
        const auto ownerThread = acquired.Lock.OwnerThreadId();
        RuntimeLaunchLockReleaseStatus crossThreadStatus{
            RuntimeLaunchLockReleaseStatus::SystemError
        };
        std::thread wrongThread{ [&] {
            crossThreadStatus = acquired.Lock.Release();
        } };
        wrongThread.join();

        VERIFY_IS_TRUE(crossThreadStatus == RuntimeLaunchLockReleaseStatus::WrongThread);
        VERIFY_IS_TRUE(static_cast<bool>(acquired.Lock));
        VERIFY_ARE_EQUAL(ownerThread, acquired.Lock.OwnerThreadId());
        VERIFY_IS_TRUE(acquired.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
    }

    void RuntimeLaunchLockOwnershipTests::AbandonedMutexIsAcquiredAndReported()
    {
        UniqueActivationIdentity identity;
        auto security = CreateRuntimeSecurityAttributes(identity.Identity().UserSid);
        VERIFY_IS_TRUE(security.Succeeded());
        auto ownerReady = MakeTestEvent();
        auto allowOwnerExit = MakeTestEvent();
        std::atomic<HANDLE> ownerHandle{};
        std::thread abandonedOwner{ [&] {
            ownerHandle.store(CreateMutexW(
                const_cast<SECURITY_ATTRIBUTES*>(security.Security.Attributes()),
                TRUE,
                identity.Identity().LaunchMutexName.c_str()),
                std::memory_order_release);
            SetEvent(ownerReady.get());
            WaitForSingleObject(allowOwnerExit.get(), 2000);
            // Intentionally leave the mutex owned. Thread exit abandons it.
        } };

        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(ownerReady.get(), 1000));
        const auto owner = ownerHandle.load(std::memory_order_acquire);
        VERIFY_IS_NOT_NULL(owner);
        const auto keeper = OpenMutexW(
            SYNCHRONIZE | MUTEX_MODIFY_STATE,
            FALSE,
            identity.Identity().LaunchMutexName.c_str());
        VERIFY_IS_NOT_NULL(keeper);
        SetEvent(allowOwnerExit.get());
        abandonedOwner.join();

        const auto services = MakeRuntimeActivationWindowsServices();
        auto acquired = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(acquired.Acquired());
        VERIFY_IS_TRUE(acquired.Lock.Abandoned());
        VERIFY_IS_TRUE(acquired.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);

        if (keeper)
        {
            CloseHandle(keeper);
        }
        if (owner)
        {
            CloseHandle(owner);
        }
    }

    void RuntimeLaunchLockOwnershipTests::RejectsPrecreatedMutexWithBroadAcl()
    {
        UniqueActivationIdentity identity;
        const auto& sid = identity.Identity().UserSid;
        const auto sddl = L"O:" + sid + L"G:" + sid +
                          L"D:P(A;;GA;;;WD)S:(ML;;NW;;;ME)";
        const auto precreated = _createMutexWithSddl(
            identity.Identity().LaunchMutexName,
            sddl);
        VERIFY_IS_NOT_NULL(precreated);
        if (!precreated)
        {
            return;
        }

        const auto services = MakeRuntimeActivationWindowsServices();
        const auto rejected = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(rejected.Status == RuntimeLaunchLockStatus::SecurityFailure);
        VERIFY_IS_FALSE(rejected.Acquired());
        CloseHandle(precreated);
    }

    void RuntimeLaunchLockOwnershipTests::RejectsPrecreatedMutexThatDeniesCurrentUser()
    {
        UniqueActivationIdentity identity;
        const auto& sid = identity.Identity().UserSid;
        const auto sddl = L"O:" + sid + L"G:" + sid +
                          L"D:P(A;;GA;;;SY)S:(ML;;NW;;;ME)";
        const auto precreated = _createMutexWithSddl(
            identity.Identity().LaunchMutexName,
            sddl);
        VERIFY_IS_NOT_NULL(precreated);
        if (!precreated)
        {
            return;
        }

        const auto services = MakeRuntimeActivationWindowsServices();
        const auto rejected = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(rejected.Status == RuntimeLaunchLockStatus::SecurityFailure);
        VERIFY_IS_FALSE(rejected.Acquired());
        CloseHandle(precreated);
    }
}
