// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalLifecycle.h"

#include "TaskTerminalStatePolicy.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;

    RuntimeTerminalLifecycleState::RuntimeTerminalLifecycleState(
        RuntimeTerminalLifecycleConfiguration configuration) noexcept :
        _configuration{ std::move(configuration) },
        _state{ _configuration.InitialState },
        _generation{ _configuration.InitialGeneration }
    {
        _valid = _configuration.IdleSuspendMilliseconds != 0 &&
                 _validState(_state, _generation);
        _setPhaseFromState();
        if (!_valid)
        {
            _phase = RuntimeTerminalLifecyclePhase::Failed;
        }
    }

    bool RuntimeTerminalLifecycleState::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeTerminalLifecycleResult RuntimeTerminalLifecycleState::Observe(
        const bool hasActiveRun,
        const size_t attachedClients,
        const RuntimeTerminalLifecycleMilliseconds now) noexcept
    {
        if (!_valid)
        {
            return _result(RuntimeTerminalLifecycleStatus::InvalidConfiguration);
        }
        if (!_observeTime(now))
        {
            return _fail(RuntimeTerminalLifecycleStatus::ClockRegression);
        }

        _hasActiveRun = hasActiveRun;
        _attachedClients = attachedClients;
        if (_state != TaskTerminalState::Ready)
        {
            _clearDeadline();
            _setPhaseFromState();
            if (_state == TaskTerminalState::Suspended)
            {
                return _result(RuntimeTerminalLifecycleStatus::Suspended);
            }
            if (_state == TaskTerminalState::Closed)
            {
                return _result(RuntimeTerminalLifecycleStatus::WakeRejected);
            }
            return _result(
                _state == TaskTerminalState::Starting ||
                        _state == TaskTerminalState::Busy ?
                    RuntimeTerminalLifecycleStatus::Live :
                    RuntimeTerminalLifecycleStatus::Dormant);
        }

        if (_hasActiveRun || _attachedClients != 0)
        {
            _clearDeadline();
            _phase = RuntimeTerminalLifecyclePhase::Live;
            return _result(RuntimeTerminalLifecycleStatus::Live);
        }
        if (!_deadline)
        {
            if (!_armDeadline(now))
            {
                return _fail(RuntimeTerminalLifecycleStatus::ClockExhausted);
            }
            _phase = RuntimeTerminalLifecyclePhase::IdleGrace;
            return _result(
                RuntimeTerminalLifecycleStatus::IdleGraceStarted,
                RuntimeTerminalLifecycleAction::Wait);
        }
        if (now < *_deadline)
        {
            _phase = RuntimeTerminalLifecyclePhase::IdleGrace;
            return _result(
                RuntimeTerminalLifecycleStatus::IdleGraceContinues,
                RuntimeTerminalLifecycleAction::Wait);
        }

        const auto decision = EvaluateTaskTerminalTransition(
            _state,
            TaskTerminalState::Suspended,
            {
                _generation,
                _generation,
                false,
                _hasActiveRun,
            });
        if (!decision.Allowed)
        {
            return _fail(RuntimeTerminalLifecycleStatus::InvalidTransition);
        }
        _state = decision.NextState;
        _clearDeadline();
        _phase = RuntimeTerminalLifecyclePhase::Suspended;
        return _result(
            RuntimeTerminalLifecycleStatus::Suspended,
            RuntimeTerminalLifecycleAction::SuspendProjection);
    }

    RuntimeTerminalLifecycleView RuntimeTerminalLifecycleState::Snapshot() const noexcept
    {
        return {
            _phase,
            _state,
            _generation,
            _hasActiveRun,
            _attachedClients,
            _deadline,
            _lastObservation,
        };
    }

    bool RuntimeTerminalLifecycleState::_validState(
        const TaskTerminalState state,
        const uint64_t generation) noexcept
    {
        if (state > TaskTerminalState::Closed)
        {
            return false;
        }
        if (state == TaskTerminalState::NotCreated)
        {
            return generation == 0;
        }
        if (state == TaskTerminalState::Closed)
        {
            return true;
        }
        return generation != 0;
    }

    bool RuntimeTerminalLifecycleState::_observeTime(
        const RuntimeTerminalLifecycleMilliseconds now) noexcept
    {
        if (_lastObservation && now < *_lastObservation)
        {
            return false;
        }
        _lastObservation = now;
        return true;
    }

    bool RuntimeTerminalLifecycleState::_armDeadline(
        const RuntimeTerminalLifecycleMilliseconds now) noexcept
    {
        const auto maximum =
            (std::numeric_limits<RuntimeTerminalLifecycleMilliseconds>::max)();
        if (_configuration.IdleSuspendMilliseconds > maximum - now)
        {
            return false;
        }
        _deadline = now + _configuration.IdleSuspendMilliseconds;
        return true;
    }

    RuntimeTerminalLifecycleResult RuntimeTerminalLifecycleState::_result(
        const RuntimeTerminalLifecycleStatus status,
        const RuntimeTerminalLifecycleAction action) const noexcept
    {
        return {
            status,
            action,
            _state,
            _generation,
            _deadline,
        };
    }

    RuntimeTerminalLifecycleResult RuntimeTerminalLifecycleState::_fail(
        const RuntimeTerminalLifecycleStatus status) noexcept
    {
        _valid = false;
        _clearDeadline();
        _phase = RuntimeTerminalLifecyclePhase::Failed;
        return _result(status);
    }

    void RuntimeTerminalLifecycleState::_clearDeadline() noexcept
    {
        _deadline.reset();
    }

    void RuntimeTerminalLifecycleState::_setPhaseFromState() noexcept
    {
        switch (_state)
        {
        case TaskTerminalState::Starting:
            _phase = RuntimeTerminalLifecyclePhase::Starting;
            break;
        case TaskTerminalState::Ready:
        case TaskTerminalState::Busy:
            _phase = RuntimeTerminalLifecyclePhase::Live;
            break;
        case TaskTerminalState::Suspended:
            _phase = RuntimeTerminalLifecyclePhase::Suspended;
            break;
        case TaskTerminalState::Closed:
            _phase = RuntimeTerminalLifecyclePhase::Closed;
            break;
        default:
            _phase = RuntimeTerminalLifecyclePhase::Dormant;
            break;
        }
    }
}
