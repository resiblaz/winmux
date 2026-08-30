// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeHelperIdleMilliseconds = uint64_t;

    inline constexpr uint32_t DefaultRuntimeHelperIdleExitMilliseconds{
        2 * 60 * 1000
    };

    struct RuntimeHelperActivitySnapshot
    {
        bool Observed{ true };
        uint64_t ActivitySequence{};
        size_t ControlClients{};
        size_t TerminalClients{};
        size_t ActiveRuns{};
        size_t PendingWork{};
        size_t LiveTerminalSessions{};

        [[nodiscard]] bool Active() const noexcept
        {
            return ControlClients != 0 || TerminalClients != 0 ||
                   ActiveRuns != 0 || PendingWork != 0 ||
                   LiveTerminalSessions != 0;
        }
    };

    struct RuntimeHelperIdleConfiguration
    {
        uint32_t IdleExitMilliseconds{
            DefaultRuntimeHelperIdleExitMilliseconds
        };
    };

    enum class RuntimeHelperIdlePhase
    {
        Active,
        IdleGrace,
        ExitReady,
        Failed,
    };

    enum class RuntimeHelperIdleAction
    {
        None,
        Wait,
        ExitRuntime,
    };

    enum class RuntimeHelperIdleStatus
    {
        Active,
        IdleGraceStarted,
        IdleGraceContinues,
        ExitReady,
        ObservationUnavailable,
        InvalidConfiguration,
        ClockRegression,
        ClockExhausted,
        ActivitySequenceRegression,
    };

    struct RuntimeHelperIdleResult
    {
        RuntimeHelperIdleStatus Status{
            RuntimeHelperIdleStatus::InvalidConfiguration
        };
        RuntimeHelperIdleAction Action{ RuntimeHelperIdleAction::None };
        std::optional<RuntimeHelperIdleMilliseconds> Deadline;
    };

    struct RuntimeHelperIdleView
    {
        RuntimeHelperIdlePhase Phase{ RuntimeHelperIdlePhase::Failed };
        RuntimeHelperActivitySnapshot Activity;
        std::optional<RuntimeHelperIdleMilliseconds> Deadline;
        std::optional<RuntimeHelperIdleMilliseconds> LastObservation;
        std::optional<uint64_t> LastActivitySequence;
    };

    // Pure caller-serialized helper lifetime policy. The host supplies bounded
    // activity observations; this type owns no thread, wait, server, or store.
    class RuntimeHelperIdleState final
    {
    public:
        explicit RuntimeHelperIdleState(
            RuntimeHelperIdleConfiguration configuration = {}) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeHelperIdleResult Observe(
            const RuntimeHelperActivitySnapshot& activity,
            RuntimeHelperIdleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeHelperIdleView Snapshot() const noexcept;

    private:
        [[nodiscard]] bool _observeTime(
            RuntimeHelperIdleMilliseconds now) noexcept;
        [[nodiscard]] bool _armDeadline(
            RuntimeHelperIdleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeHelperIdleResult _result(
            RuntimeHelperIdleStatus status,
            RuntimeHelperIdleAction action = RuntimeHelperIdleAction::None) const noexcept;
        [[nodiscard]] RuntimeHelperIdleResult _fail(
            RuntimeHelperIdleStatus status) noexcept;
        void _cancelIdle() noexcept;

        RuntimeHelperIdleConfiguration _configuration;
        RuntimeHelperIdlePhase _phase{ RuntimeHelperIdlePhase::Failed };
        RuntimeHelperActivitySnapshot _activity;
        std::optional<RuntimeHelperIdleMilliseconds> _deadline;
        std::optional<RuntimeHelperIdleMilliseconds> _lastObservation;
        std::optional<uint64_t> _lastActivitySequence;
        bool _valid{};
    };
}
