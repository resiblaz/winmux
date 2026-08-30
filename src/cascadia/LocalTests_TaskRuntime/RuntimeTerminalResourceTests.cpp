// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimePipe.h"
#include "RuntimeTerminalChannel.h"
#include "RuntimeTerminalEndpoint.h"
#include "TaskRuntimeTestFixtures.h"

#include "TerminalFrame.h"

#include <thread>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    class RuntimeTerminalResourceTests
    {
        TEST_CLASS(RuntimeTerminalResourceTests);

        TEST_METHOD(RepeatedConnectRoundTripAndCloseReleasesHandles);
    };

    void RuntimeTerminalResourceTests::RepeatedConnectRoundTripAndCloseReleasesHandles()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        const auto endpoint = BuildRuntimeTerminalEndpoint(
            identity,
            "96000000-0000-4000-8000-000000000001",
            1);
        VERIFY_IS_TRUE(endpoint.Succeeded());
        auto security = RequireSecurity(identity.UserSid);

        const auto roundTrip = [&] {
            auto listener = CreateRuntimeTerminalPipe(
                endpoint.Endpoint,
                security.Attributes());
            VERIFY_IS_TRUE(listener.Created());
            RuntimeTerminalChannelResult accepted;
            std::thread acceptThread{ [&] {
                accepted = AcceptRuntimeTerminalChannel(
                    listener.Pipe,
                    endpoint.Endpoint,
                    64 * 1024,
                    5000);
            } };
            auto connected = ConnectRuntimeTerminalChannel(
                endpoint.Endpoint,
                64 * 1024,
                5000);
            acceptThread.join();
            VERIFY_IS_TRUE(accepted.Succeeded());
            VERIFY_IS_TRUE(connected.Succeeded());

            TerminalFrame ping;
            ping.Kind = TerminalFrameKind::Ping;
            ping.Sequence = 1;
            ping.Generation = 0;
            VERIFY_IS_TRUE(connected.Channel.Send(ping, 2000).Succeeded());
            const auto received = accepted.Channel.Receive(2000);
            VERIFY_IS_TRUE(received.Succeeded());
            VERIFY_IS_TRUE(received.Frame.Kind == TerminalFrameKind::Ping);
            connected.Channel.Close();
            accepted.Channel.Close();
        };

        roundTrip();
        DWORD handlesBefore{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
            GetCurrentProcess(),
            &handlesBefore));
        for (size_t iteration = 0; iteration < 100; ++iteration)
        {
            roundTrip();
        }
        DWORD handlesAfter{};
        VERIFY_IS_TRUE(GetProcessHandleCount(
            GetCurrentProcess(),
            &handlesAfter));
        VERIFY_IS_LESS_THAN_OR_EQUAL(
            handlesAfter,
            handlesBefore + 2);
    }
}
