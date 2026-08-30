// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientSubscriptionStream.h"
#include "RuntimeClientSubscriptionStreamInternal.h"

#include "ProtocolRpc.h"
#include "ProtocolValidation.h"
#include "ProtocolVersion.h"

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    namespace
    {
        bool _checkedAdd(size_t& total, const size_t value) noexcept
        {
            if (value > (std::numeric_limits<size_t>::max)() - total)
            {
                return false;
            }
            total += value;
            return true;
        }

        std::optional<size_t> _eventBytes(const RuntimeClientSubscriptionEvent& event) noexcept
        {
            size_t total{ RuntimeClientSubscriptionEventOverheadBytes };
            if ((!event.SnapshotJson.empty() && !_checkedAdd(total, event.SnapshotJson.capacity())) ||
                (!event.Diagnostic.empty() && !_checkedAdd(total, event.Diagnostic.capacity())))
            {
                return std::nullopt;
            }
            if (event.Notification &&
                ((!event.Notification->SubscriptionId.empty() &&
                  !_checkedAdd(total, event.Notification->SubscriptionId.capacity())) ||
                 (!event.Notification->UnknownKind.empty() &&
                  !_checkedAdd(total, event.Notification->UnknownKind.capacity())) ||
                 (!event.Notification->PayloadJson.empty() &&
                  !_checkedAdd(total, event.Notification->PayloadJson.capacity()))))
            {
                return std::nullopt;
            }
            return total;
        }

        bool _compact(std::string& value) noexcept
        {
            try
            {
                std::string compact{ value.data(), value.size() };
                value.swap(compact);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool _compact(RuntimeClientSubscriptionEvent& event) noexcept
        {
            if (!_compact(event.SnapshotJson) || !_compact(event.Diagnostic))
            {
                return false;
            }
            return !event.Notification ||
                   (_compact(event.Notification->SubscriptionId) &&
                    _compact(event.Notification->UnknownKind) &&
                    _compact(event.Notification->PayloadJson));
        }

        bool _validNotification(const ControlNotification& notification) noexcept
        {
            const auto knownKind = !ControlNotificationKindName(notification.Kind).empty();
            return IsCanonicalProtocolUuid(notification.SubscriptionId) &&
                   notification.Generation != 0 && notification.Sequence != 0 &&
                   !notification.PayloadJson.empty() &&
                   notification.PayloadJson.size() <= MaximumControlFrameBytes &&
                   IsValidUtf8(notification.PayloadJson) &&
                   notification.UnknownKind.size() <= MaximumNotificationKindBytes &&
                   IsValidUtf8(notification.UnknownKind) &&
                   (knownKind || !notification.UnknownKind.empty());
        }

        bool _validEvent(
            const RuntimeClientSubscriptionEvent& event,
            const RuntimeClientLogicalSubscriptionId logicalId,
            const RuntimeClientSubscriptionRevision revision) noexcept
        {
            if (event.LogicalId != logicalId || event.Revision != revision ||
                !IsValidUtf8(event.SnapshotJson) || !IsValidUtf8(event.Diagnostic) ||
                event.SnapshotJson.size() > MaximumControlFrameBytes ||
                event.Diagnostic.size() > MaximumSubscriptionEventDiagnosticBytes ||
                (event.Notification && !_validNotification(*event.Notification)))
            {
                return false;
            }
            switch (event.Kind)
            {
            case RuntimeClientSubscriptionEventKind::Snapshot:
                return event.Epoch != 0 && !event.SnapshotJson.empty() && !event.Notification;
            case RuntimeClientSubscriptionEventKind::Notification:
                return event.Epoch != 0 && event.SnapshotJson.empty() && event.Notification.has_value() &&
                       event.Cursor == event.Notification->Sequence;
            case RuntimeClientSubscriptionEventKind::ResnapshotRequired:
                return event.Epoch != 0 && event.SnapshotJson.empty();
            case RuntimeClientSubscriptionEventKind::Removed:
            case RuntimeClientSubscriptionEventKind::Faulted:
            case RuntimeClientSubscriptionEventKind::ClientStopped:
                return event.SnapshotJson.empty() && !event.Notification;
            }
            return false;
        }

        bool _terminalKind(const RuntimeClientSubscriptionEventKind kind) noexcept
        {
            return kind == RuntimeClientSubscriptionEventKind::Removed ||
                   kind == RuntimeClientSubscriptionEventKind::Faulted ||
                   kind == RuntimeClientSubscriptionEventKind::ClientStopped;
        }

        size_t _defaulted(const size_t value, const size_t fallback) noexcept
        {
            return value == 0 ? fallback : value;
        }

        bool _normalize(RuntimeClientSubscribeOptions& options) noexcept
        {
            options.MaximumQueuedEvents = _defaulted(
                options.MaximumQueuedEvents,
                DefaultMaximumSubscriptionStreamEvents);
            options.MaximumQueuedBytes = _defaulted(
                options.MaximumQueuedBytes,
                DefaultMaximumSubscriptionStreamBytes);
            options.MaximumEventBytes = _defaulted(
                options.MaximumEventBytes,
                DefaultMaximumSubscriptionEventBytes);
            return options.MaximumQueuedEvents <= AbsoluteMaximumSubscriptionStreamEvents &&
                   options.MaximumQueuedBytes >= RuntimeClientSubscriptionEventOverheadBytes &&
                   options.MaximumQueuedBytes <= AbsoluteMaximumSubscriptionStreamBytes &&
                   options.MaximumEventBytes >= RuntimeClientSubscriptionEventOverheadBytes &&
                   options.MaximumEventBytes <= AbsoluteMaximumSubscriptionEventBytes &&
                   options.MaximumEventBytes <= options.MaximumQueuedBytes &&
                   !options.ParamsJson.empty() && options.ParamsJson.size() <= MaximumControlFrameBytes &&
                   IsValidUtf8(options.ParamsJson);
        }

        void _updateSignalLocked(details::RuntimeClientSubscriptionStreamState& state) noexcept
        {
            if (!state.Queue.empty() || (state.TerminalSet && !state.TerminalDelivered))
            {
                SetEvent(state.Changed);
            }
            else
            {
                ResetEvent(state.Changed);
            }
        }
    }

    static_assert(std::is_nothrow_move_constructible_v<RuntimeClientSubscriptionEvent>);

    namespace details
    {
        RuntimeClientSubscriptionStreamState::RuntimeClientSubscriptionStreamState(
            const RuntimeClientLogicalSubscriptionId logicalId,
            const RuntimeClientSubscriptionRevision revision,
            const size_t maximumQueuedEvents,
            const size_t maximumQueuedBytes,
            const size_t maximumEventBytes,
            RuntimeClientSubscriptionRemoveCallback removeCallback) :
            LogicalId{ logicalId },
            Revision{ revision },
            MaximumQueuedEvents{ maximumQueuedEvents },
            MaximumQueuedBytes{ maximumQueuedBytes },
            MaximumEventBytes{ maximumEventBytes },
            Changed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            RemoveCallback{ std::move(removeCallback) }
        {
        }

        RuntimeClientSubscriptionStreamState::~RuntimeClientSubscriptionStreamState()
        {
            if (Changed)
            {
                CloseHandle(Changed);
            }
        }

        bool RuntimeClientSubscriptionStreamState::Valid() const noexcept
        {
            return LogicalId != 0 && Revision != 0 && Changed && MaximumQueuedEvents != 0 &&
                   MaximumQueuedBytes >= MaximumEventBytes;
        }

        RuntimeClientSubscriptionPublishStatus RuntimeClientSubscriptionStreamState::Publish(
            RuntimeClientSubscriptionEvent event) noexcept
        {
            if (!Valid() || !_validEvent(event, LogicalId, Revision) || _terminalKind(event.Kind))
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            if (!_compact(event))
            {
                return RuntimeClientSubscriptionPublishStatus::AllocationFailure;
            }
            const auto eventBytes = _eventBytes(event);
            if (!eventBytes)
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            if (*eventBytes > MaximumEventBytes)
            {
                return RuntimeClientSubscriptionPublishStatus::EventTooLarge;
            }

            try
            {
                std::scoped_lock lock{ Mutex };
                if (TerminalSet)
                {
                    return RuntimeClientSubscriptionPublishStatus::Closed;
                }
                if (Queue.size() >= MaximumQueuedEvents)
                {
                    return RuntimeClientSubscriptionPublishStatus::CountLimitReached;
                }
                if (QueuedBytes > MaximumQueuedBytes || *eventBytes > MaximumQueuedBytes - QueuedBytes)
                {
                    return RuntimeClientSubscriptionPublishStatus::ByteLimitReached;
                }
                Queue.emplace_back(RuntimeClientQueuedSubscriptionEvent{ std::move(event), *eventBytes });
                QueuedBytes += *eventBytes;
                SetEvent(Changed);
                return RuntimeClientSubscriptionPublishStatus::Accepted;
            }
            catch (...)
            {
                return RuntimeClientSubscriptionPublishStatus::AllocationFailure;
            }
        }

        RuntimeClientSubscriptionPublishStatus RuntimeClientSubscriptionStreamState::Close(
            RuntimeClientSubscriptionEvent terminal) noexcept
        {
            if (!Valid() || !_terminalKind(terminal.Kind) ||
                !_validEvent(terminal, LogicalId, Revision))
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            if (!_compact(terminal))
            {
                return RuntimeClientSubscriptionPublishStatus::AllocationFailure;
            }
            const auto bytes = _eventBytes(terminal);
            if (!bytes)
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            if (*bytes > MaximumEventBytes)
            {
                std::string{}.swap(terminal.Diagnostic);
            }

            std::scoped_lock lock{ Mutex };
            if (TerminalSet)
            {
                return RuntimeClientSubscriptionPublishStatus::Closed;
            }
            Terminal.emplace(std::move(terminal));
            TerminalSet = true;
            SetEvent(Changed);
            return RuntimeClientSubscriptionPublishStatus::Accepted;
        }

        RuntimeClientSubscriptionPullResult RuntimeClientSubscriptionStreamState::Pull(
            const uint32_t timeoutMilliseconds,
            const HANDLE cancellationEvent) noexcept
        {
            const auto infinite = timeoutMilliseconds == INFINITE;
            const auto deadline = infinite ? uint64_t{} : GetTickCount64() + timeoutMilliseconds;
            while (true)
            {
                if (cancellationEvent)
                {
                    const auto cancelled = WaitForSingleObject(cancellationEvent, 0);
                    if (cancelled == WAIT_OBJECT_0 || cancelled == WAIT_FAILED)
                    {
                        return { RuntimeClientSubscriptionPullStatus::Cancelled, std::nullopt };
                    }
                }
                {
                    std::scoped_lock lock{ Mutex };
                    if (!Queue.empty())
                    {
                        RuntimeClientSubscriptionPullResult result;
                        result.Status = RuntimeClientSubscriptionPullStatus::Event;
                        result.Value = std::move(Queue.front().Value);
                        QueuedBytes -= Queue.front().Bytes;
                        Queue.pop_front();
                        _updateSignalLocked(*this);
                        return result;
                    }
                    if (TerminalSet && !TerminalDelivered)
                    {
                        RuntimeClientSubscriptionPullResult result;
                        result.Status = RuntimeClientSubscriptionPullStatus::Event;
                        result.Value = std::move(Terminal);
                        Terminal.reset();
                        TerminalDelivered = true;
                        _updateSignalLocked(*this);
                        return result;
                    }
                    if (TerminalDelivered)
                    {
                        return { RuntimeClientSubscriptionPullStatus::Closed, std::nullopt };
                    }
                    ResetEvent(Changed);
                }

                const auto now = GetTickCount64();
                const auto remaining = infinite ? INFINITE :
                    now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
                DWORD waited{};
                if (cancellationEvent)
                {
                    const std::array handles{ cancellationEvent, Changed };
                    waited = WaitForMultipleObjects(2, handles.data(), FALSE, remaining);
                    if (waited == WAIT_OBJECT_0)
                    {
                        return { RuntimeClientSubscriptionPullStatus::Cancelled, std::nullopt };
                    }
                    if (waited == WAIT_OBJECT_0 + 1)
                    {
                        continue;
                    }
                }
                else
                {
                    waited = WaitForSingleObject(Changed, remaining);
                    if (waited == WAIT_OBJECT_0)
                    {
                        continue;
                    }
                }
                return waited == WAIT_TIMEOUT ?
                           RuntimeClientSubscriptionPullResult{
                               RuntimeClientSubscriptionPullStatus::Timeout,
                               std::nullopt,
                           } :
                           RuntimeClientSubscriptionPullResult{
                               RuntimeClientSubscriptionPullStatus::Cancelled,
                               std::nullopt,
                           };
            }
        }

        bool RuntimeClientSubscriptionStreamState::RequestRemoval() noexcept
        {
            RuntimeClientSubscriptionRemoveCallback callback;
            {
                std::scoped_lock lock{ Mutex };
                if (!RemovalArmed || RemovalRequested)
                {
                    return false;
                }
                RemovalRequested = true;
                callback = std::move(RemoveCallback);
            }
            if (callback)
            {
                try
                {
                    callback(LogicalId, Revision);
                }
                catch (...)
                {
                }
            }
            return true;
        }

        RuntimeClientSubscriptionStreamProducer::RuntimeClientSubscriptionStreamProducer(
            std::shared_ptr<RuntimeClientSubscriptionStreamState> state) noexcept :
            _state{ std::move(state) }
        {
        }

        RuntimeClientSubscriptionPublishStatus RuntimeClientSubscriptionStreamProducer::Publish(
            RuntimeClientSubscriptionEvent event) noexcept
        {
            const auto state = _state;
            return state ? state->Publish(std::move(event)) :
                           RuntimeClientSubscriptionPublishStatus::Closed;
        }

        RuntimeClientSubscriptionPublishStatus RuntimeClientSubscriptionStreamProducer::Close(
            RuntimeClientSubscriptionEvent terminal) noexcept
        {
            const auto state = _state;
            return state ? state->Close(std::move(terminal)) :
                           RuntimeClientSubscriptionPublishStatus::Closed;
        }

        RuntimeClientSubscriptionStreamEndpoints RuntimeClientSubscriptionStreamFactory::Create(
            const RuntimeClientLogicalSubscriptionId logicalId,
            const RuntimeClientSubscriptionRevision revision,
            RuntimeClientSubscribeOptions options,
            RuntimeClientSubscriptionRemoveCallback removeCallback)
        {
            RuntimeClientSubscriptionStreamEndpoints result;
            if (logicalId == 0 || revision == 0 || !_normalize(options))
            {
                return result;
            }
            try
            {
                auto state = std::make_shared<RuntimeClientSubscriptionStreamState>(
                    logicalId,
                    revision,
                    options.MaximumQueuedEvents,
                    options.MaximumQueuedBytes,
                    options.MaximumEventBytes,
                    std::move(removeCallback));
                if (!state->Valid())
                {
                    result.Status = RuntimeClientSubscriptionStreamCreateStatus::SystemError;
                    return result;
                }
                result.Producer = std::make_shared<RuntimeClientSubscriptionStreamProducer>(state);
                result.Stream = std::shared_ptr<RuntimeClientSubscriptionStream>(
                    new RuntimeClientSubscriptionStream{ state });
                {
                    std::scoped_lock lock{ state->Mutex };
                    state->RemovalArmed = true;
                }
                result.Status = RuntimeClientSubscriptionStreamCreateStatus::Succeeded;
                return result;
            }
            catch (...)
            {
                result.Stream.reset();
                result.Producer.reset();
                result.Status = RuntimeClientSubscriptionStreamCreateStatus::AllocationFailure;
                return result;
            }
        }
    }

    RuntimeClientSubscriptionStream::RuntimeClientSubscriptionStream(
        std::shared_ptr<details::RuntimeClientSubscriptionStreamState> state) noexcept :
        _state{ std::move(state) }
    {
    }

    RuntimeClientSubscriptionStream::~RuntimeClientSubscriptionStream()
    {
        const auto state = _state;
        if (state)
        {
            (void)state->RequestRemoval();
        }
    }

    RuntimeClientSubscriptionPullResult RuntimeClientSubscriptionStream::Pull(
        const uint32_t timeoutMilliseconds,
        const HANDLE cancellationEvent) noexcept
    {
        const auto state = _state;
        return state ? state->Pull(timeoutMilliseconds, cancellationEvent) :
                       RuntimeClientSubscriptionPullResult{
                           RuntimeClientSubscriptionPullStatus::Closed,
                           std::nullopt,
                       };
    }

    bool RuntimeClientSubscriptionStream::RequestUnsubscribe() noexcept
    {
        const auto state = _state;
        return state && state->RequestRemoval();
    }

    RuntimeClientLogicalSubscriptionId RuntimeClientSubscriptionStream::LogicalId() const noexcept
    {
        return _state ? _state->LogicalId : 0;
    }

    RuntimeClientSubscriptionRevision RuntimeClientSubscriptionStream::Revision() const noexcept
    {
        return _state ? _state->Revision : 0;
    }
}
