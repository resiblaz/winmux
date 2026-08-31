// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSourceCapture.h"

#include "GitSourceCommandOutput.h"
#include "GitSourceCapturePolicy.h"
#include "GitSourceDigest.h"
#include "GitSourceStatus.h"
#include "GitTrackedSource.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        constexpr size_t CaptureStdoutBytes{
            MaximumGitOutputBytes - 64 * 1024
        };
        constexpr size_t CaptureStderrBytes{ 64 * 1024 };

    }

    GitSourceCapture::GitSourceCapture(
        std::filesystem::path gitExecutable,
        GitCommandExecutor executor,
        GitSnapshotFileHasher fileHasher) :
        _gitExecutable{ std::move(gitExecutable) },
        _executor{ executor ?
                       std::move(executor) :
                       GitCommandExecutor{ [](const GitCommandRequest& request) {
                           return GitCommandRunner{}.Run(request);
                       } } },
        _fileHasher{ fileHasher ?
                         std::move(fileHasher) :
                         GitSnapshotFileHasher{ HashGitSnapshotFile } }
    {
    }

    GitSourceCaptureResult GitSourceCapture::Capture(
        const std::filesystem::path& sourcePath,
        const HANDLE cancellationEvent,
        const uint32_t timeoutMilliseconds) const noexcept
    {
        try
        {
            if (!_executor || !_fileHasher || _gitExecutable.empty() ||
                !cancellationEvent || cancellationEvent == INVALID_HANDLE_VALUE ||
                timeoutMilliseconds == 0 ||
                timeoutMilliseconds > MaximumGitSourceCaptureTimeoutMilliseconds)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::InvalidRequest,
                    ERROR_INVALID_PARAMETER,
                    "Git source capture request is invalid");
            }
            const auto deadline = GetTickCount64() + timeoutMilliseconds;
            const auto remaining = [&]() -> std::optional<uint32_t> {
                const auto now = GetTickCount64();
                return now < deadline ?
                           std::optional<uint32_t>{ static_cast<uint32_t>(
                               (std::min<uint64_t>)(deadline - now, UINT32_MAX)) } :
                           std::nullopt;
            };

            GitRepositoryInspector inspector{ _gitExecutable, _executor };
            const auto initialTime = remaining();
            if (!initialTime)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git source capture deadline expired before inspection");
            }
            const auto initial = inspector.Inspect(
                sourcePath,
                cancellationEvent,
                (std::min)(
                    *initialTime,
                    MaximumGitCommandTimeoutMilliseconds));
            if (!initial.Succeeded())
            {
                return details::GitSourceCaptureFailure(
                    details::MapGitInspectionFailure(initial.Status),
                    initial.SystemError,
                    "Git source capture initial inspection failed");
            }

            const auto lockTime = remaining();
            if (!lockTime)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git source capture deadline expired before locking");
            }
            auto gitLock = AcquireGitRepositoryLock(
                initial.Inspection->CommonDirectoryIdentity,
                cancellationEvent,
                (std::min)(
                    *lockTime,
                    GitRepositoryLockTimeoutMilliseconds));
            if (!gitLock.Acquired())
            {
                return details::GitSourceCaptureFailure(
                    gitLock.Status == TaskDirectoryLockStatus::Cancelled ?
                        GitSourceCaptureStatus::Cancelled :
                    gitLock.Status == TaskDirectoryLockStatus::TimedOut ?
                        GitSourceCaptureStatus::TimedOut :
                        GitSourceCaptureStatus::LockFailed,
                    gitLock.SystemError,
                    "Git source capture common-directory lock failed");
            }

            const auto lockedTime = remaining();
            if (!lockedTime)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git source capture deadline expired after locking");
            }
            const auto locked = inspector.Inspect(
                sourcePath,
                cancellationEvent,
                (std::min)(
                    *lockedTime,
                    MaximumGitCommandTimeoutMilliseconds));
            if (!locked.Succeeded() ||
                !details::SameGitSourceRepository(
                    *initial.Inspection,
                    *locked.Inspection))
            {
                return details::GitSourceCaptureFailure(
                    locked.Succeeded() ? GitSourceCaptureStatus::SourceChanged :
                                         details::MapGitInspectionFailure(locked.Status),
                    locked.SystemError,
                    "Git source identity changed while acquiring its lock");
            }

            GitSourceCaptureResult commandFailure;
            const auto run = [&](std::vector<std::wstring> arguments) ->
                std::optional<GitCommandResult> {
                const auto time = remaining();
                if (!time)
                {
                    commandFailure = details::GitSourceCaptureFailure(
                        GitSourceCaptureStatus::TimedOut,
                        WAIT_TIMEOUT,
                        "Git source capture deadline expired");
                    return std::nullopt;
                }
                GitCommandRequest request;
                request.GitExecutable = locked.Inspection->GitExecutable;
                request.WorkingDirectory = locked.Inspection->WorktreeIdentity;
                request.Arguments = std::move(arguments);
                request.CancellationEvent = cancellationEvent;
                request.TimeoutMilliseconds = (std::min)(
                    *time,
                    MaximumGitCommandTimeoutMilliseconds);
                request.MaximumStdoutBytes = CaptureStdoutBytes;
                request.MaximumStderrBytes = CaptureStderrBytes;
                const auto result = _executor(request);
                if (result.Status != GitCommandStatus::Completed)
                {
                    commandFailure = details::GitSourceCaptureFailure(
                        details::MapGitCommandFailure(result.Status),
                        result.SystemError,
                        "Git source capture command failed");
                    return std::nullopt;
                }
                return result;
            };

            const auto statusBefore = run({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
                L"--ignored=matching",
                L"--no-renames",
            });
            const auto pathsBefore = run({
                L"ls-files",
                L"--others",
                L"--exclude-standard",
                L"-z",
            });
            const auto stashBefore = run({
                L"rev-parse",
                L"--verify",
                L"--quiet",
                L"refs/stash",
            });
            if (!statusBefore || !pathsBefore || !stashBefore)
            {
                return commandFailure;
            }
            if (statusBefore->ExitCode != 0 || pathsBefore->ExitCode != 0 ||
                (stashBefore->ExitCode != 0 && stashBefore->ExitCode != 1 &&
                 stashBefore->ExitCode != 128))
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::GitFailure,
                    ERROR_INVALID_DATA,
                    "Git source inventory command returned an unexpected exit code");
            }
            const auto parsedStatus = details::ParseGitStatusPorcelainV2(
                statusBefore->Stdout);
            const auto parsedPaths = details::ParseGitNulPathList(
                pathsBefore->Stdout,
                MaximumGitSnapshotUntrackedFiles);
            if (!parsedStatus.Succeeded || !parsedPaths.Succeeded)
            {
                return details::GitSourceCaptureFailure(
                    parsedPaths.CountExceeded ?
                        GitSourceCaptureStatus::FileCountExceeded :
                        GitSourceCaptureStatus::InvalidGitOutput,
                    ERROR_INVALID_DATA,
                    "Git source inventory output is invalid");
            }
            if (parsedStatus.Summary.UntrackedFiles != parsedPaths.Paths.size())
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git source untracked inventories disagree");
            }
            const auto stashReferenceBefore = stashBefore->ExitCode == 0 ?
                                                  details::ParseGitSourceLine(*stashBefore) :
                                                  std::optional<std::string>{ std::string{} };
            if (!stashReferenceBefore ||
                (!stashReferenceBefore->empty() &&
                 !details::IsGitSourceObjectId(
                     *stashReferenceBefore,
                     locked.Inspection->ObjectFormat)))
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::InvalidGitOutput,
                    ERROR_INVALID_DATA,
                    "Git source stash reference is invalid");
            }

            const details::GitSourceCommandInvoker trackedCommands{ run };
            const auto proofA = details::CaptureGitTrackedSourceProof(
                parsedStatus.Summary,
                locked.Inspection->ObjectFormat,
                trackedCommands);
            if (!proofA.Succeeded())
            {
                return details::MapGitTrackedProofFailure(
                    proofA,
                    commandFailure);
            }

            std::vector<GitSourceFileRecord> firstFiles;
            firstFiles.reserve(parsedPaths.Paths.size());
            uint64_t totalBytes{};
            for (const auto& relativePath : parsedPaths.Paths)
            {
                const auto file = _fileHasher(
                    locked.Inspection->WorktreeIdentity,
                    relativePath,
                    cancellationEvent);
                if (!file.Succeeded())
                {
                return details::GitSourceCaptureFailure(
                        details::MapGitSnapshotFileFailure(file.Status),
                        file.SystemError,
                        "Git source untracked file could not be captured");
                }
                if (!details::ValidateGitSourceFileRecord(
                        *file.File,
                        relativePath))
                {
                return details::GitSourceCaptureFailure(
                        GitSourceCaptureStatus::UnsafeUntrackedFile,
                        ERROR_INVALID_DATA,
                        "Git source untracked file record is invalid");
                }
                if (file.File->Size > MaximumGitSnapshotUntrackedBytes - totalBytes)
                {
                return details::GitSourceCaptureFailure(
                        GitSourceCaptureStatus::ByteLimitExceeded,
                        ERROR_FILE_TOO_LARGE,
                        "Git source untracked files exceed their total byte limit");
                }
                totalBytes += file.File->Size;
                firstFiles.emplace_back(*file.File);
            }

            const auto proofB = details::CaptureGitTrackedSourceProof(
                parsedStatus.Summary,
                locked.Inspection->ObjectFormat,
                trackedCommands);
            if (!proofB.Succeeded())
            {
                return details::MapGitTrackedProofFailure(
                    proofB,
                    commandFailure);
            }
            if (proofA.Proof->WorktreeTree != proofB.Proof->WorktreeTree ||
                proofA.Proof->IndexTree != proofB.Proof->IndexTree)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git tracked source changed during snapshot capture");
            }
            const auto statusAfter = run({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
                L"--ignored=matching",
                L"--no-renames",
            });
            const auto pathsAfter = run({
                L"ls-files",
                L"--others",
                L"--exclude-standard",
                L"-z",
            });
            if (!statusAfter || !pathsAfter)
            {
                return commandFailure;
            }
            if (statusAfter->ExitCode != 0 || pathsAfter->ExitCode != 0 ||
                statusAfter->Stdout != statusBefore->Stdout ||
                pathsAfter->Stdout != pathsBefore->Stdout)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git source inventory changed during snapshot capture");
            }

            for (size_t index = 0; index < parsedPaths.Paths.size(); ++index)
            {
                const auto file = _fileHasher(
                    locked.Inspection->WorktreeIdentity,
                    parsedPaths.Paths[index],
                    cancellationEvent);
                if (!file.Succeeded() ||
                    !details::ValidateGitSourceFileRecord(
                        *file.File,
                        parsedPaths.Paths[index]) ||
                    !details::SameGitSourceFile(firstFiles[index], *file.File))
                {
                return details::GitSourceCaptureFailure(
                        file.Succeeded() ? GitSourceCaptureStatus::SourceChanged :
                                           details::MapGitSnapshotFileFailure(file.Status),
                        file.SystemError,
                        "Git source untracked file changed during snapshot capture");
                }
            }

            const auto proofC = details::CaptureGitTrackedSourceProof(
                parsedStatus.Summary,
                locked.Inspection->ObjectFormat,
                trackedCommands);
            if (!proofC.Succeeded())
            {
                return details::MapGitTrackedProofFailure(
                    proofC,
                    commandFailure);
            }
            const auto finalStatus = run({
                L"status",
                L"--porcelain=v2",
                L"-z",
                L"--untracked-files=all",
                L"--ignored=matching",
                L"--no-renames",
            });
            const auto finalPaths = run({
                L"ls-files",
                L"--others",
                L"--exclude-standard",
                L"-z",
            });
            const auto stashAfter = run({
                L"rev-parse",
                L"--verify",
                L"--quiet",
                L"refs/stash",
            });
            if (!finalStatus || !finalPaths || !stashAfter)
            {
                return commandFailure;
            }
            if (proofB.Proof->WorktreeTree != proofC.Proof->WorktreeTree ||
                proofB.Proof->IndexTree != proofC.Proof->IndexTree ||
                finalStatus->ExitCode != 0 || finalPaths->ExitCode != 0 ||
                finalStatus->Stdout != statusBefore->Stdout ||
                finalPaths->Stdout != pathsBefore->Stdout ||
                (stashAfter->ExitCode != 0 && stashAfter->ExitCode != 1 &&
                 stashAfter->ExitCode != 128))
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git source final proof differs from the captured state");
            }
            const auto stashReferenceAfter = stashAfter->ExitCode == 0 ?
                                                 details::ParseGitSourceLine(*stashAfter) :
                                                 std::optional<std::string>{ std::string{} };
            if (!stashReferenceAfter ||
                *stashReferenceAfter != *stashReferenceBefore)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git stash reference changed during source capture");
            }

            const auto finalTime = remaining();
            if (!finalTime)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::TimedOut,
                    WAIT_TIMEOUT,
                    "Git source capture deadline expired before final inspection");
            }
            const auto finalInspection = inspector.Inspect(
                sourcePath,
                cancellationEvent,
                (std::min)(
                    *finalTime,
                    MaximumGitCommandTimeoutMilliseconds));
            if (!finalInspection.Succeeded() ||
                !details::SameGitSourceRepository(
                    *locked.Inspection,
                    *finalInspection.Inspection))
            {
                return details::GitSourceCaptureFailure(
                    finalInspection.Succeeded() ? GitSourceCaptureStatus::SourceChanged :
                                                  details::MapGitInspectionFailure(
                                                      finalInspection.Status),
                    finalInspection.SystemError,
                    "Git source repository identity changed during capture");
            }

            const auto statusDigest = details::ComputeGitSourceStatusDigest(
                statusBefore->Stdout);
            if (!statusDigest)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::ResourceFailure,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Git source status digest failed");
            }

            GitSourceCaptureResult result;
            result.Status = GitSourceCaptureStatus::Succeeded;
            result.Snapshot.emplace();
            result.Snapshot->Repository = *locked.Inspection;
            result.Snapshot->Status = parsedStatus.Summary;
            result.Snapshot->TrackedSnapshotCommit =
                proofC.Proof->SnapshotCommit;
            result.Snapshot->WorktreeTree = proofC.Proof->WorktreeTree;
            result.Snapshot->IndexTree = proofC.Proof->IndexTree;
            result.Snapshot->StashReference = *stashReferenceBefore;
            result.Snapshot->StatusDigest = *statusDigest;
            result.Snapshot->UntrackedFiles = std::move(firstFiles);
            result.Snapshot->UntrackedBytes = totalBytes;
            result.Snapshot->CapturedAtUnixMilliseconds =
                finalInspection.Inspection->CapturedAtUnixMilliseconds;
            const auto snapshotDigest = details::ComputeGitSourceSnapshotDigest(
                *result.Snapshot);
            if (!snapshotDigest)
            {
                return details::GitSourceCaptureFailure(
                    GitSourceCaptureStatus::ResourceFailure,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Git source snapshot digest failed");
            }
            result.Snapshot->SnapshotDigest = *snapshotDigest;
            return result;
        }
        catch (...)
        {
                return details::GitSourceCaptureFailure(
                GitSourceCaptureStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git source capture exhausted local resources");
        }
    }
}
