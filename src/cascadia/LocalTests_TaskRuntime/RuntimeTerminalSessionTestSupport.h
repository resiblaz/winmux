// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "FakeRuntimeTerminalBackend.h"
#include "RuntimeTerminalSession.h"

namespace TaskRuntimeLocalTests::RuntimeTerminalSessionTestSupport
{
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    inline constexpr uint64_t Generation{ 7 };
    inline constexpr std::string_view ClientA{
        "99000000-0000-4000-8000-000000000001"
    };
    inline constexpr std::string_view ClientB{
        "99000000-0000-4000-8000-000000000002"
    };
    inline constexpr std::string_view ClientC{
        "99000000-0000-4000-8000-000000000003"
    };

    inline RuntimeTerminalSessionConfiguration Configuration()
    {
        RuntimeTerminalSessionConfiguration configuration;
        configuration.Generation = Generation;
        configuration.MaximumOutputRingBytes = 256;
        configuration.MaximumOutputRingFrames = 8;
        configuration.MaximumClients = 3;
        configuration.MaximumQueuedBytesPerClient = 512;
        configuration.MaximumQueuedFramesPerClient = 8;
        configuration.MaximumFrameBytes = 128;
        configuration.LeaseDurationMilliseconds = 100;
        return configuration;
    }

    inline TerminalFrame Input(
        const uint64_t sequence,
        std::vector<uint8_t> payload,
        const uint64_t generation = Generation)
    {
        TerminalFrame frame;
        frame.Kind = TerminalFrameKind::Input;
        frame.Sequence = sequence;
        frame.Generation = generation;
        frame.Payload = std::move(payload);
        return frame;
    }

    inline TerminalFrame Resize(
        const uint64_t sequence,
        const uint32_t columns,
        const uint32_t rows,
        const uint64_t generation = Generation)
    {
        TerminalFrame frame;
        frame.Kind = TerminalFrameKind::Resize;
        frame.Sequence = sequence;
        frame.Generation = generation;
        const auto payload = EncodeResizePayload(columns, rows);
        frame.Payload.assign(payload.begin(), payload.end());
        return frame;
    }

    inline TerminalFrame Acknowledge(
        const uint64_t sequence,
        const uint64_t acknowledgedOutputSequence,
        const uint64_t generation = Generation)
    {
        TerminalFrame frame;
        frame.Kind = TerminalFrameKind::Acknowledge;
        frame.Sequence = sequence;
        frame.Generation = generation;
        const auto payload = EncodeAcknowledgementPayload(acknowledgedOutputSequence);
        frame.Payload.assign(payload.begin(), payload.end());
        return frame;
    }

    inline TerminalFrame Ping(const uint64_t sequence)
    {
        TerminalFrame frame;
        frame.Kind = TerminalFrameKind::Ping;
        frame.Sequence = sequence;
        return frame;
    }
}
