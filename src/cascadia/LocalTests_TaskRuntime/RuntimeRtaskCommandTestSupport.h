// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "FakeRuntimeRtaskCapabilityTokenSource.h"
#include "RuntimeRtaskCommandService.h"

#include "TaskCoreStoreTestFixtures.h"

namespace TaskRuntimeLocalTests::RuntimeRtaskCommandTestSupport
{
    using namespace Microsoft::Terminal::TaskCore;
    using namespace Microsoft::Terminal::TaskRuntime;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;
    using namespace TaskCoreLocalTests::TestFixtures;

    class Fixture final
    {
    public:
        Fixture()
        {
            Task = MakeCreateTaskCommand();
            Run = MakeCreateRunCommand(Task, 1, 2, 1, 1100);
            Store = std::make_shared<SqliteTaskStore>(Path.Path());
            VERIFY_IS_TRUE(Store->CreateTask(Task).Succeeded());
            VERIFY_IS_TRUE(Store->CreateTaskRun(Run).Succeeded());
            VERIFY_IS_TRUE(Store->ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        Run,
                                        3,
                                        2,
                                        1200,
                                        TaskRunState::Preparing))
                               .Succeeded());
            VERIFY_IS_TRUE(Store->ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        Run,
                                        4,
                                        3,
                                        1300,
                                        TaskRunState::Starting))
                               .Succeeded());
            VERIFY_IS_TRUE(Store->ChangeTaskRunState(
                                    MakeRunStateCommand(
                                        Run,
                                        5,
                                        4,
                                        1400,
                                        TaskRunState::Running))
                               .Succeeded());

            Tokens = std::make_shared<FakeRuntimeRtaskCapabilityTokenSource>();
            Tokens->Tokens.emplace_back(std::string(RtaskCapabilityBytes, 'a'));
            Tokens->Tokens.emplace_back(std::string(RtaskCapabilityBytes, 'b'));
            Capabilities = std::make_shared<RuntimeRtaskCapabilityRegistry>(
                RuntimeRtaskCapabilityRegistryConfiguration{ 8, 1000 },
                Tokens);
            Service = std::make_shared<RuntimeRtaskCommandService>(
                Store,
                Capabilities);
            VERIFY_IS_TRUE(Service->Valid());
        }

        [[nodiscard]] RuntimeRtaskCapabilityResult Issue(
            const RtaskMethodSet methods = AllRtaskMethods,
            const RuntimeRtaskMilliseconds now = 10)
        {
            return Service->IssueForActiveRun(
                Task.Task.Id.Value(),
                Run.Run.Id.Value(),
                methods,
                100,
                now);
        }

        [[nodiscard]] RtaskCommandRequest Request(
            const RuntimeRtaskCapabilityGrant& grant,
            const RtaskMethod method,
            const uint64_t commandSeed,
            std::string text = {}) const
        {
            return {
                MakeId<CommandId>(6000 + commandSeed).Value(),
                Run.Run.Id.Value(),
                grant.Capability,
                method,
                std::move(text),
            };
        }

        [[nodiscard]] RuntimeRtaskCommandContext Context(
            const uint64_t eventSeed,
            const RuntimeRtaskMilliseconds monotonic = 20,
            const int64_t unixMilliseconds = 1500) const
        {
            return {
                monotonic,
                unixMilliseconds,
                MakeId<TaskEventId>(7000 + eventSeed).Value(),
            };
        }

        TemporaryDatabasePath Path;
        std::shared_ptr<SqliteTaskStore> Store;
        CreateTaskCommand Task;
        CreateTaskRunCommand Run;
        std::shared_ptr<FakeRuntimeRtaskCapabilityTokenSource> Tokens;
        std::shared_ptr<RuntimeRtaskCapabilityRegistry> Capabilities;
        std::shared_ptr<RuntimeRtaskCommandService> Service;
    };
}
