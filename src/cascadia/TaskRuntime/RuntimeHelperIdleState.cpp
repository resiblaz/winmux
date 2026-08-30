// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeHelperIdleState.h"

#include <limits>

namespace Microsoft::Terminal::TaskRuntime
{
    RuntimeHelperIdleState::RuntimeHelperIdleState(
        RuntimeHelperIdleConfiguration configuration) noexcept :
        _configuration{ std::move(configuration) }
    {
        _valid = _configuration.IdleExitMilliseconds != 0;
        _phase = _valid ? RuntimeHelperIdlePhase::Active :
                          RuntimeHelperIdlePhase::Failed;
    }

    bool RuntimeHelperIdleState::Valid() const noexcept
    {
        return _valid;
    }

    RuntimeHelperIdleResult RuntimeHelperIdleState::Observe(
        const RuntimeHelperActivitySnapshot& activity,
        const RuntimeHelperIdleMilliseconds now) noexcept
    {
        if (!_valid)
        {
            return _result(RuntimeHelperIdleStatus::InvalidConfiguration);
        }
        if (!_observeTime(now))
        {
            return _fail(RuntimeHelperIdleStatus::ClockRegression);
        }
        if (!activity.Observed)
        {
            _activity = activity;
            _cancelIdle();
            return _result(RuntimeHelperIdleStatus::ObservationUnavailable);
        }
        if (_lastActivitySequence &&
            activity.ActivitySequence < *_lastActivitySequence)
        {
            return _fail(RuntimeHelperIdleStatus::ActivitySequenceRegression);
        }

        const auto newActivity = _lastActivitySequence &&
                                 activity.ActivitySequence != *_lastActivitySequence;
        _lastActivitySequence = activity.ActivitySequence;
        _activity = activity;
        if (_activity.Active())
        {
            _cancelIdle();
            return _result(RuntimeHelperIdleStatus::Active);
        }
        if (newActivity)
        {
            _cancelIdle();
        }
        if (!_deadline)
        {
            if (!_armDeadline(now))
            {
                return _fail(RuntimeHelperIdleStatus::ClockExhausted);
            }
            _phase = RuntimeHelperIdlePhase::IdleGrace;
            return _result(
                RuntimeHelperIdleStatus::IdleGraceStarted,
                RuntimeHelperIdleAction::Wait);
        }
        if (now < *_deadline)
        {
            _phase = RuntimeHelperIdlePhase::IdleGrace;
            return _result(
                RuntimeHelperIdleStatus::IdleGraceContinues,
                RuntimeHelperIdleAction::Wait);
        }

        _phase = RuntimeHelperIdlePhase::ExitReady;
        return _result(
            RuntimeHelperIdleStatus::ExitReady,
            RuntimeHelperIdleAction::ExitRuntime);
    }

    RuntimeHelperIdleView RuntimeHelperIdleState::Snapshot() const noexcept
    {
        return {
            _phase,
            _activity,
            _deadline,
            _lastObservation,
            _lastActivitySequence,
        };
    }

    bool RuntimeHelperIdleState::_observeTime(
        const RuntimeHelperIdleMilliseconds now) noexcept
    {
        if (_lastObservation && now < *_lastObservation)
        {
            return false;
        }
        _lastObservation = now;
        return true;
    }

    bool RuntimeHelperIdleState::_armDeadline(
        const RuntimeHelperIdleMilliseconds now) noexcept
    {
        const auto maximum =
            (std::numeric_limits<RuntimeHelperIdleMilliseconds>::max)();
        if (_configuration.IdleExitMilliseconds > maximum - now)
        {
            return false;
        }
        _deadline = now + _configuration.IdleExitMilliseconds;
        return true;
    }

    RuntimeHelperIdleResult RuntimeHelperIdleState::_result(
        const RuntimeHelperIdleStatus status,
        const RuntimeHelperIdleAction action) const noexcept
    {
        return { status, action, _deadline };
    }

    RuntimeHelperIdleResult RuntimeHelperIdleState::_fail(
        const RuntimeHelperIdleStatus status) noexcept
    {
        _valid = false;
        _deadline.reset();
        _phase = RuntimeHelperIdlePhase::Failed;
        return _result(status);
    }

    void RuntimeHelperIdleState::_cancelIdle() noexcept
    {
        _deadline.reset();
        _phase = RuntimeHelperIdlePhase::Active;
    }
}
