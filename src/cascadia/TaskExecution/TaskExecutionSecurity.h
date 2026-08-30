// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskExecution::details
{
    struct TaskExecutionSecurityResult;

    class TaskExecutionSecurityAttributes final
    {
    public:
        TaskExecutionSecurityAttributes() noexcept = default;
        ~TaskExecutionSecurityAttributes();

        TaskExecutionSecurityAttributes(const TaskExecutionSecurityAttributes&) = delete;
        TaskExecutionSecurityAttributes& operator=(const TaskExecutionSecurityAttributes&) = delete;
        TaskExecutionSecurityAttributes(TaskExecutionSecurityAttributes&& other) noexcept;
        TaskExecutionSecurityAttributes& operator=(TaskExecutionSecurityAttributes&& other) noexcept;

        [[nodiscard]] const SECURITY_ATTRIBUTES* Attributes() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend struct TaskExecutionSecurityResult;
        friend TaskExecutionSecurityResult CreateTaskExecutionSecurityAttributes(
            std::wstring_view) noexcept;

        PSECURITY_DESCRIPTOR _descriptor{};
        SECURITY_ATTRIBUTES _attributes{};
    };

    struct TaskExecutionSecurityResult
    {
        bool Succeeded{};
        TaskExecutionSecurityAttributes Security;
        DWORD SystemError{};
        std::string Error;
    };

    struct TaskExecutionMutexSecurityValidationResult
    {
        bool Succeeded{};
        DWORD SystemError{};
        std::string Error;
    };

    [[nodiscard]] std::optional<std::wstring> CurrentTaskExecutionUserSid(
        std::string& error) noexcept;
    [[nodiscard]] TaskExecutionSecurityResult CreateTaskExecutionSecurityAttributes(
        std::wstring_view userSid) noexcept;
    [[nodiscard]] TaskExecutionMutexSecurityValidationResult ValidateTaskExecutionMutexSecurity(
        HANDLE mutex,
        std::wstring_view currentUserSid) noexcept;
}
