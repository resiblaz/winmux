// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskStoreResults.h"

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeTaskRetryDecision
    {
        bool Retry{};
        uint32_t ExecutionNumber{};
        uint32_t MaximumExecutions{};
        uint32_t BackoffMilliseconds{};
    };

    [[nodiscard]] RuntimeTaskRetryDecision EvaluateRuntimeTaskAutomaticRetry(
        const Microsoft::Terminal::TaskCore::TaskRunRecord& failedRun,
        const std::vector<Microsoft::Terminal::TaskCore::TaskRunRecord>& runs) noexcept;
    [[nodiscard]] bool ShouldClaimRuntimeTaskMessages(
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate) noexcept;
    [[nodiscard]] std::optional<std::string> BuildRuntimeTaskPrompt(
        const Microsoft::Terminal::TaskCore::TaskAggregateSnapshot& aggregate,
        const Microsoft::Terminal::TaskCore::TaskRunRecord& run);
}
