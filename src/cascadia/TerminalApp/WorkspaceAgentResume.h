// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <winrt/Microsoft.Terminal.Settings.Model.h>

namespace winrt::TerminalApp::implementation::WorkspaceAgentResume
{
    inline constexpr uint32_t CurrentVersion{ 1 };
    inline constexpr std::wstring_view CodexProvider{ L"codex" };

    inline bool IsValidCodexSessionId(const std::wstring_view sessionId) noexcept
    {
        if (sessionId.size() != 36)
        {
            return false;
        }

        for (size_t index = 0; index < sessionId.size(); ++index)
        {
            const auto ch = sessionId[index];
            const auto hyphen = index == 8 || index == 13 || index == 18 || index == 23;
            if (hyphen ? ch != L'-' : !((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F')))
            {
                return false;
            }
        }
        return true;
    }

    inline winrt::hstring NormalizeCodexSessionId(const std::wstring_view sessionId)
    {
        if (!IsValidCodexSessionId(sessionId))
        {
            return {};
        }

        std::wstring normalized{ sessionId };
        for (auto& ch : normalized)
        {
            if (ch >= L'A' && ch <= L'F')
            {
                ch += L'a' - L'A';
            }
        }
        return winrt::hstring{ normalized };
    }

    inline bool IsSupported(const uint32_t version,
                            const winrt::guid& workspaceId,
                            const std::wstring_view provider,
                            const std::wstring_view sessionId,
                            const bool requiresConfirmation) noexcept
    {
        return version == CurrentVersion &&
               provider == CodexProvider &&
               requiresConfirmation &&
               workspaceId != winrt::guid{} &&
               IsValidCodexSessionId(sessionId);
    }

    inline bool IsSupported(const Microsoft::Terminal::Settings::Model::AgentResumeInfo& info) noexcept
    {
        return IsSupported(info.Version, info.WorkspaceId, info.Provider, info.SessionId, info.RequiresConfirmation);
    }

    inline bool IsSafeResolvedCliPath(const std::wstring_view path) noexcept
    {
        return !path.empty() &&
               path.find_first_of(L"\"%\r\n") == std::wstring_view::npos;
    }

    inline winrt::hstring BuildCodexCommandline(const std::wstring_view sessionId,
                                                const std::wstring_view commandProcessorPath,
                                                const std::wstring_view resolvedCliPath)
    {
        if (!IsValidCodexSessionId(sessionId) ||
            !IsSafeResolvedCliPath(commandProcessorPath) ||
            !IsSafeResolvedCliPath(resolvedCliPath))
        {
            return {};
        }

        // The only interpolated value is a strictly validated UUID. Provider,
        // verb and flags are fixed and never loaded from state. The executable
        // is resolved from PATH before launch and quoted as one cmd token.
        return winrt::hstring{ fmt::format(FMT_COMPILE(LR"("{}" /d /v:off /s /c ""{}" resume {}")"), commandProcessorPath, resolvedCliPath, sessionId) };
    }

    inline winrt::hstring BuildCommandline(const Microsoft::Terminal::Settings::Model::AgentResumeInfo& info,
                                           const std::wstring_view commandProcessorPath,
                                           const std::wstring_view resolvedCliPath)
    {
        if (!IsSupported(info))
        {
            return {};
        }

        return BuildCodexCommandline(info.SessionId, commandProcessorPath, resolvedCliPath);
    }

    inline winrt::hstring ResolveSystemCommandProcessorPath() noexcept
    {
        std::array<wchar_t, MAX_PATH> systemDirectory{};
        const auto length = GetSystemDirectoryW(systemDirectory.data(), gsl::narrow_cast<UINT>(systemDirectory.size()));
        if (length == 0 || length >= systemDirectory.size())
        {
            return {};
        }

        const auto path = fmt::format(FMT_COMPILE(L"{}\\cmd.exe"), std::wstring_view{ systemDirectory.data(), length });
        return IsSafeResolvedCliPath(path) ? winrt::hstring{ path } : winrt::hstring{};
    }

    inline winrt::hstring ResolveCodexCliPath() noexcept
    {
        try
        {
            const auto pathLength = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (pathLength == 0 || pathLength > 32767)
            {
                return {};
            }

            std::wstring searchPath(pathLength, L'\0');
            const auto written = GetEnvironmentVariableW(L"PATH", searchPath.data(), pathLength);
            if (written == 0 || written >= pathLength)
            {
                return {};
            }
            searchPath.resize(written);

            size_t offset = 0;
            while (offset <= searchPath.size())
            {
                const auto separator = searchPath.find(L';', offset);
                auto entry = searchPath.substr(offset, separator == std::wstring::npos ? std::wstring::npos : separator - offset);
                if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
                {
                    entry = entry.substr(1, entry.size() - 2);
                }

                const std::filesystem::path directory{ entry };
                if (!entry.empty() && !entry.starts_with(L"\\\\") && directory.is_absolute())
                {
                    const auto candidate = (directory / L"codex.cmd").lexically_normal().native();
                    const auto attributes = GetFileAttributesW(candidate.c_str());
                    if (attributes != INVALID_FILE_ATTRIBUTES &&
                        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                        IsSafeResolvedCliPath(candidate))
                    {
                        return winrt::hstring{ candidate };
                    }
                }

                if (separator == std::wstring::npos)
                {
                    break;
                }
                offset = separator + 1;
            }
            return {};
        }
        catch (...)
        {
            return {};
        }
    }
}
