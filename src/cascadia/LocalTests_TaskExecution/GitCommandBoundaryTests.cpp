// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitCommandLine.h"
#include "GitEnvironment.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;

namespace TaskExecutionLocalTests
{
    namespace
    {
        class EnvironmentRestore final
        {
        public:
            explicit EnvironmentRestore(std::wstring name) :
                _name{ std::move(name) }
            {
                const auto required = GetEnvironmentVariableW(
                    _name.c_str(),
                    nullptr,
                    0);
                if (required != 0)
                {
                    std::wstring value(required, L'\0');
                    const auto written = GetEnvironmentVariableW(
                        _name.c_str(),
                        value.data(),
                        required);
                    if (written != 0 && written < required)
                    {
                        value.resize(written);
                        _value = std::move(value);
                    }
                }
            }

            ~EnvironmentRestore()
            {
                SetEnvironmentVariableW(
                    _name.c_str(),
                    _value ? _value->c_str() : nullptr);
            }

        private:
            std::wstring _name;
            std::optional<std::wstring> _value;
        };

        std::vector<std::wstring> _entries(const std::vector<wchar_t>& block)
        {
            std::vector<std::wstring> result;
            if (block.empty())
            {
                return result;
            }
            for (auto current = block.data(); *current != L'\0';)
            {
                result.emplace_back(current);
                current += result.back().size() + 1;
            }
            return result;
        }

        bool _containsName(
            const std::vector<std::wstring>& entries,
            const std::wstring_view name)
        {
            return std::ranges::any_of(entries, [&](const auto& entry) {
                const auto equals = entry.find(L'=');
                const auto entryName = std::wstring_view{ entry }.substr(0, equals);
                return CompareStringOrdinal(
                           entryName.data(),
                           static_cast<int>(entryName.size()),
                           name.data(),
                           static_cast<int>(name.size()),
                           TRUE) == CSTR_EQUAL;
            });
        }
    }

    class GitCommandBoundaryTests
    {
        TEST_CLASS(GitCommandBoundaryTests);

        TEST_METHOD(WindowsQuotingPreservesEmptyQuotesBackslashesAndMetacharacters);
        TEST_METHOD(CommandLineBoundsFailBeforeProcessLaunch);
        TEST_METHOD(EnvironmentRemovesRepositorySelectorsAndAddsFixedPolicy);
        TEST_METHOD(EnvironmentOverridesAreAllowlistedBoundedAndUnique);
    };

    void GitCommandBoundaryTests::WindowsQuotingPreservesEmptyQuotesBackslashesAndMetacharacters()
    {
        VERIFY_ARE_EQUAL(std::wstring{ L"\"\"" }, *details::QuoteWindowsArgument(L""));
        VERIFY_ARE_EQUAL(std::wstring{ L"\"plain\"" }, *details::QuoteWindowsArgument(L"plain"));
        VERIFY_ARE_EQUAL(std::wstring{ L"\"two words\"" }, *details::QuoteWindowsArgument(L"two words"));
        VERIFY_ARE_EQUAL(std::wstring{ L"\"a\\\"b\"" }, *details::QuoteWindowsArgument(L"a\"b"));
        VERIFY_ARE_EQUAL(std::wstring{ L"\"C:\\path\\\\\"" }, *details::QuoteWindowsArgument(L"C:\\path\\"));

        const std::vector<std::wstring> arguments{
            L"status",
            L"value & echo injected | < > ^ % !",
            L"quote\"and\\trailing\\",
        };
        const auto commandLine = details::BuildGitCommandLine(
            LR"(C:\tools\git.exe)",
            arguments);
        VERIFY_IS_TRUE(commandLine.has_value());
        VERIFY_IS_TRUE(commandLine->starts_with(L"\"C:\\tools\\git.exe\" "));
        VERIFY_IS_TRUE(commandLine->find(L"\"value & echo injected | < > ^ % !\"") !=
                       std::wstring::npos);
    }

    void GitCommandBoundaryTests::CommandLineBoundsFailBeforeProcessLaunch()
    {
        std::wstring embeddedNull{ L"valid" };
        embeddedNull.push_back(L'\0');
        embeddedNull.append(L"hidden");
        VERIFY_IS_FALSE(details::QuoteWindowsArgument(embeddedNull).has_value());
        VERIFY_IS_FALSE(details::QuoteWindowsArgument(
                            std::wstring(MaximumGitArgumentCharacters + 1, L'x'))
                            .has_value());
        VERIFY_IS_FALSE(details::BuildGitCommandLine(
                            LR"(C:\tools\git.exe)",
                            std::span<const std::wstring>{})
                            .has_value());

        std::vector<std::wstring> tooMany(MaximumGitArgumentCount + 1, L"x");
        VERIFY_IS_FALSE(details::BuildGitCommandLine(
                            LR"(C:\tools\git.exe)",
                            tooMany)
                            .has_value());
    }

    void GitCommandBoundaryTests::EnvironmentRemovesRepositorySelectorsAndAddsFixedPolicy()
    {
        EnvironmentRestore gitDirectory{ L"GIT_DIR" };
        EnvironmentRestore gitWorktree{ L"GIT_WORK_TREE" };
        EnvironmentRestore prompt{ L"GIT_TERMINAL_PROMPT" };
        EnvironmentRestore credentialPrompt{ L"GCM_INTERACTIVE" };
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"GIT_DIR", L"C:\\poison") != FALSE);
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"GIT_WORK_TREE", L"C:\\poison") != FALSE);
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"1") != FALSE);
        VERIFY_IS_TRUE(SetEnvironmentVariableW(L"GCM_INTERACTIVE", L"Always") != FALSE);

        const auto environment = details::BuildSanitizedGitEnvironment(
            std::span<const GitEnvironmentOverride>{});
        VERIFY_IS_TRUE(environment.Succeeded);
        VERIFY_IS_TRUE(environment.Block.size() >= 2);
        VERIFY_ARE_EQUAL(L'\0', environment.Block.back());
        VERIFY_ARE_EQUAL(L'\0', environment.Block[environment.Block.size() - 2]);
        const auto entries = _entries(environment.Block);
        VERIFY_IS_FALSE(std::ranges::any_of(entries, [](const auto& entry) {
            return entry == L"GIT_DIR=C:\\poison" ||
                   entry == L"GIT_WORK_TREE=C:\\poison" ||
                   entry == L"GIT_TERMINAL_PROMPT=1" ||
                   entry == L"GCM_INTERACTIVE=Always";
        }));
        VERIFY_IS_TRUE(std::ranges::find(entries, L"GCM_INTERACTIVE=Never") != entries.end());
        VERIFY_IS_TRUE(std::ranges::find(entries, L"GIT_CONFIG_COUNT=0") != entries.end());
        VERIFY_IS_TRUE(std::ranges::find(entries, L"GIT_OPTIONAL_LOCKS=0") != entries.end());
        VERIFY_IS_TRUE(std::ranges::find(entries, L"GIT_TERMINAL_PROMPT=0") != entries.end());
        VERIFY_IS_TRUE(_containsName(entries, L"PATH"));
    }

    void GitCommandBoundaryTests::EnvironmentOverridesAreAllowlistedBoundedAndUnique()
    {
        const std::array<GitEnvironmentOverride, 2> valid{
            GitEnvironmentOverride{ L"GIT_AUTHOR_NAME", L"Richer & Runtime" },
            GitEnvironmentOverride{ L"GIT_AUTHOR_EMAIL", L"task@example.invalid" },
        };
        const auto accepted = details::BuildSanitizedGitEnvironment(valid);
        VERIFY_IS_TRUE(accepted.Succeeded);
        const auto entries = _entries(accepted.Block);
        VERIFY_IS_TRUE(std::ranges::find(
                           entries,
                           L"GIT_AUTHOR_NAME=Richer & Runtime") != entries.end());
        VERIFY_IS_TRUE(std::ranges::find(
                           entries,
                           L"GIT_AUTHOR_EMAIL=task@example.invalid") != entries.end());

        const std::array<GitEnvironmentOverride, 1> reserved{
            GitEnvironmentOverride{ L"GIT_DIR", L"C:\\escape" },
        };
        VERIFY_IS_FALSE(details::BuildSanitizedGitEnvironment(reserved).Succeeded);
        const std::array<GitEnvironmentOverride, 2> duplicate{
            GitEnvironmentOverride{ L"GIT_AUTHOR_NAME", L"first" },
            GitEnvironmentOverride{ L"git_author_name", L"second" },
        };
        VERIFY_IS_FALSE(details::BuildSanitizedGitEnvironment(duplicate).Succeeded);
        const std::array<GitEnvironmentOverride, 1> control{
            GitEnvironmentOverride{ L"GIT_AUTHOR_NAME", L"line\nbreak" },
        };
        VERIFY_IS_FALSE(details::BuildSanitizedGitEnvironment(control).Succeeded);
    }
}
