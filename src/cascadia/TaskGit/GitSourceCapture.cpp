// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSourceCapture.h"

#include "GitSourceStatus.h"

#include "GitCommandRunner.h"
#include "GitRepositoryLock.h"
#include "TaskExecutionCrypto.h"

namespace Microsoft::Terminal::TaskGit
{
    namespace
    {
        using namespace Microsoft::Terminal::TaskExecution;

        constexpr size_t CaptureStdoutBytes{
            MaximumGitOutputBytes - 64 * 1024
        };
        constexpr size_t CaptureStderrBytes{ 64 * 1024 };

        struct TrackedProof
        {
            std::string SnapshotCommit;
            std::string WorktreeTree;
            std::string IndexTree;
        };

        GitSourceCaptureResult _failure(
            const GitSourceCaptureStatus status,
            const DWORD systemError,
            std::string error)
        {
            GitSourceCaptureResult result;
            result.Status = status;
            result.SystemError = systemError;
            result.Error = std::move(error);
            return result;
        }

        GitSourceCaptureStatus _inspectionFailure(
            const GitRepositoryInspectionStatus status) noexcept
        {
            switch (status)
            {
            case GitRepositoryInspectionStatus::Cancelled:
                return GitSourceCaptureStatus::Cancelled;
            case GitRepositoryInspectionStatus::TimedOut:
                return GitSourceCaptureStatus::TimedOut;
            case GitRepositoryInspectionStatus::InvalidRequest:
                return GitSourceCaptureStatus::InvalidRequest;
            case GitRepositoryInspectionStatus::ResourceFailure:
                return GitSourceCaptureStatus::ResourceFailure;
            default:
                return GitSourceCaptureStatus::InspectionFailed;
            }
        }

        GitSourceCaptureStatus _commandFailure(
            const GitCommandStatus status) noexcept
        {
            switch (status)
            {
            case GitCommandStatus::Cancelled:
                return GitSourceCaptureStatus::Cancelled;
            case GitCommandStatus::TimedOut:
                return GitSourceCaptureStatus::TimedOut;
            case GitCommandStatus::OutputLimitExceeded:
            case GitCommandStatus::InvalidUtf8:
                return GitSourceCaptureStatus::InvalidGitOutput;
            case GitCommandStatus::ResourceFailure:
                return GitSourceCaptureStatus::ResourceFailure;
            default:
                return GitSourceCaptureStatus::GitFailure;
            }
        }

        GitSourceCaptureStatus _fileFailure(
            const GitSnapshotFileStatus status) noexcept
        {
            switch (status)
            {
            case GitSnapshotFileStatus::Cancelled:
                return GitSourceCaptureStatus::Cancelled;
            case GitSnapshotFileStatus::TooLarge:
                return GitSourceCaptureStatus::ByteLimitExceeded;
            case GitSnapshotFileStatus::Missing:
            case GitSnapshotFileStatus::SourceChanged:
            case GitSnapshotFileStatus::IdentityMismatch:
                return GitSourceCaptureStatus::SourceChanged;
            case GitSnapshotFileStatus::ReadFailure:
            case GitSnapshotFileStatus::ResourceFailure:
                return GitSourceCaptureStatus::ResourceFailure;
            default:
                return GitSourceCaptureStatus::UnsafeUntrackedFile;
            }
        }

        std::optional<std::string> _line(
            const GitCommandResult& result,
            const bool allowEmpty = false)
        {
            if (result.Status != GitCommandStatus::Completed ||
                result.Stdout.size() > CaptureStdoutBytes ||
                result.Stderr.size() > CaptureStderrBytes)
            {
                return std::nullopt;
            }
            auto value = result.Stdout;
            if (!value.empty() && value.back() == '\n')
            {
                value.pop_back();
                if (!value.empty() && value.back() == '\r')
                {
                    value.pop_back();
                }
            }
            if ((!allowEmpty && value.empty()) ||
                std::ranges::any_of(value, [](const unsigned char character) {
                    return character < 0x20 || character == 0x7f;
                }))
            {
                return std::nullopt;
            }
            return value;
        }

        bool _objectId(
            const std::string_view value,
            const GitObjectFormat format) noexcept
        {
            const auto length = format == GitObjectFormat::Sha1 ? 40 : 64;
            return value.size() == length &&
                   std::ranges::all_of(value, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        bool _sameRepository(
            const GitRepositoryInspection& left,
            const GitRepositoryInspection& right) noexcept
        {
            return left.WorktreeIdentity.Digest() == right.WorktreeIdentity.Digest() &&
                   left.CommonDirectoryIdentity.Digest() ==
                       right.CommonDirectoryIdentity.Digest() &&
                   left.GitDirectoryIdentity.Digest() ==
                       right.GitDirectoryIdentity.Digest() &&
                   left.ObjectFormat == right.ObjectFormat &&
                   left.Head == right.Head &&
                   left.BranchRef == right.BranchRef &&
                   left.LinkedWorktree == right.LinkedWorktree &&
                   left.DetachedHead == right.DetachedHead;
        }

        bool _sameFile(
            const GitSourceFileRecord& left,
            const GitSourceFileRecord& right) noexcept
        {
            return left.RelativePath == right.RelativePath &&
                   left.Type == right.Type && left.Size == right.Size &&
                   left.Sha256 == right.Sha256;
        }

        bool _validFileRecord(
            const GitSourceFileRecord& file,
            const std::string_view expectedPath) noexcept
        {
            return file.RelativePath == expectedPath &&
                   file.Type == GitSourceFileType::RegularFile &&
                   file.Sha256.size() == 64 &&
                   std::ranges::all_of(file.Sha256, [](const char character) {
                       return (character >= '0' && character <= '9') ||
                              (character >= 'a' && character <= 'f');
                   });
        }

        std::optional<std::string> _digest(
            const std::span<const uint8_t> payload)
        {
            const auto digest =
                Microsoft::Terminal::TaskExecution::details::TaskExecutionSha256(payload);
            return digest ?
                       std::optional<std::string>{
                           Microsoft::Terminal::TaskExecution::details::LowerHex(*digest) } :
                       std::nullopt;
        }

        std::wstring _wideAscii(const std::string_view value)
        {
            std::wstring result;
            result.reserve(value.size());
            for (const auto character : value)
            {
                result.push_back(static_cast<wchar_t>(
                    static_cast<unsigned char>(character)));
            }
            return result;
        }

        void _appendUint64(std::vector<uint8_t>& payload, const uint64_t value)
        {
            for (size_t index = 0; index < sizeof(value); ++index)
            {
                payload.push_back(static_cast<uint8_t>(value >> (index * 8)));
            }
        }

        void _appendText(
            std::vector<uint8_t>& payload,
            const std::string_view value)
        {
            _appendUint64(payload, value.size());
            payload.insert(payload.end(), value.begin(), value.end());
        }

        std::optional<std::string> _snapshotDigest(
            const GitSourceSnapshot& snapshot)
        {
            constexpr std::string_view domain{ "wt-task-git-source-snapshot-v1" };
            std::vector<uint8_t> payload;
            payload.insert(payload.end(), domain.begin(), domain.end());
            payload.push_back(0);
            _appendText(payload, snapshot.Repository.CommonDirectoryIdentity.Digest());
            _appendText(payload, snapshot.Repository.WorktreeIdentity.Digest());
            _appendText(payload, snapshot.Repository.Head);
            _appendText(payload, snapshot.Repository.BranchRef);
            _appendUint64(
                payload,
                snapshot.Repository.ObjectFormat == GitObjectFormat::Sha1 ? 1 : 2);
            _appendUint64(payload, snapshot.Repository.LinkedWorktree ? 1 : 0);
            _appendUint64(payload, snapshot.Repository.DetachedHead ? 1 : 0);
            // stash create embeds wall-clock identity in its transient commit.
            // The trees are the canonical tracked-state proof; including the
            // commit object would make equal source states hash differently.
            _appendText(payload, snapshot.WorktreeTree);
            _appendText(payload, snapshot.IndexTree);
            _appendText(payload, snapshot.StashReference);
            _appendText(payload, snapshot.StatusDigest);
            _appendUint64(payload, snapshot.Status.HasStagedChanges ? 1 : 0);
            _appendUint64(payload, snapshot.Status.HasUnstagedChanges ? 1 : 0);
            _appendUint64(payload, snapshot.Status.IgnoredFiles);
            _appendUint64(payload, snapshot.UntrackedBytes);
            _appendUint64(payload, snapshot.UntrackedFiles.size());
            for (const auto& file : snapshot.UntrackedFiles)
            {
                _appendText(payload, file.RelativePath);
                _appendUint64(
                    payload,
                    static_cast<uint8_t>(file.Type));
                _appendUint64(payload, file.Size);
                _appendText(payload, file.Sha256);
            }
            return _digest(payload);
        }
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
                return _failure(
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
                return _failure(
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
                return _failure(
                    _inspectionFailure(initial.Status),
                    initial.SystemError,
                    "Git source capture initial inspection failed");
            }

            const auto lockTime = remaining();
            if (!lockTime)
            {
                return _failure(
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
                return _failure(
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
                return _failure(
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
                !_sameRepository(*initial.Inspection, *locked.Inspection))
            {
                return _failure(
                    locked.Succeeded() ? GitSourceCaptureStatus::SourceChanged :
                                         _inspectionFailure(locked.Status),
                    locked.SystemError,
                    "Git source identity changed while acquiring its lock");
            }

            GitSourceCaptureResult commandFailure;
            const auto run = [&](std::vector<std::wstring> arguments) ->
                std::optional<GitCommandResult> {
                const auto time = remaining();
                if (!time)
                {
                    commandFailure = _failure(
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
                    commandFailure = _failure(
                        _commandFailure(result.Status),
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
                return _failure(
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
                return _failure(
                    parsedPaths.CountExceeded ?
                        GitSourceCaptureStatus::FileCountExceeded :
                        GitSourceCaptureStatus::InvalidGitOutput,
                    ERROR_INVALID_DATA,
                    "Git source inventory output is invalid");
            }
            if (parsedStatus.Summary.UntrackedFiles != parsedPaths.Paths.size())
            {
                return _failure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git source untracked inventories disagree");
            }
            const auto stashReferenceBefore = stashBefore->ExitCode == 0 ?
                                                  _line(*stashBefore) :
                                                  std::optional<std::string>{ std::string{} };
            if (!stashReferenceBefore ||
                (!stashReferenceBefore->empty() &&
                 !_objectId(
                     *stashReferenceBefore,
                     locked.Inspection->ObjectFormat)))
            {
                return _failure(
                    GitSourceCaptureStatus::InvalidGitOutput,
                    ERROR_INVALID_DATA,
                    "Git source stash reference is invalid");
            }

            const auto trackedProof = [&](const GitSourceStatusSummary& summary) ->
                std::optional<TrackedProof> {
                TrackedProof proof;
                const auto dirty = summary.HasStagedChanges ||
                                   summary.HasUnstagedChanges;
                if (dirty)
                {
                    const auto created = run({
                        L"-c",
                        L"user.name=Richer Task Runtime",
                        L"-c",
                        L"user.email=task-runtime@localhost",
                        L"stash",
                        L"create",
                        L"Richer Task source snapshot",
                    });
                    if (!created || created->ExitCode != 0)
                    {
                        return std::nullopt;
                    }
                    const auto commit = _line(*created);
                    if (!commit ||
                        !_objectId(*commit, locked.Inspection->ObjectFormat))
                    {
                        return std::nullopt;
                    }
                    proof.SnapshotCommit = *commit;
                    const auto worktreeTree = run({
                        L"rev-parse",
                        L"--verify",
                        _wideAscii(*commit) + L"^{tree}",
                    });
                    const auto indexTree = run({
                        L"rev-parse",
                        L"--verify",
                        _wideAscii(*commit) + L"^2^{tree}",
                    });
                    if (!worktreeTree || !indexTree || worktreeTree->ExitCode != 0 ||
                        indexTree->ExitCode != 0)
                    {
                        return std::nullopt;
                    }
                    const auto worktreeLine = _line(*worktreeTree);
                    const auto indexLine = _line(*indexTree);
                    if (!worktreeLine || !indexLine ||
                        !_objectId(*worktreeLine, locked.Inspection->ObjectFormat) ||
                        !_objectId(*indexLine, locked.Inspection->ObjectFormat))
                    {
                        return std::nullopt;
                    }
                    proof.WorktreeTree = *worktreeLine;
                    proof.IndexTree = *indexLine;
                    return proof;
                }

                const auto headTree = run({
                    L"rev-parse",
                    L"--verify",
                    L"HEAD^{tree}",
                });
                const auto tree = headTree ? _line(*headTree) : std::nullopt;
                if (!headTree || headTree->ExitCode != 0 || !tree ||
                    !_objectId(*tree, locked.Inspection->ObjectFormat))
                {
                    return std::nullopt;
                }
                proof.WorktreeTree = *tree;
                proof.IndexTree = *tree;
                return proof;
            };

            const auto proofA = trackedProof(parsedStatus.Summary);
            if (!proofA)
            {
                return !commandFailure.Error.empty() ?
                           commandFailure :
                           _failure(
                               GitSourceCaptureStatus::InvalidGitOutput,
                               ERROR_INVALID_DATA,
                               "Git tracked source proof could not be created");
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
                    return _failure(
                        _fileFailure(file.Status),
                        file.SystemError,
                        "Git source untracked file could not be captured");
                }
                if (!_validFileRecord(*file.File, relativePath))
                {
                    return _failure(
                        GitSourceCaptureStatus::UnsafeUntrackedFile,
                        ERROR_INVALID_DATA,
                        "Git source untracked file record is invalid");
                }
                if (file.File->Size > MaximumGitSnapshotUntrackedBytes - totalBytes)
                {
                    return _failure(
                        GitSourceCaptureStatus::ByteLimitExceeded,
                        ERROR_FILE_TOO_LARGE,
                        "Git source untracked files exceed their total byte limit");
                }
                totalBytes += file.File->Size;
                firstFiles.emplace_back(*file.File);
            }

            const auto proofB = trackedProof(parsedStatus.Summary);
            if (!proofB && !commandFailure.Error.empty())
            {
                return commandFailure;
            }
            if (!proofB || proofA->WorktreeTree != proofB->WorktreeTree ||
                proofA->IndexTree != proofB->IndexTree)
            {
                return _failure(
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
                return _failure(
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
                    !_validFileRecord(*file.File, parsedPaths.Paths[index]) ||
                    !_sameFile(firstFiles[index], *file.File))
                {
                    return _failure(
                        file.Succeeded() ? GitSourceCaptureStatus::SourceChanged :
                                           _fileFailure(file.Status),
                        file.SystemError,
                        "Git source untracked file changed during snapshot capture");
                }
            }

            const auto proofC = trackedProof(parsedStatus.Summary);
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
            if (!proofC || !finalStatus || !finalPaths || !stashAfter)
            {
                return !commandFailure.Error.empty() ?
                           commandFailure :
                           _failure(
                               GitSourceCaptureStatus::SourceChanged,
                               ERROR_INVALID_DATA,
                               "Git source final proof could not be created");
            }
            if (proofB->WorktreeTree != proofC->WorktreeTree ||
                proofB->IndexTree != proofC->IndexTree ||
                finalStatus->ExitCode != 0 || finalPaths->ExitCode != 0 ||
                finalStatus->Stdout != statusBefore->Stdout ||
                finalPaths->Stdout != pathsBefore->Stdout ||
                (stashAfter->ExitCode != 0 && stashAfter->ExitCode != 1 &&
                 stashAfter->ExitCode != 128))
            {
                return _failure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git source final proof differs from the captured state");
            }
            const auto stashReferenceAfter = stashAfter->ExitCode == 0 ?
                                                 _line(*stashAfter) :
                                                 std::optional<std::string>{ std::string{} };
            if (!stashReferenceAfter ||
                *stashReferenceAfter != *stashReferenceBefore)
            {
                return _failure(
                    GitSourceCaptureStatus::SourceChanged,
                    ERROR_INVALID_DATA,
                    "Git stash reference changed during source capture");
            }

            const auto finalTime = remaining();
            if (!finalTime)
            {
                return _failure(
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
                !_sameRepository(*locked.Inspection, *finalInspection.Inspection))
            {
                return _failure(
                    finalInspection.Succeeded() ? GitSourceCaptureStatus::SourceChanged :
                                                  _inspectionFailure(finalInspection.Status),
                    finalInspection.SystemError,
                    "Git source repository identity changed during capture");
            }

            const auto statusDigest = _digest(std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(statusBefore->Stdout.data()),
                statusBefore->Stdout.size(),
            });
            if (!statusDigest)
            {
                return _failure(
                    GitSourceCaptureStatus::ResourceFailure,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Git source status digest failed");
            }

            GitSourceCaptureResult result;
            result.Status = GitSourceCaptureStatus::Succeeded;
            result.Snapshot.emplace();
            result.Snapshot->Repository = *locked.Inspection;
            result.Snapshot->Status = parsedStatus.Summary;
            result.Snapshot->TrackedSnapshotCommit = proofC->SnapshotCommit;
            result.Snapshot->WorktreeTree = proofC->WorktreeTree;
            result.Snapshot->IndexTree = proofC->IndexTree;
            result.Snapshot->StashReference = *stashReferenceBefore;
            result.Snapshot->StatusDigest = *statusDigest;
            result.Snapshot->UntrackedFiles = std::move(firstFiles);
            result.Snapshot->UntrackedBytes = totalBytes;
            result.Snapshot->CapturedAtUnixMilliseconds =
                finalInspection.Inspection->CapturedAtUnixMilliseconds;
            const auto snapshotDigest = _snapshotDigest(*result.Snapshot);
            if (!snapshotDigest)
            {
                return _failure(
                    GitSourceCaptureStatus::ResourceFailure,
                    ERROR_NOT_ENOUGH_MEMORY,
                    "Git source snapshot digest failed");
            }
            result.Snapshot->SnapshotDigest = *snapshotDigest;
            return result;
        }
        catch (...)
        {
            return _failure(
                GitSourceCaptureStatus::ResourceFailure,
                ERROR_NOT_ENOUGH_MEMORY,
                "Git source capture exhausted local resources");
        }
    }
}
