// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "FakeRuntimeWindowsProcessApi.h"
#include "RuntimeParentJob.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;

namespace TaskRuntimeActivationLocalTests
{
    class RuntimeParentJobTests
    {
        TEST_CLASS(RuntimeParentJobTests);

        TEST_METHOD(PlansNoJobSilentAndExplicitBreakawayExactly);
        TEST_METHOD(DeniedAndFailedParentJobQueriesFailClosed);
        TEST_METHOD(ChildMustProveItIsOutsideEveryJob);
    };

    void RuntimeParentJobTests::PlansNoJobSilentAndExplicitBreakawayExactly()
    {
        FakeRuntimeWindowsProcessApi api;
        auto plan = PlanRuntimeParentJob(api);
        VERIFY_IS_TRUE(plan.Ready());
        VERIFY_IS_TRUE(plan.Strategy == RuntimeParentJobStrategy::NotInJob);
        VERIFY_ARE_EQUAL(DWORD{ 0 }, plan.AdditionalCreationFlags);
        VERIFY_ARE_EQUAL(size_t{ 1 }, api.CurrentProcessJobCalls);
        VERIFY_ARE_EQUAL(size_t{ 0 }, api.CurrentJobLimitCalls);

        api.CurrentProcessIsInJob = true;
        api.CurrentJobLimits = JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
                               JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        plan = PlanRuntimeParentJob(api);
        VERIFY_IS_TRUE(plan.Ready());
        VERIFY_IS_TRUE(plan.Strategy == RuntimeParentJobStrategy::SilentBreakaway);
        VERIFY_ARE_EQUAL(DWORD{ 0 }, plan.AdditionalCreationFlags);

        api.CurrentJobLimits = JOB_OBJECT_LIMIT_BREAKAWAY_OK |
                               JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        plan = PlanRuntimeParentJob(api);
        VERIFY_IS_TRUE(plan.Ready());
        VERIFY_IS_TRUE(plan.Strategy == RuntimeParentJobStrategy::ExplicitBreakaway);
        VERIFY_ARE_EQUAL(DWORD{ CREATE_BREAKAWAY_FROM_JOB }, plan.AdditionalCreationFlags);
    }

    void RuntimeParentJobTests::DeniedAndFailedParentJobQueriesFailClosed()
    {
        FakeRuntimeWindowsProcessApi api;
        api.CurrentProcessJobSucceeded = false;
        api.CurrentProcessJobError = ERROR_INVALID_HANDLE;
        auto plan = PlanRuntimeParentJob(api);
        VERIFY_IS_TRUE(plan.Status == RuntimeParentJobPlanStatus::QueryFailed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, plan.SystemError);

        api.CurrentProcessJobSucceeded = true;
        api.CurrentProcessIsInJob = true;
        api.CurrentJobLimitSucceeded = false;
        api.CurrentJobLimitError = ERROR_ACCESS_DENIED;
        plan = PlanRuntimeParentJob(api);
        VERIFY_IS_TRUE(plan.Status == RuntimeParentJobPlanStatus::QueryFailed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_ACCESS_DENIED }, plan.SystemError);

        api.CurrentJobLimitSucceeded = true;
        api.CurrentJobLimits = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        plan = PlanRuntimeParentJob(api);
        VERIFY_IS_TRUE(plan.Status == RuntimeParentJobPlanStatus::BreakawayDenied);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_ACCESS_DENIED }, plan.SystemError);
    }

    void RuntimeParentJobTests::ChildMustProveItIsOutsideEveryJob()
    {
        FakeRuntimeWindowsProcessApi api;
        const auto process = api.CreatedProcess.Process;
        auto proof = ProveRuntimeChildOutsideJobs(process, api);
        VERIFY_IS_TRUE(proof.OutsideEveryJob());

        api.ChildProcessIsInJob = true;
        proof = ProveRuntimeChildOutsideJobs(process, api);
        VERIFY_IS_TRUE(proof.Status == RuntimeChildJobProofStatus::StillInJob);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_ACCESS_DENIED }, proof.SystemError);

        api.ChildJobQuerySucceeded = false;
        api.ChildJobQueryError = ERROR_INVALID_HANDLE;
        proof = ProveRuntimeChildOutsideJobs(process, api);
        VERIFY_IS_TRUE(proof.Status == RuntimeChildJobProofStatus::QueryFailed);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_INVALID_HANDLE }, proof.SystemError);
        VERIFY_ARE_EQUAL(size_t{ 3 }, api.ChildJobQueryHandles.size());
    }
}
