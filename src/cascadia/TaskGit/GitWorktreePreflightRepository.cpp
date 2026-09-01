// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitWorktreePreflightInternal.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitWorktreePreflightResult _failure(
            const GitWorktreePreflightStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitWorktreePreflightResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        bool _expectedMissingConfig(
            const Microsoft::Terminal::TaskExecution::GitCommandResult& result) noexcept
        {
            return result.ExitCode == 1 && result.Stdout.empty();
        }
    }

    std::optional<GitWorktreePreflightResult> CheckGitWorktreeRepositorySupport(
        const GitWorktreeSourceJournal& journal,
        const GitWorktreePreflightCommandInvoker& invoke) noexcept
    {
        try
        {
            const auto sparse = invoke({
                L"config",
                L"--bool",
                L"--get",
                L"core.sparseCheckout",
            });
            if (!sparse)
            {
                return sparse.Failure;
            }
            if (sparse.Result->ExitCode == 0)
            {
                const auto value = ParseGitSourceLine(*sparse.Result);
                if (!value || (*value != "true" && *value != "false"))
                {
                    return _failure(
                        GitWorktreePreflightStatus::InvalidGitOutput,
                        ERROR_INVALID_DATA,
                        "Git sparse-checkout configuration is invalid");
                }
                if (*value == "true")
                {
                    return _failure(
                        GitWorktreePreflightStatus::UnsupportedSparseCheckout,
                        ERROR_NOT_SUPPORTED,
                        "Git sparse checkout is not supported for Task Worktrees");
                }
            }
            else if (!_expectedMissingConfig(*sparse.Result))
            {
                return _failure(
                    GitWorktreePreflightStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git sparse-checkout configuration query failed");
            }

            const auto partialClone = invoke({
                L"config",
                L"--get",
                L"extensions.partialClone",
            });
            const auto promisor = invoke({
                L"config",
                L"--bool",
                L"--get-regexp",
                LR"(^remote\..*\.promisor$)",
            });
            if (!partialClone)
            {
                return partialClone.Failure;
            }
            if (!promisor)
            {
                return promisor.Failure;
            }
            if (partialClone.Result->ExitCode != 0 &&
                !_expectedMissingConfig(*partialClone.Result))
            {
                return _failure(
                    GitWorktreePreflightStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git partial-clone configuration query failed");
            }
            std::optional<bool> hasPromisor{ false };
            if (promisor.Result->ExitCode == 0)
            {
                hasPromisor = GitConfigContainsTrueValue(
                    promisor.Result->Stdout);
                if (!hasPromisor)
                {
                    return _failure(
                        GitWorktreePreflightStatus::InvalidGitOutput,
                        ERROR_INVALID_DATA,
                        "Git promisor configuration output is invalid");
                }
            }
            else if (!_expectedMissingConfig(*promisor.Result))
            {
                return _failure(
                    GitWorktreePreflightStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git partial-clone configuration query failed");
            }
            if (partialClone.Result->ExitCode == 0 || *hasPromisor)
            {
                return _failure(
                    GitWorktreePreflightStatus::NetworkMaterialRequired,
                    ERROR_NOT_SUPPORTED,
                    "Git partial-clone or promisor material is not supported");
            }

            std::vector<std::pair<std::string, std::wstring>> objects{
                { journal.SourceHead, L"^{commit}" },
                { journal.WorktreeTree, L"^{tree}" },
                { journal.IndexTree, L"^{tree}" },
            };
            if (!journal.TrackedSnapshotCommit.empty())
            {
                objects.emplace_back(
                    journal.TrackedSnapshotCommit,
                    L"^{commit}");
            }
            for (const auto& [object, suffix] : objects)
            {
                const auto available = invoke({
                    L"cat-file",
                    L"-e",
                    WidenGitSourceAscii(object) + suffix,
                });
                if (!available)
                {
                    return available.Failure;
                }
                if (available.Result->ExitCode != 0)
                {
                    return _failure(
                        GitWorktreePreflightStatus::MissingObject,
                        ERROR_FILE_NOT_FOUND,
                        "A captured Git source object is no longer available");
                }
            }

            const std::array capturedTrees{
                std::string_view{ journal.SourceHead },
                std::string_view{ journal.WorktreeTree },
                std::string_view{ journal.IndexTree },
            };
            for (const auto tree : capturedTrees)
            {
                const auto inventory = invoke({
                    L"ls-tree",
                    L"-r",
                    L"-z",
                    WidenGitSourceAscii(tree),
                });
                if (!inventory)
                {
                    return inventory.Failure;
                }
                if (inventory.Result->ExitCode != 0)
                {
                    return _failure(
                        GitWorktreePreflightStatus::MissingObject,
                        ERROR_FILE_NOT_FOUND,
                        "A captured Git tree could not be inspected");
                }
                const auto hasSubmodules = GitTreeContainsSubmodules(
                    inventory.Result->Stdout,
                    journal.ObjectFormat);
                if (!hasSubmodules)
                {
                    return _failure(
                        GitWorktreePreflightStatus::InvalidGitOutput,
                        ERROR_INVALID_DATA,
                        "Captured Git tree inventory output is invalid");
                }
                if (*hasSubmodules)
                {
                    return _failure(
                        GitWorktreePreflightStatus::UnsupportedSubmodules,
                        ERROR_NOT_SUPPORTED,
                        "Git submodules are not supported for Task Worktrees");
                }
            }

            const auto filters = invoke({
                L"config",
                L"--get-regexp",
                LR"(^filter\..*\.(clean|smudge|process|required)$)",
            });
            if (!filters)
            {
                return filters.Failure;
            }
            std::vector<std::string> filterNames;
            if (filters.Result->ExitCode == 0)
            {
                const auto parsed = ParseGitFilterDriverNames(
                    filters.Result->Stdout);
                if (!parsed)
                {
                    return _failure(
                        GitWorktreePreflightStatus::InvalidGitOutput,
                        ERROR_INVALID_DATA,
                        "Git filter configuration output is invalid");
                }
                filterNames = *parsed;
            }
            else if (!_expectedMissingConfig(*filters.Result))
            {
                return _failure(
                    GitWorktreePreflightStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git filter configuration query failed");
            }
            if (std::ranges::find(filterNames, "lfs") == filterNames.end())
            {
                filterNames.emplace_back("lfs");
            }
            for (const auto& filter : filterNames)
            {
                const auto attributePathspec = std::wstring{ L":(attr:filter=" } +
                                               WidenGitSourceAscii(filter) +
                                               L")";
                const auto paths = invoke({
                    L"ls-files",
                    L"-z",
                    L"--",
                    attributePathspec,
                });
                if (!paths)
                {
                    return paths.Failure;
                }
                if (paths.Result->ExitCode != 0)
                {
                    return _failure(
                        GitWorktreePreflightStatus::GitFailure,
                        ERROR_INVALID_DATA,
                        "Git filter attribute query failed");
                }
                if (!paths.Result->Stdout.empty())
                {
                    return _failure(
                        GitWorktreePreflightStatus::UnsupportedFilter,
                        ERROR_NOT_SUPPORTED,
                        "Git clean/smudge filters are not supported for Task Worktrees");
                }
            }

            return std::nullopt;
        }
        catch (...)
        {
            return _failure(
                GitWorktreePreflightStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git Worktree repository checks exhausted local resources");
        }
    }
}
