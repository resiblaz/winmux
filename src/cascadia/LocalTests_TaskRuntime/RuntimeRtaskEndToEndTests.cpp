// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RtaskClient.h"
#include "RuntimeControlTestFixtures.h"
#include "RuntimeProductRouter.h"
#include "RuntimeRtaskCommandTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::Rtask;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::ControlFixtures;
using namespace TaskRuntimeLocalTests::RuntimeRtaskCommandTestSupport;
using namespace TaskRuntimeLocalTests::TestFixtures;

namespace TaskRuntimeLocalTests
{
    class RuntimeRtaskEndToEndTests
    {
        TEST_CLASS(RuntimeRtaskEndToEndTests);

        TEST_METHOD(RealPipeClientCommitsAndReplaysThroughProductRouter);
    };

    void RuntimeRtaskEndToEndTests::RealPipeClientCommitsAndReplaysThroughProductRouter()
    {
        Fixture fixture;
        const auto issued = fixture.Service->IssueForActiveRun(
            fixture.Task.Task.Id.Value(),
            fixture.Run.Run.Id.Value(),
            AllRtaskMethods,
            1000,
            GetTickCount64());
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }

        UniqueRuntimeRoot root;
        const auto identity = RequireIdentity(root.Path());
        auto security = RequireSecurity(identity.UserSid);
        auto listener = CreateRuntimeControlPipe(identity, security.Attributes(), true);
        VERIFY_IS_TRUE(listener.Created());
        auto router = std::make_shared<RuntimeProductRouter>(
            std::make_shared<RuntimeFoundationRouter>(
                identity.ProtocolIdentity,
                "af000000-0000-4000-8000-000000000001",
                GetCurrentProcessId()),
            fixture.Service);
        RuntimeControlServerConfiguration configuration;
        configuration.Handshake = RuntimeConfiguration(identity);
        configuration.Handshake.Capabilities =
            static_cast<uint64_t>(ProtocolCapability::RuntimeLifecycle) |
            static_cast<uint64_t>(ProtocolCapability::TaskCommands);
        auto started = StartRuntimeControlServer(
            identity,
            std::move(security),
            std::move(listener.Pipe),
            std::move(configuration),
            router);
        VERIFY_IS_TRUE(started.Started());
        if (!started.Started())
        {
            return;
        }
        auto server = std::move(started.Server);

        RtaskContext context;
        context.Identity = identity;
        context.RunId = fixture.Run.Run.Id.Value();
        context.Capability = issued.Grant->Capability;
        auto request = fixture.Request(
            *issued.Grant,
            RtaskMethod::Result,
            1,
            "real pipe result");
        NamedPipeRtaskClient client;
        const auto applied = client.Execute(context, request);
        VERIFY_IS_TRUE(applied.Status == RtaskClientStatus::Succeeded);
        VERIFY_IS_TRUE(applied.Result.Disposition == RtaskCommandDisposition::Applied);
        const auto replayed = client.Execute(context, request);
        VERIFY_IS_TRUE(replayed.Status == RtaskClientStatus::Succeeded);
        VERIFY_IS_TRUE(replayed.Result.Disposition == RtaskCommandDisposition::Replayed);
        VERIFY_ARE_EQUAL(applied.Result.TaskVersion, replayed.Result.TaskVersion);
        VERIFY_ARE_EQUAL(applied.Result.EventSequence, replayed.Result.EventSequence);

        const auto snapshot = fixture.Store->LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(std::string{ "real pipe result" }, snapshot->Runs[0].Result);
        const auto stopped = server.StoppedHandle();
        server.Stop();
        VERIFY_ARE_EQUAL(DWORD{ WAIT_OBJECT_0 }, WaitForSingleObject(stopped, 0));
    }
}
