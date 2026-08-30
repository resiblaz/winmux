// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeBootstrap.h"
#include "TaskRuntimeTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    class RuntimeBootstrapTests
    {
        TEST_CLASS(RuntimeBootstrapTests);

        TEST_METHOD(SingleInstanceRejectsSecondOwnerAndReleasesWithRaii);
        TEST_METHOD(ControlPipeUsesDerivedNameSecurityAndFirstInstanceGuard);
        TEST_METHOD(DifferentRuntimeIdentityUsesIndependentKernelObjects);
        TEST_METHOD(BootstrapComposesIdentitySecurityInstanceAndPipe);
        TEST_METHOD(InvalidKernelObjectConfigurationFailsClosed);
    };

    void RuntimeBootstrapTests::SingleInstanceRejectsSecondOwnerAndReleasesWithRaii()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto security = RequireSecurity(identity.UserSid);
        {
            auto first = AcquireRuntimeSingleInstance(identity.InstanceMutexName, security.Attributes());
            VERIFY_IS_TRUE(first.Acquired());
            VERIFY_IS_TRUE(static_cast<bool>(first.Instance));

            const auto second = AcquireRuntimeSingleInstance(identity.InstanceMutexName, security.Attributes());
            VERIFY_IS_FALSE(second.Acquired());
            VERIFY_IS_TRUE(second.Status == RuntimeSingleInstanceStatus::AlreadyRunning);
        }

        const auto reacquired = AcquireRuntimeSingleInstance(identity.InstanceMutexName, security.Attributes());
        VERIFY_IS_TRUE(reacquired.Acquired());
    }

    void RuntimeBootstrapTests::ControlPipeUsesDerivedNameSecurityAndFirstInstanceGuard()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto security = RequireSecurity(identity.UserSid);
        auto first = CreateRuntimeControlPipe(identity, security.Attributes(), true);
        VERIFY_IS_TRUE(first.Created());
        VERIFY_IS_TRUE(static_cast<bool>(first.Pipe));
        VERIFY_ARE_EQUAL(identity.ControlPipeName, first.Pipe.Name());

        DWORD flags{};
        VERIFY_IS_TRUE(GetNamedPipeInfo(first.Pipe.Handle(), &flags, nullptr, nullptr, nullptr));
        VERIFY_IS_TRUE((flags & PIPE_SERVER_END) != 0);

        PSID owner{};
        PSECURITY_DESCRIPTOR descriptor{};
        VERIFY_ARE_EQUAL(
            DWORD{ ERROR_SUCCESS },
            GetSecurityInfo(
                first.Pipe.Handle(),
                SE_KERNEL_OBJECT,
                OWNER_SECURITY_INFORMATION,
                &owner,
                nullptr,
                nullptr,
                nullptr,
                &descriptor));
        PSID expectedSid{};
        VERIFY_IS_TRUE(ConvertStringSidToSidW(identity.UserSid.c_str(), &expectedSid));
        VERIFY_IS_TRUE(EqualSid(owner, expectedSid));
        LocalFree(expectedSid);
        LocalFree(descriptor);

        const auto second = CreateRuntimeControlPipe(identity, security.Attributes(), true);
        VERIFY_IS_FALSE(second.Created());
        VERIFY_IS_TRUE(second.Status == RuntimePipeStatus::AlreadyExists);
    }

    void RuntimeBootstrapTests::DifferentRuntimeIdentityUsesIndependentKernelObjects()
    {
        UniqueRuntimeRoot firstRoot;
        UniqueRuntimeRoot secondRoot;
        const auto firstIdentity = RequireIdentity(firstRoot.Path());
        const auto secondIdentity = RequireIdentity(secondRoot.Path());
        auto security = RequireSecurity(firstIdentity.UserSid);

        const auto firstInstance = AcquireRuntimeSingleInstance(firstIdentity.InstanceMutexName, security.Attributes());
        const auto secondInstance = AcquireRuntimeSingleInstance(secondIdentity.InstanceMutexName, security.Attributes());
        VERIFY_IS_TRUE(firstInstance.Acquired());
        VERIFY_IS_TRUE(secondInstance.Acquired());

        const auto firstPipe = CreateRuntimeControlPipe(firstIdentity, security.Attributes(), true);
        const auto secondPipe = CreateRuntimeControlPipe(secondIdentity, security.Attributes(), true);
        VERIFY_IS_TRUE(firstPipe.Created());
        VERIFY_IS_TRUE(secondPipe.Created());
        VERIFY_IS_TRUE(firstPipe.Pipe.Name() != secondPipe.Pipe.Name());
    }

    void RuntimeBootstrapTests::BootstrapComposesIdentitySecurityInstanceAndPipe()
    {
        UniqueRuntimeRoot root;
        {
            const auto first = BootstrapCurrentUserRuntime(L"dev", root.Path());
            VERIFY_IS_TRUE(first.Ready());
            VERIFY_IS_TRUE(static_cast<bool>(first.Security));
            VERIFY_IS_TRUE(static_cast<bool>(first.Instance));
            VERIFY_IS_TRUE(static_cast<bool>(first.ControlPipe));

            const auto second = BootstrapCurrentUserRuntime(L"dev", root.Path());
            VERIFY_IS_FALSE(second.Ready());
            VERIFY_IS_TRUE(second.Status == RuntimeBootstrapStatus::AlreadyRunning);
        }
        VERIFY_IS_TRUE(BootstrapCurrentUserRuntime(L"dev", root.Path()).Ready());
    }

    void RuntimeBootstrapTests::InvalidKernelObjectConfigurationFailsClosed()
    {
        const auto invalidInstance = AcquireRuntimeSingleInstance(L"bad-name", nullptr);
        VERIFY_IS_TRUE(invalidInstance.Status == RuntimeSingleInstanceStatus::InvalidName);

        RuntimeIdentity invalidIdentity;
        SECURITY_ATTRIBUTES invalidSecurity{};
        const auto invalidPipe = CreateRuntimeControlPipe(invalidIdentity, &invalidSecurity, true);
        VERIFY_IS_TRUE(invalidPipe.Status == RuntimePipeStatus::InvalidConfiguration);

        const auto invalidBootstrap = BootstrapCurrentUserRuntime(L"bad channel", L"relative");
        VERIFY_IS_TRUE(invalidBootstrap.Status == RuntimeBootstrapStatus::InvalidIdentity);
    }
}
