// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "GitWorktreeLayout.h"
#include "TaskGitTestSupport.h"

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::TaskCore;
using namespace Microsoft::Terminal::TaskGit;
using namespace TaskGitLocalTests::TestSupport;

namespace TaskGitLocalTests
{
    namespace
    {
        const TaskId FirstTask{
            "11111111-1111-4111-8111-111111111111"
        };
        const TaskId SecondTask{
            "22222222-2222-4222-8222-222222222222"
        };
    }

    class GitWorktreeLayoutTests
    {
        TEST_CLASS(GitWorktreeLayoutTests);

        TEST_METHOD(PreparesDeterministicUnicodeManagedLayout);
        TEST_METHOD(DistinguishesTaskPathsAndRejectsInvalidRequests);
        TEST_METHOD(RejectsFileCollisionAndReparseRuntimeRoot);
    };

    void GitWorktreeLayoutTests::PreparesDeterministicUnicodeManagedLayout()
    {
        TemporaryDirectory temporary;
        const auto runtimeRoot = temporary.Path() / L"运行 数据";
        VERIFY_IS_TRUE(std::filesystem::create_directory(runtimeRoot));

        const auto first = PrepareGitWorktreeLayout(runtimeRoot, FirstTask);
        const auto repeated = PrepareGitWorktreeLayout(runtimeRoot, FirstTask);
        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(repeated.Succeeded());
        VERIFY_ARE_EQUAL(
            first.Layout->ManagedRoot.Digest(),
            repeated.Layout->ManagedRoot.Digest());
        VERIFY_ARE_EQUAL(
            first.Layout->StateRoot.Digest(),
            repeated.Layout->StateRoot.Digest());
        VERIFY_IS_TRUE(first.Layout->WorktreePath == repeated.Layout->WorktreePath);
        VERIFY_IS_TRUE(first.Layout->MarkerPath == repeated.Layout->MarkerPath);
        VERIFY_IS_TRUE(first.Layout->SnapshotPath == repeated.Layout->SnapshotPath);
        VERIFY_IS_TRUE(
            first.Layout->CopyStagingPath == repeated.Layout->CopyStagingPath);
        VERIFY_IS_TRUE(std::filesystem::is_directory(
            runtimeRoot / std::wstring{ GitWorktreeRootDirectoryName } /
            std::wstring{ GitWorktreeLayoutVersionDirectoryName }));
        VERIFY_IS_TRUE(std::filesystem::is_directory(
            runtimeRoot / std::wstring{ GitWorktreeRootDirectoryName } /
            std::wstring{ GitWorktreeLayoutVersionDirectoryName } /
            std::wstring{ GitWorktreeStateDirectoryName }));
        VERIFY_IS_FALSE(std::filesystem::exists(first.Layout->WorktreePath));
        VERIFY_ARE_EQUAL(
            std::string{ "refs/heads/wt-task/" } + FirstTask.Value(),
            first.Layout->BranchRef);
        VERIFY_ARE_EQUAL(
            std::string{ "wt-task/" } + FirstTask.Value(),
            first.Layout->BranchName);
        VERIFY_IS_FALSE(first.Layout->WorktreePathUtf8.empty());
        VERIFY_IS_TRUE(first.Layout->MarkerPath.filename() ==
                       std::filesystem::path{
                           L"11111111-1111-4111-8111-111111111111.owner-v1.bin" });
        VERIFY_IS_TRUE(first.Layout->SnapshotPath.filename() ==
                       std::filesystem::path{
                           L"11111111-1111-4111-8111-111111111111.source-v1.bin" });
        VERIFY_IS_TRUE(first.Layout->CopyStagingPath.filename() ==
                       std::filesystem::path{
                           L"11111111-1111-4111-8111-111111111111.copy-v1.tmp" });
    }

    void GitWorktreeLayoutTests::DistinguishesTaskPathsAndRejectsInvalidRequests()
    {
        TemporaryDirectory temporary;
        const auto first = PrepareGitWorktreeLayout(temporary.Path(), FirstTask);
        const auto second = PrepareGitWorktreeLayout(temporary.Path(), SecondTask);
        VERIFY_IS_TRUE(first.Succeeded());
        VERIFY_IS_TRUE(second.Succeeded());
        VERIFY_IS_FALSE(first.Layout->WorktreePath == second.Layout->WorktreePath);
        VERIFY_ARE_NOT_EQUAL(first.Layout->BranchRef, second.Layout->BranchRef);

        VERIFY_IS_TRUE(
            PrepareGitWorktreeLayout(temporary.Path(), TaskId{}).Status ==
            GitWorktreeLayoutStatus::InvalidRequest);
        VERIFY_IS_TRUE(
            PrepareGitWorktreeLayout(
                temporary.Path(),
                TaskId{ "11111111-1111-4111-8111-11111111111A" })
                    .Status == GitWorktreeLayoutStatus::InvalidRequest);
        VERIFY_IS_TRUE(
            PrepareGitWorktreeLayout(
                temporary.Path() / L"missing",
                FirstTask)
                    .Status == GitWorktreeLayoutStatus::RuntimeRootRejected);
        const auto file = temporary.Path() / L"not-a-directory";
        GitFixture::Write(file, "file\n");
        VERIFY_IS_TRUE(
            PrepareGitWorktreeLayout(file, FirstTask).Status ==
            GitWorktreeLayoutStatus::RuntimeRootRejected);
    }

    void GitWorktreeLayoutTests::RejectsFileCollisionAndReparseRuntimeRoot()
    {
        TemporaryDirectory temporary;
        const auto collisionRoot = temporary.Path() / L"collision";
        VERIFY_IS_TRUE(std::filesystem::create_directory(collisionRoot));
        GitFixture::Write(
            collisionRoot / std::wstring{ GitWorktreeRootDirectoryName },
            "collision\n");
        VERIFY_IS_TRUE(
            PrepareGitWorktreeLayout(collisionRoot, FirstTask).Status ==
            GitWorktreeLayoutStatus::ManagedRootFailure);

        const auto target = temporary.Path() / L"target";
        const auto alias = temporary.Path() / L"alias";
        VERIFY_IS_TRUE(std::filesystem::create_directory(target));
        if (CreateSymbolicLinkW(
                alias.c_str(),
                target.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY |
                    SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) == FALSE)
        {
            WEX::Logging::Log::Comment(WEX::Common::String().Format(
                L"Directory symlink creation unavailable; integration check skipped (error=%lu).",
                GetLastError()));
            return;
        }
        VERIFY_IS_TRUE(
            PrepareGitWorktreeLayout(alias, FirstTask).Status ==
            GitWorktreeLayoutStatus::RuntimeRootRejected);
    }
}
