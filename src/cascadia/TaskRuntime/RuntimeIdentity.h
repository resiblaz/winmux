// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Microsoft::Terminal::TaskRuntime
{
    enum class RuntimeIdentityStatus
    {
        Succeeded,
        InvalidChannel,
        InvalidSettingsRoot,
        InvalidUserSid,
        UserIdentityUnavailable,
        HashFailure,
    };

    struct RuntimeIdentity
    {
        std::wstring Channel;
        std::filesystem::path SettingsRoot;
        std::wstring UserSid;
        std::string ProtocolIdentity;
        std::wstring InstanceMutexName;
        std::wstring LaunchMutexName;
        std::wstring ControlPipeName;
    };

    struct RuntimeIdentityResult
    {
        RuntimeIdentityStatus Status{ RuntimeIdentityStatus::HashFailure };
        RuntimeIdentity Identity;
        std::string Error;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return Status == RuntimeIdentityStatus::Succeeded;
        }
    };

    [[nodiscard]] RuntimeIdentityResult BuildRuntimeIdentity(
        std::wstring_view channel,
        const std::filesystem::path& settingsRoot,
        std::wstring_view userSid);

    [[nodiscard]] RuntimeIdentityResult BuildCurrentUserRuntimeIdentity(
        std::wstring_view channel,
        const std::filesystem::path& settingsRoot);

    [[nodiscard]] std::optional<std::wstring> CurrentUserSid(std::string& error);
}
