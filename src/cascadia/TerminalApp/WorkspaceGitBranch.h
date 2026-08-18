// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

namespace winrt::TerminalApp::implementation
{
    struct WorkspaceGitBranchResult
    {
        winrt::hstring RepositoryRoot;
        winrt::hstring Branch;
        bool IsRepository{ false };
        bool FromCache{ false };
    };

    class WorkspaceGitBranch
    {
    public:
        static bool IsSupportedDirectory(std::wstring_view directory) noexcept;
        static WorkspaceGitBranchResult QueryCached(std::wstring_view directory,
                                                    std::chrono::milliseconds timeout,
                                                    std::chrono::milliseconds maximumAge);
        static WorkspaceGitBranchResult ParseOutputForTests(std::string_view output);
        static void ClearCacheForTests();

    private:
        static WorkspaceGitBranchResult _Query(std::wstring_view directory, std::chrono::milliseconds timeout);
    };
}
