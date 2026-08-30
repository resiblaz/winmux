// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeOnDemandActivationTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeLocalTests::ProcessFixtures;
using namespace TaskRuntimeLocalTests::OnDemandActivation;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        void _runBreakawayCase(
            const DWORD jobFlags,
            const RuntimeParentJobStrategy expectedStrategy,
            const size_t index)
        {
            ProductRuntimeTestIdentity identity;
            ActivationTestJob job{
                jobFlags | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            };
            VERIFY_IS_TRUE(static_cast<bool>(job));
            if (!job)
            {
                return;
            }
            TemporaryActivationRecord record{ identity, index };
            const auto startName = ActivationStartEventName(identity);
            ProcessFixtures::details::HandleOwner start{
                CreateEventW(nullptr, TRUE, FALSE, startName.c_str())
            };
            VERIFY_IS_TRUE(static_cast<bool>(start));
            if (!start)
            {
                return;
            }

            ActivationParentOptions options;
            options.ClientInstanceId = ActivationClientInstanceId(index);
            options.StartEventName = startName;
            options.LaunchRecord = record.Path();
            options.Job = job.Handle();
            options.AdditionalCreationFlags = CREATE_BREAKAWAY_FROM_JOB;
            auto launched = LaunchActivationParent(identity, std::move(options));
            VERIFY_IS_TRUE(launched.Succeeded());
            if (!launched.Succeeded())
            {
                return;
            }
            BOOL parentInJob{};
            VERIFY_IS_TRUE(IsProcessInJob(
                launched.Process.Handle(),
                job.Handle(),
                &parentInJob));
            VERIFY_IS_TRUE(parentInJob != FALSE);
            SetEvent(start.Get());

            DWORD parentExit{};
            VERIFY_IS_TRUE(launched.Process.WaitForExit(
                ActivationParentExitTimeoutMilliseconds,
                parentExit));
            VERIFY_ARE_EQUAL(DWORD{ 0 }, parentExit);
            if (parentExit != 0)
            {
                return;
            }
            auto probe = RequireActiveRuntime(
                identity,
                ActivationClientInstanceId(index + 100));
            ActiveRuntimeProcessGuard runtime{ probe.ProcessId };
            VERIFY_IS_TRUE(static_cast<bool>(runtime));
            const auto launch = record.Read();
            VERIFY_IS_TRUE(launch.has_value());
            if (!launch)
            {
                return;
            }
            VERIFY_ARE_EQUAL(size_t{ 1 }, launch->Calls);
            VERIFY_ARE_EQUAL(
                static_cast<int>(RuntimeProcessLaunchStatus::Started),
                launch->Status);
            VERIFY_ARE_EQUAL(
                static_cast<int>(RuntimeProcessLaunchPhase::Committed),
                launch->Phase);
            VERIFY_ARE_EQUAL(
                static_cast<int>(expectedStrategy),
                launch->Strategy);

            if (!runtime)
            {
                return;
            }
            BOOL inAnyJob{ TRUE };
            VERIFY_IS_TRUE(IsProcessInJob(
                runtime.Handle(),
                nullptr,
                &inAnyJob));
            VERIFY_IS_FALSE(inAnyJob != FALSE);
            BOOL inParentJob{ TRUE };
            VERIFY_IS_TRUE(IsProcessInJob(
                runtime.Handle(),
                job.Handle(),
                &inParentJob));
            VERIFY_IS_FALSE(inParentJob != FALSE);
            probe.Client.Channel.Close();
            VERIFY_IS_TRUE(runtime.TerminateAndWait());
        }
    }

    class RuntimeActivationJobTests
    {
        TEST_CLASS(RuntimeActivationJobTests);

        TEST_METHOD(RealSilentAndExplicitBreakawayLeaveEveryJob);
        TEST_METHOD(RealDeniedBreakawayCreatesNoRuntime);
        TEST_METHOD(RealNotInJobStrategyWhenHarnessCanBreakAway);
    };

    void RuntimeActivationJobTests::RealSilentAndExplicitBreakawayLeaveEveryJob()
    {
        _runBreakawayCase(
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
            RuntimeParentJobStrategy::SilentBreakaway,
            200);
        _runBreakawayCase(
            JOB_OBJECT_LIMIT_BREAKAWAY_OK,
            RuntimeParentJobStrategy::ExplicitBreakaway,
            201);
    }

    void RuntimeActivationJobTests::RealDeniedBreakawayCreatesNoRuntime()
    {
        ProductRuntimeTestIdentity identity;
        ActivationTestJob job{ JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE };
        VERIFY_IS_TRUE(static_cast<bool>(job));
        if (!job)
        {
            return;
        }
        TemporaryActivationRecord record{ identity, 202 };
        ActivationParentOptions options;
        options.ClientInstanceId = ActivationClientInstanceId(202);
        options.LaunchRecord = record.Path();
        options.Job = job.Handle();
        options.AdditionalCreationFlags = CREATE_BREAKAWAY_FROM_JOB;
        auto launched = LaunchActivationParent(identity, std::move(options));
        VERIFY_IS_TRUE(launched.Succeeded());
        if (!launched.Succeeded())
        {
            return;
        }

        DWORD parentExit{};
        VERIFY_IS_TRUE(launched.Process.WaitForExit(
            ActivationParentExitTimeoutMilliseconds,
            parentExit));
        VERIFY_ARE_EQUAL(DWORD{ 6 }, parentExit);
        const auto launch = record.Read();
        VERIFY_IS_TRUE(launch.has_value());
        if (!launch)
        {
            return;
        }
        VERIFY_ARE_EQUAL(size_t{ 1 }, launch->Calls);
        VERIFY_ARE_EQUAL(
            static_cast<int>(RuntimeProcessLaunchStatus::ParentJobBreakawayDenied),
            launch->Status);
        VERIFY_ARE_EQUAL(
            static_cast<int>(RuntimeProcessLaunchPhase::ParentJobPreflight),
            launch->Phase);

        const auto absent = ConnectRuntimeControlChannel(
            identity.Identity(),
            100,
            nullptr,
            RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent);
        VERIFY_IS_FALSE(absent.Succeeded());
        VERIFY_IS_TRUE(
            absent.EndpointObservation == RuntimeControlEndpointObservation::Absent);
    }

    void RuntimeActivationJobTests::RealNotInJobStrategyWhenHarnessCanBreakAway()
    {
        ProductRuntimeTestIdentity identity;
        TemporaryActivationRecord record{ identity, 203 };
        const auto startName = ActivationStartEventName(identity);
        ProcessFixtures::details::HandleOwner start{
            CreateEventW(nullptr, TRUE, FALSE, startName.c_str())
        };
        VERIFY_IS_TRUE(static_cast<bool>(start));
        if (!start)
        {
            return;
        }

        ActivationParentOptions options;
        options.ClientInstanceId = ActivationClientInstanceId(203);
        options.StartEventName = startName;
        options.LaunchRecord = record.Path();
        options.AdditionalCreationFlags = CREATE_BREAKAWAY_FROM_JOB;
        auto launched = LaunchActivationParent(identity, std::move(options));
        if (!launched.Succeeded() && launched.SystemError == ERROR_ACCESS_DENIED)
        {
            WEX::Logging::Log::Comment(
                L"Not-in-Job activation case unavailable: test host Job denies breakaway");
            return;
        }
        VERIFY_IS_TRUE(launched.Succeeded());
        if (!launched.Succeeded())
        {
            return;
        }
        BOOL parentInJob{ TRUE };
        VERIFY_IS_TRUE(IsProcessInJob(
            launched.Process.Handle(),
            nullptr,
            &parentInJob));
        if (parentInJob != FALSE)
        {
            WEX::Logging::Log::Comment(
                L"Not-in-Job activation case unavailable: helper remained in an outer Job");
            return;
        }
        SetEvent(start.Get());

        DWORD parentExit{};
        VERIFY_IS_TRUE(launched.Process.WaitForExit(
            ActivationParentExitTimeoutMilliseconds,
            parentExit));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, parentExit);
        if (parentExit != 0)
        {
            return;
        }
        auto probe = RequireActiveRuntime(
            identity,
            ActivationClientInstanceId(303));
        ActiveRuntimeProcessGuard runtime{ probe.ProcessId };
        VERIFY_IS_TRUE(static_cast<bool>(runtime));
        const auto launch = record.Read();
        VERIFY_IS_TRUE(launch.has_value());
        if (!launch)
        {
            return;
        }
        VERIFY_ARE_EQUAL(
            static_cast<int>(RuntimeParentJobStrategy::NotInJob),
            launch->Strategy);

        probe.Client.Channel.Close();
        VERIFY_IS_TRUE(runtime.TerminateAndWait());
    }
}
