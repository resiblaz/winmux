// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "RuntimeIdentity.h"
#include "RuntimeSecurity.h"

#include <array>
#include <filesystem>

namespace TaskRuntimeLocalTests::TestFixtures
{
    using namespace Microsoft::Terminal::TaskRuntime;

    class UniqueRuntimeRoot final
    {
    public:
        UniqueRuntimeRoot()
        {
            std::array<wchar_t, MAX_PATH> temporaryDirectory{};
            VERIFY_IS_GREATER_THAN(
                GetTempPathW(static_cast<DWORD>(temporaryDirectory.size()), temporaryDirectory.data()),
                DWORD{ 0 });
            std::array<wchar_t, MAX_PATH> temporaryFile{};
            VERIFY_IS_TRUE(GetTempFileNameW(temporaryDirectory.data(), L"rtt", 0, temporaryFile.data()) != 0);
            _path = temporaryFile.data();
            std::filesystem::remove(_path);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept
        {
            return _path;
        }

    private:
        std::filesystem::path _path;
    };

    inline std::wstring RequireCurrentUserSid()
    {
        std::string error;
        const auto sid = CurrentUserSid(error);
        VERIFY_IS_TRUE(sid.has_value());
        return sid.value_or(L"");
    }

    inline RuntimeIdentity RequireIdentity(
        const std::filesystem::path& root,
        const std::wstring_view channel = L"dev")
    {
        auto result = BuildRuntimeIdentity(channel, root, RequireCurrentUserSid());
        VERIFY_IS_TRUE(result.Succeeded());
        return std::move(result.Identity);
    }

    inline RuntimeSecurityAttributes RequireSecurity(const std::wstring_view sid)
    {
        auto result = CreateRuntimeSecurityAttributes(sid);
        VERIFY_IS_TRUE(result.Succeeded());
        return std::move(result.Security);
    }
}
