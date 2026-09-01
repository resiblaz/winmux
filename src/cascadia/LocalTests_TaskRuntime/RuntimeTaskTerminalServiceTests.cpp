// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeTaskTerminalService.h"
#include "RuntimeTerminalChannel.h"
#include "RuntimeTerminalHandshake.h"
#include "TaskRuntimeTestFixtures.h"

#include "ProtocolCapabilities.h"
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
        constexpr std::string_view RuntimeInstance{ "a1000000-0000-4000-8000-000000000001" };
        constexpr std::string_view TaskId{ "a2000000-0000-4000-8000-000000000002" };
        constexpr std::string_view SessionId{ "a3000000-0000-4000-8000-000000000003" };
        constexpr std::string_view ClientA{ "a4000000-0000-4000-8000-000000000004" };
        constexpr std::string_view ClientB{ "a5000000-0000-4000-8000-000000000005" };

        RuntimeTerminalChannel _connect(
            const RuntimeTerminalEndpoint& endpoint,
            const std::string_view runtimeInstance,
            const std::string_view clientId)
        {
            auto connected = ConnectRuntimeTerminalChannel(endpoint, MaximumTerminalFrameBytes, 5000);
            VERIFY_IS_TRUE(connected.Succeeded());
            HelloRequest hello;
            hello.ClientInstanceId = std::string{ clientId };
            hello.ClientKind = ProtocolClientKind::TerminalUi;
            hello.ExpectedRuntimeIdentity = endpoint.RuntimeIdentity;
            hello.RequiredCapabilities = static_cast<uint64_t>(ProtocolCapability::TerminalStream);
            hello.OptionalCapabilities = static_cast<uint64_t>(ProtocolCapability::InputControllerLease);
            VERIFY_IS_TRUE(PerformRuntimeTerminalClientHandshake(
                               connected.Channel, hello, runtimeInstance, 5000)
                               .Succeeded());
            TerminalFrame attach;
            attach.Kind = TerminalFrameKind::Attach;
            attach.Sequence = 2;
            attach.Generation = endpoint.Generation;
            attach.Payload = { '0' };
            VERIFY_IS_TRUE(connected.Channel.Send(attach, 5000).Succeeded());
            return std::move(connected.Channel);
        }

        bool _waitUntil(const std::function<bool()>& predicate)
        {
            const auto deadline = GetTickCount64() + 3000;
            while (GetTickCount64() < deadline)
            {
                if (predicate())
                    return true;
                Sleep(10);
            }
            return predicate();
        }
    }

    class RuntimeTaskTerminalServiceTests
    {
        TEST_CLASS(RuntimeTaskTerminalServiceTests);

        TEST_METHOD(OpenStreamsSnapshotInputInterruptAndGracefulClose);
        TEST_METHOD(SurfaceOwnerIsExclusiveUntilTerminalClientDetaches);
    };

    void RuntimeTaskTerminalServiceTests::OpenStreamsSnapshotInputInterruptAndGracefulClose()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        std::atomic_uint32_t inputs{};
        std::atomic_uint32_t interrupts{};
        std::string observedInput;
        std::string observedInputTask;
        std::string observedInterruptTask;
        RuntimeTaskTerminalService service{
            identity,
            std::string{ RuntimeInstance },
            [&](const std::string_view taskId, const std::span<const uint8_t> input) {
                observedInputTask.assign(taskId);
                observedInput.assign(input.begin(), input.end());
                ++inputs;
                return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} };
            },
            [&](const std::string_view taskId) {
                observedInterruptTask.assign(taskId);
                ++interrupts;
                return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} };
            }
        };
        VERIFY_IS_TRUE(service.Valid());
        const auto opened = service.Open({ std::string{ TaskId }, std::string{ SessionId }, 7, std::string{ ClientA }, "Terminal test" });
        VERIFY_IS_TRUE(opened.Succeeded());
        auto client = _connect(opened.Endpoint, RuntimeInstance, ClientA);
        const auto snapshot = client.Receive(5000);
        VERIFY_IS_TRUE(snapshot.Succeeded());
        VERIFY_IS_TRUE(snapshot.Frame.Kind == TerminalFrameKind::Output);
        VERIFY_IS_TRUE((snapshot.Frame.Flags & static_cast<uint32_t>(TerminalFrameFlag::ResetBeforePayload)) != 0);

        TerminalFrame input;
        input.Kind = TerminalFrameKind::Input;
        input.Sequence = 3;
        input.Generation = 7;
        input.Payload = { 'h', 'e', 'l', 'l', 'o' };
        VERIFY_IS_TRUE(client.Send(input, 5000).Succeeded());
        VERIFY_IS_TRUE(_waitUntil([&] { return inputs.load() == 1; }));
        VERIFY_ARE_EQUAL(std::string{ TaskId }, observedInputTask);
        VERIFY_ARE_EQUAL(std::string{ "hello" }, observedInput);
        input.Sequence = 4;
        input.Payload = { 0x1b };
        VERIFY_IS_TRUE(client.Send(input, 5000).Succeeded());
        VERIFY_IS_TRUE(_waitUntil([&] { return interrupts.load() == 1; }));
        VERIFY_ARE_EQUAL(std::string{ TaskId }, observedInterruptTask);

        RuntimeTaskTerminalServiceResult closed;
        std::thread closer{ [&] { closed = service.Close(TaskId, 7, true); } };
        const auto end = client.Receive(5000);
        VERIFY_IS_TRUE(end.Succeeded());
        VERIFY_IS_TRUE((end.Frame.Flags & static_cast<uint32_t>(TerminalFrameFlag::EndOfStream)) != 0);
        client.Close();
        closer.join();
        VERIFY_IS_TRUE(closed.Succeeded());
        VERIFY_ARE_EQUAL(size_t{ 0 }, service.SessionCount());
    }

    void RuntimeTaskTerminalServiceTests::SurfaceOwnerIsExclusiveUntilTerminalClientDetaches()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        RuntimeTaskTerminalService service{
            identity,
            std::string{ RuntimeInstance },
            [](auto&&, auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; },
            [](auto&&) { return RuntimeTerminalBackendResult{ RuntimeTerminalBackendStatus::Succeeded, {} }; }
        };
        const RuntimeTaskTerminalOpenRequest first{
            std::string{ TaskId }, std::string{ SessionId }, 9, std::string{ ClientA }, "Owner test"
        };
        const auto opened = service.Open(first);
        VERIFY_IS_TRUE(opened.Succeeded());
        auto client = _connect(opened.Endpoint, RuntimeInstance, ClientA);
        VERIFY_IS_TRUE(client.Receive(5000).Succeeded());
        auto competing = first;
        competing.OwnerClientInstanceId = ClientB;
        VERIFY_IS_TRUE(service.Open(competing).Status == RuntimeTaskTerminalServiceStatus::OwnedElsewhere);
        client.Close();
        VERIFY_IS_TRUE(_waitUntil([&] { return service.ClientCount(TaskId) == 0; }));
        VERIFY_IS_TRUE(service.Open(competing).Status == RuntimeTaskTerminalServiceStatus::AlreadyOpen);
        VERIFY_IS_TRUE(service.Open({ std::string{ TaskId }, std::string{ SessionId }, 8, std::string{ ClientB }, "stale" }).Status == RuntimeTaskTerminalServiceStatus::StaleGeneration);
        VERIFY_IS_TRUE(service.Close(TaskId, 9, false).Succeeded());
    }
}
