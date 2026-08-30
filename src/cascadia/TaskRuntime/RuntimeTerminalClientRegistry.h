// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TerminalFrame.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::TaskRuntime
{
    inline constexpr size_t DefaultRuntimeTerminalClientQueueBytes{ 4 * 1024 * 1024 };
    inline constexpr size_t DefaultRuntimeTerminalClientQueueFrames{ 4096 };
    inline constexpr size_t DefaultRuntimeTerminalMaximumClients{ 32 };

    struct RuntimeTerminalClientRegistryConfiguration
    {
        uint64_t Generation{};
        size_t MaximumClients{ DefaultRuntimeTerminalMaximumClients };
        size_t MaximumQueuedBytesPerClient{
            DefaultRuntimeTerminalClientQueueBytes
        };
        size_t MaximumQueuedFramesPerClient{
            DefaultRuntimeTerminalClientQueueFrames
        };
        uint32_t MaximumFrameBytes{
            Microsoft::Terminal::TaskRuntimeProtocol::MaximumTerminalFrameBytes
        };
    };

    enum class RuntimeTerminalClientRegistryStatus
    {
        Attached,
        AlreadyAttached,
        Detached,
        LeaseBound,
        LeaseCleared,
        Queued,
        Pulled,
        Empty,
        Acknowledged,
        DuplicateAcknowledgement,
        SlowPeer,
        NotFound,
        StaleGeneration,
        AcknowledgementAhead,
        AcknowledgementRegression,
        InvalidClient,
        InvalidLeaseRevision,
        InvalidConfiguration,
        InvalidFrame,
        CapacityExceeded,
        ResourceFailure,
    };

    struct RuntimeTerminalClientRegistryResult
    {
        RuntimeTerminalClientRegistryStatus Status{
            RuntimeTerminalClientRegistryStatus::InvalidConfiguration
        };
        std::optional<Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame> Frame;
        std::optional<uint64_t> LeaseRevision;
    };

    struct RuntimeTerminalClientSnapshot
    {
        std::string ClientInstanceId;
        uint64_t Generation{};
        uint64_t LastAcknowledgedOutputSequence{};
        std::optional<uint64_t> LeaseRevision;
        size_t QueuedFrames{};
        size_t QueuedBytes{};
    };

    class RuntimeTerminalClientRegistry final
    {
    public:
        explicit RuntimeTerminalClientRegistry(
            RuntimeTerminalClientRegistryConfiguration configuration) noexcept;
        ~RuntimeTerminalClientRegistry();

        RuntimeTerminalClientRegistry(const RuntimeTerminalClientRegistry&) = delete;
        RuntimeTerminalClientRegistry& operator=(const RuntimeTerminalClientRegistry&) = delete;

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] RuntimeTerminalClientRegistryResult Attach(
            std::string_view clientInstanceId,
            uint64_t generation,
            uint64_t acknowledgedOutputSequence);
        [[nodiscard]] RuntimeTerminalClientRegistryResult Detach(
            std::string_view clientInstanceId) noexcept;
        [[nodiscard]] RuntimeTerminalClientRegistryResult BindLease(
            std::string_view clientInstanceId,
            uint64_t leaseRevision) noexcept;
        [[nodiscard]] RuntimeTerminalClientRegistryResult ClearLease(
            std::string_view clientInstanceId) noexcept;
        [[nodiscard]] RuntimeTerminalClientRegistryResult Queue(
            std::string_view clientInstanceId,
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame);
        [[nodiscard]] RuntimeTerminalClientRegistryResult QueueMany(
            std::string_view clientInstanceId,
            std::span<const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame> frames);
        [[nodiscard]] RuntimeTerminalClientRegistryResult Pull(
            std::string_view clientInstanceId) noexcept;
        [[nodiscard]] RuntimeTerminalClientRegistryResult Acknowledge(
            std::string_view clientInstanceId,
            uint64_t generation,
            uint64_t acknowledgedOutputSequence,
            uint64_t latestOutputSequence) noexcept;

        [[nodiscard]] std::optional<RuntimeTerminalClientSnapshot> Find(
            std::string_view clientInstanceId) const;
        [[nodiscard]] std::vector<std::string> ClientIds() const;
        [[nodiscard]] size_t Size() const noexcept;

    private:
        struct Client;
        [[nodiscard]] Client* _find(std::string_view clientInstanceId) noexcept;
        [[nodiscard]] const Client* _find(std::string_view clientInstanceId) const noexcept;
        [[nodiscard]] bool _validFrame(
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame) const;
        [[nodiscard]] static size_t _wireBytes(
            const Microsoft::Terminal::TaskRuntimeProtocol::TerminalFrame& frame) noexcept;

        RuntimeTerminalClientRegistryConfiguration _configuration;
        std::vector<Client> _clients;
        bool _valid{};
    };
}
