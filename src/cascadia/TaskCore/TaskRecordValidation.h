// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t MaximumTaskTitleBytes{ 1024 };
    inline constexpr size_t MaximumTaskDescriptionBytes{ 64 * 1024 };
    inline constexpr size_t MaximumAcceptanceCriterionBytes{ 8 * 1024 };
    inline constexpr size_t MaximumAcceptanceCriteria{ 256 };
    inline constexpr size_t MaximumProviderIdentifierBytes{ 256 };
    inline constexpr size_t MaximumTaskPathBytes{ 32 * 1024 };
    inline constexpr size_t MaximumTaskGitReferenceBytes{ 4 * 1024 };
    inline constexpr size_t MaximumTaskGitJsonBytes{ 64 * 1024 };
    inline constexpr size_t MaximumTaskGitReasonCodeBytes{ 256 };

    enum class RecordValidationError
    {
        InvalidIdentifier,
        MissingRequiredValue,
        ValueTooLarge,
        InvalidVersion,
        InvalidTimestamp,
        InvalidCollection,
        InvalidExecutionMode,
        InvalidDigest,
        InvalidState,
    };

    struct RecordValidationIssue
    {
        RecordValidationError Error{};
        std::string Field;
    };

    using RecordValidationResult = std::vector<RecordValidationIssue>;

    [[nodiscard]] RecordValidationResult ValidateTaskRecord(const TaskRecord& task);
    [[nodiscard]] RecordValidationResult ValidateExecutionContextRecord(const ExecutionContextRecord& context);
    [[nodiscard]] RecordValidationResult ValidateTaskGitWorktreeRecord(const TaskGitWorktreeRecord& worktree);
    [[nodiscard]] RecordValidationResult ValidateTaskRunGitCheckpointRecord(const TaskRunGitCheckpointRecord& checkpoint);
}
