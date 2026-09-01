// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTaskOrchestratorInternal.h"

#include "RuntimeTaskCommandMetadata.h"
#include "RuntimeCrypto.h"
#include "ProtocolValidation.h"
#include "RuntimeTaskSchedulingPolicy.h"

#include "TaskRunStatePolicy.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;

    bool RuntimeTaskOrchestratorConfiguration::Valid() const noexcept
    {
        return MaximumConcurrentPreparations != 0 && MaximumConcurrentPreparations <= 16 &&
               ActivePollMilliseconds >= 50 && ActivePollMilliseconds <= 5000 &&
               IdlePollMilliseconds >= ActivePollMilliseconds && IdlePollMilliseconds <= 60000 &&
               TerminalIdleMilliseconds >= 1000;
    }

    RuntimeTaskOrchestrator::Impl::Impl(
        std::shared_ptr<ITaskStore> store,
        std::shared_ptr<RuntimeTaskProviderService> provider,
        std::shared_ptr<RuntimeRtaskCommandService> rtask,
        std::shared_ptr<RuntimeTaskTerminalService> terminals,
        std::filesystem::path runtimeDataRoot,
        std::filesystem::path gitExecutable,
        std::filesystem::path rtaskExecutable,
        std::string rtaskEndpoint,
        RuntimeTaskOrchestratorConfiguration configuration) :
        Store{ std::move(store) },
        Provider{ std::move(provider) },
        Rtask{ std::move(rtask) },
        Terminals{ std::move(terminals) },
        RuntimeDataRoot{ std::move(runtimeDataRoot) },
        GitExecutable{ std::move(gitExecutable) },
        RtaskExecutable{ std::move(rtaskExecutable) },
        RtaskEndpoint{ std::move(rtaskEndpoint) },
        Configuration{ std::move(configuration) },
        StopEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
    {
    }

    bool RuntimeTaskOrchestrator::Impl::Valid() const noexcept
    {
        return Store && Store->IsReady() && Provider && Provider->Valid() &&
               Rtask && Rtask->Valid() && Terminals && Terminals->Valid() &&
               !RuntimeDataRoot.empty() &&
               !RtaskExecutable.empty() && RtaskExecutable.is_absolute() &&
               !RtaskEndpoint.empty() && Configuration.Valid() && static_cast<bool>(StopEvent);
    }

    RuntimeTaskOrchestrator::RuntimeTaskOrchestrator(
        std::shared_ptr<ITaskStore> store,
        std::shared_ptr<RuntimeTaskProviderService> provider,
        std::shared_ptr<RuntimeRtaskCommandService> rtask,
        std::shared_ptr<RuntimeTaskTerminalService> terminals,
        std::filesystem::path runtimeDataRoot,
        std::filesystem::path gitExecutable,
        std::filesystem::path rtaskExecutable,
        std::string rtaskEndpoint,
        RuntimeTaskOrchestratorConfiguration configuration)
    {
        try
        {
            _impl = std::make_unique<Impl>(
                std::move(store), std::move(provider), std::move(rtask), std::move(terminals), std::move(runtimeDataRoot), std::move(gitExecutable), std::move(rtaskExecutable), std::move(rtaskEndpoint), std::move(configuration));
        }
        catch (...)
        {
        }
    }

    RuntimeTaskOrchestrator::~RuntimeTaskOrchestrator()
    {
        Stop();
    }

    bool RuntimeTaskOrchestrator::Valid() const noexcept
    {
        return _impl && _impl->Valid();
    }

    bool RuntimeTaskOrchestrator::Start()
    {
        if (!Valid())
            return false;
        const std::scoped_lock lock{ _impl->Mutex };
        if (_impl->Started)
            return true;
        if (_impl->Stopping)
            return false;
        try
        {
            _impl->Scheduler = std::thread{ [impl = _impl.get()] { impl->Run(); } };
            _impl->Started = true;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void RuntimeTaskOrchestrator::Notify() noexcept
    {
        if (_impl)
        {
            _impl->Changed.notify_all();
        }
    }

    bool RuntimeTaskOrchestrator::Running() const noexcept
    {
        if (!_impl)
            return false;
        const std::scoped_lock lock{ _impl->Mutex };
        return _impl->Started && !_impl->Stopping;
    }

    bool RuntimeTaskOrchestrator::SubmitTerminalMessage(
        const std::string_view taskId,
        const std::span<const uint8_t> message) noexcept
    try
    {
        const auto task = ParseId<TaskId>(taskId);
        if (!_impl || !task || message.empty() || message.size() > MaximumTaskMessageBytes ||
            !Microsoft::Terminal::TaskRuntimeProtocol::IsValidUtf8(message))
        {
            return false;
        }
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = _impl->Store->LoadTask(*task);
            const auto messageId = NewRuntimeUuid();
            if (!aggregate || !messageId)
                return false;
            EnqueueTaskMessageCommand command;
            command.Message.Id = TaskMessageId{ *messageId };
            command.Message.Task = *task;
            command.Message.Author = TaskMessageAuthor::User;
            command.Message.State = TaskMessageState::Pending;
            command.Message.Revision = 1;
            command.Message.Content.assign(
                reinterpret_cast<const char*>(message.data()), message.size());
            command.Message.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
            command.Message.UpdatedAtUnixMilliseconds = command.Message.CreatedAtUnixMilliseconds;
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "terminal-message-v1", *task, *messageId, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return false;
            command.Metadata = *metadata;
            const auto stored = _impl->Store->EnqueueTaskMessage(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict)
                continue;
            if (stored.Succeeded())
                Notify();
            return stored.Succeeded();
        }
        return false;
    }
    catch (...)
    {
        return false;
    }

    bool RuntimeTaskOrchestrator::RequestStop(const std::string_view taskId) noexcept
    try
    {
        const auto task = ParseId<TaskId>(taskId);
        if (!_impl || !task)
            return false;
        const auto aggregate = _impl->Store->LoadTask(*task);
        if (!aggregate || !std::ranges::any_of(aggregate->Runs, [](const auto& run) {
                return IsTaskRunActive(run.State);
            }))
        {
            return false;
        }
        if (aggregate->Intent && aggregate->Intent->Stage != TaskIntentStage::Completed)
        {
            Notify();
            return aggregate->Intent->Kind == TaskIntentKind::Stop;
        }
        const auto intentId = NewRuntimeUuid();
        if (!intentId)
            return false;
        StartTaskIntentCommand command;
        command.Intent.Id = TaskIntentId{ *intentId };
        command.Intent.Task = *task;
        command.Intent.Kind = TaskIntentKind::Stop;
        command.Intent.Stage = TaskIntentStage::Requested;
        command.Intent.DetailJson = R"({"source":"task_terminal_escape"})";
        command.Intent.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
        command.Intent.UpdatedAtUnixMilliseconds = command.Intent.CreatedAtUnixMilliseconds;
        const auto metadata = MakeRuntimeTaskCommandMetadata(
            "terminal-stop-v1", *task, *intentId, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
        if (!metadata)
            return false;
        command.Metadata = *metadata;
        const auto stored = _impl->Store->StartTaskIntent(command);
        if (stored.Succeeded())
            Notify();
        return stored.Succeeded();
    }
    catch (...)
    {
        return false;
    }

    void RuntimeTaskOrchestrator::Stop() noexcept
    {
        if (!_impl)
            return;
        {
            const std::scoped_lock lock{ _impl->Mutex };
            if (_impl->Stopping)
                return;
            _impl->Stopping = true;
        }
        SetEvent(_impl->StopEvent.get());
        _impl->Changed.notify_all();
        if (_impl->Scheduler.joinable())
            _impl->Scheduler.join();
        std::vector<std::thread> threads;
        {
            const std::scoped_lock lock{ _impl->Mutex };
            for (auto& [_, worker] : _impl->Workers)
            {
                threads.emplace_back(std::move(worker->Thread));
            }
            _impl->Workers.clear();
        }
        for (auto& thread : threads)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    void RuntimeTaskOrchestrator::Impl::Run() noexcept
    {
        Recover();
        while (WaitForSingleObject(StopEvent.get(), 0) == WAIT_TIMEOUT)
        {
            Tick();
            std::unique_lock lock{ Mutex };
            Changed.wait_for(
                lock,
                std::chrono::milliseconds{ Configuration.IdlePollMilliseconds },
                [&] { return Stopping; });
        }
        ReapWorkers();
    }

    void RuntimeTaskOrchestrator::Impl::Recover() noexcept
    {
        try
        {
            for (const auto& task : Store->ListTasks(std::nullopt))
            {
                const auto aggregate = Store->LoadTask(task.Id);
                if (!aggregate)
                    continue;
                for (const auto& run : aggregate->Runs)
                {
                    if (run.State == TaskRunState::Queued)
                    {
                        KnownRuns.insert_or_assign(run.Id.Value(), task.Id.Value());
                    }
                    else if (IsTaskRunActive(run.State) &&
                             (run.AgentSession || !aggregate->AgentSessions.empty()))
                    {
                        static_cast<void>(TransitionRun(task.Id, run.Id, TaskRunState::Interrupted, "runtime_recovery"));
                        KnownRuns.insert_or_assign(run.Id.Value(), task.Id.Value());
                    }
                    else if (run.State == TaskRunState::Failed || run.State == TaskRunState::Interrupted)
                    {
                        const auto retry = EvaluateRuntimeTaskAutomaticRetry(run, aggregate->Runs);
                        if (retry.Retry)
                        {
                            KnownRuns.insert_or_assign(run.Id.Value(), task.Id.Value());
                        }
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    void RuntimeTaskOrchestrator::Impl::Tick() noexcept
    try
    {
        ReapWorkers();
        const auto recovery = Store->LoadRecoverySnapshot(MaximumTaskRecoveryQueryCount);
        if (!recovery.Succeeded)
            return;

        std::unordered_set<std::string> touchedTasks;
        for (const auto& run : recovery.Snapshot.ActiveRuns)
        {
            {
                const std::scoped_lock lock{ Mutex };
                KnownRuns.insert_or_assign(run.Id.Value(), run.Task.Value());
            }
            touchedTasks.emplace(run.Task.Value());
            if (run.State == TaskRunState::Queued)
            {
                const auto aggregate = Store->LoadTask(run.Task);
                if (aggregate && !aggregate->Task.ManualHold)
                {
                    StartWorker(run.Task, run.Id);
                }
            }
        }

        std::vector<std::pair<std::string, std::string>> known;
        {
            const std::scoped_lock lock{ Mutex };
            known.assign(KnownRuns.begin(), KnownRuns.end());
        }
        for (const auto& [runId, taskId] : known)
        {
            const auto task = ParseId<TaskId>(taskId);
            const auto run = ParseId<TaskRunId>(runId);
            if (!task || !run)
                continue;
            const auto aggregate = Store->LoadTask(*task);
            if (!aggregate)
            {
                const std::scoped_lock lock{ Mutex };
                KnownRuns.erase(runId);
                continue;
            }
            const auto record = std::ranges::find(aggregate->Runs, *run, &TaskRunRecord::Id);
            if (record != aggregate->Runs.end() && IsTaskRunTerminal(record->State))
            {
                StartWorker(*task, *run);
            }
            touchedTasks.emplace(taskId);
        }

        for (const auto& message : recovery.Snapshot.UnclaimedMessages)
        {
            if (touchedTasks.emplace(message.Task.Value()).second)
            {
                ClaimMessages(message.Task);
            }
        }

        for (const auto& intent : recovery.Snapshot.PendingIntents)
        {
            HandleIntent(intent);
        }

        for (const auto& terminal : recovery.Snapshot.RecoverableTerminals)
        {
            if (const auto aggregate = Store->LoadTask(terminal.Task))
            {
                ObserveTerminalIdle(*aggregate);
            }
        }

        const auto now = GetTickCount64();
        if (LastMaintenance == 0 || now - LastMaintenance >= 60'000)
        {
            static_cast<void>(Store->PurgeExpiredTombstones(RuntimeUnixMilliseconds(), 16));
            LastMaintenance = now;
        }
    }
    catch (...)
    {
    }

    void RuntimeTaskOrchestrator::Impl::ReapWorkers() noexcept
    {
        std::vector<std::thread> completed;
        {
            const std::scoped_lock lock{ Mutex };
            for (auto iterator = Workers.begin(); iterator != Workers.end();)
            {
                if (iterator->second->Finished.load(std::memory_order_acquire))
                {
                    completed.emplace_back(std::move(iterator->second->Thread));
                    iterator = Workers.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
        }
        for (auto& thread : completed)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    void RuntimeTaskOrchestrator::Impl::StartWorker(const TaskId& task, const TaskRunId& run) noexcept
    try
    {
        const std::scoped_lock lock{ Mutex };
        if (Stopping || Workers.contains(run.Value()) ||
            Workers.size() >= Configuration.MaximumConcurrentPreparations)
        {
            return;
        }
        auto worker = std::make_shared<Worker>();
        Workers.emplace(run.Value(), worker);
        try
        {
            worker->Thread = std::thread{ [this, worker, task, run] {
                ExecuteRun(task, run);
                worker->Finished.store(true, std::memory_order_release);
                Changed.notify_all();
            } };
        }
        catch (...)
        {
            Workers.erase(run.Value());
        }
    }
    catch (...)
    {
    }

    void RuntimeTaskOrchestrator::Impl::ClaimMessages(const TaskId& task) noexcept
    try
    {
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = Store->LoadTask(task);
            if (!aggregate || !ShouldClaimRuntimeTaskMessages(*aggregate) || aggregate->ExecutionContexts.empty())
                return;
            const auto runId = NewRuntimeUuid();
            if (!runId)
                return;
            ClaimTaskMessagesAndCreateRunCommand command;
            command.Run.Id = TaskRunId{ *runId };
            command.Run.Task = task;
            command.Run.ExecutionContext = aggregate->ExecutionContexts.back().Id;
            command.Run.Trigger = TaskRunTrigger::FollowUp;
            command.Run.State = TaskRunState::Queued;
            command.Run.CreatedAtUnixMilliseconds = (std::max)(RuntimeUnixMilliseconds(), aggregate->Task.UpdatedAtUnixMilliseconds);
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "orchestrator-follow-up-v1", task, *runId, aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return;
            command.Metadata = *metadata;
            const auto stored = Store->ClaimTaskMessagesAndCreateRun(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict)
                continue;
            if (stored.Succeeded())
            {
                {
                    const std::scoped_lock lock{ Mutex };
                    KnownRuns.insert_or_assign(*runId, task.Value());
                }
                StartWorker(task, command.Run.Id);
            }
            return;
        }
    }
    catch (...)
    {
    }

    void RuntimeTaskOrchestrator::Impl::ObserveTerminalIdle(
        const TaskAggregateSnapshot& aggregate) noexcept
    try
    {
        const auto taskId = aggregate.Task.Id.Value();
        const auto active = std::ranges::any_of(aggregate.Runs, [](const auto& run) { return IsTaskRunActive(run.State); });
        const auto live = aggregate.Terminal.State == TaskTerminalState::Ready;
        if (!live || active || Terminals->ClientCount(taskId) != 0)
        {
            TerminalIdleDeadlines.erase(taskId);
            return;
        }
        const auto now = GetTickCount64();
        const auto [found, inserted] = TerminalIdleDeadlines.emplace(taskId, now + Configuration.TerminalIdleMilliseconds);
        if (inserted || now < found->second)
            return;
        const auto suspended = Provider->SuspendTask(taskId, StopEvent.get());
        if (suspended.Succeeded() ||
            suspended.Status == Microsoft::Terminal::TaskProvider::TaskProviderStatus::NotFound)
        {
            static_cast<void>(Terminals->Close(taskId, aggregate.Terminal.Generation, false));
            static_cast<void>(TransitionTerminal(aggregate.Task.Id, TaskTerminalState::Suspended, aggregate.Terminal.Generation));
        }
        TerminalIdleDeadlines.erase(taskId);
    }
    catch (...)
    {
    }

    bool RuntimeTaskOrchestrator::Impl::TransitionRun(
        const TaskId& task,
        const TaskRunId& run,
        const TaskRunState state,
        std::string reason) noexcept
    try
    {
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = Store->LoadTask(task);
            if (!aggregate)
                return false;
            const auto current = std::ranges::find(aggregate->Runs, run, &TaskRunRecord::Id);
            if (current == aggregate->Runs.end())
                return false;
            if (current->State == state)
                return true;
            ChangeTaskRunStateCommand command;
            command.Task = task;
            command.Run = run;
            command.RequestedState = state;
            command.ReasonCode = reason;
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "orchestrator-run-state-v1", task, run.Value() + ":" + std::to_string(static_cast<uint32_t>(state)), aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return false;
            command.Metadata = *metadata;
            const auto stored = Store->ChangeTaskRunState(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict)
                continue;
            return stored.Succeeded();
        }
        return false;
    }
    catch (...)
    {
        return false;
    }

    bool RuntimeTaskOrchestrator::Impl::TransitionTerminal(
        const TaskId& task,
        const TaskTerminalState state,
        const uint64_t generation,
        const bool explicitOpen) noexcept
    try
    {
        for (size_t attempt = 0; attempt < 3; ++attempt)
        {
            const auto aggregate = Store->LoadTask(task);
            if (!aggregate)
                return false;
            if (aggregate->Terminal.State == state && aggregate->Terminal.Generation == generation)
                return true;
            ChangeTaskTerminalStateCommand command;
            command.Task = task;
            command.RequestedState = state;
            command.RequestedGeneration = generation;
            command.ExplicitOpen = explicitOpen;
            const auto metadata = MakeRuntimeTaskCommandMetadata(
                "orchestrator-terminal-state-v1", task, std::to_string(generation) + ":" + std::to_string(static_cast<uint32_t>(state)), aggregate->Task.Version, aggregate->Task.UpdatedAtUnixMilliseconds);
            if (!metadata)
                return false;
            command.Metadata = *metadata;
            const auto stored = Store->ChangeTaskTerminalState(command);
            if (stored.Status == TaskStoreCommandStatus::VersionConflict)
                continue;
            return stored.Succeeded();
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}
