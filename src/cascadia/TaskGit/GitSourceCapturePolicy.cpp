// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GitSourceCapturePolicy.h"

namespace Microsoft::Terminal::TaskGit::details
{
    GitSourceCaptureResult GitSourceCaptureFailure(
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

    GitSourceCaptureStatus MapGitInspectionFailure(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitRepositoryInspectionStatus;
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

    GitSourceCaptureStatus MapGitCommandFailure(
        const Microsoft::Terminal::TaskExecution::GitCommandStatus status) noexcept
    {
        using Microsoft::Terminal::TaskExecution::GitCommandStatus;
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

    GitSourceCaptureStatus MapGitSnapshotFileFailure(
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

    bool SameGitSourceRepository(
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspection& left,
        const Microsoft::Terminal::TaskExecution::GitRepositoryInspection& right) noexcept
    {
        return left.WorktreeIdentity.Digest() == right.WorktreeIdentity.Digest() &&
               left.CommonDirectoryIdentity.Digest() ==
                   right.CommonDirectoryIdentity.Digest() &&
               left.GitDirectoryIdentity.Digest() ==
                   right.GitDirectoryIdentity.Digest() &&
               left.ObjectFormat == right.ObjectFormat &&
               left.Head == right.Head && left.BranchRef == right.BranchRef &&
               left.LinkedWorktree == right.LinkedWorktree &&
               left.DetachedHead == right.DetachedHead;
    }

    bool SameGitSourceFile(
        const GitSourceFileRecord& left,
        const GitSourceFileRecord& right) noexcept
    {
        return left.RelativePath == right.RelativePath &&
               left.Type == right.Type && left.Size == right.Size &&
               left.Sha256 == right.Sha256;
    }

    bool ValidateGitSourceFileRecord(
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

    GitSourceCaptureResult MapGitTrackedProofFailure(
        const GitTrackedSourceProofResult& proof,
        const GitSourceCaptureResult& commandFailure)
    {
        if (proof.Status == GitTrackedSourceProofStatus::CommandUnavailable &&
            !commandFailure.Error.empty())
        {
            return commandFailure;
        }
        return GitSourceCaptureFailure(
            proof.Status == GitTrackedSourceProofStatus::GitFailure ?
                GitSourceCaptureStatus::GitFailure :
                GitSourceCaptureStatus::InvalidGitOutput,
            proof.SystemError == ERROR_SUCCESS ?
                ERROR_INVALID_DATA :
                proof.SystemError,
            proof.Error.empty() ?
                "Git tracked source proof could not be created" :
                proof.Error);
    }
}
