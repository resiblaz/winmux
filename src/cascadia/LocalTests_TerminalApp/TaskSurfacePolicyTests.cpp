// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/Tasks/TaskSurfacePolicy.h"

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
}
