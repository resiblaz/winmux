// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    class RuntimeIdentityTests
    {
        TEST_CLASS(RuntimeIdentityTests);

        TEST_METHOD(IdentityIsStableAcrossCaseAndLexicalPathAliases);
        TEST_METHOD(ChannelRootAndUserSidIsolateRuntimeObjects);
        TEST_METHOD(DerivedNamesAreBoundedAndProtocolSafe);
        TEST_METHOD(RejectsRelativeRootsInvalidChannelsAndInvalidSids);
        TEST_METHOD(CurrentUserIdentityMatchesExplicitSid);
    };

    void RuntimeIdentityTests::IdentityIsStableAcrossCaseAndLexicalPathAliases()
    {
        const auto sid = RequireCurrentUserSid();
        const auto first = BuildRuntimeIdentity(
            L"Dev",
            LR"(C:\Runtime-Test\Settings\.\child\..)",
            sid);
        const auto second = BuildRuntimeIdentity(
            L"dev",
            LR"(c:\runtime-test\settings)",
            sid);
        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(second.Succeeded());
        VERIFY_ARE_EQUAL(first.Identity.ProtocolIdentity, second.Identity.ProtocolIdentity);
        VERIFY_ARE_EQUAL(first.Identity.InstanceMutexName, second.Identity.InstanceMutexName);
        VERIFY_ARE_EQUAL(first.Identity.ControlPipeName, second.Identity.ControlPipeName);
        VERIFY_ARE_EQUAL(std::wstring{ L"dev" }, first.Identity.Channel);
    }

    void RuntimeIdentityTests::ChannelRootAndUserSidIsolateRuntimeObjects()
    {
        const auto sid = RequireCurrentUserSid();
        const auto baseline = BuildRuntimeIdentity(L"dev", LR"(C:\runtime\settings)", sid);
        const auto channel = BuildRuntimeIdentity(L"stable", LR"(C:\runtime\settings)", sid);
        const auto root = BuildRuntimeIdentity(L"dev", LR"(C:\runtime\other)", sid);
        const auto user = BuildRuntimeIdentity(L"dev", LR"(C:\runtime\settings)", L"S-1-5-21-1-2-3-1001");
        VERIFY_IS_TRUE(baseline.Succeeded());
        VERIFY_IS_TRUE(channel.Succeeded());
        VERIFY_IS_TRUE(root.Succeeded());
        VERIFY_IS_TRUE(user.Succeeded());
        VERIFY_IS_TRUE(baseline.Identity.ProtocolIdentity != channel.Identity.ProtocolIdentity);
        VERIFY_IS_TRUE(baseline.Identity.ProtocolIdentity != root.Identity.ProtocolIdentity);
        VERIFY_IS_TRUE(baseline.Identity.ProtocolIdentity != user.Identity.ProtocolIdentity);
    }

    void RuntimeIdentityTests::DerivedNamesAreBoundedAndProtocolSafe()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        VERIFY_IS_TRUE(identity.ProtocolIdentity.starts_with("wt-task-runtime-v1:"));
        VERIFY_ARE_EQUAL(size_t{ 83 }, identity.ProtocolIdentity.size());
        VERIFY_IS_TRUE(identity.InstanceMutexName.starts_with(L"Global\\WT.TaskRuntime.Instance."));
        VERIFY_IS_TRUE(identity.LaunchMutexName.starts_with(L"Global\\WT.TaskRuntime.Launch."));
        VERIFY_IS_TRUE(identity.ControlPipeName.starts_with(L"\\\\.\\pipe\\WT.TaskRuntime."));
        VERIFY_IS_TRUE(identity.ControlPipeName.ends_with(L".control"));
        VERIFY_IS_TRUE(identity.InstanceMutexName.size() <= 240);
        VERIFY_IS_TRUE(identity.ControlPipeName.size() <= 240);
    }

    void RuntimeIdentityTests::RejectsRelativeRootsInvalidChannelsAndInvalidSids()
    {
        const auto sid = RequireCurrentUserSid();
        VERIFY_IS_TRUE(BuildRuntimeIdentity(L"dev", L"relative", sid).Status == RuntimeIdentityStatus::InvalidSettingsRoot);
        VERIFY_IS_TRUE(BuildRuntimeIdentity(L"bad channel", LR"(C:\settings)", sid).Status == RuntimeIdentityStatus::InvalidChannel);
        VERIFY_IS_TRUE(BuildRuntimeIdentity(L"dev", LR"(C:\settings)", L"not-a-sid").Status == RuntimeIdentityStatus::InvalidUserSid);
        VERIFY_IS_TRUE(BuildRuntimeIdentity(L"", LR"(C:\settings)", sid).Status == RuntimeIdentityStatus::InvalidChannel);
    }

    void RuntimeIdentityTests::CurrentUserIdentityMatchesExplicitSid()
    {
        UniqueRuntimeRoot root;
        const auto sid = RequireCurrentUserSid();
        const auto explicitIdentity = BuildRuntimeIdentity(L"dev", root.Path(), sid);
        const auto currentIdentity = BuildCurrentUserRuntimeIdentity(L"dev", root.Path());
        VERIFY_IS_TRUE(explicitIdentity.Succeeded());
        VERIFY_IS_TRUE(currentIdentity.Succeeded());
        VERIFY_ARE_EQUAL(explicitIdentity.Identity.ProtocolIdentity, currentIdentity.Identity.ProtocolIdentity);
        VERIFY_ARE_EQUAL(sid, currentIdentity.Identity.UserSid);
    }
}
