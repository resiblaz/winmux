// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeControlTransport.h"
#include "ProtocolRpcJson.h"

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace TaskRuntimeClientLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    enum class FakeConnectionLifecycleAction { CancelPendingIo, Close };
    using FakeRuntimeClientAutoResponseHandler = std::function<ControlResponse(const ControlRequest&)>;

    class FakeRuntimeClientIoScope final
    {
    public:
        explicit FakeRuntimeClientIoScope(
            std::atomic_size_t& active,
            const HANDLE exited = nullptr) noexcept :
            _active{ active },
            _exited{ exited }
        {
            _active.fetch_add(1, std::memory_order_acq_rel);
        }

        ~FakeRuntimeClientIoScope()
        {
            _active.fetch_sub(1, std::memory_order_acq_rel);
            if (_exited)
            {
                SetEvent(_exited);
            }
        }

        FakeRuntimeClientIoScope(const FakeRuntimeClientIoScope&) = delete;
        FakeRuntimeClientIoScope& operator=(const FakeRuntimeClientIoScope&) = delete;

    private:
        std::atomic_size_t& _active;
        HANDLE _exited{};
    };

    class FakeRuntimeClientConnectionState final
    {
    public:
        FakeRuntimeClientConnectionState() :
            _changed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            _terminal{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            _sendEntered{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _receiveEntered{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _responseQueued{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _releaseSend{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _sendExited{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            _receiveExited{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            _inboundDequeued{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _cancelEntered{ CreateEventW(nullptr, FALSE, FALSE, nullptr) },
            _releaseCancel{ CreateEventW(nullptr, FALSE, FALSE, nullptr) }
        {
            if (!_changed || !_terminal || !_sendEntered || !_receiveEntered || !_responseQueued ||
                !_releaseSend || !_sendExited || !_receiveExited || !_inboundDequeued ||
                !_cancelEntered || !_releaseCancel)
            {
                _closeHandles();
                throw std::runtime_error{ "Failed to create fake connection events" };
            }
        }
        ~FakeRuntimeClientConnectionState() { _closeHandles(); }
        FakeRuntimeClientConnectionState(const FakeRuntimeClientConnectionState&) = delete;
        FakeRuntimeClientConnectionState& operator=(const FakeRuntimeClientConnectionState&) = delete;

        void QueueInboundPayload(std::string payload)
        {
            RuntimeReceiveResult result;
            result.Status = RuntimeTransportStatus::Succeeded;
            result.Payload = std::move(payload);
            _queueInbound(std::move(result));
        }
        bool QueueInboundResponse(const ControlResponse& response)
        {
            const auto encoded = EncodeControlResponse(response);
            if (!encoded.Succeeded) { return false; }
            QueueInboundPayload(encoded.Payload);
            return true;
        }
        bool QueueInboundNotification(const ControlNotification& notification)
        {
            const auto encoded = EncodeControlNotification(notification);
            if (!encoded.Succeeded) { return false; }
            QueueInboundPayload(encoded.Payload);
            return true;
        }
        void QueueInboundFailure(RuntimeTransportStatus status, std::string error = {}, DWORD systemError = 0)
        {
            RuntimeReceiveResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            _queueInbound(std::move(result));
        }
        void QueueSendFailure(RuntimeTransportStatus status, std::string error = {}, DWORD systemError = 0)
        {
            RuntimeTransportResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            std::scoped_lock lock{ _mutex };
            _sendResults.emplace_back(std::move(result));
        }
        void EnableAutoResponse(const bool enabled = true)
        {
            std::scoped_lock lock{ _mutex };
            _autoResponse = enabled;
        }
        void SetNextAutoResponseHandler(FakeRuntimeClientAutoResponseHandler handler)
        {
            std::scoped_lock lock{ _mutex };
            _nextAutoResponseHandler = std::move(handler);
        }
        void SetAutoResponseHandler(FakeRuntimeClientAutoResponseHandler handler)
        {
            std::scoped_lock lock{ _mutex };
            _autoResponseHandler = std::move(handler);
        }
        void BlockNextSendAfterResponse()
        {
            std::scoped_lock lock{ _mutex };
            _blockNextSend = true;
        }
        void SetSendHook(std::function<void()> hook)
        {
            std::scoped_lock lock{ _mutex };
            _sendHook = std::move(hook);
        }
        void SetReceiveHook(std::function<void()> hook)
        {
            std::scoped_lock lock{ _mutex };
            _receiveHook = std::move(hook);
        }
        void SetCancelHook(std::function<void()> hook)
        {
            std::scoped_lock lock{ _mutex };
            _cancelHook = std::move(hook);
        }
        void SetCloseHook(std::function<void()> hook)
        {
            std::scoped_lock lock{ _mutex };
            _closeHook = std::move(hook);
        }
        void ReleaseSend() const noexcept { SetEvent(_releaseSend); }
        void BlockNextCancel()
        {
            std::scoped_lock lock{ _mutex };
            _blockNextCancel = true;
        }
        void ReleaseCancel() const noexcept { SetEvent(_releaseCancel); }
        [[nodiscard]] HANDLE SendEnteredEvent() const noexcept { return _sendEntered; }
        [[nodiscard]] HANDLE ReceiveEnteredEvent() const noexcept { return _receiveEntered; }
        [[nodiscard]] HANDLE ResponseQueuedEvent() const noexcept { return _responseQueued; }
        [[nodiscard]] HANDLE SendExitedEvent() const noexcept { return _sendExited; }
        [[nodiscard]] HANDLE ReceiveExitedEvent() const noexcept { return _receiveExited; }
        [[nodiscard]] HANDLE InboundDequeuedEvent() const noexcept { return _inboundDequeued; }
        [[nodiscard]] size_t InboundDequeuedCount() const noexcept
        {
            return _inboundDequeuedCount.load(std::memory_order_acquire);
        }
        [[nodiscard]] HANDLE CancelEnteredEvent() const noexcept { return _cancelEntered; }
        [[nodiscard]] std::vector<std::string> OutboundPayloads() const
        {
            std::scoped_lock lock{ _mutex };
            return _outbound;
        }
        [[nodiscard]] std::vector<ControlRequest> OutboundRequests() const
        {
            std::vector<ControlRequest> requests;
            for (const auto& payload : OutboundPayloads())
            {
                const auto decoded = DecodeControlRequest(payload);
                if (decoded.Succeeded()) { requests.emplace_back(decoded.Value); }
            }
            return requests;
        }
        [[nodiscard]] std::vector<FakeConnectionLifecycleAction> Lifecycle() const
        {
            std::scoped_lock lock{ _mutex };
            return { _lifecycle.begin(), _lifecycle.begin() + _lifecycleCount };
        }
        [[nodiscard]] size_t MaximumConcurrentSends() const noexcept
        {
            return _maximumConcurrentSends.load(std::memory_order_acquire);
        }
        [[nodiscard]] bool CloseObservedWhileIoActive() const noexcept
        {
            return _closeWhileIoActive.load(std::memory_order_acquire);
        }

    private:
        friend class FakeRuntimeClientConnection;
        void _queueInbound(RuntimeReceiveResult result)
        {
            std::scoped_lock lock{ _mutex };
            _inbound.emplace_back(std::move(result));
            SetEvent(_changed);
        }
        void _terminate(RuntimeTransportStatus status, FakeConnectionLifecycleAction action)
        {
            if (action == FakeConnectionLifecycleAction::Close &&
                (_activeSends.load(std::memory_order_acquire) != 0 ||
                 _activeReceives.load(std::memory_order_acquire) != 0))
            {
                _closeWhileIoActive.store(true, std::memory_order_release);
            }
            std::scoped_lock lock{ _mutex };
            auto expected = RuntimeTransportStatus::Succeeded;
            _terminalStatus.compare_exchange_strong(expected, status, std::memory_order_acq_rel);
            auto& recorded = action == FakeConnectionLifecycleAction::CancelPendingIo ? _cancelRecorded : _closeRecorded;
            if (!recorded && _lifecycleCount < _lifecycle.size())
            {
                _lifecycle[_lifecycleCount++] = action;
                recorded = true;
            }
            SetEvent(_terminal);
            SetEvent(_changed);
        }
        [[nodiscard]] RuntimeTransportStatus _status() const
        {
            return _terminalStatus.load(std::memory_order_acquire);
        }
        void _closeHandles() noexcept
        {
            for (const auto handle : std::array{
                     _changed,
                     _terminal,
                     _sendEntered,
                     _receiveEntered,
                     _responseQueued,
                     _releaseSend,
                     _sendExited,
                     _receiveExited,
                     _inboundDequeued,
                     _cancelEntered,
                     _releaseCancel })
            {
                if (handle) { CloseHandle(handle); }
            }
            _changed = _terminal = _sendEntered = _receiveEntered = _responseQueued =
            _releaseSend = _sendExited = _receiveExited = _inboundDequeued =
                _cancelEntered = _releaseCancel = nullptr;
        }

        mutable std::mutex _mutex;
        std::deque<RuntimeReceiveResult> _inbound;
        std::deque<RuntimeTransportResult> _sendResults;
        std::vector<std::string> _outbound;
        std::function<void()> _sendHook;
        std::function<void()> _receiveHook;
        std::function<void()> _cancelHook;
        std::function<void()> _closeHook;
        FakeRuntimeClientAutoResponseHandler _nextAutoResponseHandler;
        FakeRuntimeClientAutoResponseHandler _autoResponseHandler;
        std::array<FakeConnectionLifecycleAction, 2> _lifecycle{};
        size_t _lifecycleCount{};
        std::atomic<RuntimeTransportStatus> _terminalStatus{ RuntimeTransportStatus::Succeeded };
        std::atomic_size_t _activeSends{};
        std::atomic_size_t _activeReceives{};
        std::atomic_size_t _inboundDequeuedCount{};
        std::atomic_size_t _maximumConcurrentSends{};
        std::atomic_bool _closeWhileIoActive{};
        bool _autoResponse{}, _blockNextSend{}, _blockNextCancel{}, _cancelRecorded{}, _closeRecorded{};
        HANDLE _changed{}, _terminal{}, _sendEntered{}, _receiveEntered{}, _responseQueued{}, _releaseSend{};
        HANDLE _sendExited{}, _receiveExited{}, _inboundDequeued{};
        HANDLE _cancelEntered{}, _releaseCancel{};
    };

    class FakeRuntimeClientConnection final : public IRuntimeControlConnection
    {
    public:
        explicit FakeRuntimeClientConnection(std::shared_ptr<FakeRuntimeClientConnectionState> state) : _state{ std::move(state) } {}
        ~FakeRuntimeClientConnection() override { Close(); }
        [[nodiscard]] bool Connected() const noexcept override
        {
            return _state && _state->_status() == RuntimeTransportStatus::Succeeded;
        }
        [[nodiscard]] RuntimeTransportResult Send(std::string_view, uint32_t, HANDLE = nullptr) override;
        [[nodiscard]] RuntimeReceiveResult Receive(uint32_t, HANDLE = nullptr) override;
        void CancelPendingIo() noexcept override
        {
            if (_state)
            {
                bool block{};
                std::function<void()> hook;
                {
                    std::scoped_lock lock{ _state->_mutex };
                    block = std::exchange(_state->_blockNextCancel, false);
                    hook = std::move(_state->_cancelHook);
                }
                if (block)
                {
                    SetEvent(_state->_cancelEntered);
                    WaitForSingleObject(_state->_releaseCancel, 2000);
                }
                if (hook)
                {
                    hook();
                }
                _state->_terminate(
                    RuntimeTransportStatus::Cancelled,
                    FakeConnectionLifecycleAction::CancelPendingIo);
            }
        }
        void Close() noexcept override
        {
            if (_state)
            {
                std::function<void()> hook;
                {
                    std::scoped_lock lock{ _state->_mutex };
                    hook = std::move(_state->_closeHook);
                }
                if (hook)
                {
                    hook();
                }
                _state->_terminate(
                    RuntimeTransportStatus::Disconnected,
                    FakeConnectionLifecycleAction::Close);
            }
        }
    private:
        std::shared_ptr<FakeRuntimeClientConnectionState> _state;
    };

    inline RuntimeTransportResult FakeRuntimeClientConnection::Send(
        const std::string_view payload, const uint32_t timeoutMilliseconds, const HANDLE cancellationEvent)
    {
        FakeRuntimeClientIoScope io{ _state->_activeSends, _state->_sendExited };
        const auto concurrent = _state->_activeSends.load(std::memory_order_acquire);
        auto observedMaximum = _state->_maximumConcurrentSends.load(std::memory_order_acquire);
        while (observedMaximum < concurrent &&
               !_state->_maximumConcurrentSends.compare_exchange_weak(
                   observedMaximum,
                   concurrent,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire))
        {
        }

        RuntimeTransportResult scripted;
        if (cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0)
        {
            scripted.Status = RuntimeTransportStatus::Cancelled;
            return scripted;
        }
        bool hasScripted{}, autoResponse{}, block{};
        FakeRuntimeClientAutoResponseHandler autoResponseHandler;
        std::function<void()> hook;
        {
            std::scoped_lock lock{ _state->_mutex };
            if (_state->_terminalStatus.load(std::memory_order_acquire) != RuntimeTransportStatus::Succeeded)
            {
                scripted.Status = _state->_terminalStatus.load(std::memory_order_acquire);
                return scripted;
            }
            _state->_outbound.emplace_back(payload);
            if (!_state->_sendResults.empty())
            {
                scripted = std::move(_state->_sendResults.front());
                _state->_sendResults.pop_front();
                hasScripted = true;
            }
            autoResponse = _state->_autoResponse;
            if (_state->_nextAutoResponseHandler)
            {
                autoResponseHandler = std::move(_state->_nextAutoResponseHandler);
            }
            else if (_state->_autoResponseHandler)
            {
                autoResponseHandler = _state->_autoResponseHandler;
            }
            block = std::exchange(_state->_blockNextSend, false);
            hook = std::move(_state->_sendHook);
        }
        SetEvent(_state->_sendEntered);
        if (hook)
        {
            hook();
        }
        if (hasScripted && !scripted.Succeeded())
        {
            _state->_terminate(scripted.Status, FakeConnectionLifecycleAction::CancelPendingIo);
            return scripted;
        }
        if (autoResponseHandler || autoResponse)
        {
            const auto decoded = DecodeControlRequest(payload);
            if (decoded.Succeeded())
            {
                auto response = autoResponseHandler ? autoResponseHandler(decoded.Value) : ControlResponse{};
                if (response.RequestId.empty())
                {
                    response.RequestId = decoded.Value.RequestId;
                }
                if (!autoResponseHandler)
                {
                    response.Succeeded = true;
                }
                _state->QueueInboundResponse(response);
                SetEvent(_state->_responseQueued);
            }
        }
        if (block)
        {
            const std::array handles{ _state->_releaseSend, _state->_terminal, cancellationEvent };
            const auto wait = WaitForMultipleObjects(cancellationEvent ? 3 : 2, handles.data(), FALSE, timeoutMilliseconds);
            if (wait == WAIT_OBJECT_0 + 1) { scripted.Status = _state->_status(); return scripted; }
            if (wait == WAIT_OBJECT_0 + 2)
            {
                scripted.Status = RuntimeTransportStatus::Cancelled;
                _state->_terminate(scripted.Status, FakeConnectionLifecycleAction::CancelPendingIo);
                return scripted;
            }
            if (wait != WAIT_OBJECT_0)
            {
                scripted.Status = wait == WAIT_TIMEOUT ? RuntimeTransportStatus::Timeout : RuntimeTransportStatus::SystemError;
                _state->_terminate(scripted.Status, FakeConnectionLifecycleAction::CancelPendingIo);
                return scripted;
            }
        }
        scripted.Status = RuntimeTransportStatus::Succeeded;
        return scripted;
    }

    inline RuntimeReceiveResult FakeRuntimeClientConnection::Receive(
        const uint32_t timeoutMilliseconds, const HANDLE cancellationEvent)
    {
        FakeRuntimeClientIoScope io{ _state->_activeReceives, _state->_receiveExited };
        SetEvent(_state->_receiveEntered);
        std::function<void()> hook;
        {
            std::scoped_lock lock{ _state->_mutex };
            hook = std::move(_state->_receiveHook);
        }
        if (hook)
        {
            hook();
        }
        const auto deadline = GetTickCount64() + timeoutMilliseconds;
        while (true)
        {
            if (cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0)
            {
                RuntimeReceiveResult result; result.Status = RuntimeTransportStatus::Cancelled; return result;
            }
            {
                std::scoped_lock lock{ _state->_mutex };
                if (_state->_terminalStatus.load(std::memory_order_acquire) != RuntimeTransportStatus::Succeeded)
                {
                    RuntimeReceiveResult result; result.Status = _state->_terminalStatus.load(std::memory_order_acquire); return result;
                }
                if (!_state->_inbound.empty())
                {
                    auto result = std::move(_state->_inbound.front());
                    _state->_inbound.pop_front();
                    _state->_inboundDequeuedCount.fetch_add(1, std::memory_order_acq_rel);
                    SetEvent(_state->_inboundDequeued);
                    if (_state->_inbound.empty()) { ResetEvent(_state->_changed); }
                    return result;
                }
                ResetEvent(_state->_changed);
            }
            const auto now = GetTickCount64();
            if (now >= deadline) { RuntimeReceiveResult result; result.Status = RuntimeTransportStatus::Timeout; return result; }
            const std::array handles{ _state->_changed, _state->_terminal, cancellationEvent };
            const auto wait = WaitForMultipleObjects(cancellationEvent ? 3 : 2, handles.data(), FALSE, static_cast<DWORD>(deadline - now));
            if (wait == WAIT_OBJECT_0 + 1) { RuntimeReceiveResult result; result.Status = _state->_status(); return result; }
            if (wait == WAIT_OBJECT_0 + 2) { RuntimeReceiveResult result; result.Status = RuntimeTransportStatus::Cancelled; return result; }
            if (wait == WAIT_TIMEOUT) { RuntimeReceiveResult result; result.Status = RuntimeTransportStatus::Timeout; return result; }
            if (wait != WAIT_OBJECT_0) { RuntimeReceiveResult result; result.Status = RuntimeTransportStatus::SystemError; result.SystemError = GetLastError(); return result; }
        }
    }
}
