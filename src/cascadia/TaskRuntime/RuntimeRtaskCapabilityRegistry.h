// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RtaskCommand.h"
#include "RuntimeRtaskCapabilityToken.h"

#include <memory>
#include <optional>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    using RuntimeRtaskMilliseconds = uint64_t;

    inline constexpr size_t DefaultRuntimeRtaskMaximumCapabilities{ 1024 };
    inline constexpr size_t MaximumRuntimeRtaskCapabilities{ 4096 };
    inline constexpr uint32_t DefaultRuntimeRtaskCapabilityLifetimeMilliseconds{
        10 * 60 * 1000
    };
    inline constexpr uint32_t MaximumRuntimeRtaskCapabilityLifetimeMilliseconds{
        10 * 60 * 1000
    };

    struct RuntimeRtaskCapabilityRegistryConfiguration
    {
        size_t MaximumCapabilities{ DefaultRuntimeRtaskMaximumCapabilities };
        uint32_t MaximumLifetimeMilliseconds{
            MaximumRuntimeRtaskCapabilityLifetimeMilliseconds
        };
    };

    enum class RuntimeRtaskCapabilityStatus
    {
        Issued,
        Renewed,
        Replayed,
        Authorized,
        Invalidated,
        Missing,
        Expired,
        WrongRun,
        WrongMethod,
        ScopeConflict,
        CapacityExceeded,
        InvalidTask,
        InvalidRun,
        InvalidMethod,
        InvalidMethodSet,
        InvalidLifetime,
        InvalidToken,
        ClockRegression,
        ClockExhausted,
        TokenSourceFailure,
        TokenCollision,
        InvalidConfiguration,
        ResourceFailure,
    };

    struct RuntimeRtaskCapabilityGrant
    {
        std::string Capability;
        std::string TaskId;
        std::string RunId;
        Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethodSet Methods{};
        RuntimeRtaskMilliseconds IssuedAt{};
        RuntimeRtaskMilliseconds ExpiresAt{};
    };

    struct RuntimeRtaskCapabilityBinding
    {
        std::string TaskId;
        std::string RunId;
        Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethodSet Methods{};
        RuntimeRtaskMilliseconds ExpiresAt{};
    };

    struct RuntimeRtaskCapabilityResult
    {
        RuntimeRtaskCapabilityStatus Status{
            RuntimeRtaskCapabilityStatus::InvalidConfiguration
        };
        std::optional<RuntimeRtaskCapabilityGrant> Grant;
        std::optional<RuntimeRtaskCapabilityBinding> Binding;
        size_t InvalidatedCapabilities{};
    };

    // Single-owner state. Runtime serializes capability operations beside its
    // TaskRun lifecycle so terminal-state invalidation cannot race authorize.
    class RuntimeRtaskCapabilityRegistry final
    {
    public:
        RuntimeRtaskCapabilityRegistry(
            RuntimeRtaskCapabilityRegistryConfiguration configuration,
            std::shared_ptr<IRuntimeRtaskCapabilityTokenSource> tokenSource) noexcept;
        ~RuntimeRtaskCapabilityRegistry();

        RuntimeRtaskCapabilityRegistry(const RuntimeRtaskCapabilityRegistry&) = delete;
        RuntimeRtaskCapabilityRegistry& operator=(const RuntimeRtaskCapabilityRegistry&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeRtaskCapabilityResult IssueOrRenew(
            std::string_view taskId,
            std::string_view runId,
            Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethodSet methods,
            uint32_t lifetimeMilliseconds,
            RuntimeRtaskMilliseconds now);
        [[nodiscard]] RuntimeRtaskCapabilityResult Authorize(
            std::string_view capability,
            std::string_view runId,
            Microsoft::Terminal::TaskRuntimeProtocol::RtaskMethod method,
            RuntimeRtaskMilliseconds now);
        [[nodiscard]] RuntimeRtaskCapabilityResult InvalidateRun(
            std::string_view runId) noexcept;
        [[nodiscard]] RuntimeRtaskCapabilityResult InvalidateCapability(
            std::string_view capability) noexcept;

        [[nodiscard]] size_t Size() const noexcept;

    private:
        struct Entry;
        [[nodiscard]] RuntimeRtaskCapabilityStatus _observeTime(
            RuntimeRtaskMilliseconds now) noexcept;
        [[nodiscard]] std::optional<RuntimeRtaskMilliseconds> _deadline(
            RuntimeRtaskMilliseconds now,
            uint32_t lifetimeMilliseconds) const noexcept;
        void _purgeExpired(RuntimeRtaskMilliseconds now) noexcept;
        [[nodiscard]] static bool _sameCapability(
            std::string_view left,
            std::string_view right) noexcept;
        [[nodiscard]] static RuntimeRtaskCapabilityGrant _grant(
            const Entry& entry);

        RuntimeRtaskCapabilityRegistryConfiguration _configuration;
        std::shared_ptr<IRuntimeRtaskCapabilityTokenSource> _tokenSource;
        std::vector<Entry> _entries;
        RuntimeRtaskMilliseconds _lastObservedTime{};
        bool _observedTime{};
        bool _valid{};
    };
}
