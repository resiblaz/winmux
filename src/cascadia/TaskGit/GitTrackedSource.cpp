// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitTrackedSource.h"

#include "GitSourceCommandOutput.h"

namespace Microsoft::Terminal::TaskGit::details
{
    namespace
    {
        GitTrackedSourceProofResult _failure(
            const GitTrackedSourceProofStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitTrackedSourceProofResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }
    }

    GitTrackedSourceProofResult CaptureGitTrackedSourceProof(
        const GitSourceStatusSummary& summary,
        const Microsoft::Terminal::TaskExecution::GitObjectFormat objectFormat,
        const GitSourceCommandInvoker& invoke)
    {
        if (!invoke)
        {
            return _failure(
                GitTrackedSourceProofStatus::CommandUnavailable,
                ERROR_INVALID_PARAMETER,
                "Git tracked source command invoker is unavailable");
        }

        GitTrackedSourceProof proof;
        const auto dirty = summary.HasStagedChanges || summary.HasUnstagedChanges;
        if (dirty)
        {
            const auto created = invoke({
                L"-c",
                L"user.name=Richer Task Runtime",
                L"-c",
                L"user.email=task-runtime@localhost",
                L"stash",
                L"create",
                L"Richer Task source snapshot",
            });
            if (!created)
            {
                return _failure(
                    GitTrackedSourceProofStatus::CommandUnavailable,
                    ERROR_GEN_FAILURE,
                    "Git tracked source snapshot command is unavailable");
            }
            if (created->ExitCode != 0)
            {
                return _failure(
                    GitTrackedSourceProofStatus::GitFailure,
                    created->ExitCode,
                    "Git tracked source snapshot command failed");
            }
            const auto commit = ParseGitSourceLine(*created);
            if (!commit || !IsGitSourceObjectId(*commit, objectFormat))
            {
                return _failure(
                    GitTrackedSourceProofStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git tracked source snapshot object is invalid");
            }
            proof.SnapshotCommit = *commit;

            const auto worktreeTree = invoke({
                L"rev-parse",
                L"--verify",
                WidenGitSourceAscii(*commit) + L"^{tree}",
            });
            const auto indexTree = invoke({
                L"rev-parse",
                L"--verify",
                WidenGitSourceAscii(*commit) + L"^2^{tree}",
            });
            if (!worktreeTree || !indexTree)
            {
                return _failure(
                    GitTrackedSourceProofStatus::CommandUnavailable,
                    ERROR_GEN_FAILURE,
                    "Git tracked source tree command is unavailable");
            }
            if (worktreeTree->ExitCode != 0 || indexTree->ExitCode != 0)
            {
                return _failure(
                    GitTrackedSourceProofStatus::GitFailure,
                    worktreeTree->ExitCode != 0 ?
                        worktreeTree->ExitCode :
                        indexTree->ExitCode,
                    "Git tracked source tree command failed");
            }
            const auto worktreeLine = ParseGitSourceLine(*worktreeTree);
            const auto indexLine = ParseGitSourceLine(*indexTree);
            if (!worktreeLine || !indexLine ||
                !IsGitSourceObjectId(*worktreeLine, objectFormat) ||
                !IsGitSourceObjectId(*indexLine, objectFormat))
            {
                return _failure(
                    GitTrackedSourceProofStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git tracked source tree output is invalid");
            }
            proof.WorktreeTree = *worktreeLine;
            proof.IndexTree = *indexLine;
        }
        else
        {
            const auto headTree = invoke({
                L"rev-parse",
                L"--verify",
                L"HEAD^{tree}",
            });
            if (!headTree)
            {
                return _failure(
                    GitTrackedSourceProofStatus::CommandUnavailable,
                    ERROR_GEN_FAILURE,
                    "Git clean source tree command is unavailable");
            }
            if (headTree->ExitCode != 0)
            {
                return _failure(
                    GitTrackedSourceProofStatus::GitFailure,
                    headTree->ExitCode,
                    "Git clean source tree command failed");
            }
            const auto tree = ParseGitSourceLine(*headTree);
            if (!tree || !IsGitSourceObjectId(*tree, objectFormat))
            {
                return _failure(
                    GitTrackedSourceProofStatus::InvalidOutput,
                    ERROR_INVALID_DATA,
                    "Git clean source tree output is invalid");
            }
            proof.WorktreeTree = *tree;
            proof.IndexTree = *tree;
        }

        GitTrackedSourceProofResult result;
        result.Status = GitTrackedSourceProofStatus::Succeeded;
        result.Proof = std::move(proof);
        return result;
    }
}
