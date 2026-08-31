// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSourceCapture.h"
#include "GitSourceCaptureTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    class GitSourceCaptureTests
    {
        TEST_CLASS(GitSourceCaptureTests);

        TEST_METHOD(CapturesCleanRepositoryWithStableDigest);
        TEST_METHOD(CapturesDirtyUnicodeStateWithoutMutatingSource);
        TEST_METHOD(CapturesDirtySha256Repository);
        TEST_METHOD(DetectsVanishingAndDigestRacingUntrackedFiles);
        TEST_METHOD(DetectsTrackedAndStashReferenceRaces);
        TEST_METHOD(PropagatesCancellationDuringUntrackedCapture);
        TEST_METHOD(PreservesResourceFailureClassification);
    };

    void GitSourceCaptureTests::CapturesCleanRepositoryWithStableDigest()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"clean repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        const auto before = ObserveGitSourceState(
            fixture,
            repository,
            { L"tracked.txt" });

        GitSourceCapture capture{ fixture.GitExecutable };
        const auto first = capture.Capture(repository, fixture.Cancellation.Get());
        const auto second = capture.Capture(repository, fixture.Cancellation.Get());
        const auto after = ObserveGitSourceState(
            fixture,
            repository,
            { L"tracked.txt" });

        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(second.Succeeded());
        VERIFY_ARE_EQUAL(before.Head, after.Head);
        VERIFY_ARE_EQUAL(before.Branch, after.Branch);
        VERIFY_ARE_EQUAL(before.Status, after.Status);
        VERIFY_ARE_EQUAL(before.StashReference, after.StashReference);
        VERIFY_ARE_EQUAL(before.IndexEntries, after.IndexEntries);
        VERIFY_IS_TRUE(before.WorktreeFiles == after.WorktreeFiles);
        VERIFY_IS_FALSE(first.Snapshot->Status.HasStagedChanges);
        VERIFY_IS_FALSE(first.Snapshot->Status.HasUnstagedChanges);
        VERIFY_ARE_EQUAL(size_t{ 0 }, first.Snapshot->Status.UntrackedFiles);
        VERIFY_ARE_EQUAL(size_t{ 0 }, first.Snapshot->Status.IgnoredFiles);
        VERIFY_IS_TRUE(first.Snapshot->TrackedSnapshotCommit.empty());
        VERIFY_ARE_EQUAL(first.Snapshot->Repository.Head, before.Head);
        VERIFY_ARE_EQUAL(first.Snapshot->Repository.BranchRef, before.Branch);
        VERIFY_ARE_EQUAL(first.Snapshot->WorktreeTree, first.Snapshot->IndexTree);
        VERIFY_ARE_EQUAL(uint64_t{ 0 }, first.Snapshot->UntrackedBytes);
        VERIFY_IS_TRUE(first.Snapshot->UntrackedFiles.empty());
        VERIFY_ARE_EQUAL(size_t{ 64 }, first.Snapshot->StatusDigest.size());
        VERIFY_ARE_EQUAL(size_t{ 64 }, first.Snapshot->SnapshotDigest.size());
        VERIFY_ARE_EQUAL(first.Snapshot->SnapshotDigest, second.Snapshot->SnapshotDigest);
        VERIFY_IS_GREATER_THAN(
            first.Snapshot->CapturedAtUnixMilliseconds,
            int64_t{ 0 });

        GitFixture::Write(repository / L"new.txt", "new state\n");
        const auto changed = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(changed.Succeeded());
        VERIFY_ARE_NOT_EQUAL(
            first.Snapshot->SnapshotDigest,
            changed.Snapshot->SnapshotDigest);
    }

    void GitSourceCaptureTests::CapturesDirtyUnicodeStateWithoutMutatingSource()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"dirty repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        GitFixture::Write(repository / L".gitignore", "*.ignored\n");
        VERIFY_IS_TRUE(CommitAll(fixture, repository, L"ignore test files"));

        GitFixture::Write(repository / L"tracked.txt", "stash seed\n");
        VERIFY_IS_TRUE(CreateExistingStash(fixture, repository));
        GitFixture::Write(repository / L"tracked.txt", "staged version\n");
        VERIFY_IS_TRUE(
            fixture.Run(repository, { L"add", L"--", L"tracked.txt" }).Succeeded());
        GitFixture::Write(repository / L"tracked.txt", "worktree version\n");
        const auto nested = repository / L"嵌套";
        VERIFY_IS_TRUE(std::filesystem::create_directory(nested));
        GitFixture::Write(nested / L"文件.txt", "untracked content\n");
        GitFixture::Write(repository / L"cache.ignored", "ignored content\n");

        const std::vector<std::filesystem::path> observedFiles{
            L"tracked.txt",
            std::filesystem::path{ L"嵌套" } / L"文件.txt",
            L"cache.ignored",
        };
        const auto before = ObserveGitSourceState(
            fixture,
            repository,
            observedFiles);
        VERIFY_IS_FALSE(before.StashReference.empty());

        GitSourceCapture capture{ fixture.GitExecutable };
        const auto first = capture.Capture(repository, fixture.Cancellation.Get());
        VERIFY_IS_TRUE(first.Succeeded());
        Sleep(1100);
        const auto second = capture.Capture(repository, fixture.Cancellation.Get());
        const auto after = ObserveGitSourceState(
            fixture,
            repository,
            observedFiles);

        VERIFY_IS_TRUE(second.Succeeded());
        VERIFY_ARE_EQUAL(before.Head, after.Head);
        VERIFY_ARE_EQUAL(before.Branch, after.Branch);
        VERIFY_ARE_EQUAL(before.Status, after.Status);
        VERIFY_ARE_EQUAL(before.StashReference, after.StashReference);
        VERIFY_ARE_EQUAL(before.IndexEntries, after.IndexEntries);
        VERIFY_IS_TRUE(before.WorktreeFiles == after.WorktreeFiles);
        VERIFY_IS_FALSE(std::filesystem::exists(repository / L".git" / L"index.lock"));
        VERIFY_IS_TRUE(first.Snapshot->Status.HasStagedChanges);
        VERIFY_IS_TRUE(first.Snapshot->Status.HasUnstagedChanges);
        VERIFY_ARE_EQUAL(size_t{ 1 }, first.Snapshot->Status.UntrackedFiles);
        VERIFY_ARE_EQUAL(size_t{ 1 }, first.Snapshot->Status.IgnoredFiles);
        VERIFY_ARE_EQUAL(size_t{ 1 }, first.Snapshot->UntrackedFiles.size());
        VERIFY_ARE_EQUAL(
            Utf8Path(std::filesystem::path{ L"嵌套" } / L"文件.txt"),
            first.Snapshot->UntrackedFiles.front().RelativePath);
        VERIFY_ARE_EQUAL(
            uint64_t{ sizeof("untracked content\n") - 1 },
            first.Snapshot->UntrackedFiles.front().Size);
        VERIFY_IS_TRUE(
            first.Snapshot->UntrackedFiles.front().Type ==
            GitSourceFileType::RegularFile);
        VERIFY_ARE_EQUAL(size_t{ 64 }, first.Snapshot->UntrackedFiles.front().Sha256.size());
        VERIFY_ARE_EQUAL(
            first.Snapshot->UntrackedFiles.front().Size,
            first.Snapshot->UntrackedBytes);
        VERIFY_ARE_EQUAL(before.StashReference, first.Snapshot->StashReference);
        VERIFY_IS_FALSE(first.Snapshot->TrackedSnapshotCommit.empty());
        VERIFY_ARE_NOT_EQUAL(first.Snapshot->WorktreeTree, first.Snapshot->IndexTree);
        VERIFY_ARE_NOT_EQUAL(
            first.Snapshot->TrackedSnapshotCommit,
            second.Snapshot->TrackedSnapshotCommit);
        VERIFY_ARE_EQUAL(first.Snapshot->SnapshotDigest, second.Snapshot->SnapshotDigest);
    }

    void GitSourceCaptureTests::DetectsVanishingAndDigestRacingUntrackedFiles()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"racing repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        const auto untracked = repository / L"race.txt";
        GitFixture::Write(untracked, "first\n");

        size_t vanishCalls{};
        GitSourceCapture vanish{
            fixture.GitExecutable,
            {},
            [&](const CanonicalDirectoryIdentity& root,
                const std::string_view relativePath,
                const HANDLE cancellationEvent) {
                ++vanishCalls;
                std::error_code error;
                std::filesystem::remove(root.CanonicalPath() / L"race.txt", error);
                VERIFY_IS_FALSE(static_cast<bool>(error));
                return HashGitSnapshotFile(root, relativePath, cancellationEvent);
            }
        };
        const auto vanished = vanish.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(vanished.Status == GitSourceCaptureStatus::SourceChanged);
        VERIFY_ARE_EQUAL(size_t{ 1 }, vanishCalls);

        GitFixture::Write(untracked, "second\n");
        size_t digestCalls{};
        GitSourceCapture digestRace{
            fixture.GitExecutable,
            {},
            [&](const CanonicalDirectoryIdentity& root,
                const std::string_view relativePath,
                const HANDLE cancellationEvent) {
                auto result = HashGitSnapshotFile(root, relativePath, cancellationEvent);
                ++digestCalls;
                if (result.Succeeded() && digestCalls == 2)
                {
                    result.File->Sha256.front() =
                        result.File->Sha256.front() == '0' ? '1' : '0';
                }
                return result;
            }
        };
        const auto raced = digestRace.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(raced.Status == GitSourceCaptureStatus::SourceChanged);
        VERIFY_ARE_EQUAL(size_t{ 2 }, digestCalls);
    }

    void GitSourceCaptureTests::CapturesDirtySha256Repository()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"sha256 repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository, true));
        GitFixture::Write(repository / L"tracked.txt", "sha256 staged\n");
        VERIFY_IS_TRUE(
            fixture.Run(repository, { L"add", L"--", L"tracked.txt" }).Succeeded());

        GitSourceCapture capture{ fixture.GitExecutable };
        const auto result = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_IS_TRUE(
            result.Snapshot->Repository.ObjectFormat == GitObjectFormat::Sha256);
        VERIFY_ARE_EQUAL(size_t{ 64 }, result.Snapshot->Repository.Head.size());
        VERIFY_ARE_EQUAL(size_t{ 64 }, result.Snapshot->TrackedSnapshotCommit.size());
        VERIFY_ARE_EQUAL(size_t{ 64 }, result.Snapshot->WorktreeTree.size());
        VERIFY_ARE_EQUAL(size_t{ 64 }, result.Snapshot->IndexTree.size());
        VERIFY_IS_TRUE(result.Snapshot->Status.HasStagedChanges);
    }

    void GitSourceCaptureTests::DetectsTrackedAndStashReferenceRaces()
    {
        GitFixture fixture;
        const auto trackedRepository = fixture.Temporary.Path() / L"tracked race";
        VERIFY_IS_TRUE(fixture.Initialize(trackedRepository));
        GitFixture::Write(trackedRepository / L"tracked.txt", "first staged state\n");
        VERIFY_IS_TRUE(
            fixture.Run(trackedRepository, { L"add", L"--", L"tracked.txt" }).Succeeded());

        GitCommandRunner runner;
        bool trackedMutated{};
        GitSourceCapture trackedRace{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                auto result = runner.Run(request);
                const auto stash = std::ranges::find(
                    request.Arguments,
                    std::wstring{ L"stash" });
                if (!trackedMutated && result.Succeeded() &&
                    stash != request.Arguments.end() &&
                    std::next(stash) != request.Arguments.end() &&
                    *std::next(stash) == L"create")
                {
                    GitFixture::Write(
                        trackedRepository / L"tracked.txt",
                        "second staged state\n");
                    VERIFY_IS_TRUE(
                        fixture.Run(
                                   trackedRepository,
                                   { L"add", L"--", L"tracked.txt" })
                            .Succeeded());
                    trackedMutated = true;
                }
                return result;
            }
        };
        const auto trackedResult = trackedRace.Capture(
            trackedRepository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(trackedMutated);
        VERIFY_IS_TRUE(
            trackedResult.Status == GitSourceCaptureStatus::SourceChanged);

        const auto stashRepository = fixture.Temporary.Path() / L"stash race";
        VERIFY_IS_TRUE(fixture.Initialize(stashRepository));
        GitFixture::Write(stashRepository / L"tracked.txt", "stash state\n");
        VERIFY_IS_TRUE(CreateExistingStash(fixture, stashRepository));
        bool stashMutated{};
        GitSourceCapture stashRace{
            fixture.GitExecutable,
            [&](const GitCommandRequest& request) {
                auto result = runner.Run(request);
                if (!stashMutated && result.Succeeded() &&
                    !request.Arguments.empty() &&
                    request.Arguments.back() == L"refs/stash")
                {
                    const auto head = SingleLine(RunCompleted(
                        fixture,
                        stashRepository,
                        { L"rev-parse", L"--verify", L"HEAD" }));
                    VERIFY_IS_TRUE(
                        fixture.Run(stashRepository, {
                            L"update-ref",
                            L"refs/stash",
                            std::wstring{ head.begin(), head.end() },
                        }).Succeeded());
                    stashMutated = true;
                }
                return result;
            }
        };
        const auto stashResult = stashRace.Capture(
            stashRepository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(stashMutated);
        VERIFY_IS_TRUE(
            stashResult.Status == GitSourceCaptureStatus::SourceChanged);
    }

    void GitSourceCaptureTests::PropagatesCancellationDuringUntrackedCapture()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"cancel repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        GitFixture::Write(repository / L"cancel.txt", "cancel me\n");

        size_t calls{};
        GitSourceCapture capture{
            fixture.GitExecutable,
            {},
            [&](const CanonicalDirectoryIdentity& root,
                const std::string_view relativePath,
                const HANDLE cancellationEvent) {
                ++calls;
                VERIFY_IS_TRUE(SetEvent(cancellationEvent) != FALSE);
                return HashGitSnapshotFile(root, relativePath, cancellationEvent);
            }
        };
        const auto result = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(result.Status == GitSourceCaptureStatus::Cancelled);
        VERIFY_ARE_EQUAL(size_t{ 1 }, calls);
    }

    void GitSourceCaptureTests::PreservesResourceFailureClassification()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"resource failure";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        GitFixture::Write(repository / L"resource.txt", "resource\n");

        GitSourceCapture capture{
            fixture.GitExecutable,
            {},
            [](const CanonicalDirectoryIdentity&,
               const std::string_view,
               const HANDLE) {
                return GitSnapshotFileResult{
                    .Status = GitSnapshotFileStatus::ResourceFailure,
                    .SystemError = ERROR_NOT_ENOUGH_MEMORY,
                    .Error = "injected resource failure",
                };
            }
        };
        const auto result = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(result.Status == GitSourceCaptureStatus::ResourceFailure);
        VERIFY_ARE_EQUAL(DWORD{ ERROR_NOT_ENOUGH_MEMORY }, result.SystemError);

        GitSourceCapture malformedRecord{
            fixture.GitExecutable,
            {},
            [](const CanonicalDirectoryIdentity&,
               const std::string_view,
               const HANDLE) {
                GitSnapshotFileResult malformed;
                malformed.Status = GitSnapshotFileStatus::Succeeded;
                malformed.File = GitSourceFileRecord{
                    "different.txt",
                    GitSourceFileType::RegularFile,
                    0,
                    std::string(64, 'a'),
                };
                return malformed;
            }
        };
        VERIFY_IS_TRUE(
            malformedRecord.Capture(repository, fixture.Cancellation.Get()).Status ==
            GitSourceCaptureStatus::UnsafeUntrackedFile);
    }
}
