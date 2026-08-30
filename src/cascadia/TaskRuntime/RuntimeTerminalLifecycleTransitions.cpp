// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeTerminalLifecycle.h"

#include "TaskTerminalStatePolicy.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    using namespace Microsoft::Terminal::TaskCore;

    namespace
    {
        bool _wakeable(const TaskTerminalState state) noexcept
        {
            return state == TaskTerminalState::NotCreated ||
                   state == TaskTerminalState::Exited ||
                   state == TaskTerminalState::Suspended ||
                   state == TaskTerminalState::Error ||
                   state == TaskTerminalState::Closed;
        }
    }

    RuntimeTerminalLifecycleResult RuntimeTerminalLifecycleState::RequestWake(
        const RuntimeTerminalWakeTrigger trigger,
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

        _clearDeadline();
        if (_state == TaskTerminalState::Starting)
        {
            _phase = RuntimeTerminalLifecyclePhase::Starting;
            return _result(RuntimeTerminalLifecycleStatus::WakeReplayed);
        }
        if (_state == TaskTerminalState::Ready ||
            _state == TaskTerminalState::Busy)
        {
            _phase = RuntimeTerminalLifecyclePhase::Live;
            return _result(RuntimeTerminalLifecycleStatus::AlreadyAwake);
        }
        if (!_wakeable(_state) ||
            trigger == RuntimeTerminalWakeTrigger::PassiveReconnect ||
            (_state == TaskTerminalState::Closed &&
             trigger != RuntimeTerminalWakeTrigger::ExplicitOpen))
        {
            _setPhaseFromState();
            return _result(RuntimeTerminalLifecycleStatus::WakeRejected);
        }
        if (_generation == (std::numeric_limits<uint64_t>::max)())
        {
            return _result(RuntimeTerminalLifecycleStatus::GenerationExhausted);
        }

        const auto nextGeneration = _generation + 1;
        const auto decision = EvaluateTaskTerminalTransition(
            _state,
            TaskTerminalState::Starting,
            {
                _generation,
                nextGeneration,
                true,
                _hasActiveRun,
            });
        if (!decision.Allowed)
        {
            return _result(RuntimeTerminalLifecycleStatus::InvalidTransition);
        }
        _state = decision.NextState;
        _generation = nextGeneration;
        _phase = RuntimeTerminalLifecyclePhase::Starting;
        return _result(
            RuntimeTerminalLifecycleStatus::WakeStarted,
            RuntimeTerminalLifecycleAction::StartProjection);
    }

    RuntimeTerminalLifecycleResult RuntimeTerminalLifecycleState::CompleteWake(
        const uint64_t generation,
        const bool succeeded,
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
        if (generation != _generation)
        {
            return _result(RuntimeTerminalLifecycleStatus::StaleGeneration);
        }
        if ((_state == TaskTerminalState::Ready && succeeded) ||
            (_state == TaskTerminalState::Error && !succeeded))
        {
            return _result(
                RuntimeTerminalLifecycleStatus::WakeReplayed,
                _state == TaskTerminalState::Ready && _deadline ?
                    RuntimeTerminalLifecycleAction::Wait :
                    RuntimeTerminalLifecycleAction::None);
        }
        if (_state != TaskTerminalState::Starting)
        {
            return _result(RuntimeTerminalLifecycleStatus::InvalidTransition);
        }

        const auto requested = succeeded ?
                                   TaskTerminalState::Ready :
                                   TaskTerminalState::Error;
        const auto decision = EvaluateTaskTerminalTransition(
            _state,
            requested,
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
        _setPhaseFromState();
        if (!succeeded)
        {
            return _result(RuntimeTerminalLifecycleStatus::WakeFailed);
        }
        if (!_hasActiveRun && _attachedClients == 0)
        {
            if (!_armDeadline(now))
            {
                return _fail(RuntimeTerminalLifecycleStatus::ClockExhausted);
            }
            _phase = RuntimeTerminalLifecyclePhase::IdleGrace;
            return _result(
                RuntimeTerminalLifecycleStatus::WakeCompleted,
                RuntimeTerminalLifecycleAction::Wait);
        }
        return _result(RuntimeTerminalLifecycleStatus::WakeCompleted);
    }

    RuntimeTerminalLifecycleResult RuntimeTerminalLifecycleState::Synchronize(
        const TaskTerminalState state,
        const uint64_t generation,
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
        if (!_validState(state, generation))
        {
            return _result(RuntimeTerminalLifecycleStatus::InvalidTransition);
        }
        if (state == TaskTerminalState::Suspended && _attachedClients != 0)
        {
            return _result(RuntimeTerminalLifecycleStatus::InvalidTransition);
        }
        if (generation < _generation ||
            (generation > _generation &&
             (_generation == (std::numeric_limits<uint64_t>::max)() ||
              generation != _generation + 1 ||
              state != TaskTerminalState::Starting)))
        {
            return _result(RuntimeTerminalLifecycleStatus::StaleGeneration);
        }
        if (state == _state && generation == _generation)
        {
            if (_state == TaskTerminalState::Ready &&
                !_hasActiveRun && _attachedClients == 0 && !_deadline)
            {
                if (!_armDeadline(now))
                {
                    return _fail(RuntimeTerminalLifecycleStatus::ClockExhausted);
                }
                _phase = RuntimeTerminalLifecyclePhase::IdleGrace;
            }
            return _result(
                RuntimeTerminalLifecycleStatus::SynchronizationReplayed,
                _deadline ? RuntimeTerminalLifecycleAction::Wait :
                            RuntimeTerminalLifecycleAction::None);
        }

        const auto decision = EvaluateTaskTerminalTransition(
            _state,
            state,
            {
                _generation,
                generation,
                generation != _generation,
                _hasActiveRun,
            });
        if (!decision.Allowed)
        {
            return _result(RuntimeTerminalLifecycleStatus::InvalidTransition);
        }

        _state = decision.NextState;
        _generation = generation;
        _clearDeadline();
        _setPhaseFromState();
        if (_state == TaskTerminalState::Ready &&
            !_hasActiveRun && _attachedClients == 0)
        {
            if (!_armDeadline(now))
            {
                return _fail(RuntimeTerminalLifecycleStatus::ClockExhausted);
            }
            _phase = RuntimeTerminalLifecyclePhase::IdleGrace;
            return _result(
                RuntimeTerminalLifecycleStatus::Synchronized,
                RuntimeTerminalLifecycleAction::Wait);
        }
        return _result(RuntimeTerminalLifecycleStatus::Synchronized);
    }
}
