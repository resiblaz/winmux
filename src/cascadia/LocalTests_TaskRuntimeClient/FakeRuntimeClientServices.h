// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientInterfaces.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TaskRuntimeClientLocalTests
{
    using namespace Microsoft::Terminal::TaskRuntime;

    class DeterministicRuntimeClientRequestIdSource final : public IRuntimeClientRequestIdSource
    {
    public:
        void Enqueue(std::optional<std::string> requestId)
        {
            std::scoped_lock lock{ _mutex };
            _requestIds.emplace_back(std::move(requestId));
        }

        [[nodiscard]] std::optional<std::string> Next() noexcept override
        {
            try
            {
                std::scoped_lock lock{ _mutex };
                ++_calls;
                if (_requestIds.empty())
                {
                    return std::nullopt;
                }
                auto requestId = std::move(_requestIds.front());
                _requestIds.pop_front();
                return requestId;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        [[nodiscard]] size_t Calls() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _calls;
        }

        [[nodiscard]] size_t Remaining() const noexcept
        {
            std::scoped_lock lock{ _mutex };
            return _requestIds.size();
        }

    private:
        mutable std::mutex _mutex;
        std::deque<std::optional<std::string>> _requestIds;
        size_t _calls{};
    };

    class ScriptedRuntimeClientBackoffPolicy final : public IRuntimeClientBackoffPolicy
    {
    public:
        explicit ScriptedRuntimeClientBackoffPolicy(const uint32_t fallbackDelay = 0) noexcept :
            _fallbackDelay{ fallbackDelay }
        {
        }

        void EnqueueDelay(const uint32_t delayMilliseconds)
        {
            std::scoped_lock lock{ _mutex };
            _delays.emplace_back(delayMilliseconds);
        }

        [[nodiscard]] uint32_t DelayForFailure(const uint32_t consecutiveFailures) noexcept override
        {
            try
            {
                std::scoped_lock lock{ _mutex };
                _observedFailures.emplace_back(consecutiveFailures);
                if (_delays.empty())
                {
                    return _fallbackDelay;
                }
                const auto delay = _delays.front();
                _delays.pop_front();
                return delay;
            }
            catch (...)
            {
                return _fallbackDelay;
            }
        }

        [[nodiscard]] std::vector<uint32_t> ObservedFailures() const
        {
            std::scoped_lock lock{ _mutex };
            return _observedFailures;
        }

    private:
        mutable std::mutex _mutex;
        std::deque<uint32_t> _delays;
        std::vector<uint32_t> _observedFailures;
        uint32_t _fallbackDelay{};
    };
}
