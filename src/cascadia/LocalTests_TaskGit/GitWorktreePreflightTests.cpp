// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCapture.h"
#include "GitSourceCommandOutput.h"
#include "GitWorktreePreflight.h"
#include "TaskGitTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        struct PreparedWorktree
        {
            std::filesystem::path Repository;
            GitWorktreeLayout Layout;
            GitWorktreeMarker Marker;
            GitWorktreeSourceJournal Journal;
        };

        PreparedWorktree _prepare(
            GitFixture& fixture,
            const std::wstring_view name,
            const TaskId& task,
            const bool sha256 = false)
        {
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
                *journal.Journal,
            };
        }

        GitWorktreePreflightResult _inspect(
            const GitFixture& fixture,
            const PreparedWorktree& prepared,
            const GitWorktreePreflightMode mode,
            const HANDLE cancellationEvent = nullptr)
        {
            GitWorktreePreflightInspector inspector{ fixture.GitExecutable };
            return inspector.Inspect(
                prepared.Layout,
                prepared.Marker,
                prepared.Journal,
                cancellationEvent ? cancellationEvent : fixture.Cancellation.Get(),
                mode);
        }

        void _recapture(
            const GitFixture& fixture,
            PreparedWorktree& prepared)
        {
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
            prepared.Journal = *journal.Journal;
        }
    }

    class GitWorktreePreflightTests
    {
        TEST_CLASS(GitWorktreePreflightTests);

        TEST_METHOD(AcceptsUnoccupiedAndExactOwnedRecoveryStates);
        TEST_METHOD(RejectsDeterministicBranchAndPathCollisions);
        TEST_METHOD(RejectsSparseSubmoduleFilterAndPromisorRepositories);
        TEST_METHOD(RejectsMissingObjectsChangedSourceAndCancellation);
    };

    void GitWorktreePreflightTests::AcceptsUnoccupiedAndExactOwnedRecoveryStates()
    {
        GitFixture fixture;
        const auto prepared = _prepare(
            fixture,
            L"recoverable",
            TaskId{ "81000000-0000-4000-8000-000000000001" });
        auto result = _inspect(
            fixture,
            prepared,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.Disposition == GitWorktreePreflightDisposition::CreateNew);

        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  {
                                      L"worktree",
                                      L"add",
                                      L"--detach",
                                      prepared.Layout.WorktreePath.native(),
                                      details::WidenGitSourceAscii(
                                          prepared.Journal.SourceHead),
                                  })
                           .Succeeded());
        result = _inspect(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.Disposition ==
            GitWorktreePreflightDisposition::ResumeDetached);
        VERIFY_IS_TRUE(result.ExistingRegistration.has_value());

        const auto occupied = _inspect(
            fixture,
            prepared,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(
            occupied.Status == GitWorktreePreflightStatus::PathCollision);

        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Layout.WorktreePath,
                                  {
                                      L"switch",
                                      L"-c",
                                      details::WidenGitSourceAscii(
                                          prepared.Layout.BranchName),
                                  })
                           .Succeeded());
        result = _inspect(
            fixture,
            prepared,
            GitWorktreePreflightMode::AllowOwnedRecovery);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.Disposition ==
            GitWorktreePreflightDisposition::ResumeBranch);

        const auto sha256 = _prepare(
            fixture,
            L"sha256 available",
            TaskId{ "81000000-0000-4000-8000-000000000006" },
            true);
        result = _inspect(
            fixture,
            sha256,
            GitWorktreePreflightMode::RequireUnoccupied);
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.Disposition == GitWorktreePreflightDisposition::CreateNew);
    }

    void GitWorktreePreflightTests::RejectsDeterministicBranchAndPathCollisions()
    {
        GitFixture fixture;
        const auto branch = _prepare(
            fixture,
            L"branch collision",
            TaskId{ "82000000-0000-4000-8000-000000000002" });
        VERIFY_IS_TRUE(fixture.Run(
                                  branch.Repository,
                                  {
                                      L"branch",
                                      details::WidenGitSourceAscii(
                                          branch.Layout.BranchName),
                                      details::WidenGitSourceAscii(
                                          branch.Journal.SourceHead),
                                  })
                           .Succeeded());
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                branch,
                GitWorktreePreflightMode::AllowOwnedRecovery)
                .Status == GitWorktreePreflightStatus::BranchCollision);

        const auto path = _prepare(
            fixture,
            L"path collision",
            TaskId{ "82000000-0000-4000-8000-000000000003" });
        VERIFY_IS_TRUE(std::filesystem::create_directory(
            path.Layout.WorktreePath));
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                path,
                GitWorktreePreflightMode::AllowOwnedRecovery)
                .Status == GitWorktreePreflightStatus::PathCollision);

        const auto unsafe = _prepare(
            fixture,
            L"unsafe path",
            TaskId{ "82000000-0000-4000-8000-000000000007" });
        const auto target = fixture.Temporary.Path() / L"outside worktree";
        VERIFY_IS_TRUE(std::filesystem::create_directory(target));
        if (CreateSymbolicLinkW(
                unsafe.Layout.WorktreePath.c_str(),
                target.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY |
                    SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
        {
            WEX::Logging::Log::Comment(WEX::Common::String().Format(
                L"Directory symlink creation unavailable; preflight reparse check skipped (error=%lu).",
                GetLastError()));
        }
        else
        {
            VERIFY_IS_TRUE(
                _inspect(
                    fixture,
                    unsafe,
                    GitWorktreePreflightMode::AllowOwnedRecovery)
                    .Status == GitWorktreePreflightStatus::UnsafePath);
        }
    }

    void GitWorktreePreflightTests::RejectsSparseSubmoduleFilterAndPromisorRepositories()
    {
        GitFixture fixture;
        auto prepared = _prepare(
            fixture,
            L"unsupported",
            TaskId{ "83000000-0000-4000-8000-000000000004" });

        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"core.sparseCheckout", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                prepared,
                GitWorktreePreflightMode::RequireUnoccupied)
                .Status ==
            GitWorktreePreflightStatus::UnsupportedSparseCheckout);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"core.sparseCheckout", L"false" })
                           .Succeeded());

        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"remote.origin.promisor", L"true" })
                           .Succeeded());
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                prepared,
                GitWorktreePreflightMode::RequireUnoccupied)
                .Status ==
            GitWorktreePreflightStatus::NetworkMaterialRequired);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"remote.origin.promisor", L"false" })
                           .Succeeded());

        const auto cacheInfo = std::wstring{ L"160000," } +
                               details::WidenGitSourceAscii(
                                   prepared.Journal.SourceHead) +
                               L",vendor/module";
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  {
                                      L"update-index",
                                      L"--add",
                                      L"--cacheinfo",
                                      cacheInfo,
                                  })
                           .Succeeded());
        const auto submoduleTreeResult = fixture.Run(
            prepared.Repository,
            { L"write-tree" });
        VERIFY_IS_TRUE(submoduleTreeResult.Succeeded());
        const auto submoduleTree = details::ParseGitSourceLine(
            submoduleTreeResult);
        VERIFY_IS_TRUE(submoduleTree.has_value());
        const auto capturedJournal = prepared.Journal;
        prepared.Journal.IndexTree = *submoduleTree;
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                prepared,
                GitWorktreePreflightMode::RequireUnoccupied)
                .Status == GitWorktreePreflightStatus::UnsupportedSubmodules);
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  {
                                      L"update-index",
                                      L"--force-remove",
                                      L"--",
                                      L"vendor/module",
                                  })
                           .Succeeded());
        prepared.Journal = capturedJournal;

        GitFixture::Write(
            prepared.Repository / L".gitattributes",
            "tracked.txt filter=tasktest\n");
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"add", L"--", L".gitattributes" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  {
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
                                      L"add filter attributes",
                                  })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"filter.tasktest.clean", L"cat" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"filter.tasktest.smudge", L"cat" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"config", L"filter.tasktest.required", L"true" })
                           .Succeeded());
        _recapture(fixture, prepared);
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                prepared,
                GitWorktreePreflightMode::RequireUnoccupied)
                .Status == GitWorktreePreflightStatus::UnsupportedFilter);
    }

    void GitWorktreePreflightTests::RejectsMissingObjectsChangedSourceAndCancellation()
    {
        GitFixture fixture;
        auto prepared = _prepare(
            fixture,
            L"failure evidence",
            TaskId{ "84000000-0000-4000-8000-000000000005" });

        auto missing = prepared;
        missing.Journal.TrackedSnapshotCommit.assign(40, 'f');
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                missing,
                GitWorktreePreflightMode::RequireUnoccupied)
                .Status == GitWorktreePreflightStatus::MissingObject);

        GitFixture::Write(prepared.Repository / L"tracked.txt", "changed\n");
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  { L"add", L"--", L"tracked.txt" })
                           .Succeeded());
        VERIFY_IS_TRUE(fixture.Run(
                                  prepared.Repository,
                                  {
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
                                      L"changed source",
                                  })
                           .Succeeded());
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                prepared,
                GitWorktreePreflightMode::RequireUnoccupied)
                .Status == GitWorktreePreflightStatus::RepositoryChanged);

        const auto cancelled = MakeEvent(true);
        VERIFY_IS_TRUE(static_cast<bool>(cancelled));
        VERIFY_IS_TRUE(
            _inspect(
                fixture,
                prepared,
                GitWorktreePreflightMode::RequireUnoccupied,
                cancelled.Get())
                .Status == GitWorktreePreflightStatus::Cancelled);
    }
}
