// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskEnums.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeTerminalLifecycleMilliseconds = uint64_t;

    inline constexpr uint32_t DefaultRuntimeTerminalIdleSuspendMilliseconds{
        5 * 60 * 1000
    };

    enum class RuntimeTerminalLifecyclePhase
    {
        Dormant,
        Live,
        IdleGrace,
        Suspended,
        Starting,
        Closed,
        Failed,
    };

    enum class RuntimeTerminalLifecycleAction
    {
        None,
        Wait,
        SuspendProjection,
        StartProjection,
    };

    enum class RuntimeTerminalWakeTrigger
    {
        ExplicitOpen,
        NewWork,
        PassiveReconnect,
    };

    enum class RuntimeTerminalLifecycleStatus
    {
        Dormant,
        Live,
        IdleGraceStarted,
        IdleGraceContinues,
        Suspended,
        WakeStarted,
        WakeReplayed,
        AlreadyAwake,
        WakeRejected,
        WakeCompleted,
        WakeFailed,
        Synchronized,
        SynchronizationReplayed,
        StaleGeneration,
        InvalidTransition,
        InvalidConfiguration,
        ClockRegression,
        ClockExhausted,
        GenerationExhausted,
    };

    struct RuntimeTerminalLifecycleConfiguration
    {
        Microsoft::Terminal::TaskCore::TaskTerminalState InitialState{
            Microsoft::Terminal::TaskCore::TaskTerminalState::NotCreated
        };
        uint64_t InitialGeneration{};
        uint32_t IdleSuspendMilliseconds{
            DefaultRuntimeTerminalIdleSuspendMilliseconds
        };
    };

    struct RuntimeTerminalLifecycleResult
    {
        RuntimeTerminalLifecycleStatus Status{
            RuntimeTerminalLifecycleStatus::InvalidConfiguration
        };
        RuntimeTerminalLifecycleAction Action{
            RuntimeTerminalLifecycleAction::None
        };
        Microsoft::Terminal::TaskCore::TaskTerminalState State{
            Microsoft::Terminal::TaskCore::TaskTerminalState::NotCreated
        };
        uint64_t Generation{};
        std::optional<RuntimeTerminalLifecycleMilliseconds> Deadline;
    };

    struct RuntimeTerminalLifecycleView
    {
        RuntimeTerminalLifecyclePhase Phase{
            RuntimeTerminalLifecyclePhase::Failed
        };
        Microsoft::Terminal::TaskCore::TaskTerminalState State{
            Microsoft::Terminal::TaskCore::TaskTerminalState::NotCreated
        };
        uint64_t Generation{};
        bool HasActiveRun{};
        size_t AttachedClients{};
        std::optional<RuntimeTerminalLifecycleMilliseconds> Deadline;
        std::optional<RuntimeTerminalLifecycleMilliseconds> LastObservation;
    };

    // Pure caller-serialized lifecycle policy. The Runtime dispatcher supplies
    // authoritative active-run and attached-client counts, executes returned
    // projection actions, and persists the matching TaskTerminal transition.
    // No timer thread, provider call, transport object, or SQLite handle lives
    // in this state machine.
    class RuntimeTerminalLifecycleState final
    {
    public:
        explicit RuntimeTerminalLifecycleState(
            RuntimeTerminalLifecycleConfiguration configuration) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleResult Observe(
            bool hasActiveRun,
            size_t attachedClients,
            RuntimeTerminalLifecycleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleResult RequestWake(
            RuntimeTerminalWakeTrigger trigger,
            RuntimeTerminalLifecycleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleResult CompleteWake(
            uint64_t generation,
            bool succeeded,
            RuntimeTerminalLifecycleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleResult Synchronize(
            Microsoft::Terminal::TaskCore::TaskTerminalState state,
            uint64_t generation,
            RuntimeTerminalLifecycleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleView Snapshot() const noexcept;

    private:
        [[nodiscard]] static bool _validState(
            Microsoft::Terminal::TaskCore::TaskTerminalState state,
            uint64_t generation) noexcept;
        [[nodiscard]] bool _observeTime(
            RuntimeTerminalLifecycleMilliseconds now) noexcept;
        [[nodiscard]] bool _armDeadline(
            RuntimeTerminalLifecycleMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleResult _result(
            RuntimeTerminalLifecycleStatus status,
            RuntimeTerminalLifecycleAction action =
                RuntimeTerminalLifecycleAction::None) const noexcept;
        [[nodiscard]] RuntimeTerminalLifecycleResult _fail(
            RuntimeTerminalLifecycleStatus status) noexcept;
        void _clearDeadline() noexcept;
        void _setPhaseFromState() noexcept;

        RuntimeTerminalLifecycleConfiguration _configuration;
        RuntimeTerminalLifecyclePhase _phase{
            RuntimeTerminalLifecyclePhase::Failed
        };
        Microsoft::Terminal::TaskCore::TaskTerminalState _state{
            Microsoft::Terminal::TaskCore::TaskTerminalState::NotCreated
        };
        uint64_t _generation{};
        bool _hasActiveRun{};
        size_t _attachedClients{};
        std::optional<RuntimeTerminalLifecycleMilliseconds> _deadline;
        std::optional<RuntimeTerminalLifecycleMilliseconds> _lastObservation;
        bool _valid{};
    };
}
