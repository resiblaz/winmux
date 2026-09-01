// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskOrchestratorInternal.h"

#include "RuntimeGitWorktreeCheckpoint.h"
#include "RuntimeGitWorktreeSetup.h"
#include "RuntimeTaskCommandMetadata.h"
#include "RuntimeCrypto.h"
#include "RuntimeTaskSchedulingPolicy.h"

#include "DirectoryIdentity.h"
#include "GitRepositoryInspector.h"
#include "GitWorktreeLayout.h"
#include "RtaskCommand.h"
#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskExecution;
    using namespace Microsoft::Terminal::TaskGit;
    using namespace Microsoft::Terminal::TaskProvider;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        std::optional<std::wstring> _wide(const std::string_view value)
        {
            if (value.empty())
                return std::wstring{};
            if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
                return std::nullopt;
            const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (count <= 0)
                return std::nullopt;
            std::wstring result(static_cast<size_t>(count), L'\0');
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count) == count ?
                       std::optional<std::wstring>{ std::move(result) } :
                       std::nullopt;
        }

        std::optional<std::string> _utf8(const std::wstring_view value)
        {
            if (value.empty())
                return std::string{};
            if (value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
                return std::nullopt;
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0)
                return std::nullopt;
            std::string result(static_cast<size_t>(count), '\0');
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count ?
                       std::optional<std::string>{ std::move(result) } :
                       std::nullopt;
        }

        const ExecutionContextRecord* _context(const TaskAggregateSnapshot& aggregate) noexcept
        {
            return aggregate.ExecutionContexts.empty() ? nullptr :
                                                         &*std::ranges::max_element(aggregate.ExecutionContexts, {}, &ExecutionContextRecord::Revision);
        }

        const TaskRunRecord* _run(const TaskAggregateSnapshot& aggregate, const TaskRunId& run) noexcept
        {
            const auto found = std::ranges::find(aggregate.Runs, run, &TaskRunRecord::Id);
            return found == aggregate.Runs.end() ? nullptr : &*found;
        }

        std::optional<std::filesystem::path> _workingDirectory(
            const TaskAggregateSnapshot& aggregate,
            const ExecutionContextRecord& context)
        {
            const auto rootText = aggregate.GitWorktree &&
                                          aggregate.GitWorktree->State == TaskGitWorktreeState::Ready ?
                                      aggregate.GitWorktree->WorktreePath :
                                      context.RuntimeRoot;
            const auto root = _wide(rootText);
            const auto relative = _wide(context.RelativeWorkingDirectory);
            if (!root || !std::filesystem::path{ *root }.is_absolute() || !relative)
                return std::nullopt;
            const std::filesystem::path relativePath{ *relative };
            if (relativePath.is_absolute() || std::ranges::any_of(relativePath, [](const auto& part) { return part == L".."; }))
            {
                return std::nullopt;
            }
            return (std::filesystem::path{ *root } / relativePath).lexically_normal();
        }

        std::string _providerReason(const TaskProviderStatus status)
        {
            switch (status)
            {
            case TaskProviderStatus::Timeout:
                return "provider-timeout";
            case TaskProviderStatus::TransportClosed:
                return "runtime_recovery";
            case TaskProviderStatus::Unavailable:
                return "provider-unavailable";
            case TaskProviderStatus::Cancelled:
                return "runtime-stopping";
            case TaskProviderStatus::UnsupportedVersion:
            case TaskProviderStatus::UnsupportedCapability:
                return "provider-incompatible";
            default:
                return "provider-start-failed";
            }
        }
    }

    void RuntimeTaskOrchestrator::Impl::ExecuteRun(TaskId task, TaskRunId run) noexcept
    try
    {
        auto aggregate = Store->LoadTask(task);
        const auto current = aggregate ? _run(*aggregate, run) : nullptr;
        if (!aggregate || !current)
            return;
        if (IsTaskRunTerminal(current->State))
        {
            FinalizeRun(*aggregate, *current);
            return;
        }
        if (current->State != TaskRunState::Queued || aggregate->Task.ManualHold)
            return;
        std::optional<DirectDirectoryLease> directLease;
        const auto context = _context(*aggregate);
        if (!context || !TransitionRun(task, run, TaskRunState::Preparing))
            return;
        const auto initialContext = *context;

        if (context->Mode == ExecutionMode::Worktree)
        {
            aggregate = Store->LoadTask(task);
            if (!aggregate)
                return;
            if (!aggregate->GitWorktree)
            {
                const auto source = _wide(initialContext.SourceRoot);
                const auto layout = PrepareGitWorktreeLayout(RuntimeDataRoot, task);
                GitRepositoryInspector inspector{ GitExecutable };
                const auto repository = source ? inspector.Inspect(*source, StopEvent.get()) : GitRepositoryInspectionResult{};
                const auto nonce = NewRuntimeUuid();
                if (!source || !layout.Succeeded() || !repository.Succeeded() || !nonce)
                {
                    static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "git-preflight-failed"));
                    return;
                }
                InitializeTaskGitWorktreeCommand initialize;
                initialize.Worktree.Task = task;
                initialize.Worktree.ExecutionContext = initialContext.Id;
                initialize.Worktree.Revision = 1;
                initialize.Worktree.State = TaskGitWorktreeState::Planning;
                initialize.Worktree.RepositoryRoot = initialContext.SourceRoot;
                initialize.Worktree.RepositoryCommonDirectory = _utf8(repository.Inspection->CommonDirectoryIdentity.CanonicalPath().native()).value_or(std::string{});
                initialize.Worktree.RepositoryIdentityDigest = repository.Inspection->CommonDirectoryIdentity.Digest();
                initialize.Worktree.WorktreePath = layout.Layout->WorktreePathUtf8;
                initialize.Worktree.BranchRef = layout.Layout->BranchRef;
                initialize.Worktree.SourceHead = repository.Inspection->Head;
                initialize.Worktree.SourceReference = repository.Inspection->BranchRef;
                initialize.Worktree.OwnershipNonce = *nonce;
                initialize.Worktree.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
                initialize.Worktree.UpdatedAtUnixMilliseconds = initialize.Worktree.CreatedAtUnixMilliseconds;
                const auto metadata = MakeRuntimeTaskCommandMetadata(
                    "orchestrator-worktree-init-v1", task, run.Value(), aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
                if (!metadata)
                {
                    static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "git-preflight-metadata"));
                    return;
                }
                initialize.Metadata = *metadata;
                if (!Store->InitializeTaskGitWorktree(initialize).Succeeded())
                {
                    static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "git-worktree-initialize"));
                    return;
                }
            }
            RuntimeGitWorktreeSetupService setup{ Store, GitExecutable, RuntimeDataRoot };
            for (size_t phase = 0; phase < 8; ++phase)
            {
                const auto command = NewRuntimeUuid();
                const auto event = NewRuntimeUuid();
                if (!command || !event)
                {
                    static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "git-setup-identity"));
                    return;
                }
                const auto result = setup.Step(
                    task,
                    { CommandId{ *command }, TaskEventId{ *event }, RuntimeUnixMilliseconds() },
                    StopEvent.get());
                if (result.Status == RuntimeGitWorktreeSetupStatus::Ready)
                    break;
                if (!result.Succeeded())
                {
                    static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, result.FailureReasonCode.empty() ? "git-setup-failed" : result.FailureReasonCode));
                    return;
                }
            }
            aggregate = Store->LoadTask(task);
            if (!aggregate || !aggregate->GitWorktree || aggregate->GitWorktree->State != TaskGitWorktreeState::Ready)
            {
                static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "git-setup-incomplete"));
                return;
            }
        }
        else
        {
            if (!TransitionRun(task, run, TaskRunState::WaitingForDirectory))
                return;
            aggregate = Store->LoadTask(task);
            const auto currentContext = aggregate ? _context(*aggregate) : nullptr;
            const auto directory = aggregate && currentContext ? _workingDirectory(*aggregate, *currentContext) : std::nullopt;
            const auto identity = directory ? ResolveCanonicalDirectoryIdentity(*directory, DirectoryIdentityPolicy::Direct) : DirectoryIdentityResult{};
            if (!identity.Succeeded())
            {
                static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "direct-directory-invalid"));
                return;
            }
            auto acquired = DirectLeaseManager.Acquire(*identity.Identity, StopEvent.get());
            if (!acquired.Acquired())
            {
                static_cast<void>(TransitionRun(task, run, TaskRunState::Interrupted, acquired.Status == TaskDirectoryLockStatus::Cancelled ? "runtime-stopping" : "direct-directory-lock"));
                return;
            }
            directLease.emplace(std::move(acquired.Lease));
        }

        if (!TransitionRun(task, run, TaskRunState::Starting))
            return;
        aggregate = Store->LoadTask(task);
        if (!aggregate)
            return;
        auto generation = aggregate->Terminal.Generation;
        if (aggregate->Terminal.State == TaskTerminalState::NotCreated ||
            aggregate->Terminal.State == TaskTerminalState::Exited ||
            aggregate->Terminal.State == TaskTerminalState::Suspended ||
            aggregate->Terminal.State == TaskTerminalState::Error ||
            aggregate->Terminal.State == TaskTerminalState::Closed)
        {
            if (generation == (std::numeric_limits<uint64_t>::max)() ||
                !TransitionTerminal(task, TaskTerminalState::Starting, ++generation, true))
            {
                static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "terminal-open-failed"));
                return;
            }
        }
        aggregate = Store->LoadTask(task);
        if (!aggregate)
            return;
        const auto terminalOpened = Terminals->Open({ task.Value(), aggregate->Terminal.Id.Value(), generation, {}, aggregate->Task.Title });
        if (!terminalOpened.Succeeded() ||
            (aggregate->Terminal.State == TaskTerminalState::Starting &&
             !TransitionTerminal(task, TaskTerminalState::Ready, generation)))
        {
            static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "terminal-listener-failed"));
            return;
        }

        aggregate = Store->LoadTask(task);
        const auto runRecord = aggregate ? _run(*aggregate, run) : nullptr;
        const auto runContext = aggregate ? _context(*aggregate) : nullptr;
        const auto directory = aggregate && runContext ? _workingDirectory(*aggregate, *runContext) : std::nullopt;
        if (!aggregate || !runRecord || !runContext || !directory)
        {
            static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "execution-context-invalid"));
            return;
        }

        TaskProviderCoordinatorResult session;
        const auto currentSession = std::ranges::find_if(aggregate->AgentSessions, [](const auto& value) {
            return value.IsCurrent && value.State != AgentSessionState::Poisoned && value.State != AgentSessionState::Closed;
        });
        if (currentSession != aggregate->AgentSessions.end())
        {
            session = Provider->ResumeSession(task.Value(), currentSession->ProviderSessionId, StopEvent.get());
        }
        if (!session.Succeeded())
        {
            session = Provider->StartSession({ task.Value(), *directory, runContext->Model }, StopEvent.get());
        }
        if (!session.Succeeded() || !session.Session)
        {
            static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, _providerReason(session.Status)));
            return;
        }
        const auto capability = Rtask->IssueForActiveRun(
            task.Value(), run.Value(), AllRtaskMethods, DefaultRuntimeRtaskCapabilityLifetimeMilliseconds, GetTickCount64());
        const auto prompt = BuildRuntimeTaskPrompt(*aggregate, *runRecord);
        if (!capability.Grant || !prompt)
        {
            static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "run-context-capability"));
            return;
        }
        TaskProviderRunRequest request;
        request.SessionId = session.Session->ProviderSessionId;
        request.Prompt = *prompt;
        request.Context.TaskId = task.Value();
        request.Context.RunId = run.Value();
        request.Context.WorkingDirectory = *directory;
        request.Context.Model = runContext->Model;
        request.Context.Instructions = runContext->Instructions;
        request.Context.RtaskExecutable = RtaskExecutable;
        request.Context.RtaskEndpoint = RtaskEndpoint;
        request.Context.RtaskCapability = capability.Grant->Capability;
        const auto started = Provider->StartRun(task.Value(), request, StopEvent.get());
        if (!started.Succeeded())
        {
            static_cast<void>(Rtask->InvalidateRun(run.Value()));
            static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, _providerReason(started.Status)));
            return;
        }
        static_cast<void>(TransitionTerminal(task, TaskTerminalState::Busy, generation));
        while (WaitForSingleObject(StopEvent.get(), 100) == WAIT_TIMEOUT)
        {
            const auto currentAggregate = Store->LoadTask(task);
            const auto currentRun = currentAggregate ? _run(*currentAggregate, run) : nullptr;
            if (!currentAggregate || !currentRun)
                break;
            if (IsTaskRunTerminal(currentRun->State))
            {
                FinalizeRun(*currentAggregate, *currentRun);
                break;
            }
        }
    }
    catch (...)
    {
        static_cast<void>(TransitionRun(task, run, TaskRunState::Failed, "orchestrator-exception"));
    }

    void RuntimeTaskOrchestrator::Impl::FinalizeRun(
        const TaskAggregateSnapshot& initial,
        const TaskRunRecord& initialRun) noexcept
    try
    {
        auto aggregate = Store->LoadTask(initial.Task.Id);
        const auto run = aggregate ? _run(*aggregate, initialRun.Id) : nullptr;
        if (!aggregate || !run || !IsTaskRunTerminal(run->State))
            return;
        const auto runRecord = *run;

        static_cast<void>(Rtask->InvalidateRun(runRecord.Id.Value()));
        if (aggregate->GitWorktree && aggregate->GitWorktree->State == TaskGitWorktreeState::Ready)
        {
            RuntimeGitWorktreeCheckpointService checkpoint{ Store, GitExecutable, RuntimeDataRoot };
            auto existing = std::ranges::find_if(aggregate->GitCheckpoints, [&](const auto& value) { return value.Run == runRecord.Id; });
            const auto checkpointId = existing == aggregate->GitCheckpoints.end() ? NewRuntimeUuid() :
                                                                                    std::optional<std::string>{ existing->Id.Value() };
            if (!checkpointId)
                return;
            if (existing == aggregate->GitCheckpoints.end())
            {
                const auto command = NewRuntimeUuid();
                const auto event = NewRuntimeUuid();
                if (!command || !event)
                    return;
                const auto started = checkpoint.Start(
                    aggregate->Task.Id, runRecord.Id, { CommandId{ *command }, TaskEventId{ *event }, GitCheckpointId{ *checkpointId }, RuntimeUnixMilliseconds(), false }, StopEvent.get());
                if (!started.Succeeded())
                    return;
            }
            for (size_t step = 0; step < 4; ++step)
            {
                const auto command = NewRuntimeUuid();
                const auto event = NewRuntimeUuid();
                if (!command || !event)
                    return;
                const auto advanced = checkpoint.Step(
                    aggregate->Task.Id, runRecord.Id, { CommandId{ *command }, TaskEventId{ *event }, GitCheckpointId{ *checkpointId }, RuntimeUnixMilliseconds(), false }, StopEvent.get());
                if (advanced.Status == RuntimeGitWorktreeCheckpointStatus::Completed)
                    break;
                if (!advanced.Succeeded())
                    return;
            }
        }

        aggregate = Store->LoadTask(initial.Task.Id);
        if (!aggregate)
            return;
        if (aggregate->Terminal.State == TaskTerminalState::Busy)
        {
            static_cast<void>(TransitionTerminal(
                aggregate->Task.Id, TaskTerminalState::Ready, aggregate->Terminal.Generation));
        }

        const auto retry = EvaluateRuntimeTaskAutomaticRetry(runRecord, aggregate->Runs);
        if (retry.Retry && !aggregate->Task.ManualHold)
        {
            if (WaitForSingleObject(StopEvent.get(), retry.BackoffMilliseconds) == WAIT_TIMEOUT)
            {
                aggregate = Store->LoadTask(initial.Task.Id);
                if (aggregate && !aggregate->Task.ManualHold)
                {
                    const auto retryId = NewRuntimeUuid();
                    if (retryId)
                    {
                        CreateTaskRunCommand command;
                        command.Run.Id = TaskRunId{ *retryId };
                        command.Run.Task = aggregate->Task.Id;
                        command.Run.ExecutionContext = runRecord.ExecutionContext;
                        command.Run.RetryOfRunId = runRecord.Id;
                        command.Run.Trigger = TaskRunTrigger::AutomaticRetry;
                        command.Run.State = TaskRunState::Queued;
                        command.Run.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
                        const auto metadata = MakeRuntimeTaskCommandMetadata(
                            "orchestrator-auto-retry-v1", aggregate->Task.Id, *retryId, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
                        if (metadata)
                        {
                            command.Metadata = *metadata;
                            if (Store->CreateTaskRun(command).Succeeded())
                            {
                                const std::scoped_lock lock{ Mutex };
                                KnownRuns.insert_or_assign(*retryId, aggregate->Task.Id.Value());
                            }
                        }
                    }
                }
            }
        }
        else
        {
            ClaimMessages(initial.Task.Id);
        }
        {
            const std::scoped_lock lock{ Mutex };
            KnownRuns.erase(initialRun.Id.Value());
        }
    }
    catch (...)
    {
    }
}
