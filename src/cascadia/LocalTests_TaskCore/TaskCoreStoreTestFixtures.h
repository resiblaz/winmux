// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "SqliteTaskStore.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <winsqlite/winsqlite3.h>

namespace TaskCoreLocalTests::TestFixtures
{
    using namespace Microsoft::Terminal::TaskCore;

    class TemporaryDatabasePath final
    {
    public:
        TemporaryDatabasePath()
        {
            std::array<wchar_t, MAX_PATH> temporaryDirectory{};
            VERIFY_IS_GREATER_THAN(
                GetTempPathW(static_cast<DWORD>(temporaryDirectory.size()), temporaryDirectory.data()),
                DWORD{ 0 });
            std::array<wchar_t, MAX_PATH> temporaryFile{};
            VERIFY_IS_TRUE(GetTempFileNameW(temporaryDirectory.data(), L"tsk", 0, temporaryFile.data()) != 0);
            _path = temporaryFile.data();
            std::filesystem::remove(_path);
        }

        ~TemporaryDatabasePath()
        {
            std::error_code ignored;
            std::filesystem::remove(_path, ignored);
            std::filesystem::remove(_path.wstring() + L"-wal", ignored);
            std::filesystem::remove(_path.wstring() + L"-shm", ignored);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept
        {
            return _path;
        }

    private:
        std::filesystem::path _path;
    };

    template<typename TId>
    TId MakeId(const uint64_t value)
    {
        std::array<char, 37> buffer{};
        sprintf_s(
            buffer.data(),
            buffer.size(),
            "00000000-0000-4000-8000-%012llx",
            static_cast<unsigned long long>(value));
        return TId{ buffer.data() };
    }

    inline sqlite3* OpenRawDatabase(const std::filesystem::path& path)
    {
        const auto native = path.u8string();
        const std::string utf8{ reinterpret_cast<const char*>(native.data()), native.size() };
        sqlite3* database{};
        VERIFY_ARE_EQUAL(
            SQLITE_OK,
            sqlite3_open_v2(utf8.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr));
        return database;
    }

    inline TaskCommandMetadata MakeMetadata(
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp)
    {
        TaskCommandMetadata metadata;
        metadata.Command = MakeId<CommandId>(10000 + commandSeed);
        metadata.Event = MakeId<TaskEventId>(20000 + commandSeed);
        metadata.ExpectedTaskVersion = expectedVersion;
        metadata.RequestDigest = std::string(64, static_cast<char>('a' + commandSeed % 6));
        metadata.EventPayloadJson = "{\"source\":\"test\"}";
        metadata.ReceiptResultJson = "{\"accepted\":true}";
        metadata.TimestampUnixMilliseconds = timestamp;
        return metadata;
    }

    inline CreateTaskCommand MakeCreateTaskCommand(
        const uint64_t taskSeed = 1,
        const uint64_t workspaceSeed = 1,
        const uint64_t commandSeed = 1,
        const int64_t timestamp = 1000)
    {
        CreateTaskCommand command;
        command.Metadata = MakeMetadata(commandSeed, 0, timestamp);
        command.Task.Id = MakeId<TaskId>(taskSeed);
        command.Task.Workspace = MakeId<WorkspaceId>(100 + workspaceSeed);
        command.Task.Title = "Persist Task aggregate";
        command.Task.Description = "Exercise the transactional TaskStore boundary.";
        command.Task.AcceptanceCriteria = {
            { 0, "State is durable", false },
            { 1, "Events remain ordered", true },
        };
        command.Task.Status = TaskStatus::Todo;
        command.Task.Priority = TaskPriority::High;
        command.Task.Version = 1;
        command.Task.CreatedAtUnixMilliseconds = timestamp;
        command.Task.UpdatedAtUnixMilliseconds = timestamp;

        command.ExecutionContext.Id = MakeId<ExecutionContextId>(1000 + taskSeed);
        command.ExecutionContext.Task = command.Task.Id;
        command.ExecutionContext.Revision = 1;
        command.ExecutionContext.Mode = ExecutionMode::Direct;
        command.ExecutionContext.SourceRoot = "C:\\src\\repository";
        command.ExecutionContext.RuntimeRoot = "C:\\src\\repository";
        command.ExecutionContext.Provider = "codex";
        command.ExecutionContext.Model = "test-model";
        command.ExecutionContext.CreatedAtUnixMilliseconds = timestamp;

        command.Terminal.Id = MakeId<TaskTerminalId>(2000 + taskSeed);
        command.Terminal.Task = command.Task.Id;
        command.Terminal.State = TaskTerminalState::NotCreated;
        command.Terminal.Generation = 0;
        command.Terminal.UpdatedAtUnixMilliseconds = timestamp;
        return command;
    }

    inline ChangeTaskStatusCommand MakeStatusCommand(
        const CreateTaskCommand& task,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        const TaskStatus status,
        const TaskStatusAuthority authority = TaskStatusAuthority::User)
    {
        ChangeTaskStatusCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Task = task.Task.Id;
        command.RequestedStatus = status;
        command.Authority = authority;
        return command;
    }

    inline CreateTaskRunCommand MakeCreateRunCommand(
        const CreateTaskCommand& task,
        const uint64_t runSeed,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp)
    {
        CreateTaskRunCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Run.Id = MakeId<TaskRunId>(3000 + runSeed);
        command.Run.Task = task.Task.Id;
        command.Run.ExecutionContext = task.ExecutionContext.Id;
        command.Run.Trigger = TaskRunTrigger::Initial;
        command.Run.State = TaskRunState::Queued;
        command.Run.CreatedAtUnixMilliseconds = timestamp;
        return command;
    }

    inline ChangeTaskRunStateCommand MakeRunStateCommand(
        const CreateTaskRunCommand& run,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        const TaskRunState state,
        std::string result = {},
        std::string reasonCode = {})
    {
        ChangeTaskRunStateCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Task = run.Run.Task;
        command.Run = run.Run.Id;
        command.RequestedState = state;
        command.Result = std::move(result);
        command.ReasonCode = std::move(reasonCode);
        return command;
    }

    inline InitializeTaskGitWorktreeCommand MakeInitializeGitWorktreeCommand(
        const CreateTaskCommand& task,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp)
    {
        InitializeTaskGitWorktreeCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Worktree.Task = task.Task.Id;
        command.Worktree.ExecutionContext = task.ExecutionContext.Id;
        command.Worktree.Revision = 1;
        command.Worktree.State = TaskGitWorktreeState::Planning;
        command.Worktree.RepositoryRoot = task.ExecutionContext.SourceRoot;
        command.Worktree.RepositoryCommonDirectory =
            task.ExecutionContext.RepositoryCommonDirectory;
        command.Worktree.RepositoryIdentityDigest = std::string(64, 'a');
        command.Worktree.WorktreePath = task.ExecutionContext.WorktreePath;
        command.Worktree.BranchRef = task.ExecutionContext.Branch;
        command.Worktree.SourceHead = std::string(40, 'b');
        command.Worktree.SourceReference = "refs/heads/main";
        command.Worktree.OwnershipNonce = MakeId<CommandId>(9000 + commandSeed).Value();
        command.Worktree.CreatedAtUnixMilliseconds = timestamp;
        command.Worktree.UpdatedAtUnixMilliseconds = timestamp;
        return command;
    }

    inline AdvanceTaskGitWorktreeCommand MakeAdvanceGitWorktreeCommand(
        const CreateTaskCommand& task,
        const uint64_t commandSeed,
        const uint64_t expectedTaskVersion,
        const uint64_t expectedWorktreeRevision,
        const int64_t timestamp,
        const TaskGitWorktreeState state,
        std::string snapshotDigest = {},
        std::string baselineCommit = {},
        const bool hasUndeliveredWork = false,
        std::string preservedPath = {},
        std::string reasonCode = {})
    {
        AdvanceTaskGitWorktreeCommand command;
        command.Metadata = MakeMetadata(
            commandSeed,
            expectedTaskVersion,
            timestamp);
        command.Task = task.Task.Id;
        command.ExecutionContext = task.ExecutionContext.Id;
        command.ExpectedWorktreeRevision = expectedWorktreeRevision;
        command.RequestedState = state;
        command.SnapshotDigest = std::move(snapshotDigest);
        command.BaselineCommit = std::move(baselineCommit);
        command.HasUndeliveredWork = hasUndeliveredWork;
        command.PreservedPath = std::move(preservedPath);
        command.FailureReasonCode = std::move(reasonCode);
        return command;
    }

    inline BeginTaskRunGitCheckpointCommand MakeBeginGitCheckpointCommand(
        const CreateTaskRunCommand& run,
        const uint64_t checkpointSeed,
        const uint64_t commandSeed,
        const uint64_t expectedTaskVersion,
        const int64_t timestamp,
        std::string startCommit = std::string(40, 'd'))
    {
        BeginTaskRunGitCheckpointCommand command;
        command.Metadata = MakeMetadata(
            commandSeed,
            expectedTaskVersion,
            timestamp);
        command.Checkpoint.Id = MakeId<GitCheckpointId>(6000 + checkpointSeed);
        command.Checkpoint.Task = run.Run.Task;
        command.Checkpoint.Run = run.Run.Id;
        command.Checkpoint.Revision = 1;
        command.Checkpoint.State = TaskGitCheckpointState::Pending;
        command.Checkpoint.StartCommit = std::move(startCommit);
        command.Checkpoint.ChangedPathsJson = "[]";
        command.Checkpoint.DiffSummaryJson =
            "{\"startStateDigest\":\"" + std::string(64, 'e') + "\"}";
        command.Checkpoint.CreatedAtUnixMilliseconds = timestamp;
        command.Checkpoint.UpdatedAtUnixMilliseconds = timestamp;
        return command;
    }

    inline AdvanceTaskRunGitCheckpointCommand MakeAdvanceGitCheckpointCommand(
        const BeginTaskRunGitCheckpointCommand& begin,
        const uint64_t commandSeed,
        const uint64_t expectedTaskVersion,
        const uint64_t expectedCheckpointRevision,
        const uint64_t expectedWorktreeRevision,
        const int64_t timestamp,
        const TaskGitCheckpointState state)
    {
        AdvanceTaskRunGitCheckpointCommand command;
        command.Metadata = MakeMetadata(
            commandSeed,
            expectedTaskVersion,
            timestamp);
        command.Task = begin.Checkpoint.Task;
        command.Run = begin.Checkpoint.Run;
        command.Checkpoint = begin.Checkpoint.Id;
        command.ExpectedCheckpointRevision = expectedCheckpointRevision;
        command.ExpectedWorktreeRevision = expectedWorktreeRevision;
        command.RequestedState = state;
        command.DiffSummaryJson = begin.Checkpoint.DiffSummaryJson;
        return command;
    }

    inline ApplyTaskAgentMutationCommand MakeAgentMutationCommand(
        const CreateTaskRunCommand& run,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        const TaskAgentMutationKind kind,
        std::string text = {})
    {
        ApplyTaskAgentMutationCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Task = run.Run.Task;
        command.Run = run.Run.Id;
        command.Kind = kind;
        command.Text = std::move(text);
        return command;
    }

    inline ChangeTaskTerminalStateCommand MakeTerminalStateCommand(
        const CreateTaskCommand& task,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        const TaskTerminalState state,
        const uint64_t generation,
        const bool explicitOpen = false)
    {
        ChangeTaskTerminalStateCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Task = task.Task.Id;
        command.RequestedState = state;
        command.RequestedGeneration = generation;
        command.ExplicitOpen = explicitOpen;
        return command;
    }

    inline StartTaskIntentCommand MakeStartIntentCommand(
        const CreateTaskCommand& task,
        const uint64_t intentSeed,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        const TaskIntentKind kind)
    {
        StartTaskIntentCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Intent.Id = MakeId<TaskIntentId>(4000 + intentSeed);
        command.Intent.Task = task.Task.Id;
        command.Intent.Kind = kind;
        command.Intent.Stage = TaskIntentStage::Requested;
        command.Intent.Revision = 1;
        command.Intent.DetailJson = "{\"phase\":\"requested\"}";
        command.Intent.CreatedAtUnixMilliseconds = timestamp;
        command.Intent.UpdatedAtUnixMilliseconds = timestamp;
        return command;
    }

    inline AdvanceTaskIntentCommand MakeAdvanceIntentCommand(
        const StartTaskIntentCommand& start,
        const uint64_t commandSeed,
        const uint64_t expectedTaskVersion,
        const uint64_t expectedIntentRevision,
        const int64_t timestamp,
        const TaskIntentStage stage,
        std::string reasonCode = {})
    {
        AdvanceTaskIntentCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedTaskVersion, timestamp);
        command.Task = start.Intent.Task;
        command.Intent = start.Intent.Id;
        command.ExpectedIntentRevision = expectedIntentRevision;
        command.RequestedStage = stage;
        command.DetailJson = "{\"phase\":\"advanced\"}";
        command.ReasonCode = std::move(reasonCode);
        return command;
    }

    inline ChangeTaskManualHoldCommand MakeManualHoldCommand(
        const CreateTaskCommand& task,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        const bool hold)
    {
        ChangeTaskManualHoldCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Task = task.Task.Id;
        command.RequestedHold = hold;
        return command;
    }

    inline DeleteTaskCommand MakeDeleteTaskCommand(
        const StartTaskIntentCommand& intent,
        const uint64_t commandSeed,
        const uint64_t expectedTaskVersion,
        const uint64_t expectedIntentRevision,
        const int64_t timestamp)
    {
        DeleteTaskCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedTaskVersion, timestamp);
        command.Task = intent.Intent.Task;
        command.Intent = intent.Intent.Id;
        command.ExpectedIntentRevision = expectedIntentRevision;
        return command;
    }

    inline EnqueueTaskMessageCommand MakeEnqueueMessageCommand(
        const CreateTaskCommand& task,
        const uint64_t messageSeed,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp,
        std::string content)
    {
        EnqueueTaskMessageCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedVersion, timestamp);
        command.Message.Id = MakeId<TaskMessageId>(5000 + messageSeed);
        command.Message.Task = task.Task.Id;
        command.Message.Author = TaskMessageAuthor::User;
        command.Message.State = TaskMessageState::Pending;
        command.Message.Revision = 1;
        command.Message.Content = std::move(content);
        command.Message.CreatedAtUnixMilliseconds = timestamp;
        command.Message.UpdatedAtUnixMilliseconds = timestamp;
        return command;
    }

    inline ReviseTaskMessageCommand MakeReviseMessageCommand(
        const CreateTaskCommand& task,
        const TaskMessageId& message,
        const uint64_t commandSeed,
        const uint64_t expectedTaskVersion,
        const uint64_t expectedMessageRevision,
        const int64_t timestamp,
        std::optional<std::string> replacement,
        const bool remove = false)
    {
        ReviseTaskMessageCommand command;
        command.Metadata = MakeMetadata(commandSeed, expectedTaskVersion, timestamp);
        command.Task = task.Task.Id;
        command.Message = message;
        command.ExpectedMessageRevision = expectedMessageRevision;
        command.ReplacementContent = std::move(replacement);
        command.Remove = remove;
        return command;
    }

    inline ClaimTaskMessagesAndCreateRunCommand MakeClaimMessagesCommand(
        const CreateTaskCommand& task,
        const uint64_t runSeed,
        const uint64_t commandSeed,
        const uint64_t expectedVersion,
        const int64_t timestamp)
    {
        const auto createRun = MakeCreateRunCommand(
            task,
            runSeed,
            commandSeed,
            expectedVersion,
            timestamp);
        ClaimTaskMessagesAndCreateRunCommand command;
        command.Metadata = createRun.Metadata;
        command.Run = createRun.Run;
        command.Run.Trigger = TaskRunTrigger::FollowUp;
        return command;
    }
}
