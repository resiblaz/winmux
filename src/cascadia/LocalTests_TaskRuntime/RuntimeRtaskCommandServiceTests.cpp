// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "RuntimeRtaskCommandTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskRuntime;
using namespace Microsoft::Terminal::TaskRuntimeProtocol;
using namespace TaskRuntimeLocalTests::RuntimeRtaskCommandTestSupport;

namespace TaskRuntimeLocalTests
{
    class RuntimeRtaskCommandServiceTests
    {
        TEST_CLASS(RuntimeRtaskCommandServiceTests);

        TEST_METHOD(IssueRequiresCapabilityBoundActiveRun);
        TEST_METHOD(ExecuteCommitsAndReplaysOneAgentMutation);
        TEST_METHOD(AuthorizationRejectsWrongMethodTokenAndExpiry);
        TEST_METHOD(CrossTaskBindingAndTerminalRunFailClosed);
        TEST_METHOD(ResultPersistsAndRunInvalidationIsImmediate);
    };

    void RuntimeRtaskCommandServiceTests::IssueRequiresCapabilityBoundActiveRun()
    {
        Fixture fixture;
        const auto issued = fixture.Issue();
        VERIFY_IS_TRUE(issued.Status == RuntimeRtaskCapabilityStatus::Issued);
        VERIFY_IS_TRUE(issued.Grant.has_value());
        VERIFY_ARE_EQUAL(fixture.Task.Task.Id.Value(), issued.Grant->TaskId);
        VERIFY_ARE_EQUAL(fixture.Run.Run.Id.Value(), issued.Grant->RunId);

        VERIFY_IS_TRUE(
            fixture.Service->IssueForActiveRun(
                "not-a-task",
                fixture.Run.Run.Id.Value(),
                AllRtaskMethods,
                100,
                10).Status == RuntimeRtaskCapabilityStatus::InvalidTask);
        VERIFY_IS_TRUE(
            fixture.Service->IssueForActiveRun(
                fixture.Task.Task.Id.Value(),
                MakeId<TaskRunId>(9999).Value(),
                AllRtaskMethods,
                100,
                10).Status == RuntimeRtaskCapabilityStatus::InvalidRun);
    }

    void RuntimeRtaskCommandServiceTests::ExecuteCommitsAndReplaysOneAgentMutation()
    {
        Fixture fixture;
        const auto issued = fixture.Issue();
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        const auto request = fixture.Request(
            *issued.Grant,
            RtaskMethod::StatusInProgress,
            1);
        const auto applied = fixture.Service->Execute(request, fixture.Context(1));
        VERIFY_IS_TRUE(applied.Status == RuntimeRtaskCommandStatus::Succeeded);
        VERIFY_IS_TRUE(applied.Result.Disposition == RtaskCommandDisposition::Applied);
        VERIFY_ARE_EQUAL(uint64_t{ 6 }, applied.Result.TaskVersion);

        const auto replayed = fixture.Service->Execute(
            request,
            fixture.Context(2, 21, 1600));
        VERIFY_IS_TRUE(replayed.Status == RuntimeRtaskCommandStatus::Replayed);
        VERIFY_IS_TRUE(replayed.Result.Disposition == RtaskCommandDisposition::Replayed);
        VERIFY_ARE_EQUAL(applied.Result.TaskVersion, replayed.Result.TaskVersion);
        VERIFY_ARE_EQUAL(applied.Result.EventSequence, replayed.Result.EventSequence);
        const auto snapshot = fixture.Store->LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(uint64_t{ 6 }, snapshot->Task.Version);
        VERIFY_IS_TRUE(snapshot->Task.Status == TaskStatus::InProgress);
    }

    void RuntimeRtaskCommandServiceTests::AuthorizationRejectsWrongMethodTokenAndExpiry()
    {
        Fixture fixture;
        const auto issued = fixture.Issue(RtaskMethodBit(RtaskMethod::Result));
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        VERIFY_IS_TRUE(
            fixture.Service->Execute(
                fixture.Request(
                    *issued.Grant,
                    RtaskMethod::StatusInProgress,
                    1),
                fixture.Context(1)).Status == RuntimeRtaskCommandStatus::Unauthorized);

        auto unknown = fixture.Request(*issued.Grant, RtaskMethod::Result, 2, "result");
        unknown.Capability = std::string(RtaskCapabilityBytes, 'f');
        VERIFY_IS_TRUE(
            fixture.Service->Execute(
                unknown,
                fixture.Context(2)).Status == RuntimeRtaskCommandStatus::Unauthorized);
        VERIFY_IS_TRUE(
            fixture.Service->Execute(
                fixture.Request(*issued.Grant, RtaskMethod::Result, 3, "result"),
                fixture.Context(3, 110, 1600)).Status ==
            RuntimeRtaskCommandStatus::Unauthorized);
    }

    void RuntimeRtaskCommandServiceTests::CrossTaskBindingAndTerminalRunFailClosed()
    {
        Fixture fixture;
        const auto otherTask = MakeCreateTaskCommand(2, 2, 80, 1000);
        VERIFY_IS_TRUE(fixture.Store->CreateTask(otherTask).Succeeded());
        const auto directGrant = fixture.Capabilities->IssueOrRenew(
            otherTask.Task.Id.Value(),
            fixture.Run.Run.Id.Value(),
            AllRtaskMethods,
            100,
            10);
        VERIFY_IS_TRUE(directGrant.Grant.has_value());
        if (!directGrant.Grant)
        {
            return;
        }
        const auto crossTask = fixture.Service->Execute(
            fixture.Request(
                *directGrant.Grant,
                RtaskMethod::Result,
                1,
                "cross-task result"),
            fixture.Context(1));
        VERIFY_IS_TRUE(crossTask.Status == RuntimeRtaskCommandStatus::NotFound);

        (void)fixture.Capabilities->InvalidateRun(fixture.Run.Run.Id.Value());
        const auto valid = fixture.Issue(AllRtaskMethods, 30);
        VERIFY_IS_TRUE(valid.Grant.has_value());
        auto snapshot = fixture.Store->LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_IS_TRUE(fixture.Store->ChangeTaskRunState(MakeRunStateCommand(
                                                             fixture.Run,
                                                             90,
                                                             snapshot->Task.Version,
                                                             1700,
                                                             TaskRunState::Completed))
                           .Succeeded());
        const auto terminal = fixture.Service->Execute(
            fixture.Request(*valid.Grant, RtaskMethod::Result, 2, "too late"),
            fixture.Context(2, 31, 1800));
        VERIFY_IS_TRUE(terminal.Status == RuntimeRtaskCommandStatus::Conflict);
    }

    void RuntimeRtaskCommandServiceTests::ResultPersistsAndRunInvalidationIsImmediate()
    {
        Fixture fixture;
        const auto issued = fixture.Issue();
        VERIFY_IS_TRUE(issued.Grant.has_value());
        if (!issued.Grant)
        {
            return;
        }
        const auto result = fixture.Service->Execute(
            fixture.Request(
                *issued.Grant,
                RtaskMethod::Result,
                1,
                "service result"),
            fixture.Context(1));
        VERIFY_IS_TRUE(result.Succeeded());
        auto snapshot = fixture.Store->LoadTask(fixture.Task.Task.Id);
        VERIFY_IS_TRUE(snapshot.has_value());
        VERIFY_ARE_EQUAL(std::string{ "service result" }, snapshot->Runs[0].Result);

        VERIFY_IS_TRUE(
            fixture.Service->InvalidateRun(fixture.Run.Run.Id.Value()).Status ==
            RuntimeRtaskCapabilityStatus::Invalidated);
        const auto rejected = fixture.Service->Execute(
            fixture.Request(
                *issued.Grant,
                RtaskMethod::Result,
                2,
                "after invalidation"),
            fixture.Context(2, 30, 1600));
        VERIFY_IS_TRUE(rejected.Status == RuntimeRtaskCommandStatus::Unauthorized);
    }
}
