// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../TerminalApp/Tasks/TaskAgentSessionRegistry.h"

using namespace WEX::TestExecution;

namespace TerminalAppLocalTests
{
    using namespace winrt::TerminalApp::implementation::tasks;
    using namespace Microsoft::Terminal::TaskRuntimeProtocol;

    class TaskAgentSessionRegistryTests
    {
        BEGIN_TEST_CLASS(TaskAgentSessionRegistryTests)
            TEST_CLASS_PROPERTY(L"RunAs", L"UAP")
            TEST_CLASS_PROPERTY(L"UAP:AppXManifest", L"TestHostAppXManifest.xml")
        END_TEST_CLASS()

        TEST_METHOD(FullAndIncrementalSnapshotsExcludeOnlyCurrentTaskSessions);
        TEST_METHOD(OwnershipIndexProtectsUnloadedAndDeletedTaskSessions);
    };

    void TaskAgentSessionRegistryTests::OwnershipIndexProtectsUnloadedAndDeletedTaskSessions()
    {
        TaskAgentSessionRegistry registry; registry.RequireOwnershipIndex();
        VERIFY_IS_TRUE(registry.Contains(L"unknown-while-loading"));
        registry.MergeOwnedSessions({ "task-not-on-loaded-page" }, true);
        VERIFY_IS_TRUE(registry.Contains(L"task-not-on-loaded-page"));
        VERIFY_IS_FALSE(registry.Contains(L"manual-session"));
        registry.ReplaceAll({}); registry.RemoveTask("deleted-task");
        VERIFY_IS_TRUE(registry.Contains(L"task-not-on-loaded-page"));
        registry.RequireOwnershipIndex(); registry.MergeOwnedSessions({}, true);
        VERIFY_IS_TRUE(registry.Contains(L"task-not-on-loaded-page"));
    }

    void TaskAgentSessionRegistryTests::FullAndIncrementalSnapshotsExcludeOnlyCurrentTaskSessions()
    {
        TaskAgentSessionRegistry registry;
        TaskProjectionRecord first;
        first.Id = "10000000-0000-4000-8000-000000000001";
        first.AgentSessions.push_back({ "20000000-0000-4000-8000-000000000002", "codex", "task-thread-a", 0, true });
        TaskProjectionRecord second;
        second.Id = "30000000-0000-4000-8000-000000000003";
        second.AgentSessions.push_back({ "40000000-0000-4000-8000-000000000004", "codex", "task-thread-b", 0, true });
        registry.ReplaceAll({ first, second });
        VERIFY_ARE_EQUAL(size_t{ 2 }, registry.Size());
        VERIFY_IS_TRUE(registry.Contains(L"task-thread-a"));
        VERIFY_IS_TRUE(registry.Contains(L"task-thread-b"));
        VERIFY_IS_FALSE(registry.Contains(L"manual-thread"));

        first.AgentSessions.clear();
        first.AgentSessions.push_back({ "50000000-0000-4000-8000-000000000005", "codex", "task-thread-c", 0, true });
        registry.ReplaceTask(first);
        VERIFY_IS_FALSE(registry.Contains(L"task-thread-a"));
        VERIFY_IS_TRUE(registry.Contains(L"task-thread-b"));
        VERIFY_IS_TRUE(registry.Contains(L"task-thread-c"));
        registry.RemoveTask(second.Id);
        VERIFY_ARE_EQUAL(size_t{ 1 }, registry.Size());
        VERIFY_IS_FALSE(registry.Contains(L"task-thread-b"));
    }
}
