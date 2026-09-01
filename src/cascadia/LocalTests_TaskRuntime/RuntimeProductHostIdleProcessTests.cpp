// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "TaskRuntimeProcessTestFixtures.h"

#include "SqliteTaskStore.h"
#include "TaskCoreStoreTestFixtures.h"

#include "RuntimeFoundationRpc.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskCoreLocalTests::TestFixtures;
using namespace TaskRuntimeLocalTests::ProcessFixtures;

namespace TaskRuntimeLocalTests
{
    namespace
    {
        constexpr uint32_t IdleExitMilliseconds{ 400 };

        void _verifyReady(TaskRuntimeProcess& process)
        {
            VERIFY_IS_TRUE(process.WaitUntilReady());
            VERIFY_IS_TRUE(process.Running());
            VERIFY_ARE_NOT_EQUAL(DWORD{ 0 }, process.ProcessId());
        }

        void _verifyPing(
            RuntimeControlChannel& channel,
            const std::string_view requestId,
            const std::string_view runtimeInstanceId,
            const std::string_view clientInstanceId)
        {
            const auto response = RequireProductResponse(
                channel,
                requestId,
                ControlMethod::Ping);
            VERIFY_IS_TRUE(response.Succeeded);
            const auto decoded = DecodePingResult(response.ResultJson);
            VERIFY_IS_TRUE(decoded.Succeeded());
            VERIFY_ARE_EQUAL(std::string{ runtimeInstanceId }, decoded.Value.RuntimeInstanceId);
            VERIFY_ARE_EQUAL(std::string{ clientInstanceId }, decoded.Value.ClientInstanceId);
        }

        void _seedActiveRun(const ProductRuntimeTestIdentity& identity)
        {
            std::filesystem::create_directories(identity.SettingsRoot());
            const auto task = MakeCreateTaskCommand();
            const auto run = MakeCreateRunCommand(task, 1, 2, 1, 1100);
            SqliteTaskStore store{ identity.SettingsRoot() / L"workspace-tasks.db" };
            VERIFY_IS_TRUE(store.IsReady());
            VERIFY_IS_TRUE(store.CreateTask(task).Succeeded());
            VERIFY_IS_TRUE(store.CreateTaskRun(run).Succeeded());
            // M7 consumes queued runs immediately. Move this synthetic M2
            // fixture to an unmanaged active phase so the test isolates the
            // idle policy without asking the orchestrator to execute it.
            VERIFY_IS_TRUE(store.ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        run,
                                        3,
                                        2,
                                        1200,
                                        TaskRunState::Preparing))
                               .Succeeded());
        }
    }

    class RuntimeProductHostIdleProcessTests
    {
        TEST_CLASS(RuntimeProductHostIdleProcessTests);

        TEST_METHOD(QuiescentRuntimeExitsSuccessfullyAfterGrace);
        TEST_METHOD(ControlClientCancelsExitAndDisconnectRestartsFullGrace);
        TEST_METHOD(ActiveRunSurvivesAllUiClientsAndAllowsReconnect);
    };

    void RuntimeProductHostIdleProcessTests::QuiescentRuntimeExitsSuccessfullyAfterGrace()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{
            identity,
            { .IdleExitMilliseconds = IdleExitMilliseconds }
        };
        _verifyReady(process);

        DWORD exitCode{};
        VERIFY_IS_TRUE(process.WaitForExit(ProductProcessExitTimeoutMilliseconds, exitCode));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, exitCode);
        VERIFY_IS_FALSE(process.Running());
        VERIFY_IS_FALSE(process.ForcedCleanup());
    }

    void RuntimeProductHostIdleProcessTests::ControlClientCancelsExitAndDisconnectRestartsFullGrace()
    {
        ProductRuntimeTestIdentity identity;
        TaskRuntimeProcess process{
            identity,
            { .IdleExitMilliseconds = IdleExitMilliseconds }
        };
        _verifyReady(process);
        constexpr std::string_view clientId{
            "b1000000-0000-4000-8000-000000000001"
        };
        auto client = RequireProductControlClient(identity, clientId);

        Sleep(IdleExitMilliseconds + 250);
        VERIFY_IS_TRUE(process.Running());
        _verifyPing(
            client.Channel,
            "b2000000-0000-4000-8000-000000000001",
            client.Handshake.RuntimeInstanceId,
            clientId);

        client.Channel.Close();
        DWORD exitCode{};
        VERIFY_IS_FALSE(process.WaitForExit(IdleExitMilliseconds / 3, exitCode));
        VERIFY_IS_TRUE(process.WaitForExit(ProductProcessExitTimeoutMilliseconds, exitCode));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, exitCode);
        VERIFY_IS_FALSE(process.ForcedCleanup());
    }

    void RuntimeProductHostIdleProcessTests::ActiveRunSurvivesAllUiClientsAndAllowsReconnect()
    {
        ProductRuntimeTestIdentity identity;
        _seedActiveRun(identity);
        TaskRuntimeProcess process{
            identity,
            { .IdleExitMilliseconds = IdleExitMilliseconds }
        };
        _verifyReady(process);

        constexpr std::string_view firstClientId{
            "b1000000-0000-4000-8000-000000000002"
        };
        auto first = RequireProductControlClient(identity, firstClientId);
        const auto runtimeInstanceId = first.Handshake.RuntimeInstanceId;
        first.Channel.Close();

        Sleep(IdleExitMilliseconds + 250);
        VERIFY_IS_TRUE(process.Running());
        constexpr std::string_view secondClientId{
            "b1000000-0000-4000-8000-000000000003"
        };
        auto second = RequireProductControlClient(identity, secondClientId);
        VERIFY_ARE_EQUAL(runtimeInstanceId, second.Handshake.RuntimeInstanceId);
        _verifyPing(
            second.Channel,
            "b2000000-0000-4000-8000-000000000002",
            runtimeInstanceId,
            secondClientId);
        second.Channel.Close();

        Sleep(IdleExitMilliseconds + 250);
        VERIFY_IS_TRUE(process.Running());
        VERIFY_IS_TRUE(process.SignalShutdown());
        DWORD exitCode{};
        VERIFY_IS_TRUE(process.WaitForExit(ProductProcessExitTimeoutMilliseconds, exitCode));
        VERIFY_ARE_EQUAL(DWORD{ 0 }, exitCode);
        VERIFY_IS_FALSE(process.ForcedCleanup());
    }
}
