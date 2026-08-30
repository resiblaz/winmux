// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeProcessLauncherTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeProcessLauncherValidationTests
    {
        TEST_CLASS(RuntimeProcessLauncherValidationTests);

        TEST_METHOD(RejectsMissingApiAndMismatchedRuntimeIdentity);
        TEST_METHOD(ReportsCommandLineTooLongBeforeJobOrCreate);
        TEST_METHOD(CancellationImmediatelyBeforeCreateStillCreatesNothing);
        TEST_METHOD(ParentJobFailuresPreventCreate);
    };

    void RuntimeProcessLauncherValidationTests::RejectsMissingApiAndMismatchedRuntimeIdentity()
    {
        UniqueActivationIdentity identity;
        const auto missingApi = MakeTestProcessLauncher(nullptr);
        VERIFY_IS_NULL(missingApi.get());

        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(api);
        auto request = ValidLaunchRequest(identity);
        request.Identity.ControlPipeName.append(L".wrong");
        const auto invalid = launcher->Launch(request);
        VERIFY_IS_TRUE(invalid.Status == RuntimeProcessLaunchStatus::InvalidRequest);
        VERIFY_IS_TRUE(api->PathAttributeCalls.empty());
        VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
    }

    void RuntimeProcessLauncherValidationTests::ReportsCommandLineTooLongBeforeJobOrCreate()
    {
        UniqueActivationIdentity identity;
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        const auto launcher = MakeTestProcessLauncher(api);
        auto request = ValidLaunchRequest(identity);
        request.ExecutablePath = L"C:\\" + std::wstring(32700, L'a') + L"\\TaskRuntime.exe";
        request.TrustedRoot = request.ExecutablePath.parent_path();
        const auto result = launcher->Launch(request);
        VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::CommandLineTooLong);
        VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::Validation);
        VERIFY_IS_FALSE(api->PathAttributeCalls.empty());
        VERIFY_ARE_EQUAL(size_t{ 0 }, api->CurrentProcessJobCalls);
        VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
    }

    void RuntimeProcessLauncherValidationTests::CancellationImmediatelyBeforeCreateStillCreatesNothing()
    {
        UniqueActivationIdentity identity;
        auto cancellation = MakeTestEvent();
        auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
        api->PollResults.emplace_back(RuntimeWindowsWaitResult{
            RuntimeWindowsWaitStatus::NotSignaled,
            ERROR_SUCCESS,
        });
        api->PollResults.emplace_back(RuntimeWindowsWaitResult{
            RuntimeWindowsWaitStatus::Signaled,
            ERROR_SUCCESS,
        });
        const auto launcher = MakeTestProcessLauncher(api);
        const auto result = launcher->Launch(ValidLaunchRequest(identity, cancellation.get()));
        VERIFY_IS_TRUE(result.Status == RuntimeProcessLaunchStatus::CancelledBeforeCreate);
        VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::ProcessCreation);
        VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        VERIFY_ARE_EQUAL(size_t{ 2 }, api->PollHandles.size());
    }

    void RuntimeProcessLauncherValidationTests::ParentJobFailuresPreventCreate()
    {
        UniqueActivationIdentity identity;
        for (size_t index = 0; index < 3; ++index)
        {
            auto api = std::make_shared<FakeRuntimeWindowsProcessApi>();
            api->CurrentProcessIsInJob = index != 0;
            if (index == 0)
            {
                api->CurrentProcessJobSucceeded = false;
                api->CurrentProcessJobError = ERROR_INVALID_HANDLE;
            }
            else if (index == 1)
            {
                api->CurrentJobLimitSucceeded = false;
                api->CurrentJobLimitError = ERROR_ACCESS_DENIED;
            }
            else
            {
                api->CurrentJobLimits = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            }
            const auto launcher = MakeTestProcessLauncher(api);
            const auto result = launcher->Launch(ValidLaunchRequest(identity));
            VERIFY_IS_TRUE(result.Status == (index == 2 ?
                                                  RuntimeProcessLaunchStatus::ParentJobBreakawayDenied :
                                                  RuntimeProcessLaunchStatus::ParentJobQueryFailed));
            VERIFY_IS_TRUE(result.Phase == RuntimeProcessLaunchPhase::ParentJobPreflight);
            VERIFY_IS_TRUE(api->CreateCallSnapshot().empty());
        }
    }
}
