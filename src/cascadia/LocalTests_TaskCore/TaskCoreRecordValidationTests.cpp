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

        TaskGitWorktreeRecord _validGitWorktree()
        {
            TaskGitWorktreeRecord worktree;
            worktree.Task = TaskId{ "12345678-1234-4abc-8def-1234567890ab" };
            worktree.ExecutionContext = ExecutionContextId{ "32345678-1234-4abc-8def-1234567890ab" };
            worktree.RepositoryRoot = R"(F:\repo)";
            worktree.RepositoryCommonDirectory = R"(F:\repo\.git)";
            worktree.RepositoryIdentityDigest.assign(64, 'a');
            worktree.WorktreePath = R"(F:\task-worktrees\task-1)";
            worktree.BranchRef = "refs/heads/wt-task/12345678-1234-4abc-8def-1234567890ab";
            worktree.SourceHead.assign(40, 'b');
            worktree.SourceReference = "refs/heads/main";
            worktree.OwnershipNonce = "42345678-1234-4abc-8def-1234567890ab";
            worktree.CreatedAtUnixMilliseconds = 100;
            worktree.UpdatedAtUnixMilliseconds = 100;
            return worktree;
        }

        TaskRunGitCheckpointRecord _validGitCheckpoint()
        {
            TaskRunGitCheckpointRecord checkpoint;
            checkpoint.Id = GitCheckpointId{ "52345678-1234-4abc-8def-1234567890ab" };
            checkpoint.Task = TaskId{ "12345678-1234-4abc-8def-1234567890ab" };
            checkpoint.Run = TaskRunId{ "62345678-1234-4abc-8def-1234567890ab" };
            checkpoint.StartCommit.assign(40, 'b');
            checkpoint.CreatedAtUnixMilliseconds = 100;
            checkpoint.UpdatedAtUnixMilliseconds = 100;
            return checkpoint;
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
        TEST_METHOD(ValidatesGitWorktreeProofAndStateInvariants);
        TEST_METHOD(ValidatesGitCheckpointBoundsAndTerminalEvidence);
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

    void TaskCoreRecordValidationTests::ValidatesGitWorktreeProofAndStateInvariants()
    {
        VERIFY_IS_TRUE(ValidateTaskGitWorktreeRecord(_validGitWorktree()).empty());

        auto invalid = _validGitWorktree();
        invalid.RepositoryIdentityDigest.assign(64, 'A');
        invalid.SourceHead = "not-an-object";
        invalid.BranchRef = "refs/heads/main";
        invalid.OwnershipNonce = "invalid";
        invalid.RepositoryRoot.assign(MaximumTaskPathBytes + 1, 'x');
        invalid.State = static_cast<TaskGitWorktreeState>(255);
        auto result = ValidateTaskGitWorktreeRecord(invalid);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidDigest, "git_worktree.repository_identity_digest"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidDigest, "git_worktree.source_head"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidIdentifier, "git_worktree.branch_ref"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidIdentifier, "git_worktree.ownership_nonce"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::ValueTooLarge, "git_worktree.repository_root"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidState, "git_worktree.state"));

        auto ready = _validGitWorktree();
        ready.State = TaskGitWorktreeState::Ready;
        result = ValidateTaskGitWorktreeRecord(ready);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::MissingRequiredValue, "git_worktree.ready_proof"));

        auto preserved = _validGitWorktree();
        preserved.State = TaskGitWorktreeState::Preserved;
        result = ValidateTaskGitWorktreeRecord(preserved);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidState, "git_worktree.preserved_state"));

        auto finalized = _validGitWorktree();
        finalized.State = TaskGitWorktreeState::Finalized;
        finalized.HasUndeliveredWork = true;
        result = ValidateTaskGitWorktreeRecord(finalized);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidState, "git_worktree.finalized_state"));

        finalized.HasUndeliveredWork = false;
        finalized.CleanupEligible = true;
        VERIFY_IS_TRUE(ValidateTaskGitWorktreeRecord(finalized).empty());
    }

    void TaskCoreRecordValidationTests::ValidatesGitCheckpointBoundsAndTerminalEvidence()
    {
        VERIFY_IS_TRUE(ValidateTaskRunGitCheckpointRecord(_validGitCheckpoint()).empty());

        auto invalid = _validGitCheckpoint();
        invalid.Id = GitCheckpointId{ "invalid" };
        invalid.State = static_cast<TaskGitCheckpointState>(255);
        invalid.StartCommit.assign(40, 'A');
        invalid.ChangedPathsJson.assign(MaximumTaskGitJsonBytes + 1, 'x');
        invalid.UpdatedAtUnixMilliseconds = 99;
        auto result = ValidateTaskRunGitCheckpointRecord(invalid);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidIdentifier, "git_checkpoint.id"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidState, "git_checkpoint.state"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidDigest, "git_checkpoint.start_commit"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::ValueTooLarge, "git_checkpoint.changed_paths_json"));
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::InvalidTimestamp, "git_checkpoint.timestamps"));

        auto succeeded = _validGitCheckpoint();
        succeeded.State = TaskGitCheckpointState::Succeeded;
        result = ValidateTaskRunGitCheckpointRecord(succeeded);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::MissingRequiredValue, "git_checkpoint.end_commit"));

        auto failed = _validGitCheckpoint();
        failed.State = TaskGitCheckpointState::Failed;
        result = ValidateTaskRunGitCheckpointRecord(failed);
        VERIFY_IS_TRUE(_hasIssue(result, RecordValidationError::MissingRequiredValue, "git_checkpoint.reason_code"));

        failed.ReasonCode = "git-command-failed";
        VERIFY_IS_TRUE(ValidateTaskRunGitCheckpointRecord(failed).empty());
    }
}
