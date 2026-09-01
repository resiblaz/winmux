// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TaskStoreCommandValidation.h"

#include "TaskRecordValidation.h"
#include "TaskGitCheckpointStatePolicy.h"
#include "TaskGitWorktreeStatePolicy.h"
#include "TaskRunStatePolicy.h"
#include "TaskStoreResults.h"

#include <limits>
#include <cctype>
#include <type_traits>

namespace Microsoft::Terminal::TaskCore::details
{
    namespace
    {
        bool _isLowerHexDigest(const std::string_view value) noexcept
        {
            if (value.size() != TaskStoreDigestCharacters)
            {
                return false;
            }
            return std::ranges::all_of(value, [](const char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        }

        bool _isGitObjectId(const std::string_view value) noexcept
        {
            return (value.size() == 40 || value.size() == 64) &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        bool _validOptionalId(const auto& value) noexcept
        {
            return !value || IsCanonicalUuid(value->Value());
        }

        template<typename TEnum>
        bool _enumInRange(const TEnum value, const TEnum maximum) noexcept
        {
            return static_cast<std::underlying_type_t<TEnum>>(value) <=
                   static_cast<std::underlying_type_t<TEnum>>(maximum);
        }
    }

    bool ValidateCommandMetadata(const TaskCommandMetadata& metadata, std::string& error)
    {
        if (!IsCanonicalUuid(metadata.Command.Value()) || !IsCanonicalUuid(metadata.Event.Value()))
        {
            error = "Command and event identifiers must be canonical UUIDs";
            return false;
        }
        if (!_isLowerHexDigest(metadata.RequestDigest))
        {
            error = "Request digest must be 64 lowercase hexadecimal characters";
            return false;
        }
        if (metadata.EventPayloadJson.size() > MaximumTaskStoreJsonBytes || metadata.ReceiptResultJson.size() > MaximumTaskStoreJsonBytes)
        {
            error = "Command JSON payload exceeds its bound";
            return false;
        }
        if (metadata.TimestampUnixMilliseconds < 0)
        {
            error = "Command timestamp is invalid";
            return false;
        }
        if (metadata.ExpectedTaskVersion > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            error = "Expected Task version exceeds the storage range";
            return false;
        }
        return true;
    }

    bool ValidateCreateTaskCommand(const CreateTaskCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto taskIssues = ValidateTaskRecord(command.Task);
        const auto contextIssues = ValidateExecutionContextRecord(command.ExecutionContext);
        if (!taskIssues.empty() || !contextIssues.empty())
        {
            error = !taskIssues.empty() ? taskIssues.front().Field : contextIssues.front().Field;
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion != 0 || command.Task.Version != 1 ||
            command.Task.Status != TaskStatus::Todo ||
            !_enumInRange(command.Task.Priority, TaskPriority::High) ||
            (command.Task.ForkedFromTaskId && command.Task.ForkedFromTaskId.value() == command.Task.Id) ||
            command.ExecutionContext.Task != command.Task.Id || command.ExecutionContext.Revision != 1 ||
            command.ExecutionContext.IdentityFrozen ||
            command.Terminal.Task != command.Task.Id || !IsCanonicalUuid(command.Terminal.Id.Value()) ||
            command.Terminal.State != TaskTerminalState::NotCreated || command.Terminal.Generation != 0 ||
            command.Terminal.CurrentAgentSessionId || command.Terminal.UpdatedAtUnixMilliseconds < 0 ||
            command.ExecutionContext.CreatedAtUnixMilliseconds < command.Task.CreatedAtUnixMilliseconds ||
            command.Terminal.UpdatedAtUnixMilliseconds < command.Task.CreatedAtUnixMilliseconds ||
            command.Task.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds ||
            command.ExecutionContext.CreatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds ||
            command.Terminal.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "Create Task aggregate identity or initial state is inconsistent";
            return false;
        }
        return true;
    }

    bool ValidateChangeTaskStatusCommand(const ChangeTaskStatusCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 || !IsCanonicalUuid(command.Task.Value()) ||
            !_enumInRange(command.RequestedStatus, TaskStatus::Cancelled) ||
            command.Authority < TaskStatusAuthority::User || command.Authority > TaskStatusAuthority::SystemRecovery)
        {
            error = "Task status transition command is invalid";
            return false;
        }
        return true;
    }

    bool ValidateCreateTaskRunCommand(const CreateTaskRunCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto& run = command.Run;
        const auto retryTrigger = run.Trigger == TaskRunTrigger::ManualRetry ||
                                  run.Trigger == TaskRunTrigger::AutomaticRetry;
        if (!IsCanonicalUuid(run.Id.Value()) || !IsCanonicalUuid(run.Task.Value()) ||
            !IsCanonicalUuid(run.ExecutionContext.Value()) || !_validOptionalId(run.AgentSession) ||
            !_validOptionalId(run.RetryOfRunId) || !_enumInRange(run.Trigger, TaskRunTrigger::FollowUp) ||
            retryTrigger != run.RetryOfRunId.has_value() ||
            run.State != TaskRunState::Queued || command.Metadata.ExpectedTaskVersion == 0 ||
            run.Sequence != 0 || run.CreatedAtUnixMilliseconds < 0 || run.StartedAtUnixMilliseconds ||
            run.FinishedAtUnixMilliseconds || !run.Result.empty() || !run.ReasonCode.empty() ||
            run.CreatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "New TaskRun is not in its canonical queued state";
            return false;
        }
        return true;
    }

    bool ValidateRecordTaskAgentSessionCommand(
        const RecordTaskAgentSessionCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto& session = command.Session;
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            !IsCanonicalUuid(session.Id.Value()) ||
            !IsCanonicalUuid(session.Task.Value()) ||
            session.Provider.empty() || session.Provider.size() > MaximumProviderIdentifierBytes ||
            session.ProviderSessionId.empty() || session.ProviderSessionId.size() > MaximumProviderIdentifierBytes ||
            session.Provider.find('\0') != std::string::npos ||
            session.ProviderSessionId.find('\0') != std::string::npos ||
            !_enumInRange(session.State, AgentSessionState::Closed) ||
            session.CreatedAtUnixMilliseconds < 0 ||
            session.UpdatedAtUnixMilliseconds < session.CreatedAtUnixMilliseconds ||
            session.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "AgentSession record command is invalid";
            return false;
        }
        return true;
    }

    bool ValidateAppendTaskProviderEventCommand(
        const AppendTaskProviderEventCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto validType = !command.EventType.empty() &&
                               command.EventType.size() <= MaximumTaskProviderEventTypeBytes &&
                               std::ranges::all_of(command.EventType, [](const unsigned char value) {
                                   return std::isalnum(value) || value == '_' || value == '-' || value == '.';
                               });
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            !IsCanonicalUuid(command.Task.Value()) ||
            (command.Run && !IsCanonicalUuid(command.Run->Value())) ||
            command.ProviderSequence == 0 || !validType)
        {
            error = "Task provider event command is invalid";
            return false;
        }
        return true;
    }

    bool ValidateInitializeTaskGitWorktreeCommand(
        const InitializeTaskGitWorktreeCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto validation = ValidateTaskGitWorktreeRecord(command.Worktree);
        if (!validation.empty())
        {
            error = "Task Git Worktree record is invalid";
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            command.Worktree.Revision != 1 ||
            command.Worktree.State != TaskGitWorktreeState::Planning ||
            command.Worktree.HasUndeliveredWork ||
            command.Worktree.CleanupEligible ||
            !command.Worktree.SnapshotDigest.empty() ||
            !command.Worktree.BaselineCommit.empty() ||
            !command.Worktree.PreservedPath.empty() ||
            !command.Worktree.FailureReasonCode.empty() ||
            command.Worktree.CreatedAtUnixMilliseconds !=
                command.Metadata.TimestampUnixMilliseconds ||
            command.Worktree.UpdatedAtUnixMilliseconds !=
                command.Metadata.TimestampUnixMilliseconds)
        {
            error = "Initial Task Git Worktree state is invalid";
            return false;
        }
        return true;
    }

    bool ValidateAdvanceTaskGitWorktreeCommand(
        const AdvanceTaskGitWorktreeCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            command.ExpectedWorktreeRevision == 0 ||
            !IsCanonicalUuid(command.Task.Value()) ||
            !IsCanonicalUuid(command.ExecutionContext.Value()) ||
            !IsTaskGitWorktreeSetupState(command.RequestedState) ||
            command.RequestedState == TaskGitWorktreeState::Planning ||
            command.PreservedPath.size() > MaximumTaskPathBytes ||
            command.FailureReasonCode.size() > MaximumTaskGitReasonCodeBytes ||
            (!command.SnapshotDigest.empty() &&
             !_isLowerHexDigest(command.SnapshotDigest)) ||
            (!command.BaselineCommit.empty() &&
             !_isGitObjectId(command.BaselineCommit)))
        {
            error = "Task Git Worktree advance command is invalid";
            return false;
        }

        const auto clearFailure = command.FailureReasonCode.empty();
        switch (command.RequestedState)
        {
        case TaskGitWorktreeState::CapturingSource:
            if (!command.SnapshotDigest.empty() ||
                !command.BaselineCommit.empty() ||
                command.HasUndeliveredWork || !command.PreservedPath.empty() ||
                !clearFailure)
            {
                error = "Capturing source requires empty setup evidence";
                return false;
            }
            break;
        case TaskGitWorktreeState::Provisioning:
            if (command.SnapshotDigest.empty() ||
                !command.BaselineCommit.empty() ||
                command.HasUndeliveredWork || !command.PreservedPath.empty() ||
                !clearFailure)
            {
                error = "Provisioning requires only source snapshot evidence";
                return false;
            }
            break;
        case TaskGitWorktreeState::RestoringSource:
        case TaskGitWorktreeState::CreatingBaseline:
            if (command.SnapshotDigest.empty() ||
                !command.BaselineCommit.empty() ||
                !command.HasUndeliveredWork || command.PreservedPath.empty() ||
                !clearFailure)
            {
                error = "Reconstructed source requires its preserved Worktree path";
                return false;
            }
            break;
        case TaskGitWorktreeState::Ready:
            if (command.SnapshotDigest.empty() ||
                command.BaselineCommit.empty() ||
                command.HasUndeliveredWork || !command.PreservedPath.empty() ||
                !clearFailure)
            {
                error = "Ready Worktree requires baseline and snapshot evidence";
                return false;
            }
            break;
        case TaskGitWorktreeState::Failed:
            if (clearFailure ||
                command.HasUndeliveredWork != !command.PreservedPath.empty())
            {
                error = "Failed Worktree preservation evidence is inconsistent";
                return false;
            }
            break;
        default:
            error = "Requested Git Worktree state is not a setup phase";
            return false;
        }
        return true;
    }

    bool ValidateAdvanceTaskGitWorktreeFinalizationCommand(
        const AdvanceTaskGitWorktreeFinalizationCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            command.ExpectedWorktreeRevision == 0 ||
            !IsCanonicalUuid(command.Task.Value()) ||
            !IsCanonicalUuid(command.ExecutionContext.Value()) ||
            !IsTaskGitWorktreeFinalizationState(command.RequestedState) ||
            command.RequestedState == TaskGitWorktreeState::Ready ||
            command.PreservedPath.size() > MaximumTaskPathBytes ||
            command.FailureReasonCode.size() > MaximumTaskGitReasonCodeBytes)
        {
            error = "Task Git Worktree finalization command is invalid";
            return false;
        }
        switch (command.RequestedState)
        {
        case TaskGitWorktreeState::Finalizing:
            if (command.CleanupEligible || !command.PreservedPath.empty() ||
                !command.FailureReasonCode.empty())
            {
                error = "Finalizing Worktree cannot carry completion or failure evidence";
                return false;
            }
            break;
        case TaskGitWorktreeState::Preserved:
            if (!command.HasUndeliveredWork || command.CleanupEligible ||
                command.PreservedPath.empty() ||
                command.FailureReasonCode.empty())
            {
                error = "Preserved Worktree requires undelivered failure evidence";
                return false;
            }
            break;
        case TaskGitWorktreeState::Finalized:
            if (command.HasUndeliveredWork || !command.CleanupEligible ||
                !command.PreservedPath.empty() ||
                !command.FailureReasonCode.empty())
            {
                error = "Finalized Worktree requires complete cleanup evidence";
                return false;
            }
            break;
        default:
            error = "Requested Worktree finalization state is unsupported";
            return false;
        }
        return true;
    }

    bool ValidateBeginTaskRunGitCheckpointCommand(
        const BeginTaskRunGitCheckpointCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto& checkpoint = command.Checkpoint;
        if (!ValidateTaskRunGitCheckpointRecord(checkpoint).empty() ||
            command.Metadata.ExpectedTaskVersion == 0 ||
            checkpoint.Revision != 1 ||
            checkpoint.State != TaskGitCheckpointState::Pending ||
            checkpoint.StartCommit.empty() || !checkpoint.EndCommit.empty() ||
            !checkpoint.CheckpointCommit.empty() || checkpoint.AutoCheckpointed ||
            checkpoint.ChangedPathsJson != "[]" ||
            checkpoint.DiffSummaryJson.empty() ||
            !checkpoint.PreservedPath.empty() || !checkpoint.ReasonCode.empty() ||
            checkpoint.CreatedAtUnixMilliseconds !=
                command.Metadata.TimestampUnixMilliseconds ||
            checkpoint.UpdatedAtUnixMilliseconds !=
                command.Metadata.TimestampUnixMilliseconds)
        {
            error = "Initial TaskRun Git checkpoint state is invalid";
            return false;
        }
        return true;
    }

    bool ValidateAdvanceTaskRunGitCheckpointCommand(
        const AdvanceTaskRunGitCheckpointCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            command.ExpectedCheckpointRevision == 0 ||
            command.ExpectedWorktreeRevision == 0 ||
            !IsCanonicalUuid(command.Task.Value()) ||
            !IsCanonicalUuid(command.Run.Value()) ||
            !IsCanonicalUuid(command.Checkpoint.Value()) ||
            command.RequestedState == TaskGitCheckpointState::Pending ||
            command.RequestedState > TaskGitCheckpointState::Failed ||
            (!command.EndCommit.empty() && !_isGitObjectId(command.EndCommit)) ||
            (!command.CheckpointCommit.empty() &&
             !_isGitObjectId(command.CheckpointCommit)) ||
            command.ChangedPathsJson.size() > MaximumTaskGitJsonBytes ||
            command.DiffSummaryJson.empty() ||
            command.DiffSummaryJson.size() > MaximumTaskGitJsonBytes ||
            command.PreservedPath.size() > MaximumTaskPathBytes ||
            command.ReasonCode.size() > MaximumTaskGitReasonCodeBytes)
        {
            error = "TaskRun Git checkpoint advance command is invalid";
            return false;
        }

        switch (command.RequestedState)
        {
        case TaskGitCheckpointState::Inspecting:
            if (!command.EndCommit.empty() || !command.CheckpointCommit.empty() ||
                command.AutoCheckpointed || command.ChangedPathsJson != "[]" ||
                !command.PreservedPath.empty() || !command.ReasonCode.empty())
            {
                error = "Checkpoint inspection must retain only start evidence";
                return false;
            }
            break;
        case TaskGitCheckpointState::Committing:
            if (command.EndCommit.empty() || !command.CheckpointCommit.empty() ||
                command.AutoCheckpointed || command.ChangedPathsJson == "[]" ||
                !command.HasUndeliveredWork || !command.PreservedPath.empty() ||
                !command.ReasonCode.empty())
            {
                error = "Committing checkpoint evidence is incomplete";
                return false;
            }
            break;
        case TaskGitCheckpointState::Succeeded:
            if (command.EndCommit.empty() ||
                command.CheckpointCommit != command.EndCommit ||
                !command.AutoCheckpointed || command.ChangedPathsJson == "[]" ||
                !command.HasUndeliveredWork || !command.PreservedPath.empty() ||
                !command.ReasonCode.empty())
            {
                error = "Succeeded checkpoint evidence is inconsistent";
                return false;
            }
            break;
        case TaskGitCheckpointState::NotNeeded:
            if (command.EndCommit.empty() || !command.CheckpointCommit.empty() ||
                command.AutoCheckpointed || !command.PreservedPath.empty() ||
                !command.ReasonCode.empty())
            {
                error = "No-op checkpoint evidence is inconsistent";
                return false;
            }
            break;
        case TaskGitCheckpointState::Failed:
            if (!command.CheckpointCommit.empty() || command.AutoCheckpointed ||
                !command.HasUndeliveredWork || command.PreservedPath.empty() ||
                command.ReasonCode.empty())
            {
                error = "Failed checkpoint must preserve undelivered work";
                return false;
            }
            break;
        default:
            error = "Requested Git checkpoint state is unsupported";
            return false;
        }
        return true;
    }

    bool ValidateRunStateCommand(const ChangeTaskRunStateCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error) ||
            !IsCanonicalUuid(command.Task.Value()) || !IsCanonicalUuid(command.Run.Value()) ||
            command.Metadata.ExpectedTaskVersion == 0 ||
            !_enumInRange(command.RequestedState, TaskRunState::Interrupted))
        {
            if (error.empty())
            {
                error = "TaskRun transition identifiers are invalid";
            }
            return false;
        }
        if (command.Result.size() > MaximumTaskRunResultBytes || command.ReasonCode.size() > MaximumTaskReasonCodeBytes)
        {
            error = "TaskRun outcome payload exceeds its bound";
            return false;
        }
        if (!IsTaskRunTerminal(command.RequestedState) && (!command.Result.empty() || !command.ReasonCode.empty()))
        {
            error = "Only terminal TaskRun states may carry an outcome";
            return false;
        }
        return true;
    }

    bool ValidateTaskAgentMutationCommand(
        const ApplyTaskAgentMutationCommand& command,
        std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error) ||
            !IsCanonicalUuid(command.Task.Value()) ||
            !IsCanonicalUuid(command.Run.Value()) ||
            command.Metadata.ExpectedTaskVersion == 0 ||
            !_enumInRange(command.Kind, TaskAgentMutationKind::Result))
        {
            if (error.empty())
            {
                error = "Agent Task mutation identity or kind is invalid";
            }
            return false;
        }
        if (command.Text.find('\0') != std::string::npos)
        {
            error = "Agent Task mutation text contains an embedded NUL";
            return false;
        }
        switch (command.Kind)
        {
        case TaskAgentMutationKind::StatusInProgress:
        case TaskAgentMutationKind::StatusInReview:
            if (!command.Text.empty())
            {
                error = "Agent Task status mutation does not accept text";
                return false;
            }
            return true;
        case TaskAgentMutationKind::StatusBlocked:
            if (command.Text.empty() ||
                command.Text.size() > MaximumTaskBlockedReasonBytes)
            {
                error = "Agent Task blocked reason is empty or oversized";
                return false;
            }
            return true;
        case TaskAgentMutationKind::Result:
            if (command.Text.empty() || command.Text.size() > MaximumTaskRunResultBytes)
            {
                error = "Agent Task result is empty or oversized";
                return false;
            }
            return true;
        default:
            error = "Agent Task mutation kind is invalid";
            return false;
        }
    }

    bool ValidateTerminalStateCommand(const ChangeTaskTerminalStateCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 || !IsCanonicalUuid(command.Task.Value()) ||
            !_enumInRange(command.RequestedState, TaskTerminalState::Closed) ||
            command.RequestedGeneration > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            error = "TaskTerminal transition command is invalid";
            return false;
        }
        return true;
    }

    bool ValidateStartTaskIntentCommand(const StartTaskIntentCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto& intent = command.Intent;
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            !IsCanonicalUuid(intent.Id.Value()) || !IsCanonicalUuid(intent.Task.Value()) ||
            !_enumInRange(intent.Kind, TaskIntentKind::Delete) || intent.Stage != TaskIntentStage::Requested ||
            intent.Revision != 1 ||
            (intent.DeadlineAtUnixMilliseconds && intent.DeadlineAtUnixMilliseconds.value() < 0) ||
            intent.DetailJson.size() > MaximumTaskStoreJsonBytes || !intent.ReasonCode.empty() ||
            intent.CreatedAtUnixMilliseconds < 0 || intent.UpdatedAtUnixMilliseconds < intent.CreatedAtUnixMilliseconds ||
            intent.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "New Task intent is not in its canonical requested state";
            return false;
        }
        return true;
    }

    bool ValidateAdvanceTaskIntentCommand(const AdvanceTaskIntentCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 || command.ExpectedIntentRevision == 0 ||
            !IsCanonicalUuid(command.Task.Value()) || !IsCanonicalUuid(command.Intent.Value()) ||
            !_enumInRange(command.RequestedStage, TaskIntentStage::Completed) ||
            (command.DeadlineAtUnixMilliseconds && command.DeadlineAtUnixMilliseconds.value() < 0) ||
            command.DetailJson.size() > MaximumTaskStoreJsonBytes ||
            command.ReasonCode.size() > MaximumTaskReasonCodeBytes)
        {
            error = "Task intent advance command is invalid";
            return false;
        }
        if (command.RequestedStage == TaskIntentStage::Failed && command.ReasonCode.empty())
        {
            error = "Failed Task intent stage requires a reason code";
            return false;
        }
        return true;
    }

    bool ValidateChangeTaskManualHoldCommand(const ChangeTaskManualHoldCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error) ||
            command.Metadata.ExpectedTaskVersion == 0 || !IsCanonicalUuid(command.Task.Value()))
        {
            if (error.empty())
            {
                error = "Task manual-hold command is invalid";
            }
            return false;
        }
        return true;
    }

    bool ValidateDeleteTaskCommand(const DeleteTaskCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error) ||
            command.Metadata.ExpectedTaskVersion == 0 || command.ExpectedIntentRevision == 0 ||
            !IsCanonicalUuid(command.Task.Value()) || !IsCanonicalUuid(command.Intent.Value()))
        {
            if (error.empty())
            {
                error = "Delete Task command is invalid";
            }
            return false;
        }
        if (command.Metadata.TimestampUnixMilliseconds >
            std::numeric_limits<int64_t>::max() - TaskTombstoneRetentionMilliseconds)
        {
            error = "Delete Task timestamp exceeds the tombstone retention range";
            return false;
        }
        return true;
    }

    bool ValidateEnqueueTaskMessageCommand(const EnqueueTaskMessageCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        const auto& message = command.Message;
        if (command.Metadata.ExpectedTaskVersion == 0 ||
            !IsCanonicalUuid(message.Id.Value()) || !IsCanonicalUuid(message.Task.Value()) ||
            message.Author != TaskMessageAuthor::User || message.State != TaskMessageState::Pending ||
            message.Revision != 1 || message.Content.empty() || message.Content.size() > MaximumTaskMessageBytes ||
            message.ClaimedByRunId || message.CreatedAtUnixMilliseconds < 0 ||
            message.UpdatedAtUnixMilliseconds < message.CreatedAtUnixMilliseconds ||
            message.UpdatedAtUnixMilliseconds > command.Metadata.TimestampUnixMilliseconds)
        {
            error = "New Task message is not in its canonical pending state";
            return false;
        }
        return true;
    }

    bool ValidateReviseTaskMessageCommand(const ReviseTaskMessageCommand& command, std::string& error)
    {
        if (!ValidateCommandMetadata(command.Metadata, error))
        {
            return false;
        }
        if (command.Metadata.ExpectedTaskVersion == 0 || command.ExpectedMessageRevision == 0 ||
            !IsCanonicalUuid(command.Task.Value()) || !IsCanonicalUuid(command.Message.Value()) ||
            command.Remove == command.ReplacementContent.has_value())
        {
            error = "Task message revision command is invalid";
            return false;
        }
        if (command.ReplacementContent &&
            (command.ReplacementContent->empty() || command.ReplacementContent->size() > MaximumTaskMessageBytes))
        {
            error = "Replacement Task message content is invalid";
            return false;
        }
        return true;
    }

    bool ValidateClaimTaskMessagesCommand(const ClaimTaskMessagesAndCreateRunCommand& command, std::string& error)
    {
        const CreateTaskRunCommand runCommand{ command.Metadata, command.Run };
        if (!ValidateCreateTaskRunCommand(runCommand, error))
        {
            return false;
        }
        if (command.Run.Trigger != TaskRunTrigger::FollowUp || command.Run.RetryOfRunId)
        {
            error = "Message claim must create a canonical follow-up TaskRun";
            return false;
        }
        return true;
    }
}
