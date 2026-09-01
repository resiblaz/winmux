// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "RuntimeCodexDiscovery.h"

namespace Microsoft::Terminal::TaskRuntime
{
    namespace
    {
        std::optional<std::wstring> _environment(const wchar_t* name)
        {
            const auto required = GetEnvironmentVariableW(name, nullptr, 0);
            if (required == 0 || required > 32'768) return std::nullopt;
            std::wstring value(required, L'\0');
            const auto written = GetEnvironmentVariableW(name, value.data(), required);
            if (written == 0 || written >= required) return std::nullopt;
            value.resize(written);
            return value;
        }

        std::optional<std::filesystem::path> _searchPath()
        {
            std::wstring buffer(32'768, L'\0');
            const auto written = SearchPathW(
                nullptr, L"codex.exe", nullptr,
                static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (written == 0 || written >= buffer.size()) return std::nullopt;
            buffer.resize(written);
            return std::filesystem::path{ std::move(buffer) };
        }

        std::optional<std::filesystem::path> _npmCandidate()
        {
            const auto appData = _environment(L"APPDATA");
            if (!appData) return std::nullopt;
#if defined(_M_ARM64)
            constexpr auto package = L"codex-win32-arm64";
            constexpr auto triple = L"aarch64-pc-windows-msvc";
#else
            constexpr auto package = L"codex-win32-x64";
            constexpr auto triple = L"x86_64-pc-windows-msvc";
#endif
            return std::filesystem::path{ *appData } / L"npm" / L"node_modules" /
                   L"@openai" / L"codex" / L"node_modules" / L"@openai" /
                   package / L"vendor" / triple / L"bin" / L"codex.exe";
        }
    }

    bool IsSafeCodexExecutable(const std::filesystem::path& candidate) noexcept
    {
        try
        {
            if (candidate.empty() || !candidate.is_absolute() ||
                _wcsicmp(candidate.extension().c_str(), L".exe") != 0)
            {
                return false;
            }
            const auto attributes = GetFileAttributesW(candidate.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE)) == 0;
        }
        catch (...)
        {
            return false;
        }
    }

    std::optional<std::filesystem::path> DiscoverCodexExecutable() noexcept
    {
        try
        {
            if (const auto override = _environment(L"RICHER_CODEX_EXECUTABLE"))
            {
                std::filesystem::path candidate{ *override };
                if (IsSafeCodexExecutable(candidate)) return candidate;
            }
            if (const auto searched = _searchPath(); searched && IsSafeCodexExecutable(*searched)) return searched;
            if (const auto npm = _npmCandidate(); npm && IsSafeCodexExecutable(*npm)) return npm;
            return std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
}
