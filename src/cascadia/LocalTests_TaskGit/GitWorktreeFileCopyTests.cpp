// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSnapshotFile.h"
#include "GitSourceCaptureTestSupport.h"
#include "GitWorktreeFileCopy.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        CanonicalDirectoryIdentity _identity(
            const std::filesystem::path& path)
        {
            const auto identity = ResolveCanonicalDirectoryIdentity(
                path,
                DirectoryIdentityPolicy::Worktree);
            VERIFY_IS_TRUE(identity.Succeeded());
            return *identity.Identity;
        }

        GitSourceFileRecord _record(
            const CanonicalDirectoryIdentity& source,
            const std::filesystem::path& relative,
            const HANDLE cancellationEvent)
        {
            const auto hashed = HashGitSnapshotFile(
                source,
                Utf8Path(relative),
                cancellationEvent);
            VERIFY_IS_TRUE(hashed.Succeeded());
            return *hashed.File;
        }
    }

    class GitWorktreeFileCopyTests
    {
        TEST_CLASS(GitWorktreeFileCopyTests);

        TEST_METHOD(CopiesNestedUnicodeFileAtomicallyAndResumes);
        TEST_METHOD(RejectsSourceChangeDestinationCollisionAndCancellation);
        TEST_METHOD(RejectsSourceDestinationAndStagingReparsePoints);
        TEST_METHOD(DiscardsOwnedPartialStagingBeforeRetry);
    };

    void GitWorktreeFileCopyTests::CopiesNestedUnicodeFileAtomicallyAndResumes()
    {
        TemporaryDirectory temporary;
        const auto sourcePath = temporary.Path() / L"source";
        const auto destinationPath = temporary.Path() / L"destination";
        const auto statePath = temporary.Path() / L"state";
        VERIFY_IS_TRUE(std::filesystem::create_directory(sourcePath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(destinationPath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(statePath));
        const auto nested = std::filesystem::path{ L"嵌套" } / L"文件.txt";
        VERIFY_IS_TRUE(std::filesystem::create_directory(
            sourcePath / nested.parent_path()));
        GitFixture::Write(sourcePath / nested, "faithful copy\n");
        const auto cancellation = MakeEvent();
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto source = _identity(sourcePath);
        const auto destination = _identity(destinationPath);
        const auto state = _identity(statePath);
        const auto file = _record(source, nested, cancellation.Get());
        const auto staging = statePath / L"task.copy-v1.tmp";

        const auto copied = CopyGitWorktreeSnapshotFile(
            source,
            destination,
            state,
            staging,
            file,
            cancellation.Get());
        VERIFY_IS_TRUE(copied.Succeeded());
        VERIFY_IS_TRUE(copied.Copied());
        VERIFY_ARE_EQUAL(
            std::string{ "faithful copy\n" },
            ReadBinaryFile(destinationPath / nested));
        const auto copiedAttributes = GetFileAttributesW(
            (destinationPath / nested).c_str());
        VERIFY_IS_TRUE(copiedAttributes != INVALID_FILE_ATTRIBUTES);
        VERIFY_IS_TRUE(
            (copiedAttributes &
             (FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0);
        VERIFY_IS_FALSE(std::filesystem::exists(staging));

        const auto resumed = CopyGitWorktreeSnapshotFile(
            source,
            destination,
            state,
            staging,
            file,
            cancellation.Get());
        VERIFY_IS_TRUE(resumed.Succeeded());
        VERIFY_IS_FALSE(resumed.Copied());
        VERIFY_IS_TRUE(
            resumed.Status == GitWorktreeFileCopyStatus::AlreadyPresent);
    }

    void GitWorktreeFileCopyTests::RejectsSourceChangeDestinationCollisionAndCancellation()
    {
        TemporaryDirectory temporary;
        const auto sourcePath = temporary.Path() / L"source";
        const auto destinationPath = temporary.Path() / L"destination";
        const auto statePath = temporary.Path() / L"state";
        VERIFY_IS_TRUE(std::filesystem::create_directory(sourcePath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(destinationPath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(statePath));
        const auto cancellation = MakeEvent();
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto source = _identity(sourcePath);
        const auto destination = _identity(destinationPath);
        const auto state = _identity(statePath);
        const auto staging = statePath / L"task.copy-v1.tmp";

        GitFixture::Write(sourcePath / L"changed.txt", "before\n");
        const auto changed = _record(
            source,
            L"changed.txt",
            cancellation.Get());
        GitFixture::Write(sourcePath / L"changed.txt", "after\n");
        VERIFY_IS_TRUE(
            CopyGitWorktreeSnapshotFile(
                source,
                destination,
                state,
                staging,
                changed,
                cancellation.Get())
                .Status == GitWorktreeFileCopyStatus::SourceChanged);

        GitFixture::Write(sourcePath / L"collision.txt", "expected\n");
        const auto collision = _record(
            source,
            L"collision.txt",
            cancellation.Get());
        GitFixture::Write(destinationPath / L"collision.txt", "different\n");
        VERIFY_IS_TRUE(
            CopyGitWorktreeSnapshotFile(
                source,
                destination,
                state,
                staging,
                collision,
                cancellation.Get())
                .Status ==
            GitWorktreeFileCopyStatus::DestinationCollision);
        VERIFY_ARE_EQUAL(
            std::string{ "different\n" },
            ReadBinaryFile(destinationPath / L"collision.txt"));

        GitFixture::Write(sourcePath / L"cancelled.txt", "cancelled\n");
        const auto cancelledFile = _record(
            source,
            L"cancelled.txt",
            cancellation.Get());
        const auto cancelled = MakeEvent(true);
        VERIFY_IS_TRUE(static_cast<bool>(cancelled));
        VERIFY_IS_TRUE(
            CopyGitWorktreeSnapshotFile(
                source,
                destination,
                state,
                staging,
                cancelledFile,
                cancelled.Get())
                .Status == GitWorktreeFileCopyStatus::Cancelled);
        VERIFY_IS_FALSE(std::filesystem::exists(
            destinationPath / L"cancelled.txt"));
    }

    void GitWorktreeFileCopyTests::RejectsSourceDestinationAndStagingReparsePoints()
    {
        TemporaryDirectory temporary;
        const auto sourcePath = temporary.Path() / L"source";
        const auto destinationPath = temporary.Path() / L"destination";
        const auto statePath = temporary.Path() / L"state";
        const auto outsidePath = temporary.Path() / L"outside";
        VERIFY_IS_TRUE(std::filesystem::create_directory(sourcePath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(destinationPath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(statePath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(outsidePath));
        const auto cancellation = MakeEvent();
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto source = _identity(sourcePath);
        const auto destination = _identity(destinationPath);
        const auto state = _identity(statePath);
        const auto staging = statePath / L"task.copy-v1.tmp";

        GitFixture::Write(sourcePath / L"regular.txt", "safe\n");
        const auto regular = _record(
            source,
            L"regular.txt",
            cancellation.Get());
        const auto sourceLink = sourcePath / L"source-link.txt";
        if (CreateSymbolicLinkW(
                sourceLink.c_str(),
                (sourcePath / L"regular.txt").c_str(),
                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE)
        {
            auto linked = regular;
            linked.RelativePath = "source-link.txt";
            VERIFY_IS_TRUE(
                CopyGitWorktreeSnapshotFile(
                    source,
                    destination,
                    state,
                    staging,
                    linked,
                    cancellation.Get())
                    .Status == GitWorktreeFileCopyStatus::UnsafeSource);
        }

        VERIFY_IS_TRUE(std::filesystem::create_directory(sourcePath / L"nested"));
        GitFixture::Write(sourcePath / L"nested" / L"file.txt", "nested\n");
        const auto nested = _record(
            source,
            std::filesystem::path{ L"nested" } / L"file.txt",
            cancellation.Get());
        const auto destinationLink = destinationPath / L"nested";
        if (CreateSymbolicLinkW(
                destinationLink.c_str(),
                outsidePath.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY |
                    SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE)
        {
            VERIFY_IS_TRUE(
                CopyGitWorktreeSnapshotFile(
                    source,
                    destination,
                    state,
                    staging,
                    nested,
                    cancellation.Get())
                    .Status == GitWorktreeFileCopyStatus::UnsafeDestination);
            VERIFY_IS_FALSE(std::filesystem::exists(outsidePath / L"file.txt"));
        }

        const auto outsideFile = outsidePath / L"staging-target.txt";
        GitFixture::Write(outsideFile, "outside\n");
        if (CreateSymbolicLinkW(
                staging.c_str(),
                outsideFile.c_str(),
                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE)
        {
            VERIFY_IS_TRUE(
                CopyGitWorktreeSnapshotFile(
                    source,
                    destination,
                    state,
                    staging,
                    regular,
                    cancellation.Get())
                    .Status == GitWorktreeFileCopyStatus::UnsafeDestination);
            VERIFY_ARE_EQUAL(
                std::string{ "outside\n" },
                ReadBinaryFile(outsideFile));
        }
    }

    void GitWorktreeFileCopyTests::DiscardsOwnedPartialStagingBeforeRetry()
    {
        TemporaryDirectory temporary;
        const auto sourcePath = temporary.Path() / L"source";
        const auto destinationPath = temporary.Path() / L"destination";
        const auto statePath = temporary.Path() / L"state";
        VERIFY_IS_TRUE(std::filesystem::create_directory(sourcePath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(destinationPath));
        VERIFY_IS_TRUE(std::filesystem::create_directory(statePath));
        const auto cancellation = MakeEvent();
        VERIFY_IS_TRUE(static_cast<bool>(cancellation));
        const auto source = _identity(sourcePath);
        const auto destination = _identity(destinationPath);
        const auto state = _identity(statePath);
        GitFixture::Write(sourcePath / L"recover.txt", "recovered\n");
        const auto file = _record(
            source,
            L"recover.txt",
            cancellation.Get());
        const auto staging = statePath / L"task.copy-v1.tmp";
        GitFixture::Write(staging, "partial");

        const auto copied = CopyGitWorktreeSnapshotFile(
            source,
            destination,
            state,
            staging,
            file,
            cancellation.Get());
        VERIFY_IS_TRUE(copied.Succeeded());
        VERIFY_IS_TRUE(copied.Copied());
        VERIFY_IS_FALSE(std::filesystem::exists(staging));
        VERIFY_ARE_EQUAL(
            std::string{ "recovered\n" },
            ReadBinaryFile(destinationPath / L"recover.txt"));
    }
}
