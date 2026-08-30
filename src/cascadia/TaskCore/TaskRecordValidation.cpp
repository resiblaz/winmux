// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskRecordValidation.h"

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
}
