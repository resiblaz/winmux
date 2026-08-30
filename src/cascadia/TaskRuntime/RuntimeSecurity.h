// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    struct RuntimeSecurityResult;

    enum class RuntimeSecurityStatus
    {
        Succeeded,
        InvalidUserSid,
        DescriptorCreationFailed,
    };

    class RuntimeSecurityAttributes final
    {
    public:
        RuntimeSecurityAttributes() noexcept = default;
        ~RuntimeSecurityAttributes();

        RuntimeSecurityAttributes(const RuntimeSecurityAttributes&) = delete;
        RuntimeSecurityAttributes& operator=(const RuntimeSecurityAttributes&) = delete;
        RuntimeSecurityAttributes(RuntimeSecurityAttributes&& other) noexcept;
        RuntimeSecurityAttributes& operator=(RuntimeSecurityAttributes&& other) noexcept;

        [[nodiscard]] const SECURITY_ATTRIBUTES* Attributes() const noexcept;
        [[nodiscard]] PSECURITY_DESCRIPTOR Descriptor() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend struct RuntimeSecurityResult;
        friend RuntimeSecurityResult CreateRuntimeSecurityAttributes(std::wstring_view userSid);

        PSECURITY_DESCRIPTOR _descriptor{};
        SECURITY_ATTRIBUTES _attributes{};
    };

    struct RuntimeSecurityResult
    {
        RuntimeSecurityStatus Status{ RuntimeSecurityStatus::DescriptorCreationFailed };
        RuntimeSecurityAttributes Security;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeSecurityStatus::Succeeded;
        }
    };

    [[nodiscard]] RuntimeSecurityResult CreateRuntimeSecurityAttributes(std::wstring_view userSid);
}
