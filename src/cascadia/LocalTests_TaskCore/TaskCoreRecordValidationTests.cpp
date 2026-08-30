// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRecordValidation.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;

namespace TaskCoreLocalTests
{
    namespace
    {
        TaskRecord _validTask()
        {
            TaskRecord task;
            task.Id = TaskId{ "12345678-1234-4abc-8def-1234567890ab" };
            task.Workspace = WorkspaceId{ "22345678-1234-4abc-8def-1234567890ab" };
            task.Title = "Implement TaskCore";
            task.Description = "Create the persistent task domain.";
            task.AcceptanceCriteria = {
                { 0, "Strong identifiers", true },
                { 1, "Validated records", false },
            };
            task.CreatedAtUnixMilliseconds = 100;
            task.UpdatedAtUnixMilliseconds = 200;
            return task;
        }

        ExecutionContextRecord _validWorktreeContext()
        {
            ExecutionContextRecord context;
            context.Id = ExecutionContextId{ "32345678-1234-4abc-8def-1234567890ab" };
            context.Task = TaskId{ "12345678-1234-4abc-8def-1234567890ab" };
            context.SourceRoot = R"(F:\repo)";
            context.RuntimeRoot = R"(F:\task-worktrees\task-1)";
            context.RepositoryCommonDirectory = R"(F:\repo\.git)";
            context.WorktreePath = R"(F:\task-worktrees\task-1)";
            context.Branch = "agent/codex/task-1";
            context.Provider = "codex";
            context.CreatedAtUnixMilliseconds = 100;
            return context;
        }

        bool _hasIssue(const RecordValidationResult& result, const RecordValidationError error, const std::string_view field)
        {
            return std::ranges::any_of(result, [&](const auto& issue) {
                return issue.Error == error && issue.Field == field;
            });
        }
    }

    class TaskCoreRecordValidationTests
    {
        TEST_CLASS(TaskCoreRecordValidationTests);

        TEST_METHOD(AcceptsValidTaskAndExecutionContext);
        TEST_METHOD(RejectsInvalidTaskIdentityBoundsAndOrdering);
        TEST_METHOD(RejectsExecutionModeContradictions);
    };

    void TaskCoreRecordValidationTests::AcceptsValidTaskAndExecutionContext()
    {
        VERIFY_IS_TRUE(ValidateTaskRecord(_validTask()).empty());
        VERIFY_IS_TRUE(ValidateExecutionContextRecord(_validWorktreeContext()).empty());
    }

    void TaskCoreRecordValidationTests::RejectsInvalidTaskIdentityBoundsAndOrdering()
    {
        auto task = _validTask();
        task.Id = TaskId{ "invalid" };
        task.Title.assign(MaximumTaskTitleBytes + 1, 'x');
        task.Version = 0;
        task.UpdatedAtUnixMilliseconds = 99;
        task.AcceptanceCriteria[1].Position = 4;

        const auto result = ValidateTaskRecord(task);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidIdentifier, "task.id"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::ValueTooLarge, "task.title"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidVersion, "task.version"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidTimestamp, "task.timestamps"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidCollection, "task.acceptance_criteria"));
    }

    void TaskCoreRecordValidationTests::RejectsExecutionModeContradictions()
    {
        auto direct = _validWorktreeContext();
        direct.Mode = ExecutionMode::Direct;
        const auto directResult = ValidateExecutionContextRecord(direct);
        VERIFY_IS_TRUE(_hasIssue(directResult, RecordValidationError::InvalidExecutionMode, "execution_context.worktree_path"));

        auto worktree = _validWorktreeContext();
        worktree.WorktreePath.clear();
        const auto worktreeResult = ValidateExecutionContextRecord(worktree);
        VERIFY_IS_TRUE(_hasIssue(worktreeResult, RecordValidationError::MissingRequiredValue, "execution_context.worktree_path"));
    }
}
