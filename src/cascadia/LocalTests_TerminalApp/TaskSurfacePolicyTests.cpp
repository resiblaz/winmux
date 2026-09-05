// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/Tasks/TaskSurfacePolicy.h"
#include "../TerminalApp/Tasks/TaskTerminalInput.h"

using namespace WEX::TestExecution;

namespace TerminalAppLocalTests
{
    using namespace winrt::TerminalApp::implementation::tasks;

    class TaskSurfacePolicyTests
    {
        BEGIN_TEST_CLASS(TaskSurfacePolicyTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(UserSurfaceCloseMatrixRemainsCompatible);
        TEST_METHOD(TaskSurfaceRejectsEveryUserCloseOrigin);
        TEST_METHOD(BulkWindowAndConnectionOriginsHaveDedicatedActions);
        TEST_METHOD(RuntimeAuthorizationIsGenerationBoundAndSingleUse);
        TEST_METHOD(ReconnectReplacesFailedOrPreviousRuntimeSurfaces);
        TEST_METHOD(EscapeSequencesDoNotInterruptOrPolluteTaskInput);
        TEST_METHOD(PastedLinesAndSurrogateBackspacePreserveMessages);
    };

    void TaskSurfacePolicyTests::UserSurfaceCloseMatrixRemainsCompatible()
    {
        TaskSurfacePolicy policy;
        for (uint8_t value = 0; value <= static_cast<uint8_t>(TaskSurfaceCloseOrigin::ConnectionExit); ++value)
        {
            VERIFY_IS_TRUE(policy.Evaluate(99, static_cast<TaskSurfaceCloseOrigin>(value)).Action == TaskSurfaceCloseAction::Allow);
        }
    }

    void TaskSurfacePolicyTests::TaskSurfaceRejectsEveryUserCloseOrigin()
    {
        TaskSurfacePolicy policy;
        VERIFY_IS_TRUE(policy.Register(1, { L"task-a", 7 }));
        const std::array origins{
            TaskSurfaceCloseOrigin::TabButton,
            TaskSurfaceCloseOrigin::MiddleClick,
            TaskSurfaceCloseOrigin::Shortcut,
            TaskSurfaceCloseOrigin::CommandPalette,
            TaskSurfaceCloseOrigin::Pane,
            TaskSurfaceCloseOrigin::Workspace,
        };
        for (const auto origin : origins)
        {
            VERIFY_IS_TRUE(policy.Evaluate(1, origin).Action == TaskSurfaceCloseAction::Deny);
        }
    }

    void TaskSurfacePolicyTests::BulkWindowAndConnectionOriginsHaveDedicatedActions()
    {
        TaskSurfacePolicy policy;
        VERIFY_IS_TRUE(policy.Register(1, { L"task-a", 7 }));
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::CloseOtherTabs).Action == TaskSurfaceCloseAction::Skip);
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::CloseTabsAfter).Action == TaskSurfaceCloseAction::Skip);
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::WindowExit).Action == TaskSurfaceCloseAction::Detach);
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::ConnectionExit).Action == TaskSurfaceCloseAction::RetainPlaceholder);
    }

    void TaskSurfacePolicyTests::RuntimeAuthorizationIsGenerationBoundAndSingleUse()
    {
        TaskSurfacePolicy policy;
        VERIFY_IS_TRUE(policy.Register(1, { L"task-a", 7 }));
        VERIFY_IS_FALSE(policy.AuthorizeClose(L"task-a", 6, 10));
        VERIFY_IS_TRUE(policy.AuthorizeClose(L"task-a", 7, 10));
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::RuntimeEndOfStream, 9).Action == TaskSurfaceCloseAction::Deny);
        const auto allowed = policy.Evaluate(1, TaskSurfaceCloseOrigin::RuntimeEndOfStream, 10);
        VERIFY_IS_TRUE(allowed.Action == TaskSurfaceCloseAction::Allow);
        VERIFY_IS_TRUE(allowed.AuthorizationConsumed);
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::RuntimeEndOfStream, 10).Action == TaskSurfaceCloseAction::Deny);
        VERIFY_IS_FALSE(policy.Register(2, { L"task-a", 8 }));
    }

    void TaskSurfacePolicyTests::ReconnectReplacesFailedOrPreviousRuntimeSurfaces()
    {
        TaskSurfacePolicy policy;
        VERIFY_IS_TRUE(policy.Register(1, { L"task-a", 7, "runtime-a" }));
        VERIFY_IS_TRUE(policy.CanActivate(1, 7, "runtime-a", true));
        VERIFY_IS_FALSE(policy.CanActivate(1, 7, "runtime-a", false));
        VERIFY_IS_FALSE(policy.CanActivate(1, 8, "runtime-a", true));
        VERIFY_IS_FALSE(policy.CanActivate(1, 7, "runtime-b", true));
        VERIFY_IS_TRUE(policy.AuthorizeClose(L"task-a", 7, 10, "runtime-a"));
        VERIFY_IS_TRUE(policy.Evaluate(1, TaskSurfaceCloseOrigin::TaskDetail, 10).AuthorizationConsumed);
        policy.Unregister(1);
        VERIFY_IS_TRUE(policy.Register(2, { L"task-a", 7, "runtime-b" }));
        VERIFY_IS_FALSE(policy.AuthorizeClose(L"task-a", 7, 11, "runtime-a"));
        VERIFY_IS_TRUE(policy.CanActivate(2, 7, "runtime-b", true));
    }

    void TaskSurfacePolicyTests::EscapeSequencesDoNotInterruptOrPolluteTaskInput()
    {
        TaskTerminalInput input;
        static_cast<void>(input.Feed(L"continue"));
        for (const auto sequence : { L"\x1b[A", L"\x1b[B", L"\x1b[1;5D", L"\x1bOP", L"\x1bx" })
        {
            const auto batch = input.Feed(sequence);
            VERIFY_IS_FALSE(batch.Interrupt);
            VERIFY_IS_TRUE(batch.Echo.empty());
            VERIFY_IS_TRUE(batch.Messages.empty());
        }
        VERIFY_IS_FALSE(input.Feed(L"\x1b[").Interrupt);
        VERIFY_IS_TRUE(input.Feed(L"C").Echo.empty());
        VERIFY_IS_TRUE(input.Feed(L"\x1b").Interrupt);
        const auto submitted = input.Feed(L"\r");
        VERIFY_ARE_EQUAL(size_t{ 1 }, submitted.Messages.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"continue" }, submitted.Messages.front());
    }

    void TaskSurfacePolicyTests::PastedLinesAndSurrogateBackspacePreserveMessages()
    {
        TaskTerminalInput input;
        const auto pasted = input.Feed(L"\x1b[200~first\r\nsecond\n\x1b[201~");
        VERIFY_IS_FALSE(pasted.Interrupt);
        VERIFY_ARE_EQUAL(size_t{ 2 }, pasted.Messages.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"first" }, pasted.Messages[0]);
        VERIFY_ARE_EQUAL(std::wstring{ L"second" }, pasted.Messages[1]);
        static_cast<void>(input.Feed(L"keep\xd83d\xde00\b"));
        VERIFY_ARE_EQUAL(std::wstring{ L"keep" }, input.Feed(L"\r").Messages.front());
    }
}
