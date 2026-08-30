// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>

namespace Microsoft::Terminal::TaskCore
{
    enum class TaskStatus : uint8_t
    {
        Backlog = 0,
        Todo = 1,
        InProgress = 2,
        InReview = 3,
        Blocked = 4,
        Done = 5,
        Cancelled = 6,
    };

    enum class TaskPriority : uint8_t
    {
        Low = 0,
        Medium = 1,
        High = 2,
    };

    enum class TaskRunState : uint8_t
    {
        Queued = 0,
        Preparing = 1,
        WaitingForDirectory = 2,
        Starting = 3,
        Running = 4,
        Completed = 5,
        Failed = 6,
        Cancelled = 7,
        Interrupted = 8,
    };

    enum class TaskRunTrigger : uint8_t
    {
        Initial = 0,
        ManualRetry = 1,
        AutomaticRetry = 2,
        FollowUp = 3,
    };

    enum class TaskTerminalState : uint8_t
    {
        NotCreated = 0,
        Starting = 1,
        Ready = 2,
        Busy = 3,
        Exited = 4,
        Suspended = 5,
        Error = 6,
        Closed = 7,
    };

    enum class ExecutionMode : uint8_t
    {
        Direct = 0,
        Worktree = 1,
    };

    enum class AgentSessionState : uint8_t
    {
        Available = 0,
        Suspended = 1,
        Poisoned = 2,
        Closed = 3,
    };

    enum class TaskMessageState : uint8_t
    {
        Pending = 0,
        Deferred = 1,
        Claimed = 2,
        Removed = 3,
    };

    enum class TaskIntentKind : uint8_t
    {
        Stop = 0,
        Close = 1,
        WorktreeCleanup = 2,
        Delete = 3,
    };

    enum class TaskIntentStage : uint8_t
    {
        Requested = 0,
        Interrupting = 1,
        WaitingForProvider = 2,
        Checkpointing = 3,
        FinalizingWorktree = 4,
        RemovingSurface = 5,
        DeletingAggregate = 6,
        Failed = 7,
        Completed = 8,
    };
}
