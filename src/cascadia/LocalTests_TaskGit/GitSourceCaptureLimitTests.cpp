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
    namespace
    {
        GitSnapshotFileResult _syntheticFile(
            const std::string_view relativePath,
            const uint64_t size)
        {
            GitSnapshotFileResult result;
            result.Status = GitSnapshotFileStatus::Succeeded;
            result.File = GitSourceFileRecord{
                std::string{ relativePath },
                GitSourceFileType::RegularFile,
                size,
                std::string(64, relativePath.starts_with("a") ? 'a' : 'b'),
            };
            return result;
        }
    }

    class GitSourceCaptureLimitTests
    {
        TEST_CLASS(GitSourceCaptureLimitTests);

        TEST_METHOD(AcceptsTwoThousandUntrackedFilesAndRejectsTheNext);
        TEST_METHOD(AggregatesExactlyTwoHundredMiBAndRejectsOneMoreByte);
        TEST_METHOD(RejectsSparseOversizedAndReparseFiles);
    };

    void GitSourceCaptureLimitTests::AcceptsTwoThousandUntrackedFilesAndRejectsTheNext()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"file count repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        const auto many = repository / L"many";
        VERIFY_IS_TRUE(std::filesystem::create_directory(many));
        bool createdAllFiles{ true };
        for (size_t index = 0;
             index < MaximumGitSnapshotUntrackedFiles && createdAllFiles;
             ++index)
        {
            std::ofstream output{
                many / (L"file-" + std::to_wstring(index) + L".txt"),
                std::ios::binary | std::ios::trunc
            };
            createdAllFiles = static_cast<bool>(output);
        }
        VERIFY_IS_TRUE(createdAllFiles);

        size_t hashCalls{};
        GitSourceCapture capture{
            fixture.GitExecutable,
            {},
            [&](const CanonicalDirectoryIdentity&,
                const std::string_view relativePath,
                const HANDLE) {
                ++hashCalls;
                return _syntheticFile(relativePath, 0);
            }
        };
        const auto accepted = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(accepted.Succeeded());
        VERIFY_ARE_EQUAL(
            MaximumGitSnapshotUntrackedFiles,
            accepted.Snapshot->UntrackedFiles.size());
        VERIFY_ARE_EQUAL(
            MaximumGitSnapshotUntrackedFiles * 2,
            hashCalls);

        GitFixture::Write(repository / L"overflow.txt", {});
        hashCalls = 0;
        const auto rejected = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(rejected.Status == GitSourceCaptureStatus::FileCountExceeded);
        VERIFY_ARE_EQUAL(size_t{ 0 }, hashCalls);
    }

    void GitSourceCaptureLimitTests::AggregatesExactlyTwoHundredMiBAndRejectsOneMoreByte()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"byte limit repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        GitFixture::Write(repository / L"a.bin", {});
        GitFixture::Write(repository / L"b.bin", {});

        GitSourceCapture exact{
            fixture.GitExecutable,
            {},
            [](const CanonicalDirectoryIdentity&,
               const std::string_view relativePath,
               const HANDLE) {
                return _syntheticFile(
                    relativePath,
                    MaximumGitSnapshotUntrackedBytes / 2);
            }
        };
        const auto accepted = exact.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(accepted.Succeeded());
        VERIFY_ARE_EQUAL(
            MaximumGitSnapshotUntrackedBytes,
            accepted.Snapshot->UntrackedBytes);

        GitSourceCapture overflow{
            fixture.GitExecutable,
            {},
            [](const CanonicalDirectoryIdentity&,
               const std::string_view relativePath,
               const HANDLE) {
                return _syntheticFile(
                    relativePath,
                    relativePath.starts_with("a") ?
                        MaximumGitSnapshotUntrackedBytes :
                        uint64_t{ 1 });
            }
        };
        const auto rejected = overflow.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(rejected.Status == GitSourceCaptureStatus::ByteLimitExceeded);
    }

    void GitSourceCaptureLimitTests::RejectsSparseOversizedAndReparseFiles()
    {
        GitFixture fixture;
        const auto repository = fixture.Temporary.Path() / L"unsafe repository";
        VERIFY_IS_TRUE(fixture.Initialize(repository));
        const auto oversized = repository / L"oversized.bin";
        UniqueHandle handle{ CreateFileW(
            oversized.c_str(),
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
        handle.Reset();

        GitSourceCapture capture{ fixture.GitExecutable };
        const auto tooLarge = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(tooLarge.Status == GitSourceCaptureStatus::ByteLimitExceeded);
        VERIFY_IS_TRUE(std::filesystem::remove(oversized));

        VERIFY_IS_TRUE(
            details::ClassifyGitSnapshotFileAttributes(FILE_ATTRIBUTE_REPARSE_POINT) ==
            GitSnapshotFileStatus::UnsafeAttributes);
        const auto target = fixture.Temporary.Path() / L"outside-target.txt";
        GitFixture::Write(target, "outside\n");
        const auto link = repository / L"linked.txt";
        if (CreateSymbolicLinkW(
                link.c_str(),
                target.c_str(),
                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
        {
            WEX::Logging::Log::Comment(WEX::Common::String().Format(
                L"File symlink creation unavailable; integration check skipped (error=%lu).",
                GetLastError()));
            return;
        }

        const auto unsafe = capture.Capture(
            repository,
            fixture.Cancellation.Get());
        VERIFY_IS_TRUE(unsafe.Status == GitSourceCaptureStatus::UnsafeUntrackedFile);
    }
}
