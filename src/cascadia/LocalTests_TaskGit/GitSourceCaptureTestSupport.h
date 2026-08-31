// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "TaskGitTestSupport.h"

namespace TaskGitLocalTests::TestSupport
{
    struct ObservedGitSourceState
    {
        std::string Head;
        std::string Branch;
        std::string Status;
        std::string StashReference;
        std::string IndexEntries;
        std::vector<std::pair<std::filesystem::path, std::string>> WorktreeFiles;
    };

    inline std::string ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream input{ path, std::ios::binary };
        VERIFY_IS_TRUE(static_cast<bool>(input));
        return {
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{},
        };
    }

    inline std::string SingleLine(
        const Microsoft::Terminal::TaskExecution::GitCommandResult& result)
    {
        auto output = result.Stdout;
        if (!output.empty() && output.back() == '\n')
        {
            output.pop_back();
            if (!output.empty() && output.back() == '\r')
            {
                output.pop_back();
            }
        }
        return output;
    }

    inline Microsoft::Terminal::TaskExecution::GitCommandResult RunCompleted(
        const GitFixture& fixture,
        const std::filesystem::path& repository,
        std::vector<std::wstring> arguments,
        const DWORD expectedExitCode = 0)
    {
        using namespace Microsoft::Terminal::TaskExecution;
        auto result = fixture.Run(repository, std::move(arguments));
        VERIFY_IS_TRUE(result.Status == GitCommandStatus::Completed);
        VERIFY_ARE_EQUAL(expectedExitCode, result.ExitCode);
        return result;
    }

    inline bool CommitAll(
        const GitFixture& fixture,
        const std::filesystem::path& repository,
        const std::wstring_view message)
    {
        return fixture.Run(repository, { L"add", L"-A" }).Succeeded() &&
               fixture.Run(repository, {
                   L"-c",
                   L"user.name=Task Git Test",
                   L"-c",
                   L"user.email=task-git@example.invalid",
                   L"-c",
                   L"commit.gpgSign=false",
                   L"-c",
                   L"core.hooksPath=NUL",
                   L"commit",
                   L"--no-verify",
                   L"-m",
                   std::wstring{ message },
               }).Succeeded();
    }

    inline bool CreateExistingStash(
        const GitFixture& fixture,
        const std::filesystem::path& repository)
    {
        return fixture.Run(repository, {
            L"-c",
            L"user.name=Task Git Test",
            L"-c",
            L"user.email=task-git@example.invalid",
            L"stash",
            L"push",
            L"-m",
            L"existing test stash",
        }).Succeeded();
    }

    inline ObservedGitSourceState ObserveGitSourceState(
        const GitFixture& fixture,
        const std::filesystem::path& repository,
        const std::vector<std::filesystem::path>& worktreeFiles)
    {
        ObservedGitSourceState state;
        state.Head = SingleLine(RunCompleted(
            fixture,
            repository,
            { L"rev-parse", L"--verify", L"HEAD" }));
        state.Branch = SingleLine(RunCompleted(
            fixture,
            repository,
            { L"symbolic-ref", L"HEAD" }));
        state.Status = RunCompleted(
                           fixture,
                           repository,
                           {
                               L"status",
                               L"--porcelain=v2",
                               L"-z",
                               L"--untracked-files=all",
                               L"--ignored=matching",
                               L"--no-renames",
                           })
                           .Stdout;
        const auto stash = fixture.Run(repository, {
            L"rev-parse",
            L"--verify",
            L"--quiet",
            L"refs/stash",
        });
        VERIFY_IS_TRUE(
            stash.Status == Microsoft::Terminal::TaskExecution::GitCommandStatus::Completed);
        VERIFY_IS_TRUE(
            stash.ExitCode == 0 || stash.ExitCode == 1 || stash.ExitCode == 128);
        if (stash.ExitCode == 0)
        {
            state.StashReference = SingleLine(stash);
        }

        state.IndexEntries = RunCompleted(
                                 fixture,
                                 repository,
                                 { L"ls-files", L"--stage", L"-z" })
                                 .Stdout;
        state.WorktreeFiles.reserve(worktreeFiles.size());
        for (const auto& relativePath : worktreeFiles)
        {
            state.WorktreeFiles.emplace_back(
                relativePath,
                ReadBinaryFile(repository / relativePath));
        }
        return state;
    }

    inline bool SameGitSourceState(
        const ObservedGitSourceState& left,
        const ObservedGitSourceState& right)
    {
        return left.Head == right.Head && left.Branch == right.Branch &&
               left.Status == right.Status &&
               left.StashReference == right.StashReference &&
               left.IndexEntries == right.IndexEntries &&
               left.WorktreeFiles == right.WorktreeFiles;
    }
}
