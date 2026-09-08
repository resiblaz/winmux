// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <array>
#include <optional>

namespace winrt::TerminalApp::implementation::tasks
{
    enum class TaskStatus : uint8_t
    {
        Backlog,
        Todo,
        InProgress,
        InReview,
        Blocked,
        Done,
        Cancelled,
    };

    enum class TaskTerminalState : uint8_t
    {
        NotCreated,
        Starting,
        Ready,
        Busy,
        Exited,
        Suspended,
        Closed,
        Error,
    };

    enum class TaskPriority : uint8_t
    {
        Low,
        Medium,
        High,
    };

    enum class TaskCommand : uint8_t
    {
        Start,
        Stop,
        OpenTerminal,
        CloseTerminal,
        MarkDone,
        Reopen,
        Retry,
        Cancel,
        FinalizeWorktree,
        Delete,
        RestartProvider,
        ConfigureAgent,
        EditAcceptance,
    };

    struct TaskAcceptanceProjection
    {
        std::wstring Text;
        bool Done{};
        bool operator==(const TaskAcceptanceProjection&) const = default;
    };

    struct TaskProjection
    {
        std::wstring Id;
        std::wstring WorkspaceKey;
        std::wstring Workspace;
        std::wstring Title;
        std::wstring Description;
        TaskStatus Status{ TaskStatus::Todo };
        TaskTerminalState TerminalState{ TaskTerminalState::NotCreated };
        uint64_t TerminalGeneration{};
        TaskPriority Priority{ TaskPriority::Medium };
        std::wstring UpdatedLabel;
        std::wstring ExecutionMode;
        std::wstring WorkingDirectory;
        std::wstring Worktree;
        std::wstring Branch;
        std::wstring Result;
        std::wstring ActiveRunSummary;
        uint64_t Version{ 1 };
        uint64_t LastEventSequence{};
        int64_t UpdatedAtUnixMilliseconds{};
        bool HasActiveRun{};
        bool SupportsTerminalSurface{ true };
        bool HasGitWorktree{};
        std::wstring LatestTerminalRunId;
        std::vector<TaskAcceptanceProjection> Acceptance;
        std::vector<std::wstring> Runs;
        std::vector<std::wstring> QueuedMessages;
        std::vector<std::wstring> QueuedMessageIds;
        std::vector<uint64_t> QueuedMessageRevisions;
        std::vector<std::wstring> RunDetails;
        std::wstring AgentId;
        uint64_t AgentRevision{};
        std::wstring AgentName;
        std::wstring AgentSummary;
        std::wstring Model;
        std::wstring Instructions;
        bool IsSummary{};
        bool ManualHold{};
        bool HasPendingMessages{};
        bool HasPendingIntent{};
        bool ArchivedProject{};
        int64_t BoardRank{};
        uint64_t ColumnRevision{};
        std::wstring ActiveRunId;
        std::optional<uint8_t> LatestRunState;
        bool operator==(const TaskProjection&) const = default;
    };

    struct TaskListQuery
    {
        std::wstring ProjectId;
        std::wstring Search;
        std::array<bool, 7> Statuses{ true, true, true, true, true, false, false };
        bool Board{};
        bool operator==(const TaskListQuery&) const = default;
    };

    struct TaskDraft
    {
        std::wstring WorkspaceId;
        std::wstring Workspace;
        std::wstring Title;
        std::wstring Description;
        std::vector<std::wstring> Acceptance;
        std::wstring SourceRoot;
        std::wstring RelativeWorkingDirectory;
        std::wstring Model;
        std::wstring Instructions;
        bool UseWorktree{ true };
        std::wstring AgentId;
        uint64_t AgentRevision{};
        std::wstring AgentName;
        uint64_t ProjectVersion{};
    };

    struct TaskCreationAttempt
    {
        TaskDraft Draft;
        bool StartImmediately{};
        std::string TaskId;
        std::string CommandId;
        std::string StartCommandId;
    };

    struct TaskCreationResult
    {
        bool Created{};
        bool OutcomeUnknown{};
        TaskProjection Task;
        std::wstring Error;
    };

    enum class TaskSurfaceAction : uint8_t
    {
        OpenOrActivate,
        CloseAuthorized,
    };

    struct TaskSurfaceDescriptor
    {
        TaskSurfaceAction Action{ TaskSurfaceAction::OpenOrActivate };
        std::wstring TaskId;
        std::wstring WorkspaceId;
        std::wstring Title;
        std::string RuntimeIdentity;
        std::string RuntimeInstanceId;
        std::string ClientInstanceId;
        std::string TerminalSessionId;
        std::wstring PipeName;
        uint64_t Generation{};
        bool OwnedElsewhere{};
    };
}
