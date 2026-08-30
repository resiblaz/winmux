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
            std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> aggregateBudget,
            RuntimeClientSubscriptionRemoveCallback removeCallback,
            const HANDLE pullWaitEnteredEvent) :
            LogicalId{ logicalId },
            Revision{ revision },
            MaximumQueuedEvents{ maximumQueuedEvents },
            MaximumQueuedBytes{ maximumQueuedBytes },
            MaximumEventBytes{ maximumEventBytes },
            AggregateBudget{ std::move(aggregateBudget) },
            Changed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) },
            PullWaitEnteredEvent{ pullWaitEnteredEvent },
            RemoveCallback{ std::move(removeCallback) }
        {
        }

        RuntimeClientSubscriptionStreamState::~RuntimeClientSubscriptionStreamState()
        {
            if (AggregateBudget && QueuedBytes != 0)
            {
                AggregateBudget->Release(QueuedBytes);
                QueuedBytes = 0;
            }
            if (Changed)
            {
                CloseHandle(Changed);
            }
        }

        bool RuntimeClientSubscriptionStreamState::Valid() const noexcept
        {
            return LogicalId != 0 && Revision != 0 && Changed && MaximumQueuedEvents != 0 &&
                   MaximumQueuedBytes >= MaximumEventBytes && AggregateBudget && AggregateBudget->Valid();
        }

        RuntimeClientSubscriptionPublishStatus RuntimeClientSubscriptionStreamState::Publish(
            RuntimeClientSubscriptionEvent event) noexcept
        {
            if (!Valid() || !ValidateRuntimeClientSubscriptionEvent(event, LogicalId, Revision) ||
                IsTerminalRuntimeClientSubscriptionEvent(event.Kind))
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            if (!CanonicalizeRuntimeClientSubscriptionEvent(event))
            {
                return RuntimeClientSubscriptionPublishStatus::AllocationFailure;
            }
            const auto eventBytes = MeasureRuntimeClientSubscriptionEventBytes(event);
            if (!eventBytes)
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }
            if (*eventBytes > MaximumEventBytes)
            {
                return RuntimeClientSubscriptionPublishStatus::EventTooLarge;
            }

            bool aggregateReserved{};
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
                if (!AggregateBudget->TryReserve(*eventBytes))
                {
                    return RuntimeClientSubscriptionPublishStatus::AggregateLimitReached;
                }
                aggregateReserved = true;
                Queue.emplace_back(RuntimeClientQueuedSubscriptionEvent{ std::move(event), *eventBytes });
                QueuedBytes += *eventBytes;
                SetEvent(Changed);
                return RuntimeClientSubscriptionPublishStatus::Accepted;
            }
            catch (...)
            {
                if (aggregateReserved)
                {
                    AggregateBudget->Release(*eventBytes);
                }
                return RuntimeClientSubscriptionPublishStatus::AllocationFailure;
            }
        }

        RuntimeClientSubscriptionPublishStatus RuntimeClientSubscriptionStreamState::Close(
            RuntimeClientSubscriptionEvent terminal) noexcept
        {
            if (!Valid() || !IsTerminalRuntimeClientSubscriptionEvent(terminal.Kind) ||
                terminal.LogicalId != LogicalId || terminal.Revision != Revision)
            {
                return RuntimeClientSubscriptionPublishStatus::InvalidEvent;
            }

            terminal.SnapshotJson.clear();
            terminal.Notification.reset();
            if (terminal.Diagnostic.size() > MaximumSubscriptionEventDiagnosticBytes ||
                !IsValidUtf8(terminal.Diagnostic) ||
                !CanonicalizeRuntimeClientSubscriptionEvent(terminal))
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
                        AggregateBudget->Release(Queue.front().Bytes);
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
                if (PullWaitEnteredEvent)
                {
                    SetEvent(PullWaitEnteredEvent);
                }
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
            RuntimeClientSubscriptionRemoveCallback removeCallback,
            std::shared_ptr<RuntimeClientSubscriptionAggregateBudget> aggregateBudget,
            const HANDLE pullWaitEnteredEvent)
        {
            RuntimeClientSubscriptionStreamEndpoints result;
            if (logicalId == 0 || revision == 0 || !NormalizeRuntimeClientSubscribeOptions(options) ||
                (aggregateBudget && !aggregateBudget->Valid()))
            {
                return result;
            }
            try
            {
                if (!aggregateBudget)
                {
                    aggregateBudget = std::make_shared<RuntimeClientSubscriptionAggregateBudget>();
                }
                auto state = std::make_shared<RuntimeClientSubscriptionStreamState>(
                    logicalId,
                    revision,
                    options.MaximumQueuedEvents,
                    options.MaximumQueuedBytes,
                    options.MaximumEventBytes,
                    std::move(aggregateBudget),
                    std::move(removeCallback),
                    pullWaitEnteredEvent);
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
