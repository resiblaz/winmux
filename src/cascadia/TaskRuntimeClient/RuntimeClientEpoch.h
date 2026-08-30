// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeClientTypes.h"

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeClientEpochPhase
    {
        Offline,
        Active,
        Retiring,
        Exhausted,
    };

    enum class RuntimeClientEpochActivationStatus
    {
        Activated,
        Busy,
        Exhausted,
    };

    enum class RuntimeClientEpochRetireStatus
    {
        Started,
        AlreadyRetiring,
        StaleEpoch,
        NoActiveEpoch,
    };

    struct RuntimeClientEpochActivation
    {
        RuntimeClientEpochActivationStatus Status{ RuntimeClientEpochActivationStatus::Busy };
        RuntimeClientEpoch Epoch{};
    };

    struct RuntimeClientEpochRetirement
    {
        RuntimeClientEpochRetireStatus Status{ RuntimeClientEpochRetireStatus::NoActiveEpoch };
        RuntimeClientEpoch Epoch{};
        RuntimeClientRetireCause Cause{ RuntimeClientRetireCause::None };
    };

    // Pure caller-serialized state. The threaded client owns synchronization.
    class RuntimeClientEpochState final
    {
    public:
        explicit RuntimeClientEpochState(RuntimeClientEpoch lastIssued = 0) noexcept;

        [[nodiscard]] RuntimeClientEpochActivation Activate() noexcept;
        [[nodiscard]] RuntimeClientEpochRetirement BeginRetire(
            RuntimeClientEpoch epoch,
            RuntimeClientRetireCause cause) noexcept;
        [[nodiscard]] bool CompleteRetire(RuntimeClientEpoch epoch) noexcept;

        [[nodiscard]] bool IsActive(RuntimeClientEpoch epoch) const noexcept;
        [[nodiscard]] RuntimeClientEpochPhase Phase() const noexcept;
        [[nodiscard]] RuntimeClientEpoch Current() const noexcept;
        [[nodiscard]] RuntimeClientEpoch LastIssued() const noexcept;
        [[nodiscard]] RuntimeClientRetireCause RetireCause() const noexcept;

    private:
        RuntimeClientEpochPhase _phase{ RuntimeClientEpochPhase::Offline };
        RuntimeClientEpoch _current{};
        RuntimeClientEpoch _lastIssued{};
        RuntimeClientRetireCause _retireCause{ RuntimeClientRetireCause::None };
    };
}
