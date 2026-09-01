// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeGitWorktreeSetup.h"

#include "RuntimeGitWorktreeSetupEvidence.h"
#include "RuntimeGitWorktreeSetupStore.h"

#include "GitWorktreeBaseline.h"
#include "GitWorktreeCaptureJournal.h"
#include "GitWorktreeProvision.h"
#include "GitWorktreeReplay.h"

#include "TaskRecordValidation.h"
#include "TaskGitWorktreeStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskCore;
        using namespace Microsoft::Terminal::TaskGit;

        RuntimeGitWorktreeSetupResult _result(
            const RuntimeGitWorktreeSetupStatus status,
            const TaskGitWorktreeState current,
            const TaskGitWorktreeState next,
            std::string error = {})
        {
            RuntimeGitWorktreeSetupResult result;
            result.Status = status;
            result.CurrentState = current;
            result.NextState = next;
            result.Error = std::move(error);
            return result;
        }

        RuntimeGitWorktreeSetupResult _stored(
            const TaskStoreCommandResult& stored,
            const TaskGitWorktreeState current,
            const TaskGitWorktreeState next,
            std::string snapshot = {},
            std::string baseline = {})
        {
            auto result = _result(
                stored.Succeeded() ?
                    (next == TaskGitWorktreeState::Ready ?
                         RuntimeGitWorktreeSetupStatus::Ready :
                         RuntimeGitWorktreeSetupStatus::Advanced) :
                    RuntimeGitWorktreeSetupStatus::StoreFailure,
                current,
                next,
                stored.Error);
            result.StoreStatus = stored.Status;
            result.SnapshotDigest = std::move(snapshot);
            result.BaselineCommit = std::move(baseline);
            return result;
        }
    }

    RuntimeGitWorktreeSetupService::RuntimeGitWorktreeSetupService(
        std::shared_ptr<ITaskStore> store,
        std::filesystem::path gitExecutable,
        std::filesystem::path runtimeDataRoot) noexcept :
        _store{ std::move(store) },
        _gitExecutable{ std::move(gitExecutable) },
        _runtimeDataRoot{ std::move(runtimeDataRoot) }
    {
        _valid = _store && _store->IsReady() && !_gitExecutable.empty() &&
                 !_runtimeDataRoot.empty();
    }

    bool RuntimeGitWorktreeSetupService::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeGitWorktreeSetupResult RuntimeGitWorktreeSetupService::Step(
        const TaskId& task,
        const RuntimeGitWorktreeSetupContext& context,
        const HANDLE cancellationEvent)
    {
        try
        {
            if (!_valid || !IsCanonicalUuid(task.Value()) ||
                !IsCanonicalUuid(context.Command.Value()) ||
                !IsCanonicalUuid(context.Event.Value()) ||
                context.UnixMilliseconds < 0 || !cancellationEvent ||
                cancellationEvent == INVALID_HANDLE_VALUE)
            {
                return _result(
                    RuntimeGitWorktreeSetupStatus::InvalidRequest,
                    TaskGitWorktreeState::Planning,
                    TaskGitWorktreeState::Planning,
                    "Runtime Git Worktree setup request is invalid");
            }
            const auto aggregate = _store->LoadTask(task);
            if (!aggregate || !aggregate->GitWorktree)
            {
                return _result(
                    RuntimeGitWorktreeSetupStatus::NotFound,
                    TaskGitWorktreeState::Planning,
                    TaskGitWorktreeState::Planning,
                    "Runtime Git Worktree setup record was not found");
            }
            const auto& record = *aggregate->GitWorktree;
            const auto current = record.State;
            if (current == TaskGitWorktreeState::Ready)
            {
                auto result = _result(
                    RuntimeGitWorktreeSetupStatus::Ready,
                    current,
                    current);
                result.SnapshotDigest = record.SnapshotDigest;
                result.BaselineCommit = record.BaselineCommit;
                return result;
            }
            if (current == TaskGitWorktreeState::Failed ||
                !IsTaskGitWorktreeSetupState(current))
            {
                return _result(
                    RuntimeGitWorktreeSetupStatus::UnsupportedState,
                    current,
                    current,
                    "Runtime Git Worktree setup requires an explicit retry transition");
            }

            const auto layout = details::PrepareRuntimeGitWorktreeLayoutEvidence(
                record,
                _runtimeDataRoot);
            if (!layout.Succeeded)
            {
                return _result(
                    RuntimeGitWorktreeSetupStatus::InvalidRequest,
                    current,
                    current,
                    layout.Error);
            }

            const auto persistFailure = [&](std::string reason,
                                            std::string error,
                                            const bool preserved,
                                            std::string snapshot = {},
                                            std::string baseline = {}) {
                const auto stored = details::AdvanceRuntimeGitWorktreeSetup(
                    *_store,
                    *aggregate,
                    context,
                    TaskGitWorktreeState::Failed,
                    std::move(snapshot),
                    std::move(baseline),
                    preserved,
                    preserved ? record.WorktreePath : std::string{},
                    reason);
                auto result = _result(
                    stored.Succeeded() ?
                        RuntimeGitWorktreeSetupStatus::OperationFailed :
                        RuntimeGitWorktreeSetupStatus::StoreFailure,
                    current,
                    TaskGitWorktreeState::Failed,
                    stored.Succeeded() ? std::move(error) : stored.Error);
                result.StoreStatus = stored.Status;
                result.FailurePersisted = stored.Succeeded();
                result.FailureReasonCode = std::move(reason);
                return result;
            };

            if (current == TaskGitWorktreeState::Planning)
            {
                const auto stored = details::AdvanceRuntimeGitWorktreeSetup(
                    *_store,
                    *aggregate,
                    context,
                    TaskGitWorktreeState::CapturingSource);
                return _stored(
                    stored,
                    current,
                    TaskGitWorktreeState::CapturingSource);
            }

            if (current == TaskGitWorktreeState::CapturingSource)
            {
                auto expectedMarker = *layout.ExpectedMarker;
                expectedMarker.SnapshotDigest.clear();
                expectedMarker.BaselineCommit.clear();
                GitWorktreeCaptureJournal capture{ _gitExecutable };
                const auto captured = capture.CaptureOrLoad(
                    *layout.Layout,
                    expectedMarker,
                    *layout.SourcePath,
                    cancellationEvent);
                if (!captured.Succeeded())
                {
                    return persistFailure(
                        "git-source-capture-failed",
                        captured.Error,
                        false);
                }
                if (captured.Journal->SourceHead != record.SourceHead ||
                    captured.Journal->SourceReference != record.SourceReference ||
                    captured.Journal->RepositoryIdentityDigest !=
                        record.RepositoryIdentityDigest)
                {
                    return persistFailure(
                        "git-source-identity-changed",
                        "Captured Git source identity differs from its Task record",
                        false);
                }
                const auto stored = details::AdvanceRuntimeGitWorktreeSetup(
                    *_store,
                    *aggregate,
                    context,
                    TaskGitWorktreeState::Provisioning,
                    captured.Journal->SnapshotDigest);
                return _stored(
                    stored,
                    current,
                    TaskGitWorktreeState::Provisioning,
                    captured.Journal->SnapshotDigest);
            }

            const auto evidence = details::LoadRuntimeGitWorktreeEvidence(
                record,
                *layout.Layout,
                *layout.ExpectedMarker);
            if (!evidence.Succeeded)
            {
                return persistFailure(
                    "git-setup-evidence-invalid",
                    evidence.Error,
                    true,
                    record.SnapshotDigest,
                    record.BaselineCommit);
            }

            if (current == TaskGitWorktreeState::Provisioning)
            {
                GitWorktreeProvisioner provision{ _gitExecutable };
                const auto provisioned = provision.Provision(
                    *layout.Layout,
                    *evidence.Marker,
                    *evidence.Journal,
                    cancellationEvent,
                    GitWorktreePreflightMode::AllowOwnedRecovery);
                if (!provisioned.Succeeded())
                {
                    return persistFailure(
                        "git-worktree-provision-failed",
                        provisioned.Error,
                        provisioned.Preserved,
                        record.SnapshotDigest);
                }
                const auto stored = details::AdvanceRuntimeGitWorktreeSetup(
                    *_store,
                    *aggregate,
                    context,
                    TaskGitWorktreeState::RestoringSource,
                    record.SnapshotDigest,
                    {},
                    true,
                    record.WorktreePath);
                return _stored(
                    stored,
                    current,
                    TaskGitWorktreeState::RestoringSource,
                    record.SnapshotDigest);
            }

            if (current == TaskGitWorktreeState::RestoringSource)
            {
                GitWorktreeReplayer replay{ _gitExecutable };
                const auto replayed = replay.Replay(
                    *layout.Layout,
                    *evidence.Marker,
                    *evidence.Journal,
                    cancellationEvent);
                if (!replayed.Succeeded())
                {
                    return persistFailure(
                        "git-source-replay-failed",
                        replayed.Error,
                        true,
                        record.SnapshotDigest);
                }
                const auto stored = details::AdvanceRuntimeGitWorktreeSetup(
                    *_store,
                    *aggregate,
                    context,
                    TaskGitWorktreeState::CreatingBaseline,
                    record.SnapshotDigest,
                    {},
                    true,
                    record.WorktreePath);
                return _stored(
                    stored,
                    current,
                    TaskGitWorktreeState::CreatingBaseline,
                    record.SnapshotDigest);
            }

            if (current == TaskGitWorktreeState::CreatingBaseline)
            {
                GitWorktreeBaselineCreator baseline{ _gitExecutable };
                const auto created = baseline.Create(
                    *layout.Layout,
                    *evidence.Marker,
                    *evidence.Journal,
                    cancellationEvent);
                if (!created.Succeeded())
                {
                    return persistFailure(
                        "git-baseline-create-failed",
                        created.Error,
                        true,
                        record.SnapshotDigest,
                        created.BaselineCommit);
                }
                const auto stored = details::AdvanceRuntimeGitWorktreeSetup(
                    *_store,
                    *aggregate,
                    context,
                    TaskGitWorktreeState::Ready,
                    record.SnapshotDigest,
                    created.BaselineCommit);
                return _stored(
                    stored,
                    current,
                    TaskGitWorktreeState::Ready,
                    record.SnapshotDigest,
                    created.BaselineCommit);
            }

            return _result(
                RuntimeGitWorktreeSetupStatus::UnsupportedState,
                current,
                current,
                "Runtime Git Worktree setup phase is unsupported");
        }
        catch (...)
        {
            return _result(
                RuntimeGitWorktreeSetupStatus::ResourceFailure,
                TaskGitWorktreeState::Planning,
                TaskGitWorktreeState::Planning,
                "Runtime Git Worktree setup exhausted local resources");
        }
    }
}
