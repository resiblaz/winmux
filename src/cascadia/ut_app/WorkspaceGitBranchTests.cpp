// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../TerminalApp/WorkspaceGitBranch.h"

using namespace WEX::TestExecution;

namespace TerminalAppUnitTests
{
    class WorkspaceGitBranchTests
    {
        BEGIN_TEST_CLASS(WorkspaceGitBranchTests)
        END_TEST_CLASS()

        TEST_METHOD(QueriesRepositoryAndCachesResult);
        TEST_METHOD(RejectsUnsafeAndUncDirectorySyntax);

    private:
        static std::filesystem::path _findRepositoryRoot();
    };

    std::filesystem::path WorkspaceGitBranchTests::_findRepositoryRoot()
    {
        std::array candidates{
            std::filesystem::current_path(),
            std::filesystem::absolute(std::filesystem::path{ __FILE__ }).parent_path(),
        };

        for (auto candidate : candidates)
        {
            for (size_t depth = 0; depth < 8 && !candidate.empty(); ++depth)
            {
                if (std::filesystem::is_directory(candidate / L".git"))
                {
                    return candidate;
                }
                candidate = candidate.parent_path();
            }
        }
        return {};
    }

    void WorkspaceGitBranchTests::QueriesRepositoryAndCachesResult()
    {
        using winrt::TerminalApp::implementation::WorkspaceGitBranch;

        const auto repositoryRoot = _findRepositoryRoot();
        VERIFY_IS_FALSE(repositoryRoot.empty());

        WorkspaceGitBranch::ClearCacheForTests();
        const auto queried = WorkspaceGitBranch::QueryCached(repositoryRoot.wstring(), std::chrono::seconds{ 3 }, std::chrono::milliseconds{ 0 });
        VERIFY_IS_TRUE(queried.IsRepository);
        VERIFY_IS_FALSE(queried.RepositoryRoot.empty());
        VERIFY_IS_FALSE(queried.Branch.empty());
        VERIFY_IS_FALSE(queried.FromCache);

        const auto cached = WorkspaceGitBranch::QueryCached(repositoryRoot.wstring(), std::chrono::seconds{ 3 }, std::chrono::minutes{ 1 });
        VERIFY_IS_TRUE(cached.IsRepository);
        VERIFY_ARE_EQUAL(queried.RepositoryRoot, cached.RepositoryRoot);
        VERIFY_ARE_EQUAL(queried.Branch, cached.Branch);
        VERIFY_IS_TRUE(cached.FromCache);

        WorkspaceGitBranch::ClearCacheForTests();
        const auto withTrailingSlash = WorkspaceGitBranch::QueryCached(repositoryRoot.wstring() + L"\\", std::chrono::seconds{ 3 }, std::chrono::milliseconds{ 0 });
        VERIFY_IS_TRUE(withTrailingSlash.IsRepository);
        VERIFY_ARE_EQUAL(queried.RepositoryRoot, withTrailingSlash.RepositoryRoot);
        VERIFY_ARE_EQUAL(queried.Branch, withTrailingSlash.Branch);
    }

    void WorkspaceGitBranchTests::RejectsUnsafeAndUncDirectorySyntax()
    {
        using winrt::TerminalApp::implementation::WorkspaceGitBranch;

        for (const auto rejected : {
                 L"",
                 L"C:",
                 L"C:relative",
                 L"relative\\path",
                 L"\\\\server\\share",
                 L"//server/share",
                 L"\\\\?\\C:\\src",
                 L"\\\\.\\C:\\src",
                 L"\\??\\C:\\src",
                 L"file://C:/src",
                 L"C:\\src\\..\\repo",
                 L"C:/src/../repo",
                 L"C:\\src\nrepo",
             })
        {
            VERIFY_IS_FALSE(WorkspaceGitBranch::IsSupportedDirectorySyntax(rejected));
            VERIFY_IS_FALSE(WorkspaceGitBranch::IsSupportedDirectory(rejected));
        }

        for (const auto accepted : {
                 L"C:\\",
                 L"C:/",
                 L"C:\\src\\repo",
                 L"D:/work/repo",
             })
        {
            VERIFY_IS_TRUE(WorkspaceGitBranch::IsSupportedDirectorySyntax(accepted));
        }
    }
}
