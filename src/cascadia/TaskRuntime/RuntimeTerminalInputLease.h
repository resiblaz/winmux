// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeTerminalLeaseMilliseconds = uint64_t;
    using RuntimeTerminalLeaseRevision = uint64_t;

    inline constexpr uint32_t DefaultRuntimeTerminalLeaseDurationMilliseconds{ 15000 };
    inline constexpr uint32_t MaximumRuntimeTerminalLeaseDurationMilliseconds{ 60000 };

    struct RuntimeTerminalInputLeaseConfiguration
    {
        uint64_t Generation{};
        uint32_t LeaseDurationMilliseconds{
            DefaultRuntimeTerminalLeaseDurationMilliseconds
        };
        RuntimeTerminalLeaseRevision InitialLeaseRevision{};
    };

    struct RuntimeTerminalInputLeaseSnapshot
    {
        std::string OwnerClientInstanceId;
        uint64_t Generation{};
        RuntimeTerminalLeaseRevision LeaseRevision{};
        RuntimeTerminalLeaseMilliseconds ExpiresAt{};

        [[nodiscard]] bool Owned() const noexcept
        {
            return !OwnerClientInstanceId.empty() && Generation != 0 &&
                   LeaseRevision != 0 && ExpiresAt != 0;
        }
    };

    enum class RuntimeTerminalInputLeaseStatus
    {
        Acquired,
        Renewed,
        Released,
        AlreadyReleased,
        Authorized,
        Busy,
        NoLease,
        NotOwner,
        StaleLease,
        StaleGeneration,
        Expired,
        InvalidClient,
        InvalidConfiguration,
        ClockRegression,
        ClockExhausted,
        RevisionExhausted,
        ResourceFailure,
    };

    struct RuntimeTerminalInputLeaseResult
    {
        RuntimeTerminalInputLeaseStatus Status{
            RuntimeTerminalInputLeaseStatus::InvalidConfiguration
        };
        std::optional<RuntimeTerminalInputLeaseSnapshot> Lease;
    };

    class RuntimeTerminalInputLease final
    {
    public:
        explicit RuntimeTerminalInputLease(
            RuntimeTerminalInputLeaseConfiguration configuration) noexcept;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTerminalInputLeaseResult Acquire(
            std::string_view clientInstanceId,
            uint64_t generation,
            RuntimeTerminalLeaseMilliseconds now);
        [[nodiscard]] RuntimeTerminalInputLeaseResult Release(
            std::string_view clientInstanceId,
            uint64_t generation,
            RuntimeTerminalLeaseRevision leaseRevision,
            RuntimeTerminalLeaseMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalInputLeaseResult AuthorizeInput(
            std::string_view clientInstanceId,
            uint64_t generation,
            RuntimeTerminalLeaseRevision leaseRevision,
            RuntimeTerminalLeaseMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalInputLeaseResult Disconnect(
            std::string_view clientInstanceId,
            RuntimeTerminalLeaseMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalInputLeaseResult Expire(
            RuntimeTerminalLeaseMilliseconds now) noexcept;

        [[nodiscard]] std::optional<RuntimeTerminalInputLeaseSnapshot> Current() const;
        [[nodiscard]] RuntimeTerminalLeaseRevision LastIssuedRevision() const noexcept;

    private:
        [[nodiscard]] RuntimeTerminalInputLeaseStatus _observeTime(
            RuntimeTerminalLeaseMilliseconds now) noexcept;
        [[nodiscard]] RuntimeTerminalInputLeaseStatus _expireIfDue(
            RuntimeTerminalLeaseMilliseconds now) noexcept;
        [[nodiscard]] std::optional<RuntimeTerminalLeaseMilliseconds> _deadline(
            RuntimeTerminalLeaseMilliseconds now) const noexcept;
        [[nodiscard]] RuntimeTerminalInputLeaseResult _result(
            RuntimeTerminalInputLeaseStatus status) const noexcept;

        RuntimeTerminalInputLeaseConfiguration _configuration;
        std::optional<RuntimeTerminalInputLeaseSnapshot> _current;
        RuntimeTerminalLeaseRevision _lastIssuedRevision{};
        RuntimeTerminalLeaseMilliseconds _lastObservedNow{};
        bool _observedTime{};
        bool _valid{};
    };
}
