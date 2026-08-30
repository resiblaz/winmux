// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeProcessLaunchTypes.h"
#include "RuntimeWindowsProcessApiInternal.h"

#include <string>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeParentJobPlanStatus
    {
        Ready,
        QueryFailed,
        BreakawayDenied,
    };

    struct RuntimeParentJobPlan
    {
        RuntimeParentJobPlanStatus Status{ RuntimeParentJobPlanStatus::QueryFailed };
        RuntimeParentJobStrategy Strategy{ RuntimeParentJobStrategy::NotInJob };
        DWORD AdditionalCreationFlags{};
        DWORD SystemError{};
        std::string Error;

        [[nodiscard]] bool Ready() const noexcept
        {
            return Status == RuntimeParentJobPlanStatus::Ready;
        }
    };

    enum class RuntimeChildJobProofStatus
    {
        OutsideEveryJob,
        QueryFailed,
        StillInJob,
    };

    struct RuntimeChildJobProof
    {
        RuntimeChildJobProofStatus Status{ RuntimeChildJobProofStatus::QueryFailed };
        DWORD SystemError{};

        [[nodiscard]] bool OutsideEveryJob() const noexcept
        {
            return Status == RuntimeChildJobProofStatus::OutsideEveryJob;
        }
    };

    [[nodiscard]] RuntimeParentJobPlan PlanRuntimeParentJob(
        details::IRuntimeWindowsProcessApi& api) noexcept;
    [[nodiscard]] RuntimeChildJobProof ProveRuntimeChildOutsideJobs(
        HANDLE process,
        details::IRuntimeWindowsProcessApi& api) noexcept;
}
