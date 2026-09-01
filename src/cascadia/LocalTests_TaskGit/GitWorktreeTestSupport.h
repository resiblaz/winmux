// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "GitSourceCapture.h"
#include "GitWorktreePreflight.h"
#include "TaskGitTestSupport.h"

namespace TaskGitLocalTests::TestSupport
{
    struct PreparedTaskWorktree
    {
        std::filesystem::path Repository;
        Microsoft::Terminal::TaskGit::GitWorktreeLayout Layout;
        Microsoft::Terminal::TaskGit::GitWorktreeMarker Marker;
        Microsoft::Terminal::TaskGit::GitSourceSnapshot SourceSnapshot;
        Microsoft::Terminal::TaskGit::GitWorktreeSourceJournal Journal;
    };

    inline PreparedTaskWorktree PrepareTaskWorktree(
        GitFixture& fixture,
        const std::wstring_view name,
        const Microsoft::Terminal::TaskCore::TaskId& task,
        const bool sha256 = false)
    {
        using namespace Microsoft::Terminal::TaskGit;
        const auto repository = fixture.Temporary.Path() /
                                (std::wstring{ name } + L" repository");
        const auto runtimeRoot = fixture.Temporary.Path() /
                                 (std::wstring{ name } + L" runtime");
        VERIFY_IS_TRUE(fixture.Initialize(repository, sha256));
        VERIFY_IS_TRUE(std::filesystem::create_directory(runtimeRoot));

        GitSourceCapture capture{ fixture.GitExecutable };
        const auto snapshot = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(snapshot.Succeeded());
        const auto layout = PrepareGitWorktreeLayout(runtimeRoot, task);
        VERIFY_IS_TRUE(layout.Succeeded());

        GitWorktreeMarker marker;
        marker.Task = task;
        marker.RepositoryIdentityDigest =
            snapshot.Snapshot->Repository.CommonDirectoryIdentity.Digest();
        marker.BranchRef = layout.Layout->BranchRef;
        marker.OwnershipNonce =
            "99999999-9999-4999-8999-999999999999";
        marker.WorktreePath = layout.Layout->WorktreePathUtf8;
        const auto journal = MakeGitWorktreeSourceJournal(
            *layout.Layout,
            marker,
            *snapshot.Snapshot);
        VERIFY_IS_TRUE(journal.Succeeded());
        return {
            repository,
            *layout.Layout,
            std::move(marker),
            *snapshot.Snapshot,
            *journal.Journal,
        };
    }

    inline Microsoft::Terminal::TaskGit::GitWorktreePreflightResult
    InspectTaskWorktreePreflight(
        const GitFixture& fixture,
        const PreparedTaskWorktree& prepared,
        const Microsoft::Terminal::TaskGit::GitWorktreePreflightMode mode,
        const HANDLE cancellationEvent = nullptr)
    {
        Microsoft::Terminal::TaskGit::GitWorktreePreflightInspector inspector{
            fixture.GitExecutable
        };
        return inspector.Inspect(
            prepared.Layout,
            prepared.Marker,
            prepared.Journal,
            cancellationEvent ? cancellationEvent : fixture.Cancellation.Get(),
            mode);
    }

    inline void PersistTaskWorktreeEvidence(
        PreparedTaskWorktree& prepared)
    {
        using namespace Microsoft::Terminal::TaskGit;
        VERIFY_IS_TRUE(CreateGitWorktreeMarker(
                           prepared.Layout,
                           prepared.Marker)
                           .Succeeded());
        VERIFY_IS_TRUE(CreateGitWorktreeSourceJournal(
                           prepared.Layout,
                           prepared.Marker,
                           prepared.Journal)
                           .Succeeded());
        auto captured = prepared.Marker;
        captured.SnapshotDigest = prepared.Journal.SnapshotDigest;
        VERIFY_IS_TRUE(ReplaceGitWorktreeMarker(
                           prepared.Layout,
                           prepared.Marker,
                           captured)
                           .Succeeded());
        prepared.Marker = std::move(captured);
    }

    inline void RecaptureTaskWorktree(
        const GitFixture& fixture,
        PreparedTaskWorktree& prepared)
    {
        using namespace Microsoft::Terminal::TaskGit;
        GitSourceCapture capture{ fixture.GitExecutable };
        const auto snapshot = capture.Capture(
            prepared.Repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(snapshot.Succeeded());
        const auto journal = MakeGitWorktreeSourceJournal(
            prepared.Layout,
            prepared.Marker,
            *snapshot.Snapshot);
        VERIFY_IS_TRUE(journal.Succeeded());
        prepared.SourceSnapshot = *snapshot.Snapshot;
        prepared.Journal = *journal.Journal;
    }
}
