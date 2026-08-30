// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeClientEpoch.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    static_assert(static_cast<int>(RuntimeClientRetireCause::Count) == 6,
                  "Review client epoch retire-cause sanitization when adding a cause");

    namespace
    {
        RuntimeClientRetireCause _sanitizeRetireCause(
            const RuntimeClientRetireCause cause) noexcept
        {
            switch (cause)
            {
            case RuntimeClientRetireCause::StopRequested:
            case RuntimeClientRetireCause::TransportFailure:
            case RuntimeClientRetireCause::ProtocolError:
            case RuntimeClientRetireCause::HeartbeatTimeout:
            case RuntimeClientRetireCause::Replaced:
                return cause;
            case RuntimeClientRetireCause::None:
            case RuntimeClientRetireCause::Count:
            default:
                return RuntimeClientRetireCause::ProtocolError;
            }
        }
    }

    RuntimeClientEpochState::RuntimeClientEpochState(const RuntimeClientEpoch lastIssued) noexcept :
        _lastIssued{ lastIssued }
    {
    }

    RuntimeClientEpochActivation RuntimeClientEpochState::Activate() noexcept
    {
        if (_phase == RuntimeClientEpochPhase::Exhausted)
        {
            return { RuntimeClientEpochActivationStatus::Exhausted, 0 };
        }
        if (_phase != RuntimeClientEpochPhase::Offline)
        {
            return { RuntimeClientEpochActivationStatus::Busy, _current };
        }
        if (_lastIssued == std::numeric_limits<RuntimeClientEpoch>::max())
        {
            _phase = RuntimeClientEpochPhase::Exhausted;
            return { RuntimeClientEpochActivationStatus::Exhausted, 0 };
        }

        _current = ++_lastIssued;
        _phase = RuntimeClientEpochPhase::Active;
        _retireCause = RuntimeClientRetireCause::None;
        return { RuntimeClientEpochActivationStatus::Activated, _current };
    }

    RuntimeClientEpochRetirement RuntimeClientEpochState::BeginRetire(
        const RuntimeClientEpoch epoch,
        const RuntimeClientRetireCause cause) noexcept
    {
        if (_phase == RuntimeClientEpochPhase::Offline ||
            _phase == RuntimeClientEpochPhase::Exhausted || _current == 0)
        {
            return { RuntimeClientEpochRetireStatus::NoActiveEpoch, 0, _retireCause };
        }
        if (epoch == 0 || epoch != _current)
        {
            return { RuntimeClientEpochRetireStatus::StaleEpoch, _current, _retireCause };
        }
        if (_phase == RuntimeClientEpochPhase::Retiring)
        {
            return { RuntimeClientEpochRetireStatus::AlreadyRetiring, _current, _retireCause };
        }

        _phase = RuntimeClientEpochPhase::Retiring;
        _retireCause = _sanitizeRetireCause(cause);
        return { RuntimeClientEpochRetireStatus::Started, _current, _retireCause };
    }

    bool RuntimeClientEpochState::CompleteRetire(const RuntimeClientEpoch epoch) noexcept
    {
        if (_phase != RuntimeClientEpochPhase::Retiring || epoch == 0 || epoch != _current)
        {
            return false;
        }
        _current = 0;
        _phase = RuntimeClientEpochPhase::Offline;
        return true;
    }

    bool RuntimeClientEpochState::IsActive(const RuntimeClientEpoch epoch) const noexcept
    {
        return epoch != 0 && _phase == RuntimeClientEpochPhase::Active && _current == epoch;
    }

    RuntimeClientEpochPhase RuntimeClientEpochState::Phase() const noexcept
    {
        return _phase;
    }

    RuntimeClientEpoch RuntimeClientEpochState::Current() const noexcept
    {
        return _current;
    }

    RuntimeClientEpoch RuntimeClientEpochState::LastIssued() const noexcept
    {
        return _lastIssued;
    }

    RuntimeClientRetireCause RuntimeClientEpochState::RetireCause() const noexcept
    {
        return _retireCause;
    }
}
