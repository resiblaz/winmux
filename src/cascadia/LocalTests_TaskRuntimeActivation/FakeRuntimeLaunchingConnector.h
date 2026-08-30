// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeLaunchingConnector.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace TaskRuntimeActivationLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    class FakeConnectedControlConnection final : public IRuntimeControlConnection
    {
    public:
        bool Connected() const noexcept override
        {
            return _connected.load(std::memory_order_acquire);
        }

        RuntimeTransportResult Send(
            std::string_view,
            uint32_t,
            HANDLE = nullptr) override
        {
            RuntimeTransportResult result;
            result.Status = Connected() ?
                                RuntimeTransportStatus::Succeeded :
                                RuntimeTransportStatus::Disconnected;
            return result;
        }

        RuntimeReceiveResult Receive(uint32_t, HANDLE = nullptr) override
        {
            RuntimeReceiveResult result;
            result.Status = Connected() ?
                                RuntimeTransportStatus::Timeout :
                                RuntimeTransportStatus::Disconnected;
            return result;
        }

        void CancelPendingIo() noexcept override
        {
            _connected.store(false, std::memory_order_release);
        }

        void Close() noexcept override
        {
            _connected.store(false, std::memory_order_release);
        }

    private:
        std::atomic_bool _connected{ true };
    };

    inline RuntimeControlConnectionResult ConnectedResult()
    {
        RuntimeControlConnectionResult result;
        result.Status = RuntimeTransportStatus::Succeeded;
        result.Phase = RuntimeControlConnectPhase::Connected;
        result.FailureReason = RuntimeControlConnectFailureReason::None;
        result.Connection = std::make_unique<FakeConnectedControlConnection>();
        return result;
    }

    inline RuntimeControlConnectionResult AbsentResult()
    {
        RuntimeControlConnectionResult result;
        result.Status = RuntimeTransportStatus::Disconnected;
        result.SystemError = ERROR_FILE_NOT_FOUND;
        result.Phase = RuntimeControlConnectPhase::Connecting;
        result.FailureReason = RuntimeControlConnectFailureReason::EndpointAbsent;
        return result;
    }

    inline RuntimeControlConnectionResult FailureResult(
        const RuntimeTransportStatus status,
        const RuntimeControlConnectFailureReason reason,
        const RuntimeControlConnectPhase phase = RuntimeControlConnectPhase::Connecting)
    {
        RuntimeControlConnectionResult result;
        result.Status = status;
        result.Phase = phase;
        result.FailureReason = reason;
        result.SystemError = status == RuntimeTransportStatus::Cancelled ?
                                 ERROR_OPERATION_ABORTED :
                                 ERROR_GEN_FAILURE;
        return result;
    }

    class FakeDirectConnector final : public IRuntimeControlConnector
    {
    public:
        RuntimeControlConnectionResult Connect(
            const RuntimeIdentity&,
            const HelloRequest&,
            const uint32_t timeoutMilliseconds,
            const HANDLE cancellationEvent) noexcept override
        {
            try
            {
                std::scoped_lock lock{ _mutex };
                Timeouts.emplace_back(timeoutMilliseconds);
                CancellationEvents.emplace_back(cancellationEvent);
                ++Calls;
                if (_handler)
                {
                    return _handler();
                }
                if (_results.empty())
                {
                    return FailureResult(
                        RuntimeTransportStatus::SystemError,
                        RuntimeControlConnectFailureReason::Unclassified);
                }
                auto result = std::move(_results.front());
                _results.pop_front();
                return result;
            }
            catch (...)
            {
                return FailureResult(
                    RuntimeTransportStatus::SystemError,
                    RuntimeControlConnectFailureReason::ResourceFailure);
            }
        }

        void Enqueue(RuntimeControlConnectionResult result)
        {
            std::scoped_lock lock{ _mutex };
            _results.emplace_back(std::move(result));
        }

        void SetHandler(std::function<RuntimeControlConnectionResult()> handler)
        {
            std::scoped_lock lock{ _mutex };
            _handler = std::move(handler);
        }

        std::atomic_size_t Calls{};
        std::vector<uint32_t> Timeouts;
        std::vector<HANDLE> CancellationEvents;

    private:
        std::mutex _mutex;
        std::deque<RuntimeControlConnectionResult> _results;
        std::function<RuntimeControlConnectionResult()> _handler;
    };

    struct FakeLeaseState
    {
        std::atomic_size_t Active{};
        std::atomic_size_t Released{};
    };

    class FakeLaunchLockLease final : public IRuntimeLaunchLockLease
    {
    public:
        FakeLaunchLockLease(std::shared_ptr<FakeLeaseState> state, const bool abandoned) :
            _state{ std::move(state) },
            _abandoned{ abandoned }
        {
            _state->Active.fetch_add(1, std::memory_order_relaxed);
        }

        ~FakeLaunchLockLease() override
        {
            _state->Active.fetch_sub(1, std::memory_order_relaxed);
            _state->Released.fetch_add(1, std::memory_order_relaxed);
        }

        bool Abandoned() const noexcept override
        {
            return _abandoned;
        }

    private:
        std::shared_ptr<FakeLeaseState> _state;
        bool _abandoned{};
    };

    class FakeLaunchLockProvider final : public IRuntimeLaunchLockProvider
    {
    public:
        RuntimeLaunchLockLeaseResult Acquire(
            const RuntimeIdentity&,
            const RuntimeActivationDeadline deadline,
            const HANDLE cancellationEvent,
            IRuntimeActivationServices&) noexcept override
        {
            std::scoped_lock lock{ _mutex };
            ++Calls;
            Deadlines.emplace_back(deadline);
            CancellationEvents.emplace_back(cancellationEvent);
            RuntimeLaunchLockLeaseResult result;
            result.Status = Status;
            result.SystemError = SystemError;
            if (Status == RuntimeLaunchLockStatus::Acquired)
            {
                result.Lease = std::make_unique<FakeLaunchLockLease>(State, Abandoned);
            }
            return result;
        }

        std::atomic_size_t Calls{};
        RuntimeLaunchLockStatus Status{ RuntimeLaunchLockStatus::Acquired };
        DWORD SystemError{};
        bool Abandoned{};
        std::shared_ptr<FakeLeaseState> State{ std::make_shared<FakeLeaseState>() };
        std::vector<RuntimeActivationDeadline> Deadlines;
        std::vector<HANDLE> CancellationEvents;

    private:
        std::mutex _mutex;
    };

    class FakeProcessLauncher final : public IRuntimeProcessLauncher
    {
    public:
        RuntimeProcessLaunchResult Launch(
            const RuntimeProcessLaunchRequest& request) noexcept override
        {
            try
            {
                std::scoped_lock lock{ _mutex };
                ++Calls;
                Requests.emplace_back(request);
                return Handler ? Handler(request) : RuntimeProcessLaunchResult{};
            }
            catch (...)
            {
                RuntimeProcessLaunchResult result;
                result.Status = RuntimeProcessLaunchStatus::ResourceFailure;
                result.SystemError = ERROR_NOT_ENOUGH_MEMORY;
                return result;
            }
        }

        std::atomic_size_t Calls{};
        std::function<RuntimeProcessLaunchResult(const RuntimeProcessLaunchRequest&)> Handler;
        std::vector<RuntimeProcessLaunchRequest> Requests;

    private:
        std::mutex _mutex;
    };
}
