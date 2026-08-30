// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeControlChannel.h"
#include "RuntimeControlConnector.h"
#include "RuntimeControlTestFixtures.h"
#include "TaskRuntimeTestFixtures.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace TaskRuntimeLocalTests::ControlFixtures;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        class TestEvent final
        {
        public:
            explicit TestEvent(const bool signaled = false) :
                _handle{ CreateEventW(nullptr, TRUE, signaled, nullptr) }
            {
                VERIFY_IS_NOT_NULL(_handle);
            }

            ~TestEvent()
            {
                if (_handle)
                {
                    CloseHandle(_handle);
                }
            }

            [[nodiscard]] HANDLE Get() const noexcept
            {
                return _handle;
            }

        private:
            HANDLE _handle{};
        };
    }

    class RuntimeControlDiscoveryTests
    {
        TEST_CLASS(RuntimeControlDiscoveryTests);

        TEST_METHOD(ImmediatePolicyReturnsPreciseEndpointAbsent);
        TEST_METHOD(PrecancelledImmediateDiscoveryStillReturnsCancelled);
        TEST_METHOD(BusyEndpointNeverLooksAbsent);
        TEST_METHOD(InvalidDiscoveryPolicyFailsClosed);
    };

    void RuntimeControlDiscoveryTests::ImmediatePolicyReturnsPreciseEndpointAbsent()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        RuntimeNamedPipeControlConnector connector{
            RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent
        };
        const auto started = GetTickCount64();
        const auto result = connector.Connect(
            identity,
            Hello(identity, ClientOneId),
            5000);
        const auto elapsed = GetTickCount64() - started;

        VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Disconnected);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_FILE_NOT_FOUND }, result.SystemError);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Connecting);
        VERIFY_IS_TRUE(result.FailureReason == RuntimeControlConnectFailureReason::EndpointAbsent);
        VERIFY_IS_NULL(result.Connection.get());
        VERIFY_IS_TRUE(elapsed < 1000);

        const auto channel = ConnectRuntimeControlChannel(
            identity,
            5000,
            nullptr,
            RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent);
        VERIFY_IS_TRUE(channel.EndpointObservation == RuntimeControlEndpointObservation::Absent);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_FILE_NOT_FOUND }, channel.SystemError);
    }

    void RuntimeControlDiscoveryTests::PrecancelledImmediateDiscoveryStillReturnsCancelled()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        TestEvent cancelled{ true };
        RuntimeNamedPipeControlConnector connector{
            RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent
        };
        const auto result = connector.Connect(
            identity,
            Hello(identity, ClientOneId),
            5000,
            cancelled.Get());

        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Cancelled);
        VERIFY_IS_TRUE(result.FailureReason == RuntimeControlConnectFailureReason::Cancelled);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Connecting);
        VERIFY_IS_NULL(result.Connection.get());
    }

    void RuntimeControlDiscoveryTests::BusyEndpointNeverLooksAbsent()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto security = RequireSecurity(identity.UserSid);
        auto listener = CreateRuntimeControlPipe(identity, security.Attributes(), true);
        VERIFY_IS_TRUE(listener.Created());
        auto occupied = ConnectRuntimeControlChannel(identity, 1000);
        VERIFY_IS_TRUE(occupied.Succeeded());

        RuntimeNamedPipeControlConnector connector{
            RuntimeControlEndpointDiscoveryPolicy::ReturnImmediatelyIfAbsent
        };
        const auto result = connector.Connect(
            identity,
            Hello(identity, ClientTwoId),
            150);
        VERIFY_IS_FALSE(result.Succeeded());
        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::Timeout);
        VERIFY_IS_TRUE(result.FailureReason == RuntimeControlConnectFailureReason::EndpointBusy);
        VERIFY_IS_TRUE(result.FailureReason != RuntimeControlConnectFailureReason::EndpointAbsent);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Connecting);
        occupied.Channel.Close();
    }

    void RuntimeControlDiscoveryTests::InvalidDiscoveryPolicyFailsClosed()
    {
        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        RuntimeNamedPipeControlConnector connector{
            static_cast<RuntimeControlEndpointDiscoveryPolicy>(99)
        };
        const auto result = connector.Connect(
            identity,
            Hello(identity, ClientOneId),
            1000);
        VERIFY_IS_TRUE(result.Status == RuntimeTransportStatus::InvalidConfiguration);
        VERIFY_IS_TRUE(result.FailureReason == RuntimeControlConnectFailureReason::InvalidConfiguration);
        VERIFY_IS_TRUE(result.Phase == RuntimeControlConnectPhase::Connecting);
    }
}
