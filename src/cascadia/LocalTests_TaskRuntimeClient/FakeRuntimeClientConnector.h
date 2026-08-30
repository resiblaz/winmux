// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "FakeRuntimeClientConnection.h"

#include "RuntimeControlConnector.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace TaskRuntimeClientLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    struct FakeRuntimeClientConnectStep
    {
        RuntimeTransportStatus Status{ RuntimeTransportStatus::Succeeded };
        RuntimeControlConnectPhase Phase{ RuntimeControlConnectPhase::Connected };
        HelloResponse Response;
        HandshakeError HandshakeErrorValue{ HandshakeError::None };
        DWORD SystemError{};
        std::string Error;
        std::shared_ptr<FakeRuntimeClientConnectionState> Connection;
        bool Block{};
    };

    struct FakeRuntimeClientConnectCall
    {
        std::string ProtocolIdentity;
        HelloRequest Request;
        uint32_t TimeoutMilliseconds{};
        bool HadCancellationEvent{};
    };

    class FakeRuntimeClientConnector final : public IRuntimeControlConnector
    {
    public:
        FakeRuntimeClientConnector() :
            _connectEntered{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _releaseConnect{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _connectExited{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }
        {
            if (!_connectEntered || !_releaseConnect || !_connectExited)
            {
                if (_connectEntered)
                {
                    CloseHandle(_connectEntered);
                }
                if (_releaseConnect)
                {
                    CloseHandle(_releaseConnect);
                }
                if (_connectExited)
                {
                    CloseHandle(_connectExited);
                }
                throw std::runtime_error{ "Failed to create fake connector events" };
            }
        }

        ~FakeRuntimeClientConnector() override
        {
            CloseHandle(_connectEntered);
            CloseHandle(_releaseConnect);
            CloseHandle(_connectExited);
        }

        void EnqueueSuccess(
            std::shared_ptr<FakeRuntimeClientConnectionState> connection,
            HelloResponse response,
            const bool block = false)
        {
            FakeRuntimeClientConnectStep step;
            step.Response = std::move(response);
            step.Connection = std::move(connection);
            step.Block = block;
            std::scoped_lock lock{ _mutex };
            _steps.emplace_back(std::move(step));
        }

        void EnqueueFailure(
            const RuntimeTransportStatus status,
            const RuntimeControlConnectPhase phase = RuntimeControlConnectPhase::Connecting,
            const bool block = false,
            std::string error = {},
            const HandshakeError handshakeError = HandshakeError::InvalidRequest,
            const DWORD systemError = ERROR_SUCCESS)
        {
            FakeRuntimeClientConnectStep step;
            step.Status = status;
            step.Phase = phase;
            step.Error = std::move(error);
            step.HandshakeErrorValue = handshakeError;
            step.SystemError = systemError;
            step.Block = block;
            std::scoped_lock lock{ _mutex };
            _steps.emplace_back(std::move(step));
        }

        [[nodiscard]] DWORD WaitForConnectEntered(const DWORD timeout = 2000) const noexcept
        {
            return WaitForSingleObject(_connectEntered, timeout);
        }
        [[nodiscard]] DWORD WaitForConnectExited(const DWORD timeout = 2000) const noexcept
        {
            return WaitForSingleObject(_connectExited, timeout);
        }

        void ReleaseConnect() const noexcept
        {
            SetEvent(_releaseConnect);
        }

        void SetConnectHook(std::function<void()> hook)
        {
            std::scoped_lock lock{ _mutex };
            _connectHook = std::move(hook);
        }

        [[nodiscard]] std::vector<FakeRuntimeClientConnectCall> Calls() const
        {
            std::scoped_lock lock{ _mutex };
            return _calls;
        }

        [[nodiscard]] RuntimeControlConnectionResult Connect(
            const RuntimeIdentity& identity,
            const HelloRequest& request,
            const uint32_t timeoutMilliseconds,
            const HANDLE cancellationEvent = nullptr) noexcept override
        {
            struct ExitSignal
            {
                HANDLE Event{};
                ~ExitSignal() { SetEvent(Event); }
            } exitSignal{ _connectExited };
            try
            {
                FakeRuntimeClientConnectStep step;
                std::function<void()> hook;
                {
                    std::scoped_lock lock{ _mutex };
                    _calls.push_back({ identity.ProtocolIdentity, request, timeoutMilliseconds, cancellationEvent != nullptr });
                    hook = std::move(_connectHook);
                }
                if (hook)
                {
                    hook();
                }
                if (cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0)
                {
                    RuntimeControlConnectionResult cancelled;
                    cancelled.Status = RuntimeTransportStatus::Cancelled;
                    cancelled.SystemError = ERROR_OPERATION_ABORTED;
                    return cancelled;
                }
                {
                    std::scoped_lock lock{ _mutex };
                    if (_steps.empty())
                    {
                        RuntimeControlConnectionResult missing;
                        missing.Status = RuntimeTransportStatus::SystemError;
                        missing.Error = "No scripted connector step";
                        return missing;
                    }
                    step = std::move(_steps.front());
                    _steps.pop_front();
                }

                if (step.Block)
                {
                    SetEvent(_connectEntered);
                    const std::array handles{ _releaseConnect, cancellationEvent };
                    const auto wait = WaitForMultipleObjects(
                        cancellationEvent ? 2 : 1,
                        handles.data(),
                        FALSE,
                        timeoutMilliseconds);
                    if (wait == WAIT_OBJECT_0 + 1)
                    {
                        RuntimeControlConnectionResult cancelled;
                        cancelled.Status = RuntimeTransportStatus::Cancelled;
                        cancelled.SystemError = ERROR_OPERATION_ABORTED;
                        return cancelled;
                    }
                    if (wait != WAIT_OBJECT_0)
                    {
                        RuntimeControlConnectionResult timeout;
                        timeout.Status = wait == WAIT_TIMEOUT ? RuntimeTransportStatus::Timeout : RuntimeTransportStatus::SystemError;
                        timeout.SystemError = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
                        return timeout;
                    }
                }

                RuntimeControlConnectionResult result;
                result.Status = step.Status;
                result.Phase = step.Phase;
                result.Response = std::move(step.Response);
                result.HandshakeError = step.HandshakeErrorValue;
                result.SystemError = step.SystemError;
                result.Error = std::move(step.Error);
                if (step.Status == RuntimeTransportStatus::Succeeded && step.Connection)
                {
                    result.Phase = RuntimeControlConnectPhase::Connected;
                    result.Connection = std::make_unique<FakeRuntimeClientConnection>(std::move(step.Connection));
                }
                return result;
            }
            catch (const std::exception& exception)
            {
                RuntimeControlConnectionResult result;
                result.Status = RuntimeTransportStatus::SystemError;
                result.Error = exception.what();
                return result;
            }
            catch (...)
            {
                RuntimeControlConnectionResult result;
                result.Status = RuntimeTransportStatus::SystemError;
                result.Error = "Unknown fake connector failure";
                return result;
            }
        }

    private:
        mutable std::mutex _mutex;
        std::deque<FakeRuntimeClientConnectStep> _steps;
        std::vector<FakeRuntimeClientConnectCall> _calls;
        std::function<void()> _connectHook;
        HANDLE _connectEntered{};
        HANDLE _releaseConnect{};
        HANDLE _connectExited{};
    };
}
