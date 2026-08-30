// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlChannel.h"
#include "RuntimePipe.h"
#include "RuntimeTerminalChannel.h"
#include "RuntimeTerminalEndpoint.h"
#include "TaskRuntimeProcessWin32Support.h"
#include "TaskRuntimeTestFixtures.h"

#include "TerminalFrame.h"

#include <atomic>
#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr std::string_view TerminalSessionId{
            "94000000-0000-4000-8000-000000000001"
        };
        constexpr uint64_t TerminalGeneration{ 7 };

        struct TerminalChannelPair
        {
            RuntimeTerminalChannel Server;
            RuntimeTerminalChannel Client;
            RuntimeTerminalEndpoint Endpoint;
        };

        struct ControlChannelPair
        {
            RuntimeControlChannel Server;
            RuntimeControlChannel Client;
        };

        TerminalChannelPair _terminalPair(
            const uint32_t maximumPayloadBytes = MaximumTerminalFrameBytes)
        {
            UniqueRuntimeRoot root;
            const auto identity = RequireIdentity(root.Path());
            const auto endpoint = BuildRuntimeTerminalEndpoint(
                identity,
                TerminalSessionId,
                TerminalGeneration);
            VERIFY_IS_TRUE(endpoint.Succeeded());
            auto security = RequireSecurity(identity.UserSid);
            auto listener = CreateRuntimeTerminalPipe(
                endpoint.Endpoint,
                security.Attributes());
            VERIFY_IS_TRUE(listener.Created());

            RuntimeTerminalChannelResult accepted;
            std::thread acceptThread{ [&] {
                accepted = AcceptRuntimeTerminalChannel(
                    listener.Pipe,
                    endpoint.Endpoint,
                    maximumPayloadBytes,
                    5000);
            } };
            auto connected = ConnectRuntimeTerminalChannel(
                endpoint.Endpoint,
                maximumPayloadBytes,
                5000);
            acceptThread.join();
            VERIFY_IS_TRUE(accepted.Succeeded());
            VERIFY_IS_TRUE(connected.Succeeded());
            return {
                std::move(accepted.Channel),
                std::move(connected.Channel),
                endpoint.Endpoint,
            };
        }

        ControlChannelPair _controlPair()
        {
            UniqueRuntimeRoot root;
            const auto identity = RequireIdentity(root.Path());
            auto security = RequireSecurity(identity.UserSid);
            auto listener = CreateRuntimeControlPipe(
                identity,
                security.Attributes());
            VERIFY_IS_TRUE(listener.Created());

            RuntimeControlChannelResult accepted;
            std::thread acceptThread{ [&] {
                accepted = AcceptRuntimeControlChannel(listener.Pipe, 5000);
            } };
            auto connected = ConnectRuntimeControlChannel(identity, 5000);
            acceptThread.join();
            VERIFY_IS_TRUE(accepted.Succeeded());
            VERIFY_IS_TRUE(connected.Succeeded());
            return {
                std::move(accepted.Channel),
                std::move(connected.Channel),
            };
        }

        TerminalFrame _frame(
            const TerminalFrameKind kind,
            const uint64_t sequence,
            const uint64_t generation,
            std::vector<uint8_t> payload = {})
        {
            TerminalFrame frame;
            frame.Kind = kind;
            frame.Sequence = sequence;
            frame.Generation = generation;
            frame.Payload = std::move(payload);
            return frame;
        }

        void _writeRaw(
            const HANDLE handle,
            const std::span<const uint8_t> bytes)
        {
            const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            VERIFY_IS_NOT_NULL(event);
            if (!event)
            {
                return;
            }
            OVERLAPPED operation{};
            operation.hEvent = event;
            DWORD transferred{};
            const auto started = WriteFile(
                handle,
                bytes.data(),
                static_cast<DWORD>(bytes.size()),
                &transferred,
                &operation);
            if (!started)
            {
                VERIFY_ARE_EQUAL(DWORD{ ERROR_IO_PENDING }, GetLastError());
                VERIFY_ARE_EQUAL(
                    DWORD{ WAIT_OBJECT_0 },
                    WaitForSingleObject(event, 5000));
                VERIFY_IS_TRUE(GetOverlappedResult(
                    handle,
                    &operation,
                    &transferred,
                    FALSE));
            }
            VERIFY_ARE_EQUAL(static_cast<DWORD>(bytes.size()), transferred);
            CloseHandle(event);
        }

        void _append(
            std::vector<uint8_t>& target,
            const std::vector<uint8_t>& source)
        {
            target.insert(target.end(), source.begin(), source.end());
        }
    }

    class RuntimeTerminalChannelTests
    {
        TEST_CLASS(RuntimeTerminalChannelTests);

        TEST_METHOD(BinaryFramesRoundTripAcrossFragmentedAndCoalescedWrites);
        TEST_METHOD(ChannelRejectsStaleGenerationAndNonMonotonicSend);
        TEST_METHOD(PayloadBoundsAndMalformedInputFailClosed);
        TEST_METHOD(CancellationInterruptsPendingTerminalRead);
        TEST_METHOD(TerminalBackpressureDoesNotBlockControlPipe);
        TEST_METHOD(MissingTerminalEndpointTimeoutAndCancellationAreBounded);
    };

    void RuntimeTerminalChannelTests::BinaryFramesRoundTripAcrossFragmentedAndCoalescedWrites()
    {
        auto pair = _terminalPair();
        const auto output = EncodeTerminalFrame(_frame(
            TerminalFrameKind::Output,
            1,
            TerminalGeneration,
            { 0x1b, '[', '3', '1', 'm', 0xff }));
        VERIFY_IS_TRUE(output.Succeeded);
        _writeRaw(pair.Server.Handle(), std::span{ output.Bytes }.first(7));
        _writeRaw(pair.Server.Handle(), std::span{ output.Bytes }.subspan(7, 19));
        _writeRaw(pair.Server.Handle(), std::span{ output.Bytes }.subspan(26));

        const auto decodedOutput = pair.Client.Receive(2000);
        VERIFY_IS_TRUE(decodedOutput.Succeeded());
        VERIFY_IS_TRUE(decodedOutput.Frame.Kind == TerminalFrameKind::Output);
        VERIFY_IS_TRUE((
            decodedOutput.Frame.Payload ==
            std::vector<uint8_t>{ 0x1b, '[', '3', '1', 'm', 0xff }));

        const auto ping = EncodeTerminalFrame(_frame(
            TerminalFrameKind::Ping,
            2,
            0));
        const auto attach = EncodeTerminalFrame(_frame(
            TerminalFrameKind::Attach,
            3,
            TerminalGeneration,
            { '{', '}' }));
        std::vector<uint8_t> coalesced;
        _append(coalesced, ping.Bytes);
        _append(coalesced, attach.Bytes);
        _writeRaw(pair.Server.Handle(), coalesced);

        const auto decodedPing = pair.Client.Receive(2000);
        const auto decodedAttach = pair.Client.Receive(2000);
        VERIFY_IS_TRUE(decodedPing.Succeeded());
        VERIFY_IS_TRUE(decodedAttach.Succeeded());
        VERIFY_IS_TRUE(decodedPing.Frame.Kind == TerminalFrameKind::Ping);
        VERIFY_IS_TRUE(decodedAttach.Frame.Kind == TerminalFrameKind::Attach);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, decodedAttach.Frame.Sequence);
    }

    void RuntimeTerminalChannelTests::ChannelRejectsStaleGenerationAndNonMonotonicSend()
    {
        auto pair = _terminalPair();
        const auto stale = pair.Server.Send(
            _frame(TerminalFrameKind::Output, 1, TerminalGeneration + 1, { 'x' }),
            2000);
        VERIFY_IS_TRUE(stale.Status == RuntimeTransportStatus::InvalidConfiguration);
        VERIFY_IS_TRUE(pair.Server.Connected());

        const auto sent = pair.Server.Send(
            _frame(TerminalFrameKind::Output, 2, TerminalGeneration, { 'y' }),
            2000);
        VERIFY_IS_TRUE(sent.Succeeded());
        const auto duplicate = pair.Server.Send(
            _frame(TerminalFrameKind::Output, 2, TerminalGeneration, { 'z' }),
            2000);
        VERIFY_IS_TRUE(
            duplicate.Status == RuntimeTransportStatus::InvalidConfiguration);
        VERIFY_IS_TRUE(pair.Client.Receive(2000).Succeeded());

        const auto staleInbound = EncodeTerminalFrame(_frame(
            TerminalFrameKind::Output,
            3,
            TerminalGeneration + 1,
            { 'z' }));
        VERIFY_IS_TRUE(staleInbound.Succeeded);
        _writeRaw(pair.Server.Handle(), staleInbound.Bytes);
        const auto rejected = pair.Client.Receive(2000);
        VERIFY_IS_TRUE(rejected.Status == RuntimeTransportStatus::MalformedFrame);
        VERIFY_IS_FALSE(pair.Client.Connected());
        VERIFY_IS_TRUE(
            pair.Client.Receive(2000).Status == RuntimeTransportStatus::MalformedFrame);
    }

    void RuntimeTerminalChannelTests::PayloadBoundsAndMalformedInputFailClosed()
    {
        constexpr uint32_t maximumPayload{ 1024 };
        auto pair = _terminalPair(maximumPayload);
        const auto oversized = pair.Server.Send(
            _frame(
                TerminalFrameKind::Output,
                1,
                TerminalGeneration,
                std::vector<uint8_t>(maximumPayload + 1, 'x')),
            2000);
        VERIFY_IS_TRUE(
            oversized.Status == RuntimeTransportStatus::InvalidConfiguration);

        const auto rawOversized = EncodeTerminalFrame(
            _frame(
                TerminalFrameKind::Output,
                1,
                TerminalGeneration,
                std::vector<uint8_t>(maximumPayload + 1, 'x')),
            MaximumTerminalFrameBytes);
        VERIFY_IS_TRUE(rawOversized.Succeeded);
        _writeRaw(pair.Server.Handle(), rawOversized.Bytes);
        VERIFY_IS_TRUE(
            pair.Client.Receive(2000).Status == RuntimeTransportStatus::MalformedFrame);
        VERIFY_IS_FALSE(pair.Client.Connected());
    }

    void RuntimeTerminalChannelTests::CancellationInterruptsPendingTerminalRead()
    {
        auto pair = _terminalPair();
        ProcessFixtures::details::HandleOwner cancellation{
            CreateEventW(nullptr, TRUE, FALSE, nullptr)
        };
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        RuntimeTerminalReceiveResult cancelled;
        std::thread reader{ [&] {
            cancelled = pair.Client.Receive(5000, cancellation.Get());
        } };
        Sleep(50);
        SetEvent(cancellation.Get());
        reader.join();
        VERIFY_IS_TRUE(cancelled.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(pair.Client.Connected());

        RuntimeTerminalReceiveResult broadlyCancelled;
        std::thread broadReader{ [&] {
            broadlyCancelled = pair.Client.Receive(5000);
        } };
        Sleep(50);
        pair.Client.CancelPendingIo();
        broadReader.join();
        VERIFY_IS_TRUE(
            broadlyCancelled.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_FALSE(pair.Client.Connected());
    }

    void RuntimeTerminalChannelTests::TerminalBackpressureDoesNotBlockControlPipe()
    {
        auto terminal = _terminalPair();
        auto control = _controlPair();
        ProcessFixtures::details::HandleOwner cancellation{
            CreateEventW(nullptr, TRUE, FALSE, nullptr)
        };
        ProcessFixtures::details::HandleOwner writerStarted{
            CreateEventW(nullptr, TRUE, FALSE, nullptr)
        };
        ProcessFixtures::details::HandleOwner writerFinished{
            CreateEventW(nullptr, TRUE, FALSE, nullptr)
        };
        VERIFY_IS_TRUE(cancellation && writerStarted && writerFinished);

        RuntimeTransportResult terminalResult;
        std::thread blockedWriter{ [&] {
            SetEvent(writerStarted.Get());
            const auto frame = _frame(
                TerminalFrameKind::Output,
                1,
                TerminalGeneration,
                std::vector<uint8_t>(MaximumTerminalFrameBytes, 'v'));
            terminalResult = terminal.Server.Send(
                frame,
                10000,
                cancellation.Get());
            SetEvent(writerFinished.Get());
        } };
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(writerStarted.Get(), 2000));
        Sleep(100);
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_TIMEOUT },
            WaitForSingleObject(writerFinished.Get(), 0));

        const auto started = GetTickCount64();
        VERIFY_IS_TRUE(control.Client.Send("{\"stop\":true}", 1000).Succeeded());
        const auto request = control.Server.Receive(1000);
        VERIFY_IS_TRUE(request.Succeeded());
        VERIFY_ARE_EQUAL(std::string{ "{\"stop\":true}" }, request.Payload);
        VERIFY_IS_TRUE(control.Server.Send("{\"ok\":true}", 1000).Succeeded());
        VERIFY_IS_TRUE(control.Client.Receive(1000).Succeeded());
        VERIFY_IS_TRUE(GetTickCount64() - started < 1000);

        SetEvent(cancellation.Get());
        VERIFY_ARE_EQUAL(
            DWORD{ WAIT_OBJECT_0 },
            WaitForSingleObject(writerFinished.Get(), 2000));
        blockedWriter.join();
        VERIFY_IS_TRUE(terminalResult.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(control.Client.Connected());
        VERIFY_IS_TRUE(control.Server.Connected());
    }

    void RuntimeTerminalChannelTests::MissingTerminalEndpointTimeoutAndCancellationAreBounded()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        const auto endpoint = BuildRuntimeTerminalEndpoint(
            identity,
            TerminalSessionId,
            TerminalGeneration);
        VERIFY_IS_TRUE(endpoint.Succeeded());

        const auto started = GetTickCount64();
        const auto timeout = ConnectRuntimeTerminalChannel(
            endpoint.Endpoint,
            MaximumTerminalFrameBytes,
            50);
        VERIFY_IS_TRUE(timeout.Status == RuntimeTransportStatus::Timeout);
        VERIFY_IS_TRUE(GetTickCount64() - started < 1000);

        ProcessFixtures::details::HandleOwner cancellation{
            CreateEventW(nullptr, TRUE, TRUE, nullptr)
        };
        const auto cancelled = ConnectRuntimeTerminalChannel(
            endpoint.Endpoint,
            MaximumTerminalFrameBytes,
            5000,
            cancellation.Get());
        VERIFY_IS_TRUE(cancelled.Status == RuntimeTransportStatus::Cancelled);
    }
}
