// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    };

    struct TaskAcceptanceProjection
    {
        std::wstring Text;
        bool Done{};
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
        std::vector<TaskAcceptanceProjection> Acceptance;
        std::vector<std::wstring> Runs;
        std::vector<std::wstring> QueuedMessages;
        std::vector<std::wstring> RunDetails;
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
    };
}
