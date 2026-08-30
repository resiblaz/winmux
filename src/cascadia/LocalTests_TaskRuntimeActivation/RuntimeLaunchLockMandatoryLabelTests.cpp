// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "ActivationTestSupport.h"
#include "RuntimeLaunchLock.h"

#include <sddl.h>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    namespace
    {
        HANDLE _createPresecuredMutex(
            const RuntimeIdentity& identity,
            const std::wstring_view mandatoryLabel)
        {
            const auto sddl = L"O:" + identity.UserSid +
                              L"G:" + identity.UserSid +
                              L"D:P(A;;GA;;;" + identity.UserSid +
                              L")(A;;GA;;;SY)" + std::wstring{ mandatoryLabel };
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
            const auto mutex = CreateMutexW(
                &attributes,
                FALSE,
                identity.LaunchMutexName.c_str());
            LocalFree(descriptor);
            return mutex;
        }
    }

    class RuntimeLaunchLockMandatoryLabelTests
    {
        TEST_CLASS(RuntimeLaunchLockMandatoryLabelTests);

        TEST_METHOD(AcceptsPrecreatedMutexWithExactMediumNoWriteUpLabel);
        TEST_METHOD(RejectsMissingLowAndWrongMandatoryLabels);
    };

    void RuntimeLaunchLockMandatoryLabelTests::AcceptsPrecreatedMutexWithExactMediumNoWriteUpLabel()
    {
        UniqueActivationIdentity identity;
        const auto precreated = _createPresecuredMutex(
            identity.Identity(),
            L"S:(ML;;NW;;;ME)");
        VERIFY_IS_NOT_NULL(precreated);
        if (!precreated)
        {
            return;
        }

        const auto services = MakeRuntimeActivationWindowsServices();
        auto acquired = AcquireRuntimeLaunchLock(
            identity.Identity(),
            DeadlineAfter(*services),
            nullptr,
            *services);
        VERIFY_IS_TRUE(acquired.Acquired());
        VERIFY_IS_TRUE(acquired.Lock.Release() == RuntimeLaunchLockReleaseStatus::Released);
        CloseHandle(precreated);
    }

    void RuntimeLaunchLockMandatoryLabelTests::RejectsMissingLowAndWrongMandatoryLabels()
    {
        // A medium-integrity test process cannot portably create a High label
        // object with MUTEX_ALL_ACCESS. Production validation compares the
        // label SID exactly, while these constructible cases cover absence,
        // lower integrity, and an incorrect mandatory policy mask.
        const std::array<std::wstring_view, 3> labels{
            L"",
            L"S:(ML;;NW;;;LW)",
            L"S:(ML;;NR;;;ME)",
        };
        for (const auto label : labels)
        {
            UniqueActivationIdentity identity;
            const auto precreated = _createPresecuredMutex(identity.Identity(), label);
            VERIFY_IS_NOT_NULL(precreated);
            if (!precreated)
            {
                continue;
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
}
