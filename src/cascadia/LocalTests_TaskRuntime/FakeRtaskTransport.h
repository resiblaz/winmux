// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RtaskClient.h"

#include <deque>

namespace TaskRuntimeLocalTests
{
    struct FakeRtaskTransportState
    {
        Microsoft::Terminal::TaskRuntime::RuntimeIdentity Identity;
        Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest Hello;
        Microsoft::Terminal::TaskRuntime::RuntimeTransportResult SendResult{
            Microsoft::Terminal::TaskRuntime::RuntimeTransportStatus::Succeeded
        };
        Microsoft::Terminal::TaskRuntime::RuntimeReceiveResult ReceiveResult;
        std::vector<std::string> SentPayloads;
        uint32_t ConnectTimeout{};
        uint32_t SendTimeout{};
        uint32_t ReceiveTimeout{};
        bool ConnectFails{};
        bool Closed{};
    };

    class FakeRtaskConnection final :
        public Microsoft::Terminal::TaskRuntime::IRuntimeControlConnection
    {
    public:
        explicit FakeRtaskConnection(std::shared_ptr<FakeRtaskTransportState> state) :
            _state{ std::move(state) }
        {
        }

        [[nodiscard]] bool Connected() const noexcept override
        {
            return _state && !_state->Closed;
        }

        [[nodiscard]] Microsoft::Terminal::TaskRuntime::RuntimeTransportResult Send(
            const std::string_view payload,
            const uint32_t timeoutMilliseconds,
            HANDLE) override
        {
            _state->SendTimeout = timeoutMilliseconds;
            _state->SentPayloads.emplace_back(payload);
            return _state->SendResult;
        }

        [[nodiscard]] Microsoft::Terminal::TaskRuntime::RuntimeReceiveResult Receive(
            const uint32_t timeoutMilliseconds,
            HANDLE) override
        {
            _state->ReceiveTimeout = timeoutMilliseconds;
            return _state->ReceiveResult;
        }

        void CancelPendingIo() noexcept override
        {
            _state->Closed = true;
        }

        void Close() noexcept override
        {
            _state->Closed = true;
        }

    private:
        std::shared_ptr<FakeRtaskTransportState> _state;
    };

    class FakeRtaskConnector final :
        public Microsoft::Terminal::TaskRuntime::IRuntimeControlConnector
    {
    public:
        explicit FakeRtaskConnector(std::shared_ptr<FakeRtaskTransportState> state) :
            _state{ std::move(state) }
        {
        }

        [[nodiscard]] Microsoft::Terminal::TaskRuntime::RuntimeControlConnectionResult Connect(
            const Microsoft::Terminal::TaskRuntime::RuntimeIdentity& identity,
            const Microsoft::Terminal::TaskRuntimeProtocol::HelloRequest& request,
            const uint32_t timeoutMilliseconds,
            HANDLE) noexcept override
        {
            Microsoft::Terminal::TaskRuntime::RuntimeControlConnectionResult result;
            try
            {
                _state->Identity = identity;
                _state->Hello = request;
                _state->ConnectTimeout = timeoutMilliseconds;
                if (_state->ConnectFails)
                {
                    result.Status = Microsoft::Terminal::TaskRuntime::RuntimeTransportStatus::Disconnected;
                    result.Error = "fake connect failure";
                    return result;
                }
                _state->Closed = false;
                result.Connection = std::make_unique<FakeRtaskConnection>(_state);
                result.Phase = Microsoft::Terminal::TaskRuntime::RuntimeControlConnectPhase::Connected;
                result.Status = Microsoft::Terminal::TaskRuntime::RuntimeTransportStatus::Succeeded;
                return result;
            }
            catch (...)
            {
                result.Status = Microsoft::Terminal::TaskRuntime::RuntimeTransportStatus::SystemError;
                result.Error = "fake connector resource failure";
                return result;
            }
        }

    private:
        std::shared_ptr<FakeRtaskTransportState> _state;
    };

    class FakeRtaskIdSource final : public Microsoft::Terminal::Rtask::IRtaskIdSource
    {
    public:
        [[nodiscard]] std::optional<std::string> Next() override
        {
            if (Ids.empty())
            {
                return std::nullopt;
            }
            auto result = std::move(Ids.front());
            Ids.pop_front();
            return result;
        }

        std::deque<std::string> Ids;
    };
}
