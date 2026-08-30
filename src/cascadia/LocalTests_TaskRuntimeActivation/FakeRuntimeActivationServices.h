// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeActivationServices.h"

#include <deque>
#include <mutex>
#include <vector>

namespace TaskRuntimeActivationLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;

    class FakeRuntimeActivationServices final : public IRuntimeActivationServices
    {
    public:
        explicit FakeRuntimeActivationServices(
            const RuntimeActivationMonotonicMilliseconds now = 0) noexcept :
            _now{ now }
        {
        }

        RuntimeActivationMonotonicMilliseconds Now() noexcept override
        {
            std::scoped_lock lock{ _mutex };
            return _now;
        }

        RuntimeActivationWaitResult WaitUntil(
            const RuntimeActivationDeadline deadline,
            const std::span<const HANDLE> wakeHandles) noexcept override
        {
            try
            {
                std::scoped_lock lock{ _mutex };
                _deadlines.emplace_back(deadline);
                _wakeHandles.emplace_back(wakeHandles.begin(), wakeHandles.end());
                if (_results.empty())
                {
                    return {
                        RuntimeActivationWaitStatus::Failed,
                        InvalidRuntimeActivationWaitIndex,
                        ERROR_INVALID_STATE,
                    };
                }
                auto result = _results.front();
                _results.pop_front();
                if (AdvanceToDeadlineOnTimeout &&
                    result.Status == RuntimeActivationWaitStatus::DeadlineReached)
                {
                    _now = deadline.Value;
                }
                return result;
            }
            catch (...)
            {
                return {
                    RuntimeActivationWaitStatus::Failed,
                    InvalidRuntimeActivationWaitIndex,
                    ERROR_NOT_ENOUGH_MEMORY,
                };
            }
        }

        void Enqueue(RuntimeActivationWaitResult result)
        {
            std::scoped_lock lock{ _mutex };
            _results.emplace_back(result);
        }

        void SetNow(const RuntimeActivationMonotonicMilliseconds now) noexcept
        {
            std::scoped_lock lock{ _mutex };
            _now = now;
        }

        [[nodiscard]] std::vector<RuntimeActivationDeadline> Deadlines() const
        {
            std::scoped_lock lock{ _mutex };
            return _deadlines;
        }

        [[nodiscard]] std::vector<std::vector<HANDLE>> WakeHandles() const
        {
            std::scoped_lock lock{ _mutex };
            return _wakeHandles;
        }

        bool AdvanceToDeadlineOnTimeout{};

    private:
        mutable std::mutex _mutex;
        RuntimeActivationMonotonicMilliseconds _now{};
        std::deque<RuntimeActivationWaitResult> _results;
        std::vector<RuntimeActivationDeadline> _deadlines;
        std::vector<std::vector<HANDLE>> _wakeHandles;
    };
}
