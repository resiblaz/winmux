// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitSnapshotFile.h"
#include "TaskGitTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskExecution;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    class GitSnapshotFileTests
    {
        TEST_CLASS(GitSnapshotFileTests);

        TEST_METHOD(HashesUnicodeRegularFileWithStableIdentity);
        TEST_METHOD(RejectsInvalidMissingDirectoryAndReparseAttributes);
        TEST_METHOD(RejectsOversizedAndCancelledFilesWithoutReadingThem);
    };

    void GitSnapshotFileTests::HashesUnicodeRegularFileWithStableIdentity()
    {
        TemporaryDirectory temporary;
        const auto folder = temporary.Path() / L"目录";
        VERIFY_IS_TRUE(std::filesystem::create_directory(folder));
        const auto file = folder / L"文件.txt";
        GitFixture::Write(file, "abc");
        const auto root = ResolveCanonicalDirectoryIdentity(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        VERIFY_IS_TRUE(root.Succeeded());
        auto cancellation = MakeEvent();
        const auto relative = Utf8Path(file.lexically_relative(temporary.Path()));
        const auto result = HashGitSnapshotFile(
            *root.Identity,
            relative,
            cancellation.Get());
        VERIFY_IS_TRUE(result.Succeeded());
        VERIFY_ARE_EQUAL(relative, result.File->RelativePath);
        VERIFY_ARE_EQUAL(uint64_t{ 3 }, result.File->Size);
        VERIFY_ARE_EQUAL(
            std::string{ "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
            result.File->Sha256);
    }

    void GitSnapshotFileTests::RejectsInvalidMissingDirectoryAndReparseAttributes()
    {
        TemporaryDirectory temporary;
        const auto root = ResolveCanonicalDirectoryIdentity(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        VERIFY_IS_TRUE(root.Succeeded());
        auto cancellation = MakeEvent();
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "../escape", cancellation.Get()).Status ==
            GitSnapshotFileStatus::InvalidPath);
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "/absolute", cancellation.Get()).Status ==
            GitSnapshotFileStatus::InvalidPath);
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "C:drive-relative", cancellation.Get()).Status ==
            GitSnapshotFileStatus::InvalidPath);
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "line\nbreak", cancellation.Get()).Status ==
            GitSnapshotFileStatus::InvalidPath);
        const std::string invalidUtf8{ "\xff", 1 };
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, invalidUtf8, cancellation.Get()).Status ==
            GitSnapshotFileStatus::InvalidPath);
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "missing.txt", cancellation.Get()).Status ==
            GitSnapshotFileStatus::Missing);

        const auto directory = temporary.Path() / L"directory";
        VERIFY_IS_TRUE(std::filesystem::create_directory(directory));
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "directory", cancellation.Get()).Status ==
            GitSnapshotFileStatus::UnsafeAttributes);
        VERIFY_IS_TRUE(
            details::ClassifyGitSnapshotFileAttributes(
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ==
            GitSnapshotFileStatus::UnsafeAttributes);
        VERIFY_IS_TRUE(
            details::ClassifyGitSnapshotFileAttributes(FILE_ATTRIBUTE_NORMAL) ==
            GitSnapshotFileStatus::Succeeded);
    }

    void GitSnapshotFileTests::RejectsOversizedAndCancelledFilesWithoutReadingThem()
    {
        TemporaryDirectory temporary;
        const auto file = temporary.Path() / L"large.bin";
        UniqueHandle handle{ CreateFileW(
            file.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr) };
        VERIFY_IS_TRUE(static_cast<bool>(handle));
        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(
            MaximumGitSnapshotUntrackedBytes + 1);
        VERIFY_IS_TRUE(SetFilePointerEx(handle.Get(), size, nullptr, FILE_BEGIN) != FALSE);
        VERIFY_IS_TRUE(SetEndOfFile(handle.Get()) != FALSE);
        handle = UniqueHandle{};

        const auto root = ResolveCanonicalDirectoryIdentity(
            temporary.Path(),
            DirectoryIdentityPolicy::Worktree);
        VERIFY_IS_TRUE(root.Succeeded());
        auto cancellation = MakeEvent();
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "large.bin", cancellation.Get()).Status ==
            GitSnapshotFileStatus::TooLarge);

        const auto smallFile = temporary.Path() / L"small.txt";
        GitFixture::Write(smallFile, "content");
        auto cancelled = MakeEvent(true);
        VERIFY_IS_TRUE(
            HashGitSnapshotFile(*root.Identity, "small.txt", cancelled.Get()).Status ==
            GitSnapshotFileStatus::Cancelled);
    }
}
