// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskProviderCoordinator.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskProvider;

namespace TaskProviderLocalTests
{
    namespace
    {
        struct FakeAdapterState
        {
            size_t Starts{};
            size_t Stops{};
            size_t SessionStarts{};
            size_t RunStarts{};
            size_t CloseCalls{};
            size_t SuspendCalls{};
            size_t ApprovalCalls{};
            TaskProviderStatus SteerStatus{ TaskProviderStatus::Succeeded };
            TaskProviderStatus InterruptStatus{ TaskProviderStatus::Succeeded };
            std::optional<TaskProviderStatus> NextFailure;
            std::deque<TaskProviderEvent> Events;
        };

        class FakeAdapter final : public ITaskProviderAdapter
        {
        public:
            explicit FakeAdapter(std::shared_ptr<FakeAdapterState> state) : _state{ std::move(state) } {}

            TaskProviderCapabilitiesResult ProbeCapabilities(HANDLE) override
            {
                ++_state->Starts;
                TaskProviderCapabilitiesResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderCapabilities{ "fake", "1", 2, 0, RequiredTaskProviderCapabilities };
                return result;
            }

            TaskProviderSessionResult StartSession(const TaskProviderSessionRequest&, HANDLE) override
            {
                TaskProviderSessionResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderSession{
                    "provider-thread-" + std::to_string(++_state->SessionStarts),
                    TaskProviderSessionState::Ready,
                };
                return result;
            }

            TaskProviderSessionResult ResumeSession(std::string_view sessionId, HANDLE) override
            {
                TaskProviderSessionResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderSession{ std::string{ sessionId }, TaskProviderSessionState::Ready };
                return result;
            }

            TaskProviderRunResult StartRun(const TaskProviderRunRequest& request, HANDLE) override
            {
                TaskProviderRunResult result;
                result.Status = TaskProviderStatus::Succeeded;
                result.Value = TaskProviderRun{
                    request.SessionId,
                    "provider-run-" + std::to_string(++_state->RunStarts),
                    TaskProviderRunState::Running,
                };
                return result;
            }

            TaskProviderResult SteerRun(std::string_view, std::string_view, std::string_view, HANDLE) override
            {
                return { _state->SteerStatus, _state->SteerStatus == TaskProviderStatus::Succeeded ? "" : "steer rejected" };
            }

            TaskProviderResult InterruptRun(std::string_view, std::string_view, HANDLE) override
            {
                return { _state->InterruptStatus, _state->InterruptStatus == TaskProviderStatus::Succeeded ? "" : "interrupt pending" };
            }

            TaskProviderEventResult NextEvent(uint32_t, HANDLE) override
            {
                TaskProviderEventResult result;
                if (_state->NextFailure)
                {
                    result.Status = *_state->NextFailure;
                    result.Error = "scripted provider failure";
                    _state->NextFailure.reset();
                    return result;
                }
                result.Status = TaskProviderStatus::Succeeded;
                if (_state->Events.empty())
                {
                    result.NoEvent = true;
                    return result;
                }
                result.Value = std::move(_state->Events.front());
                _state->Events.pop_front();
                return result;
            }

            TaskProviderResult RespondToApproval(std::string_view, std::string_view, std::string_view, TaskProviderApprovalDecision, HANDLE) override
            {
                ++_state->ApprovalCalls;
                return { TaskProviderStatus::Succeeded, {} };
            }

            TaskProviderResult SuspendSession(std::string_view, HANDLE) override
            {
                ++_state->SuspendCalls;
                return { TaskProviderStatus::Succeeded, {} };
            }

            TaskProviderResult CloseSessionProjection(std::string_view, HANDLE) override
            {
                ++_state->CloseCalls;
                return { TaskProviderStatus::Succeeded, {} };
            }

            TaskProviderFailureDisposition ClassifyFailureAndResumeSafety(TaskProviderStatus, std::string_view) const noexcept override
            {
                return {
                    TaskProviderFailureClass::RuntimeRecovery,
                    TaskProviderResumeSafety::Safe,
                    true,
                    2,
                    "runtime_recovery",
                };
            }

            void Stop() noexcept override
            {
                ++_state->Stops;
            }

        private:
            std::shared_ptr<FakeAdapterState> _state;
        };

        struct RecordingSink final : ITaskProviderCoordinatorSink
        {
            std::vector<TaskProviderSessionBinding> Sessions;
            std::vector<TaskProviderRunBinding> Runs;
            std::vector<TaskProviderFailureDisposition> Failures;
            std::vector<TaskProviderEvent> Events;
            std::vector<std::string> Deferred;

            TaskProviderResult OnSessionStateChanged(const TaskProviderSessionBinding& session) override
            {
                Sessions.emplace_back(session);
                return { TaskProviderStatus::Succeeded, {} };
            }
            TaskProviderResult OnRunStateChanged(const TaskProviderRunBinding& run, const std::optional<TaskProviderFailureDisposition>& failure) override
            {
                Runs.emplace_back(run);
                if (failure) Failures.emplace_back(*failure);
                return { TaskProviderStatus::Succeeded, {} };
            }
            TaskProviderResult OnEvent(const TaskProviderSessionBinding&, const std::optional<TaskProviderRunBinding>&, const TaskProviderEvent& event) override
            {
                Events.emplace_back(event);
                return { TaskProviderStatus::Succeeded, {} };
            }
            TaskProviderResult OnSteeringDeferred(const TaskProviderRunBinding&, const std::string_view message, const std::string_view reason) override
            {
                Deferred.emplace_back(std::string{ reason } + ":" + std::string{ message });
                return { TaskProviderStatus::Succeeded, {} };
            }
        };

        constexpr std::string_view Task1{ "10000000-0000-4000-8000-000000000001" };
        constexpr std::string_view Task2{ "10000000-0000-4000-8000-000000000002" };
        constexpr std::string_view Run1{ "20000000-0000-4000-8000-000000000001" };
        constexpr std::string_view Run2{ "20000000-0000-4000-8000-000000000002" };

        TaskProviderSessionRequest _session(std::string task)
        {
            TaskProviderSessionRequest request;
            request.TaskId = std::move(task);
            request.WorkingDirectory = LR"(C:\work\task)";
            return request;
        }

        TaskProviderRunRequest _run(std::string session, std::string task, std::string run)
        {
            TaskProviderRunRequest request;
            request.SessionId = std::move(session);
            request.Prompt = "work";
            request.Context.TaskId = std::move(task);
            request.Context.RunId = std::move(run);
            return request;
        }
    }

    class TaskProviderCoordinatorTests
    {
        TEST_CLASS(TaskProviderCoordinatorTests);

        TEST_METHOD(LazilySharesOneProviderAndReconcilesCrashAcrossTasks);
        TEST_METHOD(DefersRejectedSteeringDurably);
        TEST_METHOD(ClosePendingDoesNotStopSharedProvider);
        TEST_METHOD(SuspendResumeAndApprovalRemainTaskScoped);
    };

    void TaskProviderCoordinatorTests::LazilySharesOneProviderAndReconcilesCrashAcrossTasks()
    {
        const auto state = std::make_shared<FakeAdapterState>();
        const auto sink = std::make_shared<RecordingSink>();
        size_t factoryCalls{};
        TaskProviderCoordinator coordinator{
            [&, state] {
                ++factoryCalls;
                return std::make_unique<FakeAdapter>(state);
            },
            sink,
        };
        VERIFY_IS_TRUE(coordinator.Valid());
        VERIFY_IS_FALSE(coordinator.ProviderRunning());

        const auto session1 = coordinator.StartSession(_session(std::string{ Task1 }), nullptr);
        const auto session2 = coordinator.StartSession(_session(std::string{ Task2 }), nullptr);
        VERIFY_IS_TRUE(session1.Succeeded());
        VERIFY_IS_TRUE(session2.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, factoryCalls);
        VERIFY_ARE_EQUAL(size_t{ 1 }, state->Starts);
        VERIFY_IS_TRUE(coordinator.StartRun(Task1, _run(session1.Session->ProviderSessionId, std::string{ Task1 }, std::string{ Run1 }), nullptr).Succeeded());
        VERIFY_IS_TRUE(coordinator.StartRun(Task2, _run(session2.Session->ProviderSessionId, std::string{ Task2 }, std::string{ Run2 }), nullptr).Succeeded());

        state->NextFailure = TaskProviderStatus::TransportClosed;
        const auto crashed = coordinator.PumpOneEvent(nullptr);
        VERIFY_IS_TRUE(crashed.Status == TaskProviderStatus::TransportClosed);
        VERIFY_ARE_EQUAL(size_t{ 2 }, crashed.AffectedTaskIds.size());
        VERIFY_ARE_EQUAL(size_t{ 2 }, sink->Failures.size());
        VERIFY_IS_FALSE(coordinator.ProviderRunning());
        VERIFY_ARE_EQUAL(size_t{ 1 }, state->Stops);
        for (const auto& run : coordinator.Runs()) VERIFY_IS_TRUE(run.State == TaskProviderRunState::Interrupted);
    }

    void TaskProviderCoordinatorTests::DefersRejectedSteeringDurably()
    {
        const auto state = std::make_shared<FakeAdapterState>();
        state->SteerStatus = TaskProviderStatus::Rejected;
        const auto sink = std::make_shared<RecordingSink>();
        TaskProviderCoordinator coordinator{ [state] { return std::make_unique<FakeAdapter>(state); }, sink };
        const auto session = coordinator.StartSession(_session(std::string{ Task1 }), nullptr);
        VERIFY_IS_TRUE(session.Succeeded());
        VERIFY_IS_TRUE(coordinator.StartRun(Task1, _run(session.Session->ProviderSessionId, std::string{ Task1 }, std::string{ Run1 }), nullptr).Succeeded());
        const auto steered = coordinator.SteerRun(Task1, Run1, "follow up", nullptr);
        VERIFY_IS_TRUE(steered.Succeeded());
        VERIFY_IS_TRUE(steered.Deferred);
        VERIFY_ARE_EQUAL(size_t{ 1 }, sink->Deferred.size());
        VERIFY_IS_TRUE(sink->Deferred.front().find("provider_steering_rejected") != std::string::npos);
    }

    void TaskProviderCoordinatorTests::ClosePendingDoesNotStopSharedProvider()
    {
        const auto state = std::make_shared<FakeAdapterState>();
        const auto sink = std::make_shared<RecordingSink>();
        TaskProviderCoordinator coordinator{ [state] { return std::make_unique<FakeAdapter>(state); }, sink };
        const auto session1 = coordinator.StartSession(_session(std::string{ Task1 }), nullptr);
        const auto session2 = coordinator.StartSession(_session(std::string{ Task2 }), nullptr);
        VERIFY_IS_TRUE(coordinator.StartRun(Task1, _run(session1.Session->ProviderSessionId, std::string{ Task1 }, std::string{ Run1 }), nullptr).Succeeded());
        VERIFY_IS_TRUE(coordinator.StartRun(Task2, _run(session2.Session->ProviderSessionId, std::string{ Task2 }, std::string{ Run2 }), nullptr).Succeeded());

        state->InterruptStatus = TaskProviderStatus::Timeout;
        const auto closing = coordinator.CloseTask(Task1, nullptr);
        VERIFY_IS_TRUE(closing.Succeeded());
        VERIFY_IS_TRUE(closing.ClosePending);
        VERIFY_ARE_EQUAL(size_t{ 0 }, state->Stops);
        VERIFY_IS_TRUE(coordinator.ProviderRunning());

        TaskProviderEvent terminal;
        terminal.Sequence = 1;
        terminal.Kind = TaskProviderEventKind::RunInterrupted;
        terminal.SessionId = session1.Session->ProviderSessionId;
        terminal.RunId = "provider-run-1";
        state->Events.emplace_back(std::move(terminal));
        const auto pumped = coordinator.PumpOneEvent(nullptr);
        VERIFY_IS_TRUE(pumped.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, state->CloseCalls);
        const auto sessions = coordinator.Sessions();
        const auto closed = std::find_if(sessions.begin(), sessions.end(), [](const auto& value) { return value.TaskId == Task1; });
        const auto other = std::find_if(sessions.begin(), sessions.end(), [](const auto& value) { return value.TaskId == Task2; });
        VERIFY_IS_TRUE(closed->State == TaskProviderSessionState::Closed);
        VERIFY_IS_TRUE(other->State == TaskProviderSessionState::Busy);
    }

    void TaskProviderCoordinatorTests::SuspendResumeAndApprovalRemainTaskScoped()
    {
        const auto state = std::make_shared<FakeAdapterState>();
        const auto sink = std::make_shared<RecordingSink>();
        TaskProviderCoordinator coordinator{ [state] { return std::make_unique<FakeAdapter>(state); }, sink };
        auto session = coordinator.ResumeSession(Task1, "resumed-thread", nullptr);
        VERIFY_IS_TRUE(session.Succeeded());
        VERIFY_IS_TRUE(coordinator.SuspendTask(Task1, nullptr).Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, state->SuspendCalls);

        VERIFY_IS_TRUE(coordinator.StartRun(Task1, _run("resumed-thread", std::string{ Task1 }, std::string{ Run1 }), nullptr).Succeeded());
        VERIFY_IS_TRUE(coordinator.RespondToApproval(
            Task1, Run1, "approval-1", TaskProviderApprovalDecision::Denied, nullptr).Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 1 }, state->ApprovalCalls);
        VERIFY_IS_TRUE(coordinator.RespondToApproval(
            Task2, Run1, "approval-1", TaskProviderApprovalDecision::Denied, nullptr).Status == TaskProviderStatus::NotFound);
    }
}
