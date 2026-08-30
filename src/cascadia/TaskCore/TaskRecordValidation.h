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

    enum class RecordValidationError
    {
        InvalidIdentifier,
        MissingRequiredValue,
        ValueTooLarge,
        InvalidVersion,
        InvalidTimestamp,
        InvalidCollection,
        InvalidExecutionMode,
    };

    struct RecordValidationIssue
    {
        RecordValidationError Error{};
        std::string Field;
    };

    using RecordValidationResult = std::vector<RecordValidationIssue>;

    [[nodiscard]] RecordValidationResult ValidateTaskRecord(const TaskRecord& task);
    [[nodiscard]] RecordValidationResult ValidateExecutionContextRecord(const ExecutionContextRecord& context);
}
