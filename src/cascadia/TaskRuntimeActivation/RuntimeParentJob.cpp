// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeParentJob.h"

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeParentJobPlan PlanRuntimeParentJob(
        details::IRuntimeWindowsProcessApi& api) noexcept
    {
        bool inJob{};
        DWORD error{};
        if (!api.CurrentProcessInJob(inJob, error))
        {
            return {
                RuntimeParentJobPlanStatus::QueryFailed,
                RuntimeParentJobStrategy::NotInJob,
                0,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
                "Current process Job membership could not be queried",
            };
        }
        if (!inJob)
        {
            return {
                RuntimeParentJobPlanStatus::Ready,
                RuntimeParentJobStrategy::NotInJob,
                0,
                ERROR_SUCCESS,
                {},
            };
        }

        DWORD limitFlags{};
        if (!api.CurrentJobLimitFlags(limitFlags, error))
        {
            return {
                RuntimeParentJobPlanStatus::QueryFailed,
                RuntimeParentJobStrategy::NotInJob,
                0,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
                "Current process Job limits could not be queried",
            };
        }
        if ((limitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) != 0)
        {
            return {
                RuntimeParentJobPlanStatus::Ready,
                RuntimeParentJobStrategy::SilentBreakaway,
                0,
                ERROR_SUCCESS,
                {},
            };
        }
        if ((limitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) != 0)
        {
            return {
                RuntimeParentJobPlanStatus::Ready,
                RuntimeParentJobStrategy::ExplicitBreakaway,
                CREATE_BREAKAWAY_FROM_JOB,
                ERROR_SUCCESS,
                {},
            };
        }
        return {
            RuntimeParentJobPlanStatus::BreakawayDenied,
            RuntimeParentJobStrategy::NotInJob,
            0,
            ERROR_ACCESS_DENIED,
            "Current process Job does not permit Runtime breakaway",
        };
    }

    RuntimeChildJobProof ProveRuntimeChildOutsideJobs(
        const HANDLE process,
        details::IRuntimeWindowsProcessApi& api) noexcept
    {
        bool inJob{};
        DWORD error{};
        if (!api.ProcessInAnyJob(process, inJob, error))
        {
            return {
                RuntimeChildJobProofStatus::QueryFailed,
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error,
            };
        }
        return {
            inJob ? RuntimeChildJobProofStatus::StillInJob :
                    RuntimeChildJobProofStatus::OutsideEveryJob,
            inJob ? DWORD{ ERROR_ACCESS_DENIED } : DWORD{ ERROR_SUCCESS },
        };
    }
}
