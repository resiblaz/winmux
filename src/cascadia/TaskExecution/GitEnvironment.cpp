// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitEnvironment.h"

namespace Microsoft::Terminal::TaskExecution::details
{
    namespace
    {
        bool _equalsIgnoreCase(
            const std::wstring_view left,
            const std::wstring_view right) noexcept
        {
            return left.size() == right.size() &&
                   CompareStringOrdinal(
                       left.data(),
                       static_cast<int>(left.size()),
                       right.data(),
                       static_cast<int>(right.size()),
                       TRUE) == CSTR_EQUAL;
        }

        bool _startsWithIgnoreCase(
            const std::wstring_view value,
            const std::wstring_view prefix) noexcept
        {
            return value.size() >= prefix.size() &&
                   CompareStringOrdinal(
                       value.data(),
                       static_cast<int>(prefix.size()),
                       prefix.data(),
                       static_cast<int>(prefix.size()),
                       TRUE) == CSTR_EQUAL;
        }

        bool _validValue(const std::wstring_view value) noexcept
        {
            return value.size() <= 4096 &&
                   std::ranges::none_of(value, [](const wchar_t character) {
                       return character < L' ' || character == 0x7f;
                   });
        }

        std::wstring_view _name(const std::wstring_view entry) noexcept
        {
            const auto offset = entry.starts_with(L'=') ? 1 : 0;
            const auto equals = entry.find(L'=', offset);
            return equals == std::wstring_view::npos ? entry : entry.substr(0, equals);
        }

        bool _removedInheritedName(const std::wstring_view name) noexcept
        {
            return _startsWithIgnoreCase(name, L"GIT_") ||
                   _equalsIgnoreCase(name, L"GCM_INTERACTIVE");
        }
    }

    bool IsAllowedGitEnvironmentOverride(const std::wstring_view name) noexcept
    {
        constexpr std::array<std::wstring_view, 6> allowed{
            L"GIT_AUTHOR_NAME",
            L"GIT_AUTHOR_EMAIL",
            L"GIT_AUTHOR_DATE",
            L"GIT_COMMITTER_NAME",
            L"GIT_COMMITTER_EMAIL",
            L"GIT_COMMITTER_DATE",
        };
        return std::ranges::any_of(allowed, [&](const auto candidate) {
            return _equalsIgnoreCase(name, candidate);
        });
    }

    GitEnvironmentResult BuildSanitizedGitEnvironment(
        const std::span<const GitEnvironmentOverride> overrides) noexcept
    {
        GitEnvironmentResult result;
        try
        {
            if (overrides.size() > 16)
            {
                result.Error = "Git environment override count exceeds its bound";
                return result;
            }

            std::vector<std::wstring> entries;
            const auto environment = GetEnvironmentStringsW();
            if (!environment)
            {
                result.Error = "Process environment is unavailable";
                return result;
            }
            struct EnvironmentOwner
            {
                wchar_t* Value{};
                ~EnvironmentOwner()
                {
                    if (Value)
                    {
                        FreeEnvironmentStringsW(Value);
                    }
                }
            } environmentOwner{ environment };

            for (auto current = environment; *current != L'\0';)
            {
                const std::wstring_view entry{ current };
                if (!_removedInheritedName(_name(entry)))
                {
                    entries.emplace_back(entry);
                }
                current += entry.size() + 1;
            }

            for (size_t index = 0; index < overrides.size(); ++index)
            {
                const auto& overrideValue = overrides[index];
                if (!IsAllowedGitEnvironmentOverride(overrideValue.Name) ||
                    !_validValue(overrideValue.Value) ||
                    std::ranges::any_of(
                        overrides.subspan(0, index),
                        [&](const auto& prior) {
                            return _equalsIgnoreCase(
                                prior.Name,
                                overrideValue.Name);
                        }))
                {
                    result.Error = "Git environment override is invalid or duplicated";
                    return result;
                }
                entries.emplace_back(
                    overrideValue.Name + L"=" + overrideValue.Value);
            }
            entries.emplace_back(L"GCM_INTERACTIVE=Never");
            entries.emplace_back(L"GIT_CONFIG_COUNT=0");
            entries.emplace_back(L"GIT_OPTIONAL_LOCKS=0");
            entries.emplace_back(L"GIT_TERMINAL_PROMPT=0");

            std::ranges::sort(entries, [](const auto& left, const auto& right) {
                return CompareStringOrdinal(
                           left.c_str(),
                           -1,
                           right.c_str(),
                           -1,
                           TRUE) == CSTR_LESS_THAN;
            });
            size_t characters{ 1 };
            for (const auto& entry : entries)
            {
                if (entry.size() > MaximumGitEnvironmentCharacters ||
                    characters > MaximumGitEnvironmentCharacters - entry.size() - 1)
                {
                    result.Error = "Git environment block exceeds its bound";
                    return result;
                }
                characters += entry.size() + 1;
            }

            result.Block.reserve(characters);
            for (const auto& entry : entries)
            {
                result.Block.insert(result.Block.end(), entry.begin(), entry.end());
                result.Block.push_back(L'\0');
            }
            result.Block.push_back(L'\0');
            result.Succeeded = true;
            return result;
        }
        catch (...)
        {
            result.Error = "Git environment construction exhausted local resources";
            return result;
        }
    }
}
