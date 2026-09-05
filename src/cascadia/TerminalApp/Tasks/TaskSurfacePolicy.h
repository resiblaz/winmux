// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace winrt::TerminalApp::implementation::tasks
{
    using TaskSurfaceId = uint64_t;
    using TaskSurfaceCloseAuthorization = uint64_t;

    enum class TaskSurfaceCloseOrigin : uint8_t
    {
        TabButton,
        MiddleClick,
        Shortcut,
        CommandPalette,
        CloseOtherTabs,
        CloseTabsAfter,
        Pane,
        Workspace,
        TaskDetail,
        RuntimeEndOfStream,
        WindowExit,
        ConnectionExit,
    };

    enum class TaskSurfaceCloseAction : uint8_t
    {
        Allow,
        Deny,
        Skip,
        Detach,
        RetainPlaceholder,
    };

    struct TaskSurfaceOwnership
    {
        std::wstring TaskId;
        uint64_t Generation{};
        std::string RuntimeInstanceId;
    };

    struct TaskSurfaceCloseDecision
    {
        TaskSurfaceCloseAction Action{ TaskSurfaceCloseAction::Deny };
        std::wstring TaskId;
        bool AuthorizationConsumed{};
    };

    class TaskSurfacePolicy final
    {
    public:
        [[nodiscard]] bool Register(TaskSurfaceId surface, TaskSurfaceOwnership ownership);
        void Unregister(TaskSurfaceId surface) noexcept;
        [[nodiscard]] bool IsTaskOwned(TaskSurfaceId surface) const noexcept;
        [[nodiscard]] std::optional<TaskSurfaceId> Find(std::wstring_view taskId) const noexcept;
        [[nodiscard]] const TaskSurfaceOwnership* Ownership(TaskSurfaceId surface) const noexcept;
        [[nodiscard]] bool CanActivate(
            TaskSurfaceId surface, uint64_t generation,
            std::string_view runtimeInstanceId, bool connectionUsable) const noexcept;
        [[nodiscard]] bool AuthorizeClose(
            std::wstring_view taskId,
            uint64_t generation,
            TaskSurfaceCloseAuthorization authorization,
            std::string_view runtimeInstanceId = {}) noexcept;
        [[nodiscard]] TaskSurfaceCloseDecision Evaluate(
            TaskSurfaceId surface,
            TaskSurfaceCloseOrigin origin,
            std::optional<TaskSurfaceCloseAuthorization> authorization = std::nullopt) noexcept;

    private:
        struct Entry
        {
            TaskSurfaceOwnership Ownership;
            std::optional<TaskSurfaceCloseAuthorization> Authorization;
        };

        std::unordered_map<TaskSurfaceId, Entry> _entries;
    };
}
