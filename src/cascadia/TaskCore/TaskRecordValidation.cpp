// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskRecordValidation.h"

#include <array>

namespace Microsoft::Terminal::TaskCore
{
    namespace
    {
        void _requireId(RecordValidationResult& result, const bool valid, const char* field)
        {
            if (!valid)
            {
                result.push_back({ RecordValidationError::InvalidIdentifier, field });
            }
        }

        void _requireText(RecordValidationResult& result,
                          const std::string_view value,
                          const size_t maximumBytes,
                          const char* field)
        {
            if (value.empty())
            {
                result.push_back({ RecordValidationError::MissingRequiredValue, field });
            }
            else if (value.size() > maximumBytes)
            {
                result.push_back({ RecordValidationError::ValueTooLarge, field });
            }
        }

        bool _lowerHex(const std::string_view value, const size_t length) noexcept
        {
            return value.size() == length &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        bool _gitObjectId(const std::string_view value) noexcept
        {
            return _lowerHex(value, 40) || _lowerHex(value, 64);
        }

        void _requireBoundedOptionalText(
            RecordValidationResult& result,
            const std::string_view value,
            const size_t maximumBytes,
            const char* field)
        {
            if (value.size() > maximumBytes)
            {
                result.push_back({ RecordValidationError::ValueTooLarge, field });
            }
        }
    }

    RecordValidationResult ValidateTaskRecord(const TaskRecord& task)
    {
        RecordValidationResult result;
        _requireId(result, IsCanonicalUuid(task.Id.Value()), "task.id");
        _requireId(result, IsCanonicalUuid(task.Workspace.Value()), "task.workspace_id");
        if (task.ForkedFromTaskId)
        {
            _requireId(result, IsCanonicalUuid(task.ForkedFromTaskId->Value()), "task.forked_from_task_id");
        }
        _requireText(result, task.Title, MaximumTaskTitleBytes, "task.title");
        if (task.Description.size() > MaximumTaskDescriptionBytes)
        {
            result.push_back({ RecordValidationError::ValueTooLarge, "task.description" });
        }
        if (task.Version == 0)
        {
            result.push_back({ RecordValidationError::InvalidVersion, "task.version" });
        }
        if (task.CreatedAtUnixMilliseconds < 0 || task.UpdatedAtUnixMilliseconds < task.CreatedAtUnixMilliseconds)
        {
            result.push_back({ RecordValidationError::InvalidTimestamp, "task.timestamps" });
        }
        if (task.AcceptanceCriteria.size() > MaximumAcceptanceCriteria)
        {
            result.push_back({ RecordValidationError::InvalidCollection, "task.acceptance_criteria" });
        }
        for (size_t index = 0; index < task.AcceptanceCriteria.size(); ++index)
        {
            const auto& criterion = task.AcceptanceCriteria[index];
            if (criterion.Position != index || criterion.Text.empty())
            {
                result.push_back({ RecordValidationError::InvalidCollection, "task.acceptance_criteria" });
                break;
            }
            if (criterion.Text.size() > MaximumAcceptanceCriterionBytes)
            {
                result.push_back({ RecordValidationError::ValueTooLarge, "task.acceptance_criteria.text" });
                break;
            }
        }
        return result;
    }

    RecordValidationResult ValidateExecutionContextRecord(const ExecutionContextRecord& context)
    {
        RecordValidationResult result;
        _requireId(result, IsCanonicalUuid(context.Id.Value()), "execution_context.id");
        _requireId(result, IsCanonicalUuid(context.Task.Value()), "execution_context.task_id");
        if (context.Revision == 0)
        {
            result.push_back({ RecordValidationError::InvalidVersion, "execution_context.revision" });
        }
        _requireText(result, context.SourceRoot, 32 * 1024, "execution_context.source_root");
        _requireText(result, context.RuntimeRoot, 32 * 1024, "execution_context.runtime_root");
        _requireText(result, context.Provider, MaximumProviderIdentifierBytes, "execution_context.provider" );
        if (context.CreatedAtUnixMilliseconds < 0)
        {
            result.push_back({ RecordValidationError::InvalidTimestamp, "execution_context.created_at" });
        }

        switch (context.Mode)
        {
        case ExecutionMode::Direct:
            if (!context.WorktreePath.empty())
            {
                result.push_back({ RecordValidationError::InvalidExecutionMode, "execution_context.worktree_path" });
            }
            break;
        case ExecutionMode::Worktree:
            _requireText(result, context.RepositoryCommonDirectory, 32 * 1024, "execution_context.repository_common_directory");
            _requireText(result, context.WorktreePath, 32 * 1024, "execution_context.worktree_path");
            _requireText(result, context.Branch, 4 * 1024, "execution_context.branch");
            break;
        default:
            result.push_back({ RecordValidationError::InvalidExecutionMode, "execution_context.mode" });
            break;
        }
        return result;
    }

    RecordValidationResult ValidateTaskGitWorktreeRecord(
        const TaskGitWorktreeRecord& worktree)
    {
        RecordValidationResult result;
        _requireId(result, IsCanonicalUuid(worktree.Task.Value()), "git_worktree.task_id");
        _requireId(
            result,
            IsCanonicalUuid(worktree.ExecutionContext.Value()),
            "git_worktree.execution_context_id");
        if (worktree.Revision == 0)
        {
            result.push_back({ RecordValidationError::InvalidVersion, "git_worktree.revision" });
        }
        if (worktree.State > TaskGitWorktreeState::Failed)
        {
            result.push_back({ RecordValidationError::InvalidState, "git_worktree.state" });
        }
        _requireText(result, worktree.RepositoryRoot, MaximumTaskPathBytes, "git_worktree.repository_root");
        _requireText(result, worktree.RepositoryCommonDirectory, MaximumTaskPathBytes, "git_worktree.repository_common_directory");
        _requireText(result, worktree.WorktreePath, MaximumTaskPathBytes, "git_worktree.worktree_path");
        _requireText(result, worktree.BranchRef, MaximumTaskGitReferenceBytes, "git_worktree.branch_ref");
        if (!worktree.BranchRef.starts_with("refs/heads/wt-task/"))
        {
            result.push_back({ RecordValidationError::InvalidIdentifier, "git_worktree.branch_ref" });
        }
        if (!_lowerHex(worktree.RepositoryIdentityDigest, 64))
        {
            result.push_back({ RecordValidationError::InvalidDigest, "git_worktree.repository_identity_digest" });
        }
        if (!_gitObjectId(worktree.SourceHead))
        {
            result.push_back({ RecordValidationError::InvalidDigest, "git_worktree.source_head" });
        }
        _requireBoundedOptionalText(result, worktree.SourceReference, MaximumTaskGitReferenceBytes, "git_worktree.source_reference");
        if (!worktree.SnapshotDigest.empty() && !_lowerHex(worktree.SnapshotDigest, 64))
        {
            result.push_back({ RecordValidationError::InvalidDigest, "git_worktree.snapshot_digest" });
        }
        if (!worktree.BaselineCommit.empty() && !_gitObjectId(worktree.BaselineCommit))
        {
            result.push_back({ RecordValidationError::InvalidDigest, "git_worktree.baseline_commit" });
        }
        _requireId(
            result,
            IsCanonicalUuid(worktree.OwnershipNonce),
            "git_worktree.ownership_nonce");
        _requireBoundedOptionalText(result, worktree.PreservedPath, MaximumTaskPathBytes, "git_worktree.preserved_path");
        _requireBoundedOptionalText(result, worktree.FailureReasonCode, MaximumTaskGitReasonCodeBytes, "git_worktree.failure_reason_code");
        if (worktree.CreatedAtUnixMilliseconds < 0 ||
            worktree.UpdatedAtUnixMilliseconds < worktree.CreatedAtUnixMilliseconds)
        {
            result.push_back({ RecordValidationError::InvalidTimestamp, "git_worktree.timestamps" });
        }
        if (worktree.State == TaskGitWorktreeState::Ready &&
            (worktree.SnapshotDigest.empty() || worktree.BaselineCommit.empty()))
        {
            result.push_back({ RecordValidationError::MissingRequiredValue, "git_worktree.ready_proof" });
        }
        if (worktree.State == TaskGitWorktreeState::Preserved &&
            (!worktree.HasUndeliveredWork || worktree.PreservedPath.empty()))
        {
            result.push_back({ RecordValidationError::InvalidState, "git_worktree.preserved_state" });
        }
        if (worktree.State == TaskGitWorktreeState::Finalized &&
            (worktree.HasUndeliveredWork || !worktree.CleanupEligible))
        {
            result.push_back({ RecordValidationError::InvalidState, "git_worktree.finalized_state" });
        }
        return result;
    }

    RecordValidationResult ValidateTaskRunGitCheckpointRecord(
        const TaskRunGitCheckpointRecord& checkpoint)
    {
        RecordValidationResult result;
        _requireId(result, IsCanonicalUuid(checkpoint.Id.Value()), "git_checkpoint.id");
        _requireId(result, IsCanonicalUuid(checkpoint.Task.Value()), "git_checkpoint.task_id");
        _requireId(result, IsCanonicalUuid(checkpoint.Run.Value()), "git_checkpoint.run_id");
        if (checkpoint.Revision == 0)
        {
            result.push_back({ RecordValidationError::InvalidVersion, "git_checkpoint.revision" });
        }
        if (checkpoint.State > TaskGitCheckpointState::Failed)
        {
            result.push_back({ RecordValidationError::InvalidState, "git_checkpoint.state" });
        }
        for (const auto& [value, field] : std::array{
                 std::pair{ std::string_view{ checkpoint.StartCommit }, "git_checkpoint.start_commit" },
                 std::pair{ std::string_view{ checkpoint.EndCommit }, "git_checkpoint.end_commit" },
                 std::pair{ std::string_view{ checkpoint.CheckpointCommit }, "git_checkpoint.checkpoint_commit" },
             })
        {
            if (!value.empty() && !_gitObjectId(value))
            {
                result.push_back({ RecordValidationError::InvalidDigest, field });
            }
        }
        _requireBoundedOptionalText(result, checkpoint.ChangedPathsJson, MaximumTaskGitJsonBytes, "git_checkpoint.changed_paths_json");
        _requireBoundedOptionalText(result, checkpoint.DiffSummaryJson, MaximumTaskGitJsonBytes, "git_checkpoint.diff_summary_json");
        _requireBoundedOptionalText(result, checkpoint.PreservedPath, MaximumTaskPathBytes, "git_checkpoint.preserved_path");
        _requireBoundedOptionalText(result, checkpoint.ReasonCode, MaximumTaskGitReasonCodeBytes, "git_checkpoint.reason_code");
        if (checkpoint.CreatedAtUnixMilliseconds < 0 ||
            checkpoint.UpdatedAtUnixMilliseconds < checkpoint.CreatedAtUnixMilliseconds)
        {
            result.push_back({ RecordValidationError::InvalidTimestamp, "git_checkpoint.timestamps" });
        }
        if (checkpoint.State == TaskGitCheckpointState::Succeeded &&
            checkpoint.EndCommit.empty())
        {
            result.push_back({ RecordValidationError::MissingRequiredValue, "git_checkpoint.end_commit" });
        }
        if (checkpoint.State == TaskGitCheckpointState::Failed &&
            checkpoint.ReasonCode.empty())
        {
            result.push_back({ RecordValidationError::MissingRequiredValue, "git_checkpoint.reason_code" });
        }
        return result;
    }
}
