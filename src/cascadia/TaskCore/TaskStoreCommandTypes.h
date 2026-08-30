// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskRecords.h"
#include "TaskStatusPolicy.h"

#include <cstdint>
#include <string>

namespace Microsoft::Terminal::TaskCore
{
    inline constexpr size_t TaskStoreDigestCharacters{ 64 };
    inline constexpr size_t MaximumTaskStoreJsonBytes{ 64 * 1024 };
    inline constexpr size_t MaximumTaskRunResultBytes{ 256 * 1024 };
    inline constexpr size_t MaximumTaskReasonCodeBytes{ 256 };

    struct TaskCommandMetadata
    {
        CommandId Command;
        TaskEventId Event;
        uint64_t ExpectedTaskVersion{};
        std::string RequestDigest;
        std::string EventPayloadJson{ "{}" };
        std::string ReceiptResultJson{ "{}" };
        int64_t TimestampUnixMilliseconds{};
    };

    struct CreateTaskCommand
    {
        TaskCommandMetadata Metadata;
        TaskRecord Task;
        ExecutionContextRecord ExecutionContext;
        TaskTerminalRecord Terminal;
    };

    struct ChangeTaskStatusCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskStatus RequestedStatus{ TaskStatus::Todo };
        TaskStatusAuthority Authority{ TaskStatusAuthority::User };
    };

    struct CreateTaskRunCommand
    {
        TaskCommandMetadata Metadata;
        TaskRunRecord Run;
    };

    struct ChangeTaskRunStateCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskRunId Run;
        TaskRunState RequestedState{ TaskRunState::Queued };
        std::string Result;
        std::string ReasonCode;
    };

    struct ChangeTaskTerminalStateCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskTerminalState RequestedState{ TaskTerminalState::NotCreated };
        uint64_t RequestedGeneration{};
        bool ExplicitOpen{};
    };

    struct StartTaskIntentCommand
    {
        TaskCommandMetadata Metadata;
        TaskIntentRecord Intent;
    };

    struct AdvanceTaskIntentCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskIntentId Intent;
        uint64_t ExpectedIntentRevision{};
        TaskIntentStage RequestedStage{ TaskIntentStage::Requested };
        std::optional<int64_t> DeadlineAtUnixMilliseconds;
        std::string DetailJson{ "{}" };
        std::string ReasonCode;
    };

    struct ChangeTaskManualHoldCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        bool RequestedHold{};
    };

    struct DeleteTaskCommand
    {
        TaskCommandMetadata Metadata;
        TaskId Task;
        TaskIntentId Intent;
        uint64_t ExpectedIntentRevision{};
    };
}
